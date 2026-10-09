/* clone/fork/exit for guest threads and processes. A guest thread (CLONE_VM | CLONE_THREAD) is a host pthread
 * running its own a32_cpu; other clones become a host fork. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "interp.h"
#include "runtime.h"
#include "signals.h"

__thread a32_thread *g_self;
static a32_thread main_thread;
static int live_threads = 1;
static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER; /* guards the thread list (main_thread.next...) */

a32_thread *thread_main(void) { return &main_thread; }

struct start {
    a32_thread *t;
    uint32_t flags, ptid, ctid;
    sem_t ready;
};

static void *thread_start(void *arg)
{
    struct start *s = arg;
    a32_thread *t = s->t;
    g_self = t;
    t->tid = (int)syscall(__NR_gettid);
    if (s->flags & CLONE_CHILD_SETTID) st32(s->ctid, (uint32_t)t->tid);
    if (s->flags & CLONE_PARENT_SETTID) st32(s->ptid, (uint32_t)t->tid);
    /* done by thread_exit: bionic's pthread_exit cancels the kernel's CLEARTID for detached threads */
    if (s->flags & CLONE_CHILD_CLEARTID) t->clear_tid = s->ctid;
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    sem_post(&s->ready);
    run_guest(&t->cpu);
    return NULL;
}

long sys_clone(a32_cpu *c, uint32_t flags, uint32_t stack, uint32_t ptid, uint32_t tls, uint32_t ctid)
{
    a32_thread *self = g_self;
    if ((flags & CLONE_VM) && (flags & CLONE_THREAD)) {
        a32_thread *t = calloc(1, sizeof *t);
        if (!t) return -ENOMEM;
        t->cpu = *c;
        t->cpu.thread = t;
        t->cpu.r[0] = 0;
        if (stack) t->cpu.r[13] = stack;
        if (flags & CLONE_SETTLS) t->cpu.tpidruro = tls;
        t->cpu.excl_valid = 0;
        t->sigmask = self->sigmask;
        struct start s = {t, flags, ptid, ctid};
        sem_init(&s.ready, 0, 0);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1 << 20);
        __atomic_add_fetch(&live_threads, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&list_lock);
        t->next = main_thread.next;
        main_thread.next = t;
        pthread_mutex_unlock(&list_lock);
        pthread_t th;
        int e = pthread_create(&th, &attr, thread_start, &s);
        pthread_attr_destroy(&attr);
        if (e) {
            __atomic_sub_fetch(&live_threads, 1, __ATOMIC_SEQ_CST);
            free(t);
            return -e;
        }
        while (sem_wait(&s.ready) && errno == EINTR) {}
        sem_destroy(&s.ready);
        return t->tid;
    }
    /* a new process (fork, vfork, posix_spawn's CLONE_VM|CLONE_VFORK): the memory is copied, not shared */
    long pid;
    if ((flags & 0xff) == SIGCHLD) {
        pid = fork();
        if (pid < 0) pid = -errno;
    } else {
        pid = syscall(__NR_clone, flags & 0xff, 0, NULL, NULL, 0);
        if (pid < 0) pid = -errno;
    }
    if (pid == 0) {
        self->tid = (int)syscall(__NR_gettid);
        if (stack) c->r[13] = stack;
        if (flags & CLONE_SETTLS) c->tpidruro = tls;
        if (flags & CLONE_CHILD_SETTID) st32(ctid, (uint32_t)self->tid);
        if (flags & CLONE_CHILD_CLEARTID) syscall(__NR_set_tid_address, g2h(ctid));
        live_threads = 1;
        return 0;
    }
    if (pid > 0 && (flags & CLONE_PARENT_SETTID)) st32(ptid, (uint32_t)pid);
    return pid;
}

void threads_interrupt_all(void)
{
    pthread_mutex_lock(&list_lock);
    for (a32_thread *t = &main_thread; t; t = t->next) __atomic_store_n(&t->cpu.interrupt, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&list_lock);
}

_Noreturn void thread_exit(a32_cpu *c, int status)
{
    (void)c;
    a32_thread *t = g_self;
    if (t != &main_thread) {
        pthread_mutex_lock(&list_lock);
        for (a32_thread **pp = &main_thread.next; *pp; pp = &(*pp)->next)
            if (*pp == t) {
                *pp = t->next;
                break;
            }
        pthread_mutex_unlock(&list_lock);
    }
    if (t->clear_tid) {
        __atomic_store_n((uint32_t *)g2h(t->clear_tid), 0, __ATOMIC_SEQ_CST);
        syscall(__NR_futex, g2h(t->clear_tid), FUTEX_WAKE, 1, NULL, NULL, 0);
    }
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_SEQ_CST) == 0) guest_exit(status);
    if (g_self == &main_thread) {
        syscall(__NR_exit, status);
        for (;;) {}
    }
    pthread_exit(NULL);
}

__attribute__((weak)) void jit_report(void) {}

_Noreturn void guest_exit(int status)
{
    jit_report();
    fflush(stderr);
    syscall(__NR_exit_group, status);
    for (;;) {}
}
