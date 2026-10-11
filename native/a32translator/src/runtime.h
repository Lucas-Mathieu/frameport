/* Runtime: guest address space, loader, syscalls, run loop. */
#ifndef A32_RUNTIME_H
#define A32_RUNTIME_H

#include <stdint.h>
#include <stdio.h>
#include "cpu.h"

#define GUEST_PAGE 4096u
#define GUEST_LO 0x00010000u          /* lowest mappable guest address */
#define GUEST_STACK_TOP 0xfffe0000u   /* initial stack grows down from here */
#define GUEST_STACK_SIZE (8u << 20)
#define GUEST_MMAP_TOP 0xf7000000u    /* mmap(NULL) searches top-down from here */
#define KUSER_PAGE 0xffff0000u        /* ARM Linux kuser helpers */

/* mem.c: the guest's 4 GiB, identity-mapped into the host's low address space */
int mem_init(void);
/* returns the guest address, or a negative errno */
int64_t mem_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint64_t off);
int mem_munmap(uint32_t addr, uint32_t len);
int mem_mprotect(uint32_t addr, uint32_t len, int prot);
int64_t mem_mremap(uint32_t old, uint32_t oldlen, uint32_t newlen, int flags, uint32_t newaddr);
uint32_t mem_brk(uint32_t newbrk);
void mem_set_brk(uint32_t start);
int mem_range_mapped(uint32_t addr, uint32_t len);
int mem_readable(uint32_t addr, uint32_t len);

/* loader.c */
typedef struct {
    const char *path;      /* guest executable */
    const char *sysroot;   /* prefix for PT_INTERP and absolute guest paths (like qemu -L), may be NULL */
    int argc;
    char **argv;
    char **envp;
} load_args;
int load_program(const load_args *a, a32_cpu *c);
extern char g_exe_path[4096];  /* absolute path of the guest executable (for /proc/self/exe) */

/* syscall.c */
void do_syscall(a32_cpu *c);
/* the guest called exit/exit_group (status) */
_Noreturn void guest_exit(int status);

/* dispatch.c */
extern int opt_interp;     /* interpreter only */
extern int opt_strace;     /* log syscalls */
extern int opt_trace;      /* log every executed instruction / block */
extern int opt_verbose;
extern int opt_bridge;     /* sharing the process with a host program (Android bridge): see mem.c, threads.c */
/* Runs guest code on the calling thread. Returns when a hypercall sets g_self->stop (bridge mode); a32run's
 * threads never return. Calls nest: a hypercall handler may run guest code again. */
int run_guest(a32_cpu *c);

/* Hypercalls: SVC with r7 = A32_HYPERCALL | n. The handler sets the guest's result registers itself. */
#define A32_HYPERCALL 0x0a320000u
void a32_hypercall(a32_cpu *c, uint32_t n);
#define A32_HC_RETURN 1                   /* end of a host-to-guest call */
#define A32_RETURN_TRAMPOLINE 0xffff0510u /* ARM code in the kuser page: hypercall A32_HC_RETURN */
void code_invalidate(uint32_t addr, uint32_t len);
_Noreturn void fatal(const char *fmt, ...);
/* bridge mode: a message for the system log, written synchronously (weak no-op in a32run) */
void a32_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A32_BREAK=<guest address>[,...] (up to 4): the first times the dispatcher reaches one (a block's first run always
 * does), the embedder's a32_break_hit logs where from */
extern uint32_t a32_break_pc[4];
extern int a32_break_hits[4];
void a32_break_hit(a32_cpu *c);
void a32_dump_state(FILE *f, const a32_cpu *c);

/* kuser helpers (0xffff0fa0..0xffff0fff): executes the helper at r[15], returns 1 if it was one */
int kuser_helper(a32_cpu *c);

#endif
