/* libandroid functions that gen_stubs.py can't generate (imports.txt "!" lines). */
#include <android/sensor.h>
#include <stdlib.h>
#include "imports_internal.h"

extern __typeof__(ASensorManager_getSensorList) ASensorManager_getSensorList __attribute__((weak));
extern __typeof__(ASensorManager_createEventQueue) ASensorManager_createEventQueue __attribute__((weak));

/* the list is an array of host pointers: the guest gets an array of handles (made once per call; tiny) */
void imp_ASensorManager_getSensorList(a32_cpu *c, argreader *R)
{
    ASensorManager *m = h2ptr(rd32(R));
    uint32_t out = rd32(R);
    ASensorList list = NULL;
    int n = ASensorManager_getSensorList ? ASensorManager_getSensorList(m, &list) : 0;
    uint32_t g = guest_malloc(n > 0 ? 4u * (uint32_t)n : 4);
    for (int k = 0; k < n && g; k++) st32(g + 4u * (uint32_t)k, ptr2h((void *)list[k]));
    if (out) st32(out, g);
    ret32(c, (uint32_t)n);
}

void imp_ASensorManager_createEventQueue(a32_cpu *c, argreader *R)
{
    ASensorManager *m = h2ptr(rd32(R));
    ALooper *looper = h2ptr(rd32(R));
    int ident = (int)rd32(R);
    uint32_t cb = rd32(R), data = rd32(R);
    if (cb) bridge_log(ANDROID_LOG_WARN, "ASensorManager_createEventQueue: guest callbacks aren't supported yet");
    ASensorEventQueue *q = ASensorManager_createEventQueue ? ASensorManager_createEventQueue(m, looper, ident, NULL, g2p(data)) : NULL;
    ret32(c, ptr2h(q));
}

/* ---------------------------------------------------------------- audio output (guest/opensles.c) */

#include <aaudio/AAudio.h>

#define W(f) extern __typeof__(f) f __attribute__((weak))
W(AAudio_createStreamBuilder); W(AAudioStreamBuilder_setSampleRate); W(AAudioStreamBuilder_setChannelCount);
W(AAudioStreamBuilder_setFormat); W(AAudioStreamBuilder_setPerformanceMode); W(AAudioStreamBuilder_openStream);
W(AAudioStreamBuilder_delete); W(AAudioStream_requestStart); W(AAudioStream_requestPause);
W(AAudioStream_requestFlush); W(AAudioStream_write); W(AAudioStream_close); W(AAudio_convertResultToText);
W(AAudioStream_setBufferSizeInFrames); W(AAudioStream_getBufferCapacityInFrames); W(AAudioStream_getXRunCount);
#undef W

typedef struct {
    AAudioStream *stream;
    int frame_bytes;
} audio_out;

void imp_a32_audio_open(a32_cpu *c, argreader *R)
{
    int rate = (int)rd32(R), channels = (int)rd32(R), bits = (int)rd32(R);
    AAudioStreamBuilder *b = NULL;
    if (!AAudio_createStreamBuilder || bits != 16 || AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        bridge_log(ANDROID_LOG_ERROR, "audio: can't open a %d Hz, %d channel, %d bit stream", rate, channels, bits);
        ret32(c, 0);
        return;
    }
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    /* a roomy buffer: the guest's mixer is fed back through the translator and would underrun a low-latency one */
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStream *s = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        bridge_log(ANDROID_LOG_ERROR, "audio: AAudio open failed: %s", AAudio_convertResultToText(r));
        ret32(c, 0);
        return;
    }
    /* ~40 ms: writes block at the device's pace. A larger buffer let the player thread drain the guest's queue
     * faster than real time, and the guest's mixer (FMOD) fell behind: crackling */
    if (AAudioStream_setBufferSizeInFrames) AAudioStream_setBufferSizeInFrames(s, rate / 25);
    audio_out *o = calloc(1, sizeof *o);
    o->stream = s;
    o->frame_bytes = channels * 2;
    bridge_log(ANDROID_LOG_INFO, "audio: %d Hz, %d channels through AAudio", rate, channels);
    ret32(c, ptr2h(o));
}

/* blocking, like a full OpenSL buffer queue: this paces the guest's audio thread */
void imp_a32_audio_write(a32_cpu *c, argreader *R)
{
    audio_out *o = h2ptr(rd32(R));
    const void *data = g2p(rd32(R));
    int frames = (int)rd32(R);
    aaudio_result_t r = o ? AAudioStream_write(o->stream, data, frames, 1000000000LL) : 0;
    static int xruns;
    if (o && AAudioStream_getXRunCount) {
        int x = AAudioStream_getXRunCount(o->stream);
        if (x > xruns && (xruns < 20 || x % 100 == 0)) bridge_log(ANDROID_LOG_WARN, "audio: %d underruns", x);
        xruns = x;
    }
    ret32(c, (uint32_t)r);
}

void imp_a32_audio_start(a32_cpu *c, argreader *R)
{
    audio_out *o = h2ptr(rd32(R));
    if (o) AAudioStream_requestStart(o->stream);
}

void imp_a32_audio_pause(a32_cpu *c, argreader *R)
{
    audio_out *o = h2ptr(rd32(R));
    if (o) AAudioStream_requestPause(o->stream);
}

void imp_a32_audio_close(a32_cpu *c, argreader *R)
{
    audio_out *o = h2ptr(rd32(R));
    if (!o) return;
    AAudioStream_close(o->stream);
    free(o);
}

/* ---------------------------------------------------------------- looper, input queue, assets (native_app_glue) */

#include <android/asset_manager.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <android/input.h>
#include <android/looper.h>

#define W(f) extern __typeof__(f) f __attribute__((weak))
W(ALooper_addFd); W(ALooper_pollOnce); W(AInputQueue_attachLooper); W(AInputQueue_getEvent); W(ALooper_forThread);
W(AAssetDir_getNextFileName); W(AAsset_openFileDescriptor); W(AAsset_openFileDescriptor64);
W(AAsset_getBuffer); W(AAsset_getLength64); W(AAsset_close);
#undef W

/* a guest ALooper callback int (*)(int fd, int events, void *data), called on the polling (guest) thread */
typedef struct {
    uint32_t fn, data;
} looper_cb;

static int looper_call(int fd, int events, void *p)
{
    looper_cb *cb = p;
    uint32_t w[3] = {(uint32_t)fd, (uint32_t)events, cb->data};
    return (int)guest_call(cb->fn, w, 3);
}

/* host data for a guest (callback, data) pair: the callback's own record (kept: the looper may call it until the
 * fd is removed), or the guest's data pointer itself when there is no callback */
static void *looper_data(uint32_t fn, uint32_t data, ALooper_callbackFunc *host_fn)
{
    *host_fn = NULL;
    if (!fn) return g2p(data);
    looper_cb *cb = malloc(sizeof *cb);
    if (!cb) return NULL;
    cb->fn = fn, cb->data = data;
    *host_fn = looper_call;
    return cb;
}

void imp_ALooper_addFd(a32_cpu *c, argreader *R)
{
    ALooper *l = h2ptr(rd32(R));
    int fd = (int)rd32(R), ident = (int)rd32(R), events = (int)rd32(R);
    uint32_t fn = rd32(R), data = rd32(R);
    ALooper_callbackFunc host_fn;
    void *d = looper_data(fn, data, &host_fn);
    ret32(c, ALooper_addFd ? (uint32_t)ALooper_addFd(l, fd, ident, events, host_fn, d) : (uint32_t)-1);
}

void imp_AInputQueue_attachLooper(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    ALooper *l = h2ptr(rd32(R));
    int ident = (int)rd32(R);
    uint32_t fn = rd32(R), data = rd32(R);
    ALooper_callbackFunc host_fn;
    void *d = looper_data(fn, data, &host_fn);
    if (AInputQueue_attachLooper) AInputQueue_attachLooper(q, l, ident, host_fn, d);
    input_queue_attached(q, l, ident, data, fn != 0); /* the controllers' synthetic events go here (input.c) */
}

/* outData is a void ** (4 bytes on the guest side); data without a callback is the guest's own pointer */
static void poll(a32_cpu *c, argreader *R, int all)
{
    int timeout = (int)rd32(R);
    uint32_t out_fd = rd32(R), out_events = rd32(R), out_data = rd32(R);
    int fd = 0, events = 0;
    void *data = NULL;
    /* a synthetic input event waits (input.c): the input queue's ident, like a readable queue fd */
    int ident;
    uint32_t gdata;
    if (input_queue_pending(ALooper_forThread(), &ident, &gdata)) {
        if (out_fd) st32(out_fd, (uint32_t)-1);
        if (out_events) st32(out_events, ALOOPER_EVENT_INPUT);
        if (out_data) st32(out_data, gdata);
        ret32(c, (uint32_t)ident);
        return;
    }
    /* ALooper_pollAll: removed from the NDK headers (it may miss wakes), still in libandroid for old apps */
    static int (*poll_all)(int, int *, int *, void **);
    if (all && !poll_all) poll_all = (int (*)(int, int *, int *, void **))dlsym(RTLD_DEFAULT, "ALooper_pollAll");
    int (*f)(int, int *, int *, void **) = all ? poll_all : ALooper_pollOnce;
    int r = f ? f(timeout, &fd, &events, &data) : ALOOPER_POLL_ERROR;
    if (out_fd) st32(out_fd, (uint32_t)fd);
    if (out_events) st32(out_events, (uint32_t)events);
    if (out_data) st32(out_data, (uint32_t)(uintptr_t)data);
    ret32(c, (uint32_t)r);
}

void imp_ALooper_pollAll(a32_cpu *c, argreader *R) { poll(c, R, 1); }
void imp_ALooper_pollOnce(a32_cpu *c, argreader *R) { poll(c, R, 0); }

void imp_AInputQueue_getEvent(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    uint32_t out = rd32(R);
    AInputEvent *e = input_queue_pop(q); /* synthetic controller events first */
    int r = e ? 0 : AInputQueue_getEvent ? AInputQueue_getEvent(q, &e) : -1;
    if (out) st32(out, r >= 0 ? ptr2h(e) : 0);
    ret32(c, (uint32_t)r);
}

/* the name is valid until the next call: one guest buffer, refilled each time */
void imp_AAssetDir_getNextFileName(a32_cpu *c, argreader *R)
{
    AAssetDir *d = h2ptr(rd32(R));
    const char *name = AAssetDir_getNextFileName ? AAssetDir_getNextFileName(d) : NULL;
    static uint32_t buf;
    static size_t size;
    size_t n = name ? strlen(name) + 1 : 0;
    if (n > size) {
        guest_free(buf);
        size = n < 256 ? 256 : n;
        buf = guest_malloc((uint32_t)size);
        if (!buf) size = 0;
    }
    if (n && buf) memcpy(g2p(buf), name, n);
    ret32(c, n && buf ? buf : 0);
}

/* off_t is 32-bit for the guest (AAsset_openFileDescriptor) and 64-bit in the 64 variant */
static void open_fd(a32_cpu *c, argreader *R, int wide)
{
    AAsset *a = h2ptr(rd32(R));
    uint32_t out_start = rd32(R), out_len = rd32(R);
    off64_t start = 0, len = 0;
    int fd = AAsset_openFileDescriptor64 ? AAsset_openFileDescriptor64(a, &start, &len) : -1;
    if (wide) {
        if (out_start) memcpy(g2p(out_start), &start, 8);
        if (out_len) memcpy(g2p(out_len), &len, 8);
    } else {
        if (start > 0x7fffffff || len > 0x7fffffff)
            bridge_log(ANDROID_LOG_WARN, "AAsset_openFileDescriptor: offset/length past 2 GiB for a 32-bit app");
        if (out_start) st32(out_start, (uint32_t)start);
        if (out_len) st32(out_len, (uint32_t)len);
    }
    ret32(c, (uint32_t)fd);
}

void imp_AAsset_openFileDescriptor(a32_cpu *c, argreader *R) { open_fd(c, R, 0); }
void imp_AAsset_openFileDescriptor64(a32_cpu *c, argreader *R) { open_fd(c, R, 1); }

/* AAsset_getBuffer's memory is the host's (above 4 GiB): the guest gets a copy in its own memory, kept until
 * AAsset_close like the original (Unreal reads small config/text assets this way) */
typedef struct asset_buf {
    struct asset_buf *next;
    AAsset *asset;
    uint32_t buf;
} asset_buf;

static pthread_mutex_t asset_lock = PTHREAD_MUTEX_INITIALIZER;
static asset_buf *asset_bufs;

void imp_AAsset_getBuffer(a32_cpu *c, argreader *R)
{
    AAsset *a = h2ptr(rd32(R));
    uint32_t g = 0;
    pthread_mutex_lock(&asset_lock);
    for (asset_buf *b = asset_bufs; b; b = b->next)
        if (b->asset == a) g = b->buf;
    pthread_mutex_unlock(&asset_lock);
    if (g || !a || !AAsset_getBuffer) {
        ret32(c, g);
        return;
    }
    const void *p = AAsset_getBuffer(a);
    off64_t len = p ? AAsset_getLength64(a) : 0;
    if (!p || len < 0 || len > 0x40000000) {
        if (p) bridge_log(ANDROID_LOG_WARN, "AAsset_getBuffer: %lld bytes is too big for a 32-bit app", (long long)len);
        ret32(c, 0);
        return;
    }
    asset_buf *b = malloc(sizeof *b);
    g = b ? guest_malloc(len ? (uint32_t)len : 1) : 0;
    if (!g) {
        free(b);
        bridge_log(ANDROID_LOG_WARN, "AAsset_getBuffer: no guest memory for %lld bytes", (long long)len);
        ret32(c, 0);
        return;
    }
    memcpy(g2p(g), p, (size_t)len);
    b->asset = a, b->buf = g;
    pthread_mutex_lock(&asset_lock);
    b->next = asset_bufs, asset_bufs = b;
    pthread_mutex_unlock(&asset_lock);
    ret32(c, g);
}

void imp_AAsset_close(a32_cpu *c, argreader *R)
{
    AAsset *a = h2ptr(rd32(R));
    asset_buf *found = NULL;
    pthread_mutex_lock(&asset_lock);
    for (asset_buf **pp = &asset_bufs; *pp; pp = &(*pp)->next)
        if ((*pp)->asset == a) {
            found = *pp;
            *pp = found->next;
            break;
        }
    pthread_mutex_unlock(&asset_lock);
    if (found) {
        guest_free(found->buf);
        free(found);
    }
    if (a && AAsset_close) AAsset_close(a);
}

/* logged: a game that quits by itself shows it here */
void imp_ANativeActivity_finish(a32_cpu *c, argreader *R)
{
    ANativeActivity *a = guest_activity(rd32(R));
    bridge_log(ANDROID_LOG_INFO, "guest called ANativeActivity_finish");
    if (a) ANativeActivity_finish(a);
}
