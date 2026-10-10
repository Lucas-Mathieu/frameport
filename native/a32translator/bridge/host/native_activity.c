/* NativeActivity for guest libraries. The guest gets its own 32-bit ANativeActivity (and callback table) that
 * mirrors the host's; the host activity's callbacks forward to whatever the guest's ANativeActivity_onCreate put
 * in its table. Windows, input queues and the asset manager are host pointers: the guest sees handles (ptr2h). */
#include <stdlib.h>
#include "imports_internal.h"

/* struct ANativeActivity, 32-bit */
#define GA_CALLBACKS 0
#define GA_VM 4
#define GA_ENV 8
#define GA_CLAZZ 12
#define GA_INTERNAL_PATH 16
#define GA_EXTERNAL_PATH 20
#define GA_SDK 24
#define GA_INSTANCE 28
#define GA_ASSETS 32
#define GA_OBB_PATH 36
#define GA_SIZE 40
#define NCALLBACKS 16 /* ANativeActivityCallbacks: onStart ... onLowMemory */

enum {
    CB_START, CB_RESUME, CB_SAVE_STATE, CB_PAUSE, CB_STOP, CB_DESTROY, CB_FOCUS, CB_WINDOW_CREATED,
    CB_WINDOW_RESIZED, CB_WINDOW_REDRAW, CB_WINDOW_DESTROYED, CB_QUEUE_CREATED, CB_QUEUE_DESTROYED, CB_CONTENT_RECT,
    CB_CONFIG, CB_LOW_MEMORY,
};

typedef struct {
    ANativeActivity *host;
    uint32_t guest;
} activity;

static activity acts[8];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

ANativeActivity *guest_activity(uint32_t g)
{
    ANativeActivity *r = NULL;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < 8; k++)
        if (acts[k].guest == g && g) r = acts[k].host;
    pthread_mutex_unlock(&lock);
    return r;
}

static uint32_t guest_of(ANativeActivity *a)
{
    uint32_t r = 0;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < 8; k++)
        if (acts[k].host == a) r = acts[k].guest;
    pthread_mutex_unlock(&lock);
    return r;
}

/* calls the guest's callback cb (if it set one) with the guest activity and up to two more words */
static uint64_t forward(ANativeActivity *a, int cb, int n, uint32_t w1, uint32_t w2, int *called)
{
    uint32_t g = guest_of(a);
    uint32_t fn = g ? ld32(ld32(g + GA_CALLBACKS) + 4u * (uint32_t)cb) : 0;
    if (called) *called = fn != 0;
    if (!fn) return 0;
    uint32_t w[3] = {g, w1, w2};
    return guest_call(fn, w, n + 1);
}

static void on_start(ANativeActivity *a) { forward(a, CB_START, 0, 0, 0, NULL); }
static void on_resume(ANativeActivity *a) { forward(a, CB_RESUME, 0, 0, 0, NULL); }
static void on_pause(ANativeActivity *a) { forward(a, CB_PAUSE, 0, 0, 0, NULL); }
static void on_stop(ANativeActivity *a) { forward(a, CB_STOP, 0, 0, 0, NULL); }
static void on_low_memory(ANativeActivity *a) { forward(a, CB_LOW_MEMORY, 0, 0, 0, NULL); }
static void on_config(ANativeActivity *a) { forward(a, CB_CONFIG, 0, 0, 0, NULL); }
static void on_focus(ANativeActivity *a, int focused) { forward(a, CB_FOCUS, 1, (uint32_t)focused, 0, NULL); }
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { forward(a, CB_WINDOW_CREATED, 1, ptr2h(w), 0, NULL); }
static void on_window_resized(ANativeActivity *a, ANativeWindow *w) { forward(a, CB_WINDOW_RESIZED, 1, ptr2h(w), 0, NULL); }
static void on_window_redraw(ANativeActivity *a, ANativeWindow *w) { forward(a, CB_WINDOW_REDRAW, 1, ptr2h(w), 0, NULL); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { forward(a, CB_WINDOW_DESTROYED, 1, ptr2h(w), 0, NULL); }
static void on_queue_created(ANativeActivity *a, AInputQueue *q) { forward(a, CB_QUEUE_CREATED, 1, ptr2h(q), 0, NULL); }
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) { forward(a, CB_QUEUE_DESTROYED, 1, ptr2h(q), 0, NULL); }

static void on_content_rect(ANativeActivity *a, const ARect *r)
{
    uint32_t g = guest_malloc(sizeof *r);
    if (!g) return;
    memcpy(g2p(g), r, sizeof *r);
    forward(a, CB_CONTENT_RECT, 1, g, 0, NULL);
    guest_free(g);
}

static void *on_save_state(ANativeActivity *a, size_t *out_len)
{
    *out_len = 0;
    uint32_t len = guest_malloc(4);
    if (!len) return NULL;
    st32(len, 0);
    int called;
    uint32_t p = (uint32_t)forward(a, CB_SAVE_STATE, 1, len, 0, &called);
    void *r = NULL;
    uint32_t n = ld32(len);
    if (p && n) {
        /* the framework frees the result with free() */
        r = malloc(n);
        if (r) {
            memcpy(r, g2p(p), n);
            *out_len = n;
        }
    }
    guest_free(p);
    guest_free(len);
    return r;
}

static void on_destroy(ANativeActivity *a)
{
    forward(a, CB_DESTROY, 0, 0, 0, NULL);
    pthread_mutex_lock(&lock);
    for (int k = 0; k < 8; k++)
        if (acts[k].host == a) acts[k].host = NULL, acts[k].guest = 0;
    pthread_mutex_unlock(&lock);
    /* the guest structures stay: the guest may still look at them while it shuts down */
}

void activity_create(ANativeActivity *a, void *saved, size_t saved_size, uint32_t oncreate)
{
    uint32_t g = guest_malloc(GA_SIZE + 4 * NCALLBACKS);
    memset(g2p(g), 0, GA_SIZE + 4 * NCALLBACKS);
    st32(g + GA_CALLBACKS, g + GA_SIZE);
    st32(g + GA_VM, g_info.java_vm);
    st32(g + GA_ENV, g_info.jni_env);
    st32(g + GA_CLAZZ, o2h_keep(a->clazz));
    st32(g + GA_INTERNAL_PATH, a->internalDataPath ? guest_strdup(a->internalDataPath) : 0);
    st32(g + GA_EXTERNAL_PATH, a->externalDataPath ? guest_strdup(a->externalDataPath) : 0);
    st32(g + GA_SDK, (uint32_t)a->sdkVersion);
    st32(g + GA_ASSETS, ptr2h(a->assetManager));
    st32(g + GA_OBB_PATH, a->obbPath ? guest_strdup(a->obbPath) : 0);
    pthread_mutex_lock(&lock);
    int k = 0;
    while (k < 7 && acts[k].host) k++;
    acts[k].host = a;
    acts[k].guest = g;
    pthread_mutex_unlock(&lock);

    ANativeActivityCallbacks *cb = a->callbacks;
    cb->onStart = on_start;
    cb->onResume = on_resume;
    cb->onSaveInstanceState = on_save_state;
    cb->onPause = on_pause;
    cb->onStop = on_stop;
    cb->onDestroy = on_destroy;
    cb->onWindowFocusChanged = on_focus;
    cb->onNativeWindowCreated = on_window_created;
    cb->onNativeWindowResized = on_window_resized;
    cb->onNativeWindowRedrawNeeded = on_window_redraw;
    cb->onNativeWindowDestroyed = on_window_destroyed;
    cb->onInputQueueCreated = on_queue_created;
    cb->onInputQueueDestroyed = on_queue_destroyed;
    cb->onContentRectChanged = on_content_rect;
    cb->onConfigurationChanged = on_config;
    cb->onLowMemory = on_low_memory;

    uint32_t gs = 0;
    if (saved && saved_size) {
        gs = guest_malloc((uint32_t)saved_size);
        if (gs) memcpy(g2p(gs), saved, saved_size);
    }
    uint32_t w[3] = {g, gs, (uint32_t)saved_size};
    guest_call(oncreate, w, 3);
    bridge_log(ANDROID_LOG_INFO, "guest ANativeActivity_onCreate done");
}
