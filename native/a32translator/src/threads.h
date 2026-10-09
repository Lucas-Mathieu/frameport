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
    int in_jit_section;          /* counted in jit_users (left on fault recovery) */
    uint32_t fault_pc;           /* guest pc of the instruction being executed (interpreter) */
    volatile siginfo_t fault_info;
    volatile int fault_sig;
    struct a32_thread *next;
} a32_thread;

extern __thread a32_thread *g_self;
a32_thread *thread_main(void);
long sys_clone(a32_cpu *c, uint32_t flags, uint32_t stack, uint32_t ptid, uint32_t tls, uint32_t ctid);
_Noreturn void thread_exit(a32_cpu *c, int status);
void threads_interrupt_all(void); /* make every thread leave translated code soon */

#endif
