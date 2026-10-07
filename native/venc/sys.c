/* Process entry and the few libc pieces fp_venc needs (see sys.h). */
#include "sys.h"

/* The kernel starts us with sp -> argc, argv[0..argc-1], NULL, envp..., NULL (16-byte aligned). Clear the frame
 * pointer and link register so backtraces end here, then hand sp to C. */
__asm__(".text\n"
        ".global _start\n"
        ".type _start,%function\n"
        "_start:\n"
        "  mov x29, #0\n"
        "  mov x30, #0\n"
        "  mov x0, sp\n"
        "  bl start_c\n"
        "  brk #0\n");

__attribute__((noreturn, used)) void start_c(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    sys_exit(main(argc, argv, envp));
}

/* The compiler may emit calls to these for struct copies/zeroing. Byte loops are enough: the hot paths (picture
 * conversion) never use them. -ffreestanding/-fno-builtin keep clang from turning these loops back into calls. */
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        while (n--) dp[n] = sp[n];
    }
    return d;
}

size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

int str_contains(const char *hay, const char *needle)
{
    size_t n = str_len(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] == needle[i]) i++;
        if (i == n) return 1;
    }
    return n == 0;
}

int parse_uint(const char *s, long *out)
{
    long v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (*s - '0');
        if (v > 0x7fffffffL) return 0;
    }
    *out = v;
    return 1;
}

int64_t now_ns(void)
{
    struct timespec ts = {0, 0};
    sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void write_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) {
        long r = sys_write(fd, p, n);
        if (r == -EINTR || r == -EAGAIN) continue;
        if (r <= 0) return;
        p += r;
        n -= (size_t)r;
    }
}

void ls(struct line *l, const char *s)
{
    while (*s && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = *s++;
}

void ln(struct line *l, long v)
{
    char t[24];
    int i = 0;
    unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    if (v < 0) ls(l, "-");
    do t[i++] = (char)('0' + u % 10); while ((u /= 10) && i < 23);
    while (i && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = t[--i];
}

void lms(struct line *l, int64_t ns)
{
    int64_t us = ns / 1000;
    ln(l, (long)(us / 1000));
    ls(l, ".");
    ls(l, us % 1000 < 100 ? (us % 1000 < 10 ? "00" : "0") : "");
    ln(l, (long)(us % 1000));
}
