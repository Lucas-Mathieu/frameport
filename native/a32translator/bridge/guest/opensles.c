/* OpenSL ES for guest code: the part audio engines use for output (an engine, an output mix and PCM audio players
 * fed by an Android simple buffer queue), implemented here in the guest and played through the host's AAudio
 * (a32_audio_*, host/android.c). OpenSL's objects are tables of function pointers, so keeping them on the guest side
 * needs no thunks; each playing player has a guest thread that writes its queued buffers (blocking, which paces the
 * queue like a real device) and calls the buffer-queue callback. Recording and everything else report "not
 * supported". */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "grt.h"

/* host/android.c (imports.txt) */
uint32_t a32_audio_open(int rate, int channels, int bits);
int a32_audio_write(uint32_t stream, const void *data, int frames);
void a32_audio_start(uint32_t stream);
void a32_audio_pause(uint32_t stream);
void a32_audio_close(uint32_t stream);

/* interface IDs: only their addresses matter (callers pass ours back) */
static const struct SLInterfaceID_ ids[] = {
    {0xc7b1b6a0, 0, 0, 0, {0}}, {1, 0, 0, 0, {0}}, {2, 0, 0, 0, {0}}, {3, 0, 0, 0, {0}}, {4, 0, 0, 0, {0}},
    {5, 0, 0, 0, {0}},          {6, 0, 0, 0, {0}}, {7, 0, 0, 0, {0}}, {8, 0, 0, 0, {0}}, {9, 0, 0, 0, {0}},
};
const SLInterfaceID SL_IID_NULL = &ids[0];
const SLInterfaceID SL_IID_OBJECT = &ids[1];
const SLInterfaceID SL_IID_ENGINE = &ids[2];
const SLInterfaceID SL_IID_PLAY = &ids[3];
const SLInterfaceID SL_IID_BUFFERQUEUE = &ids[4];
const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &ids[5];
const SLInterfaceID SL_IID_VOLUME = &ids[6];
const SLInterfaceID SL_IID_ANDROIDCONFIGURATION = &ids[7];
const SLInterfaceID SL_IID_RECORD = &ids[8];
const SLInterfaceID SL_IID_OUTPUTMIX = &ids[9];

enum { K_ENGINE, K_MIX, K_PLAYER };
#define QMAX 64

typedef struct obj {
    const struct SLObjectItf_ *object;
    const struct SLEngineItf_ *engine;
    const struct SLPlayItf_ *play;
    const struct SLAndroidSimpleBufferQueueItf_ *queue;
    const struct SLVolumeItf_ *volume;
    const struct SLAndroidConfigurationItf_ *config;
    int kind;
    SLuint32 state;
    /* player */
    int rate, channels, bits;
    SLuint32 play_state;
    slAndroidSimpleBufferQueueCallback qcb;
    void *qctx;
    slPlayCallback pcb;
    void *pctx;
    SLuint32 pmask;
    struct { const void *data; SLuint32 size; } q[QMAX];
    SLuint32 qhead, qcount, qindex;
    SLmillibel level;
    SLboolean mute;
    uint32_t stream;
    uint64_t frames_played;
    pthread_t thread;
    int quit, has_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} obj;

#define OBJ(self, field) ((obj *)((char *)(self) - offsetof(obj, field)))

/* ---------------------------------------------------------------- the player thread */

static void *player_thread(void *arg)
{
    obj *o = arg;
    pthread_mutex_lock(&o->lock);
    for (;;) {
        while (!o->quit && (o->play_state != SL_PLAYSTATE_PLAYING || !o->qcount)) pthread_cond_wait(&o->cond, &o->lock);
        if (o->quit) break;
        const void *data = o->q[o->qhead].data;
        SLuint32 size = o->q[o->qhead].size;
        pthread_mutex_unlock(&o->lock);
        int frame = o->channels * o->bits / 8;
        if (o->stream && frame) a32_audio_write(o->stream, data, (int)(size / (SLuint32)frame));
        pthread_mutex_lock(&o->lock);
        if (o->qcount && o->q[o->qhead].data == data) { /* not cleared meanwhile */
            o->qhead = (o->qhead + 1) % QMAX;
            o->qcount--;
            o->qindex++;
            o->frames_played += frame ? size / (SLuint32)frame : 0;
            slAndroidSimpleBufferQueueCallback cb = o->qcb;
            void *ctx = o->qctx;
            pthread_mutex_unlock(&o->lock);
            if (cb) cb((SLAndroidSimpleBufferQueueItf)&o->queue, ctx);
            pthread_mutex_lock(&o->lock);
        }
    }
    pthread_mutex_unlock(&o->lock);
    return NULL;
}

/* ---------------------------------------------------------------- SLObjectItf */

static SLresult obj_realize(SLObjectItf self, SLboolean async)
{
    obj *o = OBJ(self, object);
    if (o->kind == K_PLAYER && !o->stream) {
        o->stream = a32_audio_open(o->rate, o->channels, o->bits);
        if (!o->stream) return SL_RESULT_RESOURCE_ERROR;
        o->has_thread = !pthread_create(&o->thread, NULL, player_thread, o);
    }
    o->state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_resume(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }

static SLresult obj_get_state(SLObjectItf self, SLuint32 *state)
{
    *state = OBJ(self, object)->state;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_get_interface(SLObjectItf self, const SLInterfaceID iid, void *out)
{
    obj *o = OBJ(self, object);
    void **p = out;
    if (iid == SL_IID_OBJECT) *p = &o->object;
    else if (iid == SL_IID_ENGINE && o->kind == K_ENGINE) *p = &o->engine;
    else if (iid == SL_IID_PLAY && o->kind == K_PLAYER) *p = &o->play;
    else if ((iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE) && o->kind == K_PLAYER) *p = &o->queue;
    else if (iid == SL_IID_VOLUME && o->kind == K_PLAYER) *p = &o->volume;
    else if (iid == SL_IID_ANDROIDCONFIGURATION && o->kind == K_PLAYER) *p = &o->config;
    else return SL_RESULT_FEATURE_UNSUPPORTED;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_register_callback(SLObjectItf self, slObjectCallback cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void obj_abort(SLObjectItf self) {}

static void obj_destroy(SLObjectItf self)
{
    obj *o = OBJ(self, object);
    if (o->kind == K_PLAYER) {
        pthread_mutex_lock(&o->lock);
        o->quit = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        if (o->has_thread) pthread_join(o->thread, NULL);
        if (o->stream) a32_audio_close(o->stream);
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    free(o);
}

static SLresult obj_set_priority(SLObjectItf self, SLint32 p, SLboolean pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_get_priority(SLObjectItf self, SLint32 *p, SLboolean *pre)
{
    *p = 0, *pre = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_set_loss(SLObjectItf self, SLint16 n, SLInterfaceID *ids_, SLboolean enabled)
{
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ object_itf = {obj_realize, obj_resume, obj_get_state, obj_get_interface,
                                               obj_register_callback, obj_abort, obj_destroy, obj_set_priority,
                                               obj_get_priority, obj_set_loss};

/* ---------------------------------------------------------------- SLPlayItf */

static SLresult play_set_state(SLPlayItf self, SLuint32 state)
{
    obj *o = OBJ(self, play);
    pthread_mutex_lock(&o->lock);
    SLuint32 old = o->play_state;
    o->play_state = state;
    if (state == SL_PLAYSTATE_STOPPED) { /* stopping drops the queue */
        o->qcount = 0;
        o->frames_played = 0;
    }
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    if (o->stream && old != state) {
        if (state == SL_PLAYSTATE_PLAYING) a32_audio_start(o->stream);
        else a32_audio_pause(o->stream);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(SLPlayItf self, SLuint32 *state)
{
    *state = OBJ(self, play)->play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(SLPlayItf self, SLmillisecond *ms)
{
    *ms = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_position(SLPlayItf self, SLmillisecond *ms)
{
    obj *o = OBJ(self, play);
    *ms = o->rate ? (SLmillisecond)(o->frames_played * 1000 / (uint64_t)o->rate) : 0;
    return SL_RESULT_SUCCESS;
}

static SLresult play_register_callback(SLPlayItf self, slPlayCallback cb, void *ctx)
{
    obj *o = OBJ(self, play);
    o->pcb = cb, o->pctx = ctx;
    return SL_RESULT_SUCCESS;
}

static SLresult play_set_mask(SLPlayItf self, SLuint32 mask)
{
    OBJ(self, play)->pmask = mask;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_mask(SLPlayItf self, SLuint32 *mask)
{
    *mask = OBJ(self, play)->pmask;
    return SL_RESULT_SUCCESS;
}

static SLresult play_set_ms(SLPlayItf self, SLmillisecond ms) { return SL_RESULT_SUCCESS; }
static SLresult play_clear(SLPlayItf self) { return SL_RESULT_SUCCESS; }
static SLresult play_get_ms(SLPlayItf self, SLmillisecond *ms)
{
    *ms = 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_itf = {play_set_state, play_get_state, play_get_duration, play_get_position,
                                           play_register_callback, play_set_mask, play_get_mask, play_set_ms,
                                           play_clear, play_get_ms, play_set_ms, play_get_ms};

/* ---------------------------------------------------------------- SLAndroidSimpleBufferQueueItf */

static SLresult queue_enqueue(SLAndroidSimpleBufferQueueItf self, const void *buf, SLuint32 size)
{
    obj *o = OBJ(self, queue);
    pthread_mutex_lock(&o->lock);
    if (o->qcount == QMAX) {
        pthread_mutex_unlock(&o->lock);
        return SL_RESULT_BUFFER_INSUFFICIENT;
    }
    static int logged;
    if (logged < 8 || logged % 2000 == 0)
        grt_log(4, "a32sles", "enqueue #%d: %u bytes (%u ms), %u queued", logged, size,
                o->rate && o->channels ? size * 1000u / (SLuint32)(o->rate * o->channels * 2) : 0, o->qcount);
    logged++;
    SLuint32 at = (o->qhead + o->qcount) % QMAX;
    o->q[at].data = buf, o->q[at].size = size;
    o->qcount++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_clear(SLAndroidSimpleBufferQueueItf self)
{
    obj *o = OBJ(self, queue);
    pthread_mutex_lock(&o->lock);
    o->qcount = 0;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState *st)
{
    obj *o = OBJ(self, queue);
    pthread_mutex_lock(&o->lock);
    st->count = o->qcount, st->index = o->qindex;
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult queue_register_callback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback cb,
                                        void *ctx)
{
    obj *o = OBJ(self, queue);
    o->qcb = cb, o->qctx = ctx;
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidSimpleBufferQueueItf_ queue_itf = {queue_enqueue, queue_clear, queue_get_state,
                                                                 queue_register_callback};

/* ---------------------------------------------------------------- SLVolumeItf (kept, not applied) */

static SLresult vol_set_level(SLVolumeItf self, SLmillibel l)
{
    OBJ(self, volume)->level = l;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_level(SLVolumeItf self, SLmillibel *l)
{
    *l = OBJ(self, volume)->level;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_max(SLVolumeItf self, SLmillibel *l)
{
    *l = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_set_mute(SLVolumeItf self, SLboolean m)
{
    OBJ(self, volume)->mute = m;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_mute(SLVolumeItf self, SLboolean *m)
{
    *m = OBJ(self, volume)->mute;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_enable_pos(SLVolumeItf self, SLboolean e) { return SL_RESULT_SUCCESS; }
static SLresult vol_is_pos(SLVolumeItf self, SLboolean *e)
{
    *e = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_set_pos(SLVolumeItf self, SLpermille p) { return SL_RESULT_SUCCESS; }
static SLresult vol_get_pos(SLVolumeItf self, SLpermille *p)
{
    *p = 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_itf = {vol_set_level, vol_get_level, vol_get_max, vol_set_mute, vol_get_mute,
                                               vol_enable_pos, vol_is_pos, vol_set_pos, vol_get_pos};

/* ---------------------------------------------------------------- SLAndroidConfigurationItf */

static SLresult cfg_set(SLAndroidConfigurationItf self, const SLchar *key, const void *value, SLuint32 size)
{
    return SL_RESULT_SUCCESS;
}
static SLresult cfg_get(SLAndroidConfigurationItf self, const SLchar *key, SLuint32 *size, void *value)
{
    return SL_RESULT_PARAMETER_INVALID;
}

static const struct SLAndroidConfigurationItf_ config_itf = {cfg_set, cfg_get};

/* ---------------------------------------------------------------- SLEngineItf */

static obj *new_obj(int kind)
{
    obj *o = calloc(1, sizeof *o);
    o->object = &object_itf;
    o->kind = kind;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    return o;
}

static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult engine_create_player(SLEngineItf self, SLObjectItf *player, SLDataSource *src, SLDataSink *sink,
                                     SLuint32 n, const SLInterfaceID *iids, const SLboolean *required)
{
    SLuint32 loc = src && src->pLocator ? *(SLuint32 *)src->pLocator : 0;
    SLuint32 fmt = src && src->pFormat ? *(SLuint32 *)src->pFormat : 0;
    if ((loc != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && loc != SL_DATALOCATOR_BUFFERQUEUE) ||
        (fmt != SL_DATAFORMAT_PCM && fmt != SL_ANDROID_DATAFORMAT_PCM_EX)) {
        grt_log(5, "a32sles", "audio player source (locator %u, format %u) isn't supported", loc, fmt);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    const SLDataFormat_PCM *pcm = src->pFormat;
    if (fmt == SL_ANDROID_DATAFORMAT_PCM_EX && ((const SLAndroidDataFormat_PCM_EX *)pcm)->representation ==
                                                   SL_ANDROID_PCM_REPRESENTATION_FLOAT) {
        grt_log(5, "a32sles", "float PCM isn't supported");
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    obj *o = new_obj(K_PLAYER);
    o->play = &play_itf;
    o->queue = &queue_itf;
    o->volume = &volume_itf;
    o->config = &config_itf;
    o->rate = (int)(pcm->samplesPerSec / 1000); /* milliHz */
    o->channels = (int)pcm->numChannels;
    o->bits = (int)pcm->bitsPerSample;
    o->play_state = SL_PLAYSTATE_STOPPED;
    grt_log(4, "a32sles", "audio player: %d Hz, %d channels, %d bits", o->rate, o->channels, o->bits);
    *player = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_create_mix(SLEngineItf self, SLObjectItf *mix, SLuint32 n, const SLInterfaceID *iids,
                                  const SLboolean *required)
{
    obj *o = new_obj(K_MIX);
    *mix = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_query_num_ext(SLEngineItf self, SLuint32 *n)
{
    *n = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_is_ext(SLEngineItf self, const SLchar *name, SLboolean *supported)
{
    *supported = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_itf = {
    .CreateLEDDevice = (void *)unsupported,
    .CreateVibraDevice = (void *)unsupported,
    .CreateAudioPlayer = engine_create_player,
    .CreateAudioRecorder = (void *)unsupported,
    .CreateMidiPlayer = (void *)unsupported,
    .CreateListener = (void *)unsupported,
    .Create3DGroup = (void *)unsupported,
    .CreateOutputMix = engine_create_mix,
    .CreateMetadataExtractor = (void *)unsupported,
    .CreateExtensionObject = (void *)unsupported,
    .QueryNumSupportedInterfaces = (void *)unsupported,
    .QuerySupportedInterfaces = (void *)unsupported,
    .QueryNumSupportedExtensions = engine_query_num_ext,
    .QuerySupportedExtension = (void *)unsupported,
    .IsExtensionSupported = engine_is_ext,
};

SLresult slCreateEngine(SLObjectItf *engine, SLuint32 n_opts, const SLEngineOption *opts, SLuint32 n,
                        const SLInterfaceID *iids, const SLboolean *required)
{
    obj *o = new_obj(K_ENGINE);
    o->engine = &engine_itf;
    *engine = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *n)
{
    *n = 1;
    return SL_RESULT_SUCCESS;
}

SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *id)
{
    if (index) return SL_RESULT_PARAMETER_INVALID;
    *id = SL_IID_ENGINE;
    return SL_RESULT_SUCCESS;
}
