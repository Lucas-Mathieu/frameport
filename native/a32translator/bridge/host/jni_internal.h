/* Host side of the bridge: shared declarations. */
#ifndef A32B_JNI_INTERNAL_H
#define A32B_JNI_INTERNAL_H

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include "../../src/interp.h"
#include "../../src/runtime.h"
#include "../../src/threads.h"
#include "../abi.h"

/* ---------------------------------------------------------------- arguments (AAPCS32, softfp) */

/* Reads a guest call's arguments in order: core registers r[ncrn..3], then the stack. Doubleword values take an
 * even register pair or an 8-byte aligned stack slot; once one spills, every later argument is on the stack.
 * Also reads a va_list (ncrn = 4, nsaa = the va_list pointer). Variadic floats arrive promoted to double. */
typedef struct {
    a32_cpu *c;
    int ncrn;
    uint32_t nsaa;
} argreader;

static inline void ar_init(argreader *R, a32_cpu *c, int first, uint32_t stack)
{
    R->c = c;
    R->ncrn = first;
    R->nsaa = stack;
}

static inline uint32_t rd32(argreader *R)
{
    if (R->ncrn < 4) return R->c->r[R->ncrn++];
    uint32_t v = ld32(R->nsaa);
    R->nsaa += 4;
    return v;
}

static inline uint64_t rd64(argreader *R)
{
    if (R->ncrn & 1) R->ncrn++;
    if (R->ncrn <= 2) {
        uint64_t v = R->c->r[R->ncrn] | (uint64_t)R->c->r[R->ncrn + 1] << 32;
        R->ncrn += 2;
        return v;
    }
    R->ncrn = 4;
    R->nsaa = (R->nsaa + 7) & ~7u;
    uint64_t v = ld64(R->nsaa);
    R->nsaa += 8;
    return v;
}

static inline float rdf(argreader *R)
{
    uint32_t b = rd32(R);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static inline double rdd(argreader *R)
{
    uint64_t b = rd64(R);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* host data the guest can address directly (it lies in the low 4 GiB) */
static inline int in_guest_reach(const void *p, size_t n) { return (uintptr_t)p + n <= 0x100000000ull; }
static inline void *g2p(uint32_t a) { return (void *)(uintptr_t)a; }
static inline uint32_t p2g(const void *p) { return (uint32_t)(uintptr_t)p; }

static inline void ret32(a32_cpu *c, uint32_t v) { c->r[0] = v; }
static inline void ret64(a32_cpu *c, uint64_t v)
{
    c->r[0] = (uint32_t)v;
    c->r[1] = (uint32_t)(v >> 32);
}
static inline void retf(a32_cpu *c, float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    c->r[0] = b;
}
static inline void retd(a32_cpu *c, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    ret64(c, b);
}

/* Builds a guest call's argument words (AAPCS32): doublewords at an even word index (registers r0-r3 are words
 * 0-3, the stack follows 8-byte aligned). */
typedef struct {
    uint32_t w[64];
    int n;
} argbuilder;

static inline void ab_32(argbuilder *B, uint32_t v)
{
    if (B->n < 64) B->w[B->n++] = v;
}
static inline void ab_64(argbuilder *B, uint64_t v)
{
    if (B->n & 1) ab_32(B, 0);
    ab_32(B, (uint32_t)v);
    ab_32(B, (uint32_t)(v >> 32));
}

/* ---------------------------------------------------------------- handles (handles.c) */

/* Guest code holds 32-bit values; host references and IDs are 64-bit pointers. Each gets a handle. */
uint32_t o2h_local(jobject o);   /* a local reference: freed with the current native frame */
uint32_t o2h_global(jobject o);
uint32_t o2h_weak(jobject o);
uint32_t o2h_keep(jobject o);    /* a host reference the bridge owns (never freed by the guest) */
jobject h2o(uint32_t h);
void h_free(uint32_t h);         /* forgets the handle (the host reference is the caller's business) */
uint32_t mid2h(jmethodID m);
jmethodID h2mid(uint32_t h);
uint32_t fid2h(jfieldID f);
jfieldID h2fid(uint32_t h);
uint32_t ptr2h(void *p);         /* other host pointers (ANativeWindow *, AAssetManager *, ...) */
void *h2ptr(uint32_t h);
/* the method's argument types: 'Z','B','C','S','I','J','F','D','L' per parameter, then the result after ')' */
void mid_set_shorty(uint32_t h, const char *shorty);
const char *mid_shorty(uint32_t h);
/* native frames: local handles created after frame_mark() are forgotten by frame_pop() */
uint32_t frame_mark(void);
void frame_pop(uint32_t mark);

/* ---------------------------------------------------------------- bridge (bridge.c) */

extern struct grt_info g_info;
extern JavaVM *g_vm;
JNIEnv *host_env(void);          /* NULL if this thread isn't attached to the VM */
void bridge_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* calls a guest function on this thread's guest context (attaching one first if needed) */
uint64_t guest_call(uint32_t fn, const uint32_t *words, int n);
uint32_t guest_malloc(uint32_t size);
void guest_free(uint32_t p);
uint32_t guest_strdup(const char *s);
uint32_t guest_cstr(const char *s); /* a host string the guest can read: itself, or a copy kept for good */

/* jni.c */
int jni_dispatch(a32_cpu *c, int index);
int vm_dispatch(a32_cpu *c, int index);
int jni_dispatch_generated(a32_cpu *c, JNIEnv *env, int index, argreader *R);
extern const char *const jni_names[JNI_TABLE_SIZE];
void make_shorty(const char *sig, char *out, size_t n);

#endif
