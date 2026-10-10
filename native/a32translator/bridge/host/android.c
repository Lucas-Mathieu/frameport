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
