/* Run loop: executes guest code (interpreter, or JIT blocks once jit.c is linked in), handles syscalls, kuser
 * helpers, signals and faults. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "interp.h"
#include "runtime.h"
#include "signals.h"

int opt_interp, opt_strace, opt_trace, opt_verbose, opt_bridge;

_Noreturn void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "a32: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
    if (opt_bridge) usleep(300000); /* see signals.c default_action */
    exit(127);
}

void a32_dump_state(FILE *f, const a32_cpu *c)
{
    for (int r = 0; r < 16; r++) fprintf(f, "%sr%-2d %08x%s", r % 4 ? "  " : "  ", r, c->r[r], r % 4 == 3 ? "\n" : "");
    fprintf(f, "  nzcv %x q %u ge %x %s it %02x fpscr %08x\n", c->nzcv >> 28, c->q, c->ge, c->thumb ? "Thumb" : "ARM",
            c->itstate, cpu_fpscr(c));
}

int kuser_helper(a32_cpu *c)
{
    uint32_t pc = c->r[15];
    switch (pc) {
    case 0xffff0fe0: /* __kuser_get_tls */
        c->r[0] = c->tpidruro;
        break;
    case 0xffff0fc0: { /* __kuser_cmpxchg: r0 = oldval, r1 = newval, r2 = ptr; r0 = 0 and C set on success */
        uint32_t expected = c->r[0];
        int ok = __atomic_compare_exchange_n((uint32_t *)g2h(c->r[2]), &expected, c->r[1], 0, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        c->r[0] = ok ? 0 : 1;
        c->nzcv = ok ? (CPSR_Z | CPSR_C) : 0;
        c->r[3] = c->r[0];
        break;
    }
    case 0xffff0fa0: /* __kuser_memory_barrier */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        break;
    case 0xffff0f60: { /* __kuser_cmpxchg64: r0 = &oldval, r1 = &newval, r2 = ptr */
        uint64_t expected = ld64(c->r[0]), nv = ld64(c->r[1]);
        int ok = __atomic_compare_exchange_n((uint64_t *)g2h(c->r[2]), &expected, nv, 0, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        c->r[0] = ok ? 0 : 1;
        c->nzcv = ok ? (CPSR_Z | CPSR_C) : 0;
        break;
    }
    default:
        return 0;
    }
    uint32_t lr = c->r[14];
    c->thumb = lr & 1;
    c->r[15] = lr & ~1u;
    return 1;
}

/* JIT hooks (jit.c); weak so the interpreter-only build links without it */
__attribute__((weak)) int jit_run(a32_cpu *c) { (void)c; return -1; }
__attribute__((weak)) void jit_invalidate(uint32_t addr, uint32_t len) { (void)addr, (void)len; }
__attribute__((weak)) void jit_fault_cleanup(a32_thread *t) { (void)t; }

void code_invalidate(uint32_t addr, uint32_t len) { jit_invalidate(addr, len); }

static int handle_exit(a32_cpu *c, int r, const insn_t *in)
{
    switch (r) {
    case EXIT_NONE:
    case EXIT_FLUSH:
        return 0;
    case EXIT_SVC:
        do_syscall(c);
        return 0;
    case EXIT_SEGV: { /* instruction fetch from an unreadable address */
        siginfo_t si;
        memset(&si, 0, sizeof si);
        si.si_signo = SIGSEGV;
        si.si_code = SEGV_MAPERR;
        si.si_addr = g2h(c->fault_addr);
        deliver_fault(c, SIGSEGV, &si);
        return 0;
    }
    case EXIT_BKPT: {
        siginfo_t si;
        memset(&si, 0, sizeof si);
        si.si_signo = SIGTRAP;
        si.si_code = 1; /* TRAP_BRKPT */
        si.si_addr = g2h(c->r[15]);
        deliver_fault(c, SIGTRAP, &si);
        return 0;
    }
    default: {
        siginfo_t si;
        memset(&si, 0, sizeof si);
        si.si_signo = SIGILL;
        si.si_code = ILL_ILLOPC;
        si.si_addr = g2h(c->r[15]);
        if (opt_verbose) {
            fprintf(stderr, "a32: undefined instruction %0*x at 0x%08x (%s)\n", in && in->len == 2 ? 4 : 8,
                    in ? in->raw : 0, c->r[15], c->thumb ? "Thumb" : "ARM");
        }
        deliver_fault(c, SIGILL, &si);
        return 0;
    }
    }
}

static void interp_loop(a32_cpu *c, a32_thread *t)
{
    insn_t in;
    while (!t->stop) {
        if (signal_pending(t)) deliver_signals(c);
        uint32_t pc = c->r[15];
        if (pc >= 0xffff0f60u && kuser_helper(c)) continue;
        t->fault_pc = pc;
        t->in_guest = 1;
        a32_decode_at(c, pc, &in);
        if (opt_trace) {
            fprintf(stderr, "%08x: %0*x %s\n", pc, in.len == 2 ? 4 : 8, in.raw, in.thumb ? "T" : "A");
        }
        int r = a32_exec(c, &in);
        t->in_guest = 0;
        c->insn_count++;
        if (r) handle_exit(c, r, &in);
    }
}

int run_guest(a32_cpu *c)
{
    a32_thread *t = g_self;
    /* the fault recovery point of an outer run (this one is nested inside a hypercall) */
    sigjmp_buf outer;
    memcpy(&outer, &t->fault_jmp, sizeof outer);
    if (sigsetjmp(t->fault_jmp, 1)) {
        /* a guest memory access faulted; the faulting instruction has not completed */
        t->in_guest = 0;
        jit_fault_cleanup(t);
        siginfo_t si;
        memcpy(&si, (const void *)&t->fault_info, sizeof si);
        deliver_fault(c, t->fault_sig, &si);
    }
    if (!opt_interp) {
        while (!t->stop) {
            int r = jit_run(c);
            if (r < 0) break; /* no JIT: fall back to the interpreter */
            handle_exit(c, r, NULL);
            if (t->stop) break;
            if (signal_pending(t)) deliver_signals(c);
        }
        if (!t->stop) opt_interp = 1;
    }
    if (!t->stop) interp_loop(c, t);
    t->stop = 0;
    memcpy(&t->fault_jmp, &outer, sizeof outer);
    return 0;
}
