/* Signal handlers, SA_SIGINFO, SIGSEGV recovery with siglongjmp, sigaltstack, masks, timers, fork/wait. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int got_usr1, got_alarm, info_ok;
static sigjmp_buf jb;
static volatile uintptr_t fault_addr;

static void on_usr1(int s) { got_usr1 += s; }
static void on_info(int s, siginfo_t *si, void *uc) { (void)uc; info_ok = s == SIGUSR2 && si->si_signo == SIGUSR2 && si->si_code == SI_TKILL; }
static void on_segv(int s, siginfo_t *si, void *uc) { (void)s; (void)uc; fault_addr = (uintptr_t)si->si_addr; siglongjmp(jb, 7); }
static void on_alarm(int s) { (void)s; got_alarm++; }

int main(void)
{
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    printf("usr1 %d info %d\n", got_usr1, info_ok);

    sigset_t m, old;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, &old);
    raise(SIGUSR1);
    printf("blocked: usr1 %d\n", got_usr1);
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("unblocked: usr1 %d\n", got_usr1);

    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int r = sigsetjmp(jb, 1);
    if (r == 0) {
        p[100] = 1;
        printf("no fault?\n");
    }
    printf("segv recovered %d at offset %ld\n", r, (long)(fault_addr - (uintptr_t)p));
    r = sigsetjmp(jb, 1);
    if (r == 0) *(volatile int *)16 = 0;
    printf("null write recovered %d addr %lu\n", r, (unsigned long)fault_addr);

    signal(SIGALRM, on_alarm);
    struct itimerval it = {{0, 20000}, {0, 20000}};
    setitimer(ITIMER_REAL, &it, NULL);
    while (got_alarm < 3) pause();
    it.it_value.tv_usec = it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    printf("alarms %d\n", got_alarm >= 3);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        printf("child\n");
        fflush(stdout);
        _exit(42);
    }
    int st;
    waitpid(pid, &st, 0);
    printf("child exit %d\n", WEXITSTATUS(st));
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        *(volatile int *)0 = 0;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    printf("child signal %d\n", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    return 0;
}
