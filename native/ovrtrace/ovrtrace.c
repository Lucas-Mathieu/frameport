// SPDX-License-Identifier: GPL-3.0-only
// FramePort Meta Platform SDK tracer (diagnostics, patch frame.ovr_trace): logs the game's ovr_* calls and the
// messages that answer them, to find a request that never gets an answer (a game waiting forever, e.g. Vader
// Immortal after its intro). Nothing is answered or changed here: every call goes to the real function.
//
// Wiring: this library is the first DT_NEEDED of the game's engine library, so the engine's ovr_* imports resolve to
// the exports below. Each export is a tiny assembly stub that saves the argument registers (x0-x8, d0-d7), asks
// trace_before() for the real function (dlsym on the platform loader's handle, which also finds the language-pack
// library's functions when frame.langpacks is on), calls it with the original registers, then reports the result to
// trace_after(). Signatures don't matter (functions with more than 8 integer arguments would; the Platform SDK has
// none). Logcat tag fp_ovrtrace.
//
// What is logged: the first calls of every function (argument 0 and the result), every message ovr_PopMessage
// returns (type, request id, error flag), and every 10 s the request ids no message has answered yet with the
// function that made them ("request" = a function whose name says it starts one: the result is kept when it looks
// like a request id and the function isn't an accessor).
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TAG "fp_ovrtrace"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const char *const NAMES[] = {
#define N(name) #name,
#include "names.inc"
#undef N
};
#define COUNT (sizeof(NAMES) / sizeof(NAMES[0]))

static void *real_fn[COUNT];
static int calls[COUNT];
static int is_pop[COUNT], is_request[COUNT];
static void *loader;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef uint32_t (*get_type_t)(const void *);
typedef uint64_t (*get_id_t)(const void *);
typedef _Bool (*is_error_t)(const void *);
static get_type_t msg_type;
static get_id_t msg_id;
static is_error_t msg_error;

#define PENDING 256
static struct { uint64_t id; int fn; double t; } pending[PENDING];
static double last_report;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// accessors read data out of handles; everything else that returns a 64-bit id starts a request
static int looks_like_request(const char *n) {
    static const char *const accessors[] = {"ovr_Message_", "Array_", "_Get", "_Is", "_Has", "_Create", "_Destroy",
                                            "_Set", "_Free", "ovr_PopMessage", "ovr_Platform", "ovr_GetLoggedInUser",
                                            "ovr_Error_", "Options_", "ovr_Voip_", "ovr_Net_", "ovr_Microphone_"};
    for (size_t i = 0; i < sizeof(accessors) / sizeof(accessors[0]); ++i)
        if (strstr(n, accessors[i])) return 0;
    return 1;
}

static void init(void) {
    loader = dlopen("libovrplatformloader.so", RTLD_NOW | RTLD_NOLOAD);
    if (!loader) loader = dlopen("libovrplatformloader.so", RTLD_NOW);
    for (size_t i = 0; i < COUNT; ++i) {
        is_pop[i] = !strcmp(NAMES[i], "ovr_PopMessage");
        is_request[i] = looks_like_request(NAMES[i]);
        // "requests" whose names start with Get (e.g. ovr_User_GetLoggedInUser): Area_GetX with an upper-case X
        const char *u = strchr(NAMES[i] + 4, '_');
        if (u && !strncmp(u, "_Get", 4) && strncmp(NAMES[i], "ovr_Message_", 12) && !strstr(NAMES[i], "Array_"))
            is_request[i] = 2;  // maybe: kept only when the result looks like a request id
    }
    if (loader) {
        msg_type = (get_type_t)dlsym(loader, "ovr_Message_GetType");
        msg_id = (get_id_t)dlsym(loader, "ovr_Message_GetRequestID");
        msg_error = (is_error_t)dlsym(loader, "ovr_Message_IsError");
    }
    LOG("tracer active: %zu functions, platform loader %s", COUNT, loader ? "OK" : "MISSING");
    last_report = now();
}

void *fp_trace_before(uint64_t idx, uint64_t arg0) __attribute__((visibility("hidden")));
void *fp_trace_before(uint64_t idx, uint64_t arg0) {
    pthread_once(&once, init);
    if (idx >= COUNT) return NULL;
    if (!real_fn[idx] && loader) real_fn[idx] = dlsym(loader, NAMES[idx]);
    int n = __atomic_add_fetch(&calls[idx], 1, __ATOMIC_RELAXED);
    if (n <= 3 && !is_pop[idx]) LOG("call %s(0x%llx)%s", NAMES[idx], (unsigned long long)arg0,
                                    real_fn[idx] ? "" : " - not in the loader: returns 0");
    return real_fn[idx];
}

static void report_pending(double t) {
    int shown = 0;
    for (int i = 0; i < PENDING; ++i) {
        if (!pending[i].id) continue;
        if (shown++ < 12)
            LOG("unanswered after %.0f s: request %llu from %s", t - pending[i].t, (unsigned long long)pending[i].id,
                NAMES[pending[i].fn]);
    }
    if (shown) LOG("%d request(s) without an answer", shown);
}

uint64_t fp_trace_after(uint64_t idx, uint64_t ret) __attribute__((visibility("hidden")));
uint64_t fp_trace_after(uint64_t idx, uint64_t ret) {
    if (idx >= COUNT) return ret;
    double t = now();
    pthread_mutex_lock(&lock);
    if (is_pop[idx]) {
        if (ret && msg_type && msg_id) {
            const void *m = (const void *)(uintptr_t)ret;
            uint64_t id = msg_id(m);
            int answered = -1;
            for (int i = 0; i < PENDING; ++i)
                if (pending[i].id && pending[i].id == id) { answered = pending[i].fn; pending[i].id = 0; break; }
            LOG("message type=0x%08x request=%llu error=%d%s%s", msg_type(m), (unsigned long long)id,
                msg_error ? msg_error(m) : -1, answered >= 0 ? " answers " : "", answered >= 0 ? NAMES[answered] : "");
        }
    } else {
        int n = calls[idx];
        if (n <= 3) LOG("  %s -> 0x%llx", NAMES[idx], (unsigned long long)ret);
        // request ids are small positive counters; pointers (handles) are large
        if (is_request[idx] && ret && ret < (1ull << 40)) {
            for (int i = 0; i < PENDING; ++i)
                if (!pending[i].id) { pending[i].id = ret; pending[i].fn = (int)idx; pending[i].t = t; break; }
            if (n <= 20) LOG("request %llu <- %s", (unsigned long long)ret, NAMES[idx]);
        }
    }
    if (t - last_report >= 10.0) {
        last_report = t;
        report_pending(t);
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

// The shared stub: x16 = function index. Saves the argument registers, gets the real function, calls it with them,
// reports the result. x8 (indirect result) and d0-d7 (float/HFA arguments) are preserved, x0/x1/d0-d3 returned.
__asm__(
    ".text\n"
    ".p2align 2\n"
    "fp_trace_common:\n"
    "  stp x29, x30, [sp, #-224]!\n"
    "  mov x29, sp\n"
    "  stp x0, x1, [sp, #16]\n"
    "  stp x2, x3, [sp, #32]\n"
    "  stp x4, x5, [sp, #48]\n"
    "  stp x6, x7, [sp, #64]\n"
    "  stp d0, d1, [sp, #80]\n"
    "  stp d2, d3, [sp, #96]\n"
    "  stp d4, d5, [sp, #112]\n"
    "  stp d6, d7, [sp, #128]\n"
    "  stp x8, x16, [sp, #144]\n"
    "  mov x0, x16\n"
    "  ldr x1, [sp, #16]\n"
    "  bl fp_trace_before\n"
    "  str x0, [sp, #160]\n"
    "  ldp x0, x1, [sp, #16]\n"
    "  ldp x2, x3, [sp, #32]\n"
    "  ldp x4, x5, [sp, #48]\n"
    "  ldp x6, x7, [sp, #64]\n"
    "  ldp d0, d1, [sp, #80]\n"
    "  ldp d2, d3, [sp, #96]\n"
    "  ldp d4, d5, [sp, #112]\n"
    "  ldp d6, d7, [sp, #128]\n"
    "  ldr x8, [sp, #144]\n"
    "  ldr x17, [sp, #160]\n"
    "  cbz x17, 1f\n"
    "  blr x17\n"
    "  b 2f\n"
    "1:\n"
    "  mov x0, #0\n"
    "  mov x1, #0\n"
    "  movi d0, #0\n"
    "2:\n"
    "  stp x0, x1, [sp, #16]\n"
    "  stp d0, d1, [sp, #80]\n"
    "  stp d2, d3, [sp, #96]\n"
    "  ldr x0, [sp, #152]\n"
    "  ldr x1, [sp, #16]\n"
    "  bl fp_trace_after\n"
    "  ldp x0, x1, [sp, #16]\n"
    "  ldp d0, d1, [sp, #80]\n"
    "  ldp d2, d3, [sp, #96]\n"
    "  ldp x29, x30, [sp], #224\n"
    "  ret\n");

// one exported stub per name: x16 = its index
#define STR2(x) #x
#define STR(x) STR2(x)
static const int dummy_index_base __attribute__((unused)) = 0;
#define N(name)                                                    \
    __asm__(".text\n.p2align 2\n.globl " #name "\n.type " #name ", %function\n" #name ":\n" \
            "  mov x16, #" STR(__COUNTER__) "\n  b fp_trace_common\n");
#include "names.inc"
#undef N
