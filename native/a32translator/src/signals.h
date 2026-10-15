#ifndef A32_SIGNALS_H
#define A32_SIGNALS_H

#include <signal.h>
#include "cpu.h"
#include "threads.h"

void signals_init(void);
long sys_rt_sigaction(int sig, uint32_t act, uint32_t oact, uint32_t size);
long sys_rt_sigprocmask(a32_cpu *c, int how, uint32_t set, uint32_t oset, uint32_t size);
long sys_rt_sigsuspend(a32_cpu *c, uint32_t set, uint32_t size);
long sys_sigaltstack(a32_cpu *c, uint32_t ss, uint32_t oss);
long sys_rt_sigreturn(a32_cpu *c);
long sys_sigreturn(a32_cpu *c);
void siginfo_out(uint32_t g, const siginfo_t *si);

/* a signal that can be delivered now (pending and not blocked by the guest) */
static inline int signal_pending(const a32_thread *t) { return (t->pending & ~t->sigmask) != 0; }
/* delivers deliverable pending signals by building frames (or applying the default action) */
void deliver_signals(a32_cpu *c);
/* whether a system call interrupted by the next deliverable signal is restarted (SA_RESTART, or a signal whose
 * disposition doesn't run a handler) */
int signal_restarts(const a32_thread *t);
/* delivers a synchronous signal caused by the guest instruction at c->r[15] */
void deliver_fault(a32_cpu *c, int sig, const siginfo_t *si);

#endif
