/* Guest threads: one host pthread per guest thread. */
#ifndef A32_THREADS_H
#define A32_THREADS_H

#include <setjmp.h>
#include <signal.h>
#include "cpu.h"

typedef struct a32_thread {
    a32_cpu cpu;
    int tid;
    uint32_t clear_tid;          /* guest address cleared + futex-woken at thread exit (CLONE_CHILD_CLEARTID) */
    uint64_t sigmask;            /* guest blocked signals */
    uint64_t saved_mask;         /* mask to restore after a sigsuspend-style wait (valid if has_saved_mask) */
    int has_saved_mask;
    volatile uint64_t pending;   /* signals received by the host handler, not yet delivered */
    siginfo_t pending_info[65];
    uint32_t alt_sp, alt_size, alt_flags;
    sigjmp_buf fault_jmp;        /* recovery point for faults in guest memory accesses */
    volatile int in_guest;       /* executing guest code (interpreter or JIT) */
    volatile int in_helper;      /* JIT code called the interpreter for one instruction */
    int in_jit_section;          /* in jit_run's dispatch or translated code (a flush waits; left on fault recovery) */
    uint32_t fault_pc;           /* guest pc of the instruction being executed (interpreter) */
    volatile siginfo_t fault_info;
    volatile int fault_sig;
    volatile int stop;           /* bridge mode: leave run_guest after the current exit (set by hypercalls) */
    void *bridge;                /* bridge mode: per-thread state of the host side */
    volatile int in_host;        /* bridge mode: running host code (between guest calls, or in a hypercall) */
    int in_proxy;                /* bridge mode: running a guest signal handler from host code (signals.c) */
    struct a32_thread *next;
} a32_thread;

extern __thread a32_thread *g_self;
a32_thread *thread_main(void);
long sys_clone(a32_cpu *c, uint32_t flags, uint32_t stack, uint32_t ptid, uint32_t tls, uint32_t ctid);
_Noreturn void thread_exit(a32_cpu *c, int status);
void threads_interrupt_all(void); /* make every thread leave translated code soon */
int threads_any_in_jit(void);      /* some other thread is in jit_run's dispatch or translated code */

/* Bridge mode: the next CLONE_THREAD clone on this thread hands the new guest thread to *g_clone_adopt (another
 * host thread, which runs it with thread_adopt) instead of starting a host pthread for it. */
typedef struct a32_adopt {
    a32_thread *t;
    int tid;                     /* host tid of the adopting thread (the clone's result) */
    uint32_t flags, ptid, ctid;
    volatile int state;          /* 0 waiting, 1 handed over, 2 adopted */
} a32_adopt;
extern __thread a32_adopt *g_clone_adopt;
/* on the adopting thread: makes t its guest thread (g_self), applies the clone's TID flags */
void thread_adopt(a32_adopt *a);

#endif
