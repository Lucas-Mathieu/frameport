/* fp_venc's whole "libc": raw aarch64 Linux syscalls plus a few helpers. The program is fully static and
 * freestanding (no libc exists at build time), so everything it needs from the kernel goes through svc #0 here. */
#ifndef FP_VENC_SYS_H
#define FP_VENC_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>        /* our stub: struct timespec */
#include <asm/unistd.h>      /* syscall numbers (asm-generic table) */
#include <asm/errno.h>

/* open flags (asm-generic values, used by aarch64). Bionic's copy of linux/fcntl.h pulls in libc's bits/ headers, so
 * the few we need are spelled out here. */
#define O_RDONLY 00
#define O_RDWR 02
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
#define AT_FDCWD (-100)
#include <linux/mman.h>      /* PROT_*, MAP_* */
#include <linux/poll.h>      /* POLL* */

/* Syscalls return the kernel's value: >= 0 success, -errno failure. */
static inline long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

struct pollfd_k { int fd; short events; short revents; };   /* struct pollfd (not in the UAPI headers) */

static inline long sys_read(int fd, void *b, size_t n) { return sys6(__NR_read, fd, (long)b, (long)n, 0, 0, 0); }
static inline long sys_write(int fd, const void *b, size_t n) { return sys6(__NR_write, fd, (long)b, (long)n, 0, 0, 0); }
static inline int sys_open(const char *p, int flags) { return (int)sys6(__NR_openat, AT_FDCWD, (long)p, flags, 0, 0, 0); }
static inline int sys_close(int fd) { return (int)sys6(__NR_close, fd, 0, 0, 0, 0, 0); }
static inline int sys_ioctl(int fd, unsigned long req, void *arg) { return (int)sys6(__NR_ioctl, fd, (long)req, (long)arg, 0, 0, 0); }
static inline long sys_readlink(const char *p, char *b, size_t n) { return sys6(__NR_readlinkat, AT_FDCWD, (long)p, (long)b, (long)n, 0, 0); }
static inline void *sys_mmap(void *a, size_t len, int prot, int flags, int fd, long off)
{
    return (void *)sys6(__NR_mmap, (long)a, (long)len, prot, flags, fd, off);
}
static inline int sys_munmap(void *a, size_t len) { return (int)sys6(__NR_munmap, (long)a, (long)len, 0, 0, 0, 0); }
static inline int sys_ppoll(struct pollfd_k *f, unsigned n, const struct timespec *ts)
{
    return (int)sys6(__NR_ppoll, (long)f, n, (long)ts, 0, 8, 0);
}
static inline int sys_clock_gettime(int clk, struct timespec *ts) { return (int)sys6(__NR_clock_gettime, clk, (long)ts, 0, 0, 0, 0); }
__attribute__((noreturn)) static inline void sys_exit(int code)
{
    for (;;) sys6(__NR_exit_group, code, 0, 0, 0, 0, 0);
}

/* mmap returns -errno in the pointer on failure. */
static inline int mmap_failed(void *p) { return (unsigned long)p > (unsigned long)-4096; }

/* Helpers (sys.c). */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
size_t str_len(const char *s);
int str_eq(const char *a, const char *b);
int str_contains(const char *hay, const char *needle);
int parse_uint(const char *s, long *out);          /* plain decimal, 0..2^31-1; 1 = ok */
int64_t now_ns(void);                               /* CLOCK_MONOTONIC */
void write_all(int fd, const void *b, size_t n);   /* best effort (stderr) */

/* A small line builder for stderr/JSON output: no printf here. */
struct line { char b[600]; int n; };
void ls(struct line *l, const char *s);
void ln(struct line *l, long v);
void lms(struct line *l, int64_t ns);               /* nanoseconds as "<ms>.<3 digits>" */

int main(int argc, char **argv, char **envp);

#endif
