/* Guest signals. Host handlers only queue signals (per thread); they are delivered at instruction/block
 * boundaries by building the ARM kernel's signal frames (struct sigframe / rt_sigframe, VFP state included) on
 * the guest stack. Signals whose guest disposition is SIG_DFL/SIG_IGN keep that disposition on the host, so the
 * kernel applies default actions itself. Faults inside guest memory accesses arrive as synchronous signals. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "interp.h"
#include "runtime.h"
#include "signals.h"

#define G_SA_SIGINFO 0x00000004u
#define G_SA_ONSTACK 0x08000000u
#define G_SA_RESTART 0x10000000u
#define G_SA_NODEFER 0x40000000u
#define G_SA_RESETHAND 0x80000000u
#define G_SA_RESTORER 0x04000000u
#define G_SS_ONSTACK 1
#define G_SS_DISABLE 2
#define G_MINSIGSTKSZ 2048

#define RT_TRAMPOLINE 0xffff0508u /* mov r7, #173; svc 0 (loader.c) */
#define TRAMPOLINE 0xffff0500u    /* mov r7, #119; svc 0 */

#define VFP_MAGIC 0x56465001u
#define VFP_FRAME_SIZE 288u
#define UC_SIZE 744u   /* struct ucontext */
#define INFO_SIZE 128u /* struct siginfo */
#define UC_MCONTEXT 20u
#define UC_SIGMASK 104u
#define UC_REGSPACE 232u

struct g_sigaction { uint32_t handler, flags, restorer; uint64_t mask; } __attribute__((packed, aligned(4)));

static struct { uint32_t handler, flags, restorer; uint64_t mask; } acts[65];
/* bridge mode: the host process's own fault handlers (ART's, debuggerd's), which get faults outside guest code */
static struct sigaction host_fault_act[65];
static int host_fault_saved[65];
static pthread_mutex_t act_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t sigbit(int sig) { return 1ull << (sig - 1); }
#define UNBLOCKABLE (sigbit(SIGKILL) | sigbit(SIGSTOP))

static int is_fault_sig(int sig) { return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE; }

/* jit.c: rebuilds the guest state for a fault in translated code (weak: interpreter-only builds) */
__attribute__((weak)) int jit_fault_state(a32_thread *t, void *uc) { (void)t, (void)uc; return 0; }
__attribute__((weak)) int jit_owns_pc(uint64_t pc) { (void)pc; return 0; }

static void deliver_in_host(a32_thread *t);

static void host_handler(int sig, siginfo_t *si, void *ucv)
{
    a32_thread *t = g_self;
    if (is_fault_sig(sig) && si->si_code > 0) {
        /* translated code uses host addressing modes: an access that wrapped the guest's 4 GiB faults above it */
        if (t && t->in_guest &&
            ((uintptr_t)si->si_addr < 0x100000000ull || jit_owns_pc(((ucontext_t *)ucv)->uc_mcontext.pc))) {
            si->si_addr = (void *)(uintptr_t)(uint32_t)(uintptr_t)si->si_addr;
            t->fault_sig = sig;
            memcpy((void *)&t->fault_info, si, sizeof *si);
            if (!jit_fault_state(t, ucv)) t->cpu.r[15] = t->fault_pc; /* interpreter: state is in the cpu */
            t->in_guest = 0;
            siglongjmp(t->fault_jmp, 1);
        }
        if (opt_bridge && host_fault_saved[sig]) {
            /* not ours: the host process's code faulted */
            struct sigaction *o = &host_fault_act[sig];
            if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction) {
                o->sa_sigaction(sig, si, ucv);
                return;
            }
            if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) {
                o->sa_handler(sig);
                return;
            }
            sigaction(sig, o, NULL);
            return; /* re-executes the faulting instruction with the host's disposition */
        }
        /* a fault in the translator itself */
        static const char msg[] = "a32: internal fault\n";
        write(2, msg, sizeof msg - 1);
        if (t) a32_dump_state(stderr, &t->cpu);
        signal(sig, SIG_DFL);
        return; /* re-executes the faulting instruction with the default action */
    }
    if (!t) return;
    t->pending_info[sig] = *si;
    __atomic_or_fetch(&t->pending, sigbit(sig), __ATOMIC_SEQ_CST);
    __atomic_store_n(&t->cpu.interrupt, 1, __ATOMIC_SEQ_CST); /* leave translated code at the next block */
    if (t->in_host && !t->in_proxy && signal_pending(t)) deliver_in_host(t);
}

/* Bridge mode: the thread runs host code (Java, or a JNI/GL call made by guest code) and would only see the signal
 * once it returns to guest code, maybe never: a guest that stops the world with signals (Boehm GC: Unity's Mono)
 * waits for the handler. So the guest handler runs right here, as the kernel would run it on an ARM device: on the
 * thread's guest stack, below whatever the guest was doing, returning through the hypercall trampoline. */
static void deliver_in_host(a32_thread *t)
{
    a32_cpu *c = &t->cpu;
    a32_cpu saved;
    memcpy(&saved, c, sizeof saved);
    t->in_proxy = 1;
    c->r[15] = A32_RETURN_TRAMPOLINE; /* what the signal frame records as the interrupted pc */
    c->thumb = 0;
    c->itstate = 0;
    c->last_exit = 0;
    deliver_signals(c);
    if (c->r[15] != A32_RETURN_TRAMPOLINE) {
        t->in_host = 0;
        run_guest(c);
        t->in_host = 1;
    }
    uint32_t irq = c->interrupt;
    memcpy(c, &saved, sizeof saved);
    c->interrupt = irq;
    c->last_exit = 0;
    t->in_proxy = 0;
}

int signal_restarts(const a32_thread *t)
{
    uint64_t ready = t->pending & ~t->sigmask;
    if (!ready) return 1;
    int sig = __builtin_ctzll(ready) + 1;
    return acts[sig].handler <= 1 || (acts[sig].flags & G_SA_RESTART);
}

static void install_host(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    uint32_t h = acts[sig].handler;
    if (!is_fault_sig(sig) && (h == 0 || h == 1)) {
        sa.sa_handler = h ? SIG_IGN : SIG_DFL;
        sa.sa_flags = acts[sig].flags & (SA_NOCLDSTOP | SA_NOCLDWAIT);
    } else {
        sa.sa_sigaction = host_handler;
        /* never SA_RESTART: an interrupted host system call must come back so the guest's handler can run;
         * do_syscall restarts it afterwards if the guest asked for that */
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | (acts[sig].flags & (SA_NOCLDSTOP | SA_NOCLDWAIT));
        if (is_fault_sig(sig)) sa.sa_flags |= SA_NODEFER;
    }
    sigfillset(&sa.sa_mask);
    if (opt_bridge && is_fault_sig(sig) && !host_fault_saved[sig]) {
        sigaction(sig, &sa, &host_fault_act[sig]);
        host_fault_saved[sig] = 1;
        return;
    }
    sigaction(sig, &sa, NULL);
}

void signals_init(void)
{
    for (int s = 1; s <= 64; s++) {
        struct sigaction old;
        if (sigaction(s, NULL, &old) == 0) acts[s].handler = old.sa_handler == SIG_IGN ? 1 : 0;
    }
    int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (unsigned k = 0; k < sizeof faults / sizeof *faults; k++) install_host(faults[k]);
    /* the guest starts with the inherited mask; the host thread receives everything and queues it (bridge mode:
     * the host's mask stays, the host process decides which signals its threads take) */
    sigset_t cur, none;
    sigemptyset(&none);
    pthread_sigmask(opt_bridge ? SIG_BLOCK : SIG_SETMASK, &none, &cur);
    uint64_t m = 0;
    for (int s = 1; s <= 64; s++)
        if (sigismember(&cur, s)) m |= sigbit(s);
    thread_main()->sigmask = m & ~UNBLOCKABLE;
}

long sys_rt_sigaction(int sig, uint32_t act, uint32_t oact, uint32_t size)
{
    if (size != 8 || sig < 1 || sig > 64) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    pthread_mutex_lock(&act_lock);
    if (oact) {
        struct g_sigaction o = {acts[sig].handler, acts[sig].flags, acts[sig].restorer, acts[sig].mask};
        memcpy((void *)(uintptr_t)oact, &o, sizeof o);
    }
    if (act) {
        struct g_sigaction n;
        memcpy(&n, (void *)(uintptr_t)act, sizeof n);
        acts[sig].handler = n.handler;
        acts[sig].flags = n.flags;
        acts[sig].restorer = n.restorer;
        acts[sig].mask = n.mask & ~UNBLOCKABLE;
        install_host(sig);
    }
    pthread_mutex_unlock(&act_lock);
    return 0;
}

long sys_rt_sigprocmask(a32_cpu *c, int how, uint32_t set, uint32_t oset, uint32_t size)
{
    (void)c;
    a32_thread *t = g_self;
    if (size != 8) return -EINVAL;
    uint64_t old = t->sigmask;
    if (set) {
        uint64_t s;
        memcpy(&s, (void *)(uintptr_t)set, 8);
        switch (how) {
        case SIG_BLOCK: t->sigmask |= s; break;
        case SIG_UNBLOCK: t->sigmask &= ~s; break;
        case SIG_SETMASK: t->sigmask = s; break;
        default: return -EINVAL;
        }
        t->sigmask &= ~UNBLOCKABLE;
    }
    if (oset) memcpy((void *)(uintptr_t)oset, &old, 8);
    return 0;
}

long sys_rt_sigsuspend(a32_cpu *c, uint32_t set, uint32_t size)
{
    (void)c;
    a32_thread *t = g_self;
    if (size != 8) return -EINVAL;
    uint64_t s;
    memcpy(&s, (void *)(uintptr_t)set, 8);
    t->saved_mask = t->sigmask;
    t->has_saved_mask = 1;
    t->sigmask = s & ~UNBLOCKABLE;
    sigset_t all, none, old;
    sigfillset(&all);
    sigemptyset(&none);
    for (;;) {
        pthread_sigmask(SIG_SETMASK, &all, &old);
        if (t->pending & ~t->sigmask) {
            pthread_sigmask(SIG_SETMASK, &old, NULL);
            break;
        }
        sigsuspend(&none);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    return -EINTR;
}

static int on_altstack(const a32_thread *t, uint32_t sp)
{
    return t->alt_size && sp - t->alt_sp <= t->alt_size;
}

long sys_sigaltstack(a32_cpu *c, uint32_t ss, uint32_t oss)
{
    a32_thread *t = g_self;
    uint32_t sp = c->r[13];
    if (oss) {
        uint32_t o[3] = {t->alt_sp, t->alt_size ? (on_altstack(t, sp) ? G_SS_ONSTACK : 0) : G_SS_DISABLE, t->alt_size};
        memcpy((void *)(uintptr_t)oss, o, sizeof o);
    }
    if (ss) {
        uint32_t n[3];
        memcpy(n, (void *)(uintptr_t)ss, sizeof n);
        if (on_altstack(t, sp)) return -EPERM;
        if (n[1] & G_SS_DISABLE) {
            t->alt_sp = t->alt_size = 0;
        } else {
            if (n[1] & ~(uint32_t)G_SS_ONSTACK) return -EINVAL;
            if (n[2] < G_MINSIGSTKSZ) return -ENOMEM;
            t->alt_sp = n[0];
            t->alt_size = n[2];
        }
    }
    return 0;
}

void siginfo_out(uint32_t g, const siginfo_t *si)
{
    uint32_t w[32];
    memset(w, 0, sizeof w);
    w[0] = si->si_signo;
    w[1] = si->si_errno;
    w[2] = si->si_code;
    int s = si->si_signo;
    if (si->si_code > 0 && (is_fault_sig(s) || s == SIGTRAP)) {
        w[3] = (uint32_t)(uintptr_t)si->si_addr;
    } else if (s == SIGCHLD && si->si_code > 0) {
        w[3] = si->si_pid, w[4] = si->si_uid, w[5] = si->si_status;
        w[6] = (uint32_t)si->si_utime, w[7] = (uint32_t)si->si_stime;
    } else if (s == SIGPOLL && si->si_code > 0) {
        w[3] = (uint32_t)si->si_band, w[4] = si->si_fd;
    } else if (si->si_code == SI_TIMER) {
        w[3] = si->si_timerid, w[4] = si->si_overrun, w[5] = (uint32_t)si->si_value.sival_int;
    } else {
        w[3] = si->si_pid, w[4] = si->si_uid, w[5] = (uint32_t)si->si_value.sival_int;
    }
    memcpy((void *)(uintptr_t)g, w, sizeof w);
}

static uint32_t cpsr_of(const a32_cpu *c)
{
    uint32_t it = c->itstate;
    return cpu_apsr(c) | (c->thumb << 5) | ((it & 3) << 25) | ((it >> 2) << 10) | 0x10;
}

static void write_uc(a32_cpu *c, a32_thread *t, uint32_t uc, int sig, uint32_t fault_addr)
{
    uint32_t *w = g2h(uc);
    memset(w, 0, UC_SIZE);
    w[2] = t->alt_sp;
    w[3] = t->alt_size ? (on_altstack(t, c->r[13]) ? G_SS_ONSTACK : 0) : G_SS_DISABLE;
    w[4] = t->alt_size;
    uint32_t *mc = g2h(uc + UC_MCONTEXT);
    mc[0] = is_fault_sig(sig) ? 14 : 0; /* trap_no */
    mc[1] = 0;
    mc[2] = (uint32_t)t->sigmask;
    for (int r = 0; r < 16; r++) mc[3 + r] = c->r[r];
    mc[19] = cpsr_of(c);
    mc[20] = fault_addr;
    uint64_t mask = t->has_saved_mask ? t->saved_mask : t->sigmask;
    t->has_saved_mask = 0;
    memcpy(g2h(uc + UC_SIGMASK), &mask, 8);
    uint32_t *vfp = g2h(uc + UC_REGSPACE);
    vfp[0] = VFP_MAGIC;
    vfp[1] = VFP_FRAME_SIZE;
    memcpy(&vfp[2], c->d, 256);
    vfp[66] = cpu_fpscr(c);
    vfp[68] = 0x40000000u; /* fpexc: EN */
    vfp[VFP_FRAME_SIZE / 4] = 0; /* end magic */
}

static void setup_frame(a32_cpu *c, int sig, const siginfo_t *si, uint32_t fault_addr)
{
    a32_thread *t = g_self;
    uint32_t handler = acts[sig].handler, flags = acts[sig].flags;
    int rt = (flags & G_SA_SIGINFO) != 0;
    uint32_t sp = c->r[13];
    if ((flags & G_SA_ONSTACK) && t->alt_size && !on_altstack(t, sp)) sp = t->alt_sp + t->alt_size;
    uint32_t size = rt ? INFO_SIZE + UC_SIZE + 8 : UC_SIZE + 8;
    uint32_t frame = (sp - size) & ~7u, uc = rt ? frame + INFO_SIZE : frame;
    if (!mem_range_mapped(frame, size)) {
        /* no room for a frame: the kernel kills the process with SIGSEGV */
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
    }
    if (rt) siginfo_out(frame, si);
    write_uc(c, t, uc, sig, fault_addr);
    c->r[0] = sig;
    if (rt) {
        c->r[1] = frame;
        c->r[2] = uc;
    }
    c->r[13] = frame;
    c->r[14] = (flags & G_SA_RESTORER) ? acts[sig].restorer : (rt ? RT_TRAMPOLINE : TRAMPOLINE);
    c->thumb = handler & 1;
    c->r[15] = handler & ~1u;
    c->itstate = 0;
    c->excl_valid = 0;
    t->sigmask |= acts[sig].mask | ((flags & G_SA_NODEFER) ? 0 : sigbit(sig));
    t->sigmask &= ~UNBLOCKABLE;
    if (flags & G_SA_RESETHAND) {
        acts[sig].handler = 0;
        acts[sig].flags &= ~G_SA_SIGINFO;
        install_host(sig);
    }
}

static void restore_uc(a32_cpu *c, uint32_t uc)
{
    a32_thread *t = g_self;
    uint32_t *mc = g2h(uc + UC_MCONTEXT);
    for (int r = 0; r < 16; r++) c->r[r] = mc[3 + r];
    uint32_t cpsr = mc[19];
    c->nzcv = cpsr & 0xf0000000u;
    c->q = (cpsr >> 27) & 1;
    c->ge = (cpsr >> 16) & 0xf;
    c->thumb = (cpsr >> 5) & 1;
    c->itstate = ((cpsr >> 25) & 3) | (((cpsr >> 10) & 0x3f) << 2);
    if (c->thumb) c->r[15] &= ~1u;
    else c->r[15] &= ~3u;
    uint64_t mask;
    memcpy(&mask, g2h(uc + UC_SIGMASK), 8);
    t->sigmask = mask & ~UNBLOCKABLE;
    uint32_t *vfp = g2h(uc + UC_REGSPACE);
    if (vfp[0] == VFP_MAGIC && vfp[1] == VFP_FRAME_SIZE) {
        memcpy(c->d, &vfp[2], 256);
        cpu_set_fpscr(c, vfp[66]);
    }
    uint32_t *st = g2h(uc + 8);
    if (st[1] & G_SS_DISABLE) {
        t->alt_sp = t->alt_size = 0;
    } else if (st[2] >= G_MINSIGSTKSZ && !on_altstack(t, c->r[13])) {
        t->alt_sp = st[0];
        t->alt_size = st[2];
    }
    c->excl_valid = 0;
}

long sys_rt_sigreturn(a32_cpu *c)
{
    uint32_t frame = c->r[13];
    if (frame & 7) {
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
    }
    restore_uc(c, frame + INFO_SIZE);
    return c->r[0];
}

long sys_sigreturn(a32_cpu *c)
{
    uint32_t frame = c->r[13];
    restore_uc(c, frame);
    return c->r[0];
}

static _Noreturn void default_action(a32_cpu *c, int sig, int report)
{
    if (report && opt_verbose) {
        fprintf(stderr, "a32: guest killed by signal %d at pc 0x%08x\n", sig, c->r[15]);
        a32_dump_state(stderr, c);
    }
    fflush(stderr);
    if (opt_bridge) usleep(300000); /* the bridge forwards stderr to the system log from another thread */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    raise(sig);
    syscall(__NR_exit_group, 128 + sig);
    for (;;) {}
}

static int ignored_by_default(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
}

void deliver_signals(a32_cpu *c)
{
    a32_thread *t = g_self;
    for (;;) {
        uint64_t ready = t->pending & ~t->sigmask;
        if (!ready) return;
        int sig = __builtin_ctzll(ready) + 1;
        __atomic_and_fetch(&t->pending, ~sigbit(sig), __ATOMIC_SEQ_CST);
        siginfo_t si = t->pending_info[sig];
        uint32_t h = acts[sig].handler;
        if (h == 1) continue;
        if (h == 0) {
            if (ignored_by_default(sig)) continue;
            if (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
                raise(SIGSTOP);
                continue;
            }
            default_action(c, sig, 0);
        }
        setup_frame(c, sig, &si, 0);
    }
}

void deliver_fault(a32_cpu *c, int sig, const siginfo_t *si)
{
    a32_thread *t = g_self;
    uint32_t h = acts[sig].handler;
    if (h <= 1 || (t->sigmask & sigbit(sig))) {
        fprintf(stderr, "a32: guest %s at pc 0x%08x (%s), address 0x%08x\n",
                sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : sig == SIGILL ? "illegal instruction" : "fault",
                c->r[15], c->thumb ? "Thumb" : "ARM", (uint32_t)(uintptr_t)si->si_addr);
        a32_dump_state(stderr, c);
        /* the top of the stack, for finding return addresses by hand */
        for (uint32_t a = c->r[13] & ~3u, k = 0; k < 64 && mem_readable(a, 4); k += 8, a += 32) {
            fprintf(stderr, "  %08x:", a);
            for (uint32_t j = 0; j < 8 && mem_readable(a + 4 * j, 4); j++) fprintf(stderr, " %08x", ld32(a + 4 * j));
            fprintf(stderr, "\n");
        }
        default_action(c, sig, 0);
    }
    setup_frame(c, sig, si, (uint32_t)(uintptr_t)si->si_addr);
}
