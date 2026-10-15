/* pthreads, mutexes, condition variables, atomics (LDREX/STREX), TLS. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define N 4
static atomic_int counter;
static long long total;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static __thread int tls_val;
static _Atomic long long a64;
static _Atomic unsigned char a8;
static _Atomic unsigned short a16;
static atomic_int spin;
static int guarded;

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    tls_val = id * 100;
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 20000; i++) {
        atomic_fetch_add(&counter, 1);
        atomic_fetch_add(&a64, 3);
        atomic_fetch_add(&a8, 1);
        atomic_fetch_add(&a16, 7);
        int expect = 0;
        while (!atomic_compare_exchange_weak(&spin, &expect, 1)) expect = 0;
        guarded++;
        atomic_store(&spin, 0);
        if ((i & 255) == 0) {
            pthread_mutex_lock(&mu);
            total += i + tls_val;
            pthread_mutex_unlock(&mu);
        }
    }
    return (void *)(long)(tls_val + id);
}

int main(void)
{
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], NULL, worker, (void *)(long)i);
    pthread_mutex_lock(&mu);
    ready = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    long sum = 0;
    for (int i = 0; i < N; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("counter %d a64 %lld total %lld sum %ld a8 %u a16 %u guarded %d\n", atomic_load(&counter), (long long)a64,
           total, sum, (unsigned)a8, (unsigned)a16, guarded);
    return 0;
}
