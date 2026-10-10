/* Runtime entry: started by the host bridge as `liba32rt.so <library dir>`. Starts the service thread, reports
 * the addresses the host needs (HC_READY) and parks; from then on the host calls guest functions directly. */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grt.h"

extern const uint32_t grt_export_table[]; /* gen/exports.S: (name, address) pairs sorted by name */
extern const uint32_t grt_export_count;
extern const uint32_t grt_jni_env, grt_java_vm; /* gen/stubs.S */

uint32_t grt_export(const char *name)
{
    uint32_t lo = 0, hi = grt_export_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(name, (const char *)(uintptr_t)grt_export_table[2 * mid]);
        if (!c) return grt_export_table[2 * mid + 1];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

void grt_log(int prio, const char *tag, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    grt_hc(HC_LOG, (uint32_t)prio, (uint32_t)(uintptr_t)tag, (uint32_t)(uintptr_t)msg, 0);
}

/* a host thread adopted this guest thread (WORK_ATTACH): it only ever runs host-to-guest calls */
static void *attached_entry(void *arg)
{
    (void)arg;
    for (;;) grt_hc(HC_RETURN, 0, 0, 0, 0);
    return NULL;
}

static void *service(void *arg)
{
    (void)arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (;;) {
        uint32_t w = grt_hc(HC_WAIT_WORK, 0, 0, 0, 0);
        if (w == WORK_ATTACH) {
            pthread_t t;
            int e = pthread_create(&t, &attr, attached_entry, NULL);
            if (e) grt_log(6, "a32rt", "attach: pthread_create failed (%d)", e);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) loader_set_dir(argv[1]);
    pthread_t svc;
    if (pthread_create(&svc, NULL, service, NULL)) {
        grt_hc(HC_ABORT, (uint32_t)(uintptr_t)"cannot start the service thread", 0, 0, 0);
        return 1;
    }
    static struct grt_info info;
    info.version = A32B_VERSION;
    info.jni_env = (uint32_t)(uintptr_t)&grt_jni_env;
    info.java_vm = (uint32_t)(uintptr_t)&grt_java_vm;
    info.fn_malloc = (uint32_t)(uintptr_t)malloc;
    info.fn_free = (uint32_t)(uintptr_t)free;
    info.fn_dlopen = (uint32_t)(uintptr_t)grt_dlopen;
    info.fn_dlsym = (uint32_t)(uintptr_t)grt_dlsym;
    info.fn_dlerror = (uint32_t)(uintptr_t)grt_dlerror;
    info.fn_attached_entry = (uint32_t)(uintptr_t)attached_entry;
    info.fn_list_symbols = (uint32_t)(uintptr_t)grt_list_symbols;
    grt_hc(HC_READY, (uint32_t)(uintptr_t)&info, 0, 0, 0);
    for (;;) grt_hc(HC_RETURN, 0, 0, 0, 0);
}
