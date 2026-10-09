/* JIT-in-the-guest: generates ARM and Thumb code at run time, runs it, rewrites it (cache maintenance via
 * __builtin___clear_cache = the cacheflush syscall), runs it again. Also a signal interrupting a tight loop. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

static volatile sig_atomic_t stop;
static void on_alarm(int s) { (void)s; stop = 1; }

typedef int (*fn_t)(int);

int main(void)
{
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int total = 0;
    for (int k = 0; k < 50; k++) {
        /* ARM: add r0, r0, #k ; bx lr */
        code[0] = 0xe2800000u | (uint32_t)k;
        code[1] = 0xe12fff1eu;
        __builtin___clear_cache((char *)code, (char *)(code + 2));
        total += ((fn_t)code)(1000);
    }
    printf("arm %d\n", total);
    /* the same buffer as Thumb code: movs r1, #k ; adds r0, r0, r1 ; bx lr */
    uint16_t *t = (uint16_t *)(code + 16);
    total = 0;
    for (int k = 0; k < 50; k++) {
        t[0] = (uint16_t)(0x2100 | k);
        t[1] = 0x1840;
        t[2] = 0x4770;
        __builtin___clear_cache((char *)t, (char *)(t + 3));
        total += ((fn_t)((uintptr_t)t | 1))(7);
    }
    printf("thumb %d\n", total);
    /* rewrite through mprotect (RW, write, RX) like JIT engines with W^X */
    mprotect(code, 4096, PROT_READ | PROT_WRITE);
    code[0] = 0xe3a00063u; /* mov r0, #99 */
    code[1] = 0xe12fff1eu;
    mprotect(code, 4096, PROT_READ | PROT_EXEC);
    printf("mprotect %d\n", ((fn_t)code)(0));
    /* a signal must stop a loop that never makes a syscall */
    signal(SIGALRM, on_alarm);
    struct itimerval it = {{0, 0}, {0, 50000}};
    setitimer(ITIMER_REAL, &it, NULL);
    unsigned long spins = 0;
    while (!stop) spins++;
    printf("loop stopped %d\n", spins > 0);
    return 0;
}
