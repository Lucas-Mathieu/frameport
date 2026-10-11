/* Host side of the bridge: runs the guest runtime (liba32rt.so) inside this process, dispatches its hypercalls
 * and calls guest functions on behalf of host threads.
 *
 * Threads: the thread that calls a32b_init becomes the guest's main thread. Any other host thread that calls into
 * the guest first gets a guest thread of its own: the runtime's service thread pthread_creates one (WORK_ATTACH)
 * and the clone hands it to the waiting host thread (threads.c, thread_adopt), which then owns it for good. Guest
 * threads (pthread_create in guest code) are host pthreads with their guest context from the start. A host-to-guest
 * call saves the thread's guest state, runs the function until it returns to A32_RETURN_TRAMPOLINE (hypercall
 * HC_RETURN), and restores the state: calls nest (Java -> guest -> JNI -> Java -> guest ...). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <android/log.h>
#include "a32bridge.h"
#include "imports_internal.h"
#include "../../src/signals.h"

struct grt_info g_info;
JavaVM *g_vm;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int ready;

/* ---------------------------------------------------------------- logging */

static void log_write(int prio, const char *tag, const char *msg)
{
#ifdef A32B_STDERR_LOG
    fprintf(stderr, "%c/%s: %s\n", "??VDIWEF"[prio & 7], tag, msg);
#else
    __android_log_write(prio, tag, msg);
#endif
}

void bridge_log(int prio, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_write(prio, "a32bridge", msg);
}

void a32_report(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_write(ANDROID_LOG_WARN, "a32", msg);
}

JNIEnv *host_env(void)
{
    JNIEnv *env = NULL;
    if (!g_vm || (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return NULL;
    return env;
}

#ifndef A32B_STDERR_LOG
/* The translator reports guest crashes and internal errors on stderr, which is /dev/null in an app: forward it to
 * the system log. */
static void *stderr_pump(void *arg)
{
    FILE *f = fdopen((int)(intptr_t)arg, "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        log_write(ANDROID_LOG_WARN, "a32", line);
    }
    return NULL;
}

static void forward_stderr(void)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return;
    dup2(p[1], 2);
    close(p[1]);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, stderr_pump, (void *)(intptr_t)p[0]);
    pthread_attr_destroy(&attr);
}
#endif

/* A32_WATCH=<seconds>: logs every guest thread's pc/lr periodically, plus the return-address candidates on its
 * stack (words pointing into executable guest libraries, as lib+offset): where a game hangs or spins. Threads in
 * translated code are asked to leave it first, so their registers are current. */
typedef struct {
    uint32_t lo, hi, off;
    char name[48];
} xmap;

static int exec_maps(xmap *m, int max)
{
    FILE *f = fopen("/proc/self/maps", "re");
    int n = 0;
    char line[512];
    while (f && n < max && fgets(line, sizeof line, f)) {
        unsigned long long lo, hi, off;
        char perms[8], path[400] = "";
        if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %399s", &lo, &hi, perms, &off, path) < 4) continue;
        size_t pl = strlen(path);
        if (hi > 0x100000000ull || pl < 3 || strcmp(path + pl - 3, ".so")) continue; /* guest code isn't host-executable */
        const char *b = strrchr(path, '/');
        m[n].lo = (uint32_t)lo, m[n].hi = (uint32_t)hi, m[n].off = (uint32_t)off;
        snprintf(m[n].name, sizeof m[n].name, "%s", b ? b + 1 : path);
        n++;
    }
    if (f) fclose(f);
    return n;
}

static int describe(const xmap *m, int n, uint32_t a, char *out, size_t sz)
{
    for (int k = 0; k < n; k++)
        if (a >= m[k].lo && a < m[k].hi) {
            snprintf(out, sz, "%s+%x", m[k].name, a - m[k].lo + m[k].off);
            return 1;
        }
    return 0;
}

/* return addresses found on a guest stack (lib+offset), for logs */
static size_t stack_scan(const xmap *maps, int nm, uint32_t sp, char *bt, size_t sz)
{
    size_t k = 0;
    int found = 0;
    for (uint32_t a = sp & ~3u, e = a + 4096; a < e && found < 10 && mem_readable(a, 4); a += 4) {
        char d[64];
        uint32_t w = ld32(a);
        if ((w & 1) || ((w & 3) == 0)) /* Thumb return addresses are odd, ARM ones word-aligned */
            if (describe(maps, nm, (w & ~1u) - 2, d, sizeof d) && k + 70 < sz) {
                k += (size_t)snprintf(bt + k, sz - k, " %s", d);
                found++;
            }
    }
    return k;
}

/* logs where the current guest call came from: its return address and a scan of the stack */
void guest_backtrace(a32_cpu *c, const char *what)
{
    static xmap maps[256];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    int nm = exec_maps(maps, 256);
    char lr[64] = "?", bt[600] = "";
    describe(maps, nm, c->r[14] & ~1u, lr, sizeof lr);
    stack_scan(maps, nm, c->r[13], bt, sizeof bt);
    pthread_mutex_unlock(&m);
    bridge_log(ANDROID_LOG_INFO, "%s: called from %s |%s", what, lr, bt);
}

static void *watch(void *arg)
{
    int secs = (int)(intptr_t)arg;
    static xmap maps[256];
    for (;;) {
        sleep((unsigned)secs);
        threads_interrupt_all();
        usleep(2000);
        int nm = exec_maps(maps, 256);
        for (a32_thread *t = thread_main(); t; t = t->next) {
            char pc[64] = "?", lr[64] = "?", bt[600] = "";
            describe(maps, nm, t->cpu.r[15], pc, sizeof pc);
            describe(maps, nm, t->cpu.r[14] & ~1u, lr, sizeof lr);
            stack_scan(maps, nm, t->cpu.r[13], bt, sizeof bt);
            char comm[32] = "", cp[64];
            snprintf(cp, sizeof cp, "/proc/self/task/%d/comm", t->tid);
            FILE *cf = fopen(cp, "re");
            if (cf) {
                if (fgets(comm, sizeof comm, cf)) comm[strcspn(comm, "\n")] = 0;
                fclose(cf);
            }
            bridge_log(ANDROID_LOG_INFO, "watch: tid %d %s pc %s lr %s r0 %08x%s |%s", t->tid, comm, pc, lr, t->cpu.r[0],
                       t->in_guest ? " running" : "", bt);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- guest threads for host threads */

static pthread_mutex_t wq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wq_cond = PTHREAD_COND_INITIALIZER;
static a32_adopt *wq[64];
static int wq_n;

/* the calling host thread's guest thread (created on first use) */
static a32_thread *guest_thread(void)
{
    if (g_self) return g_self;
    a32_adopt ad;
    memset(&ad, 0, sizeof ad);
    pthread_mutex_lock(&wq_lock);
    while (wq_n == (int)(sizeof wq / sizeof *wq)) pthread_cond_wait(&wq_cond, &wq_lock);
    wq[wq_n++] = &ad;
    pthread_cond_broadcast(&wq_cond);
    pthread_mutex_unlock(&wq_lock);
    thread_adopt(&ad);
    a32_thread *t = g_self;
    t->cpu.thread = t;
    /* the new guest thread starts in bionic's thread entry and parks in attached_entry (HC_RETURN) */
    run_guest(&t->cpu);
    t->in_host = 1;
    return t;
}

/* HC_WAIT_WORK, on the service thread */
static uint32_t wait_work(void)
{
    pthread_mutex_lock(&wq_lock);
    while (!wq_n) pthread_cond_wait(&wq_cond, &wq_lock);
    a32_adopt *ad = wq[0];
    memmove(wq, wq + 1, (size_t)--wq_n * sizeof *wq);
    pthread_cond_broadcast(&wq_cond);
    pthread_mutex_unlock(&wq_lock);
    g_clone_adopt = ad; /* the service thread's next clone (its pthread_create) goes to ad */
    return WORK_ATTACH;
}

/* ---------------------------------------------------------------- calls into the guest */

typedef struct {
    uint32_t r[16], nzcv, q, ge, thumb, itstate, fpscr_nzcv, fpcr, fpsr, excl_valid;
    uint64_t d[32];
} saved_regs;

static void save_regs(const a32_cpu *c, saved_regs *s)
{
    memcpy(s->r, c->r, sizeof s->r);
    s->nzcv = c->nzcv, s->q = c->q, s->ge = c->ge, s->thumb = c->thumb, s->itstate = c->itstate;
    s->fpscr_nzcv = c->fpscr_nzcv, s->fpcr = c->fpcr, s->fpsr = c->fpsr, s->excl_valid = c->excl_valid;
    memcpy(s->d, c->d, sizeof s->d);
}

static void restore_regs(a32_cpu *c, const saved_regs *s)
{
    memcpy(c->r, s->r, sizeof s->r);
    c->nzcv = s->nzcv, c->q = s->q, c->ge = s->ge, c->thumb = s->thumb, c->itstate = s->itstate;
    c->fpscr_nzcv = s->fpscr_nzcv, c->fpcr = s->fpcr, c->fpsr = s->fpsr, c->excl_valid = s->excl_valid;
    memcpy(c->d, s->d, sizeof s->d);
}

uint64_t guest_call(uint32_t fn, const uint32_t *words, int n)
{
    a32_thread *t = guest_thread();
    a32_cpu *c = &t->cpu;
    saved_regs s;
    save_regs(c, &s);
    int nstack = n > 4 ? n - 4 : 0;
    /* below the interrupted code's stack pointer: everything there is free */
    uint32_t sp = (c->r[13] - 64 - 4u * (uint32_t)nstack) & ~7u;
    for (int k = 0; k < nstack; k++) st32(sp + 4u * (uint32_t)k, words[4 + k]);
    for (int k = 0; k < 4; k++) c->r[k] = k < n ? words[k] : 0;
    c->r[13] = sp;
    c->r[14] = A32_RETURN_TRAMPOLINE;
    c->r[15] = fn & ~1u;
    c->thumb = fn & 1;
    c->itstate = 0;
    c->last_exit = 0; /* never chain the interrupted code's last exit to the callee */
    int in_host = t->in_host;
    t->in_host = 0;
    run_guest(c);
    t->in_host = in_host;
    uint64_t r = c->r[0] | (uint64_t)c->r[1] << 32;
    restore_regs(c, &s);
    c->last_exit = 0;
    return r;
}

uint32_t guest_malloc(uint32_t size)
{
    return (uint32_t)guest_call(g_info.fn_malloc, &size, 1);
}

void guest_free(uint32_t p)
{
    if (p) guest_call(g_info.fn_free, &p, 1);
}

uint32_t guest_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t p = guest_malloc((uint32_t)n);
    if (p) memcpy(g2p(p), s, n);
    return p;
}

uint32_t guest_cstr(const char *s)
{
    static struct { const char *host; uint32_t guest; } cache[256];
    static int n;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    if (!s) return 0;
    if (in_guest_reach(s, strlen(s) + 1)) return p2g(s);
    pthread_mutex_lock(&m);
    for (int k = 0; k < n; k++)
        if (cache[k].host == s) {
            uint32_t g = cache[k].guest;
            pthread_mutex_unlock(&m);
            return g;
        }
    pthread_mutex_unlock(&m);
    uint32_t g = guest_strdup(s);
    pthread_mutex_lock(&m);
    if (n < 256) cache[n].host = s, cache[n++].guest = g;
    pthread_mutex_unlock(&m);
    return g;
}

A32B_API uint64_t a32b_call(uint32_t fn, const uint32_t *words, int n) { return guest_call(fn, words, n); }

/* ---------------------------------------------------------------- hypercalls */

/* gl.c; the PC harness has no GLES */
__attribute__((weak)) int gl_dispatch(a32_cpu *c, int index, argreader *R)
{
    (void)c, (void)index, (void)R;
    return 0;
}

static void unresolved(const char *name)
{
    /* once per name: games call missing optional functions in loops */
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static char *seen[256];
    static int nseen;
    pthread_mutex_lock(&m);
    for (int k = 0; k < nseen; k++)
        if (!strcmp(seen[k], name)) {
            pthread_mutex_unlock(&m);
            return;
        }
    if (nseen < 256) seen[nseen++] = strdup(name);
    pthread_mutex_unlock(&m);
    bridge_log(ANDROID_LOG_ERROR, "guest called %s, which no library defines (returned 0)", name);
}

static void hypercall(a32_cpu *c, uint32_t n);

/* while a hypercall runs host code, guest signals for this thread are delivered at once (signals.c) */
void a32_hypercall(a32_cpu *c, uint32_t n)
{
    a32_thread *t = c->thread;
    int in_host = t->in_host;
    t->in_host = 1;
    hypercall(c, n);
    t->in_host = in_host;
}

static void hypercall(a32_cpu *c, uint32_t n)
{
    a32_thread *t = c->thread;
    switch (n) {
    case HC_RETURN:
        t->stop = 1;
        return;
    case HC_READY:
        memcpy(&g_info, g2p(c->r[0]), sizeof g_info);
        t->stop = 1;
        return;
    case HC_WAIT_WORK:
        c->r[0] = wait_work();
        return;
    case HC_LOG:
        log_write((int)c->r[0], c->r[1] ? (const char *)g2p(c->r[1]) : "", c->r[2] ? (const char *)g2p(c->r[2]) : "");
        return;
    case HC_UNRESOLVED:
        unresolved((const char *)g2p(c->r[0]));
        c->r[0] = c->r[1] = 0;
        return;
    case HC_ABORT:
        bridge_log(ANDROID_LOG_FATAL, "guest runtime: %s", (const char *)g2p(c->r[0]));
        abort();
    }
    if (n >= HC_JNI && n < HC_JNI + JNI_TABLE_SIZE && jni_dispatch(c, (int)(n - HC_JNI))) return;
    if (n >= HC_VM && n < HC_VM + VM_TABLE_SIZE && vm_dispatch(c, (int)(n - HC_VM))) return;
    if (n >= HC_GL && n < HC_GL + 0x1000) {
        argreader R;
        ar_init(&R, c, 0, c->r[13] + 8);
        if (gl_dispatch(c, (int)(n - HC_GL), &R)) return;
    }
    if (n >= HC_IMPORT && n < HC_IMPORT + 0x100) {
        argreader R;
        ar_init(&R, c, 0, c->r[13] + 8); /* the stub pushed {r7, lr} */
        if (import_dispatch(c, (int)(n - HC_IMPORT), &R)) return;
        unresolved(import_names[n - HC_IMPORT]);
        c->r[0] = c->r[1] = 0;
        return;
    }
    bridge_log(ANDROID_LOG_ERROR, "unknown hypercall 0x%x", n);
    c->r[0] = (uint32_t)-ENOSYS;
}

/* ---------------------------------------------------------------- start-up and libraries */

extern char **environ;

A32B_API int a32b_init(JavaVM *vm, const char *libdir)
{
    if (ready) return 0;
    pthread_mutex_lock(&init_lock);
    if (ready) {
        pthread_mutex_unlock(&init_lock);
        return 0;
    }
    g_vm = vm;
    opt_bridge = 1;
#ifndef A32B_STDERR_LOG
    forward_stderr();
#endif
    if (getenv("A32_INTERP")) opt_interp = 1;
    if (getenv("A32_STRACE")) opt_strace = 1;
    if (getenv("A32_BREAK")) {
        char *p = getenv("A32_BREAK");
        for (int b = 0; b < 4 && *p; b++) {
            a32_break_pc[b] = (uint32_t)strtoul(p, &p, 0);
            if (*p == ',') p++;
        }
    }
    if (getenv("A32_VERBOSE")) opt_verbose = 1;
    a32_thread *t = thread_main();
    g_self = t;
    t->tid = (int)syscall(__NR_gettid);
    t->cpu.thread = t;
    int r = -1;
    if (mem_init()) {
        bridge_log(ANDROID_LOG_FATAL, "cannot set up the guest address space");
        goto out;
    }
    signals_init();
    static char rt[PATH_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", libdir);
    snprintf(rt, sizeof rt, "%s/liba32rt.so", libdir);
    char *argv[] = {rt, dir, NULL};
    load_args la = {rt, NULL, 2, argv, environ};
    if (load_program(&la, &t->cpu)) {
        bridge_log(ANDROID_LOG_FATAL, "cannot load the guest runtime %s", rt);
        goto out;
    }
    t->cpu.thread = t;
    run_guest(&t->cpu); /* until HC_READY */
    if (g_info.version != A32B_VERSION) {
        bridge_log(ANDROID_LOG_FATAL, "guest runtime version %u, bridge %u", g_info.version, A32B_VERSION);
        goto out;
    }
    t->in_host = 1;
    bridge_log(ANDROID_LOG_INFO, "guest runtime ready (%s, %s)", rt, opt_interp ? "interpreter" : "JIT");
    if (getenv("A32_WATCH")) {
        pthread_t w;
        pthread_create(&w, NULL, watch, (void *)(intptr_t)atoi(getenv("A32_WATCH")));
    }
    ready = 1;
    r = 0;
out:
    pthread_mutex_unlock(&init_lock);
    return r;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

A32B_API uint32_t a32b_load(const char *stub_path)
{
    if (!ready) return 0;
    uint32_t name = guest_strdup(base_name(stub_path));
    uint32_t w[2] = {name, 2 /* RTLD_NOW */};
    uint32_t h = (uint32_t)guest_call(g_info.fn_dlopen, w, 2);
    if (!h) {
        uint32_t e = (uint32_t)guest_call(g_info.fn_dlerror, NULL, 0);
        bridge_log(ANDROID_LOG_ERROR, "cannot load the guest library %s: %s", base_name(stub_path),
                   e ? (const char *)g2p(e) : "?");
    }
    guest_free(name);
    return h;
}

A32B_API uint32_t a32b_sym(uint32_t handle, const char *name)
{
    uint32_t gn = guest_strdup(name);
    uint32_t w[2] = {handle, gn};
    uint32_t a = (uint32_t)guest_call(g_info.fn_dlsym, w, 2);
    guest_free(gn);
    return a;
}

static int dir_of(const char *path, char *out, size_t n)
{
    const char *s = strrchr(path, '/');
    if (!s) return snprintf(out, n, "."), 0;
    snprintf(out, n, "%.*s", (int)(s - path), path);
    return 0;
}

A32B_API jint a32b_jni_onload(JavaVM *vm, const char *stub_path)
{
    char dir[PATH_MAX];
    dir_of(stub_path, dir, sizeof dir);
    if (a32b_init(vm, dir)) return JNI_ERR;
    uint32_t h = a32b_load(stub_path);
    if (!h) return JNI_ERR;
    jint ver = JNI_VERSION_1_6;
    uint32_t fn = a32b_sym(h, "JNI_OnLoad");
    if (fn) {
        uint32_t w[2] = {g_info.java_vm, 0};
        ver = (jint)guest_call(fn, w, 2);
        bridge_log(ANDROID_LOG_INFO, "%s: JNI_OnLoad returned 0x%x", base_name(stub_path), ver);
    }
    JNIEnv *env = host_env();
    if (env) natives_register_java(env, h, base_name(stub_path));
    return ver;
}

A32B_API void a32b_native_activity_oncreate(struct ANativeActivity *activity, void *saved, size_t saved_size,
                                            const char *stub_path)
{
    char dir[PATH_MAX];
    dir_of(stub_path, dir, sizeof dir);
    if (a32b_init(activity->vm, dir)) abort();
    uint32_t h = a32b_load(stub_path);
    uint32_t fn = h ? a32b_sym(h, "ANativeActivity_onCreate") : 0;
    if (!fn) {
        bridge_log(ANDROID_LOG_FATAL, "%s has no ANativeActivity_onCreate", base_name(stub_path));
        abort();
    }
    activity_create(activity, saved, saved_size, fn);
}

/* A32_BREAK: where the guest came from */
void a32_break_hit(a32_cpu *c)
{
    char what[40];
    snprintf(what, sizeof what, "A32_BREAK %08x", c->r[15]);
    guest_backtrace(c, what);
}
