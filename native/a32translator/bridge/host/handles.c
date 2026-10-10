/* Handle tables: 32-bit guest values for 64-bit host references, method/field IDs and other pointers. */
#include <pthread.h>
#include <stdlib.h>
#include "jni_internal.h"

enum { K_FREE, K_LOCAL, K_GLOBAL, K_WEAK, K_KEEP };

typedef struct {
    void *p;
    uint32_t serial;
    uint8_t kind;
} slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static slot *objs;
static uint32_t nobjs, capobjs, freelist = 0; /* freelist: index + 1 of a free slot (p holds the next) */
static uint32_t serial_ctr;

/* per thread: the local handles created on it (index, serial), popped with the native frame */
static __thread struct { uint32_t *v; uint32_t n, cap; } locals;

static uint32_t new_obj(void *p, uint8_t kind)
{
    if (!p) return 0;
    pthread_mutex_lock(&lock);
    uint32_t i;
    if (freelist) {
        i = freelist - 1;
        freelist = (uint32_t)(uintptr_t)objs[i].p;
    } else {
        if (nobjs == capobjs) {
            capobjs = capobjs ? capobjs * 2 : 1024;
            objs = realloc(objs, capobjs * sizeof *objs);
        }
        i = nobjs++;
    }
    objs[i].p = p;
    objs[i].kind = kind;
    objs[i].serial = ++serial_ctr;
    uint32_t serial = objs[i].serial;
    pthread_mutex_unlock(&lock);
    if (kind == K_LOCAL) {
        if (locals.n + 2 > locals.cap) {
            locals.cap = locals.cap ? locals.cap * 2 : 256;
            locals.v = realloc(locals.v, locals.cap * sizeof *locals.v);
        }
        locals.v[locals.n++] = i;
        locals.v[locals.n++] = serial;
    }
    return (i + 1) << 2;
}

uint32_t o2h_local(jobject o) { return new_obj(o, K_LOCAL); }
uint32_t o2h_global(jobject o) { return new_obj(o, K_GLOBAL); }
uint32_t o2h_weak(jobject o) { return new_obj(o, K_WEAK); }
uint32_t o2h_keep(jobject o) { return new_obj(o, K_KEEP); }

jobject h2o(uint32_t h)
{
    if (!h) return NULL;
    uint32_t i = (h >> 2) - 1;
    pthread_mutex_lock(&lock);
    void *p = i < nobjs && objs[i].kind != K_FREE ? objs[i].p : NULL;
    pthread_mutex_unlock(&lock);
    if (!p) bridge_log(6, "invalid object handle 0x%x", h);
    return p;
}

static void free_slot(uint32_t i)
{
    objs[i].kind = K_FREE;
    objs[i].p = (void *)(uintptr_t)freelist;
    freelist = i + 1;
}

void h_free(uint32_t h)
{
    if (!h) return;
    uint32_t i = (h >> 2) - 1;
    pthread_mutex_lock(&lock);
    if (i < nobjs && objs[i].kind != K_FREE) free_slot(i);
    pthread_mutex_unlock(&lock);
}

uint32_t frame_mark(void) { return locals.n; }

void frame_pop(uint32_t mark)
{
    if (locals.n <= mark) return;
    pthread_mutex_lock(&lock);
    for (uint32_t k = mark; k + 1 < locals.n; k += 2) {
        uint32_t i = locals.v[k], serial = locals.v[k + 1];
        if (i < nobjs && objs[i].kind == K_LOCAL && objs[i].serial == serial) free_slot(i);
    }
    pthread_mutex_unlock(&lock);
    locals.n = mark;
}

/* ---------------------------------------------------------------- IDs and pointers: stable, never freed */

typedef struct {
    void *p;
    char *shorty;
} idslot;

typedef struct {
    idslot *v;
    uint32_t n, cap;
    uint32_t *hash; /* open addressing: index + 1 */
    uint32_t hcap;
} idtab;

static idtab mids, fids, ptrs;

static uint32_t hashp(void *p) { return (uint32_t)(((uintptr_t)p * 0x9e3779b97f4a7c15ull) >> 32); }

static uint32_t id_get(idtab *t, void *p)
{
    if (!p) return 0;
    pthread_mutex_lock(&lock);
    if (t->n * 2 >= t->hcap) {
        uint32_t nc = t->hcap ? t->hcap * 2 : 1024;
        uint32_t *nh = calloc(nc, 4);
        for (uint32_t k = 0; k < t->n; k++) {
            uint32_t j = hashp(t->v[k].p) & (nc - 1);
            while (nh[j]) j = (j + 1) & (nc - 1);
            nh[j] = k + 1;
        }
        free(t->hash);
        t->hash = nh;
        t->hcap = nc;
    }
    uint32_t j = hashp(p) & (t->hcap - 1);
    while (t->hash[j]) {
        if (t->v[t->hash[j] - 1].p == p) {
            uint32_t h = t->hash[j] << 2;
            pthread_mutex_unlock(&lock);
            return h;
        }
        j = (j + 1) & (t->hcap - 1);
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 512;
        t->v = realloc(t->v, t->cap * sizeof *t->v);
    }
    t->v[t->n].p = p;
    t->v[t->n].shorty = NULL;
    t->hash[j] = ++t->n;
    uint32_t h = t->n << 2;
    pthread_mutex_unlock(&lock);
    return h;
}

static idslot *id_slot(idtab *t, uint32_t h)
{
    uint32_t i = (h >> 2) - 1;
    return h && i < t->n ? &t->v[i] : NULL;
}

static void *id_ptr(idtab *t, uint32_t h)
{
    pthread_mutex_lock(&lock);
    idslot *s = id_slot(t, h);
    void *p = s ? s->p : NULL;
    pthread_mutex_unlock(&lock);
    return p;
}

uint32_t mid2h(jmethodID m) { return id_get(&mids, m); }
jmethodID h2mid(uint32_t h) { return id_ptr(&mids, h); }
uint32_t fid2h(jfieldID f) { return id_get(&fids, f); }
jfieldID h2fid(uint32_t h) { return id_ptr(&fids, h); }
uint32_t ptr2h(void *p) { return id_get(&ptrs, p); }
void *h2ptr(uint32_t h) { return id_ptr(&ptrs, h); }

void mid_set_shorty(uint32_t h, const char *shorty)
{
    pthread_mutex_lock(&lock);
    idslot *s = id_slot(&mids, h);
    if (s && !s->shorty) s->shorty = strdup(shorty);
    pthread_mutex_unlock(&lock);
}

const char *mid_shorty(uint32_t h)
{
    pthread_mutex_lock(&lock);
    idslot *s = id_slot(&mids, h);
    const char *r = s ? s->shorty : NULL;
    pthread_mutex_unlock(&lock);
    return r;
}
