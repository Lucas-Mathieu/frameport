/* Controller input for Gear VR-era games. They read the headset's touchpad and back button and Bluetooth gamepads
 * as Android input events (Unity: mouse/touch, Escape, joystick buttons and axes). A thread polls the Frame's
 * controllers through the 64-bit VrApi bridge and turns them into such events, handed to the game the way Android
 * would: NativeActivity games (Unreal, native_app_glue) get them in their AInputQueue (synthetic AInputEvents the
 * AInputQueue, AInputEvent, AKeyEvent and AMotionEvent imports know), Unity games through
 * UnityPlayer.injectEvent, others through the activity's dispatchKeyEvent. Off with A32_NO_INPUT=1.
 *
 * Gear VR had a touchpad on the headset (tap, hold, swipe) and a back button; games that took a gamepad read it too:
 *   right trigger -> touchpad held (Gear VR games: walk), A -> a short tap
 *   right stick   -> a swipe in that direction (flick it); NativeActivity games: up/down = swipe forward/back
 *   B, left menu  -> back (Unity: Escape)
 *   X, Y, sticks  -> gamepad X/Y, joystick axes X/Y (left) and Z/RZ (right), as a real gamepad if there is one */
#include <android/input.h>
#include <android/looper.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "imports_internal.h"

/* weak like the generated imports (the PC test harness has no libandroid) */
#define W(f) extern __typeof__(f) f __attribute__((weak))
W(ALooper_wake); W(AInputQueue_detachLooper); W(AInputQueue_hasEvents); W(AInputQueue_preDispatchEvent);
W(AInputQueue_finishEvent); W(AInputEvent_getType); W(AInputEvent_getDeviceId); W(AInputEvent_getSource);
W(AKeyEvent_getAction); W(AKeyEvent_getKeyCode); W(AKeyEvent_getMetaState); W(AKeyEvent_getRepeatCount);
W(AMotionEvent_getAction); W(AMotionEvent_getPointerCount); W(AMotionEvent_getPointerId); W(AMotionEvent_getX);
W(AMotionEvent_getY); W(AMotionEvent_getRawX); W(AMotionEvent_getRawY); W(AMotionEvent_getAxisValue);
W(AMotionEvent_getButtonState);
#undef W

typedef struct { int32_t Type; uint32_t DeviceID; } caps_header;
typedef struct {
    caps_header Header;
    uint32_t ControllerCapabilities, ButtonCapabilities;
    uint16_t TrackpadMaxX, TrackpadMaxY;
    float TrackpadSizeX, TrackpadSizeY;
    uint32_t HapticSamplesMax, HapticSampleDurationMS, TouchCapabilities, Reserved4, Reserved5;
} remote_caps;
typedef struct {
    int32_t ControllerType;
    double TimeInSeconds;
    uint32_t Buttons, TrackpadStatus;
    float TrackpadPosition[2];
    uint8_t Battery, RecenterCount;
    uint16_t Reserved;
    float IndexTrigger, GripTrigger;
    uint32_t Touches, Reserved5a;
    float Joystick[2], JoystickNoDeadZone[2];
} remote_state;
_Static_assert(sizeof(remote_caps) == 48 && sizeof(remote_state) == 72, "VrApi input ABI");

#define TRACKED_REMOTE 4
#define LEFT_HAND 4u
#define B_A 0x1u
#define B_B 0x2u
#define B_RTHUMB 0x4u
#define B_X 0x100u
#define B_Y 0x200u
#define B_LTHUMB 0x400u
#define B_ENTER 0x100000u

/* android.view.KeyEvent / MotionEvent / InputDevice constants */
#define KEY_DOWN 0
#define KEY_UP 1
#define KEYCODE_BACK 4
#define KEYCODE_BUTTON_A 96
#define KEYCODE_BUTTON_B 97
#define KEYCODE_BUTTON_X 99
#define KEYCODE_BUTTON_Y 100
#define KEYCODE_BUTTON_THUMBL 106
#define KEYCODE_BUTTON_THUMBR 107
#define SOURCE_KEYBOARD 0x101
#define SOURCE_GAMEPAD 0x401
#define SOURCE_TOUCHSCREEN 0x1002
#define SOURCE_JOYSTICK 0x1000010
#define ACTION_DOWN 0
#define ACTION_UP 1
#define ACTION_MOVE 2
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14

typedef struct {
    int (*enumerate)(void *, uint32_t, caps_header *);
    int (*caps)(void *, caps_header *);
    int (*state)(void *, uint32_t, remote_state *);
} input_api;

extern int a32_remote_in_use; /* vrapi.c */

static struct {
    void *mobile;
    input_api api;
    jobject activity, target; /* target: the UnityPlayer, else the activity */
    jmethodID inject;         /* UnityPlayer.injectEvent(InputEvent), else Activity.dispatchKeyEvent */
    int unity, java;          /* java: the Java delivery is set up */
    jclass key_class, motion_class, coords_class, props_class, clock_class;
    jmethodID key_init, motion_obtain, motion_obtain_full, motion_set_source, motion_recycle, coords_init,
        coords_set_axis, props_init, uptime;
    jfieldID props_id, props_tool;
    volatile int running, gen; /* gen: a new input_start ends the previous thread */
    int gamepad_id; /* an Android input device with gamepad buttons, or 0 */
} I;

/* ---------------------------------------------------------------- synthetic AInputEvents (NativeActivity) */

/* native_app_glue attaches the activity's AInputQueue to its looper without a callback and polls it; the bridge
 * puts its own events in front of the queue's: pollAll/pollOnce return the queue's ident while one waits (an
 * ALooper_wake ends a blocking poll), AInputQueue_getEvent hands it out, finishEvent frees it. */
#define NFAKE 256
#define TOUCH_DEVICE 1 /* the Gear VR touchpad's device ID, as the game sees it */

typedef struct {
    int used, type, source, device, action, code;
    float x, y;
} fake_event;

static fake_event pool[NFAKE];
static int fifo[NFAKE], fifo_head, fifo_count, pool_next;
static pthread_mutex_t nq_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    void *queue, *looper;
    int ident;
    uint32_t data;
} NQ;

static int is_fake(const void *p) { return (const char *)p >= (const char *)pool && (const char *)p < (const char *)(pool + NFAKE); }
static int native_active(void) { return NQ.queue != NULL; }

void input_queue_attached(void *queue, void *looper, int ident, uint32_t data, int has_callback)
{
    if (has_callback) {
        bridge_log(ANDROID_LOG_WARN, "controllers: input queue attached with a callback: synthetic events not supported");
        return;
    }
    pthread_mutex_lock(&nq_lock);
    NQ.queue = queue, NQ.looper = looper, NQ.ident = ident, NQ.data = data;
    pthread_mutex_unlock(&nq_lock);
    bridge_log(ANDROID_LOG_INFO, "controllers: the game reads its AInputQueue (ident %d): Gear VR touchpad events go there", ident);
}

void input_queue_detached(void *queue)
{
    pthread_mutex_lock(&nq_lock);
    if (NQ.queue == queue) {
        memset(&NQ, 0, sizeof NQ);
        for (int k = 0; k < NFAKE; k++) pool[k].used = 0;
        fifo_count = 0;
    }
    pthread_mutex_unlock(&nq_lock);
}

static void native_push(int type, int source, int device, int action, int code, float x, float y)
{
    pthread_mutex_lock(&nq_lock);
    void *looper = NQ.looper;
    fake_event *e = NULL;
    for (int k = 0; k < NFAKE && NQ.queue && fifo_count < NFAKE; k++) {
        int i = (pool_next + k) % NFAKE;
        if (pool[i].used) continue;
        e = &pool[i];
        pool_next = (i + 1) % NFAKE;
        fifo[(fifo_head + fifo_count++) % NFAKE] = i;
        break;
    }
    if (e) *e = (fake_event){1, type, source, device, action, code, x, y};
    pthread_mutex_unlock(&nq_lock);
    if (e && looper && ALooper_wake) ALooper_wake(looper);
}

int input_queue_pending(void *looper, int *ident, uint32_t *data)
{
    pthread_mutex_lock(&nq_lock);
    int r = NQ.queue && looper && NQ.looper == looper && fifo_count > 0;
    if (r) *ident = NQ.ident, *data = NQ.data;
    pthread_mutex_unlock(&nq_lock);
    return r;
}

void *input_queue_pop(void *queue)
{
    void *e = NULL;
    pthread_mutex_lock(&nq_lock);
    if (queue && queue == NQ.queue && fifo_count > 0) {
        e = &pool[fifo[fifo_head]];
        fifo_head = (fifo_head + 1) % NFAKE;
        fifo_count--;
    }
    pthread_mutex_unlock(&nq_lock);
    return e;
}

static int queue_has_fake(void *queue)
{
    pthread_mutex_lock(&nq_lock);
    int r = queue && queue == NQ.queue && fifo_count > 0;
    pthread_mutex_unlock(&nq_lock);
    return r;
}

/* the guest's view of the queue and events (imports.txt "!" lines) */
void imp_AInputQueue_detachLooper(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    input_queue_detached(q);
    if (q && AInputQueue_detachLooper) AInputQueue_detachLooper(q);
}

void imp_AInputQueue_hasEvents(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    ret32(c, queue_has_fake(q) ? 1u : q && AInputQueue_hasEvents ? (uint32_t)AInputQueue_hasEvents(q) : 0u);
}

void imp_AInputQueue_preDispatchEvent(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    AInputEvent *e = h2ptr(rd32(R));
    ret32(c, is_fake(e) || !q || !e || !AInputQueue_preDispatchEvent ? 0u : (uint32_t)AInputQueue_preDispatchEvent(q, e));
}

void imp_AInputQueue_finishEvent(a32_cpu *c, argreader *R)
{
    AInputQueue *q = h2ptr(rd32(R));
    AInputEvent *e = h2ptr(rd32(R));
    int handled = (int)rd32(R);
    if (is_fake(e)) {
        pthread_mutex_lock(&nq_lock);
        ((fake_event *)e)->used = 0;
        pthread_mutex_unlock(&nq_lock);
    } else if (q && e && AInputQueue_finishEvent) {
        AInputQueue_finishEvent(q, e, handled);
    }
}

#define EV(fn, member)                                                          \
    void imp_##fn(a32_cpu *c, argreader *R)                                     \
    {                                                                           \
        const AInputEvent *e = h2ptr(rd32(R));                                  \
        ret32(c, is_fake(e) ? (uint32_t)((const fake_event *)e)->member        \
                 : e && fn ? (uint32_t)fn(e) : 0u);                             \
    }
EV(AInputEvent_getType, type)
EV(AInputEvent_getDeviceId, device)
EV(AInputEvent_getSource, source)
EV(AKeyEvent_getAction, action)
EV(AKeyEvent_getKeyCode, code)
EV(AMotionEvent_getAction, action)
#undef EV

void imp_AKeyEvent_getMetaState(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    ret32(c, is_fake(e) || !e || !AKeyEvent_getMetaState ? 0u : (uint32_t)AKeyEvent_getMetaState(e));
}

void imp_AKeyEvent_getRepeatCount(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    ret32(c, is_fake(e) || !e || !AKeyEvent_getRepeatCount ? 0u : (uint32_t)AKeyEvent_getRepeatCount(e));
}

void imp_AMotionEvent_getPointerCount(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    ret32(c, is_fake(e) ? 1u : e && AMotionEvent_getPointerCount ? (uint32_t)AMotionEvent_getPointerCount(e) : 0u);
}

void imp_AMotionEvent_getPointerId(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    size_t i = rd32(R);
    ret32(c, is_fake(e) || !e || !AMotionEvent_getPointerId ? 0u : (uint32_t)AMotionEvent_getPointerId(e, i));
}

static void motion_xy(a32_cpu *c, argreader *R, int y, float (*real)(const AInputEvent *, size_t))
{
    const AInputEvent *e = h2ptr(rd32(R));
    size_t i = rd32(R);
    retf(c, is_fake(e) ? (y ? ((const fake_event *)e)->y : ((const fake_event *)e)->x) : e && real ? real(e, i) : 0.0f);
}

void imp_AMotionEvent_getX(a32_cpu *c, argreader *R) { motion_xy(c, R, 0, AMotionEvent_getX); }
void imp_AMotionEvent_getY(a32_cpu *c, argreader *R) { motion_xy(c, R, 1, AMotionEvent_getY); }
void imp_AMotionEvent_getRawX(a32_cpu *c, argreader *R) { motion_xy(c, R, 0, AMotionEvent_getRawX); }
void imp_AMotionEvent_getRawY(a32_cpu *c, argreader *R) { motion_xy(c, R, 1, AMotionEvent_getRawY); }

void imp_AMotionEvent_getAxisValue(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    int32_t axis = (int32_t)rd32(R);
    size_t i = rd32(R);
    if (is_fake(e)) {
        const fake_event *f = (const fake_event *)e;
        retf(c, axis == AMOTION_EVENT_AXIS_X ? f->x : axis == AMOTION_EVENT_AXIS_Y ? f->y : 0.0f);
    } else {
        retf(c, e && AMotionEvent_getAxisValue ? AMotionEvent_getAxisValue(e, axis, i) : 0.0f);
    }
}

void imp_AMotionEvent_getButtonState(a32_cpu *c, argreader *R)
{
    const AInputEvent *e = h2ptr(rd32(R));
    ret32(c, is_fake(e) || !e || !AMotionEvent_getButtonState ? 0u : (uint32_t)AMotionEvent_getButtonState(e));
}

/* ---------------------------------------------------------------- Java delivery */

static jlong now_ms(JNIEnv *env) { return (*env)->CallStaticLongMethod(env, I.clock_class, I.uptime); }

static void deliver(JNIEnv *env, jobject ev)
{
    if (!ev || !I.java) return;
    (*env)->CallBooleanMethod(env, I.target, I.inject, ev);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, ev);
}

static void key(JNIEnv *env, int action, int code, int source)
{
    if (native_active()) {
        /* no gamepad: Unreal assigns gamepads through Java's InputDevice list, which doesn't have ours */
        if (source != SOURCE_GAMEPAD) native_push(AINPUT_EVENT_TYPE_KEY, source, 0, action, code, 0, 0);
        return;
    }
    if (!I.java) return;
    jlong t = now_ms(env);
    int dev = source == SOURCE_GAMEPAD ? I.gamepad_id : 0;
    jobject ev = (*env)->NewObject(env, I.key_class, I.key_init, t, t, action, code, 0, 0, dev, 0, 0, source);
    deliver(env, ev);
}

#define CX 960.0f
#define CY 540.0f

/* the touchpad, as Unity saw it on a Gear VR: touch events (mouse button 0 and the mouse position); positions are
 * given on a 1920x1080 screen and scaled to the activity's window for NativeActivity games (Unreal divides them by
 * its window size) */
static void touch(JNIEnv *env, int action, float x, float y)
{
    if (native_active()) {
        float w = g_window_w > 0 ? (float)g_window_w : 2 * CX, h = g_window_h > 0 ? (float)g_window_h : 2 * CY;
        native_push(AINPUT_EVENT_TYPE_MOTION, SOURCE_TOUCHSCREEN, TOUCH_DEVICE, action, 0, x / (2 * CX) * w,
                    y / (2 * CY) * h);
        return;
    }
    if (!I.java || !I.unity) return; /* dispatchKeyEvent takes key events only */
    static jlong down;
    jlong t = now_ms(env);
    if (action == ACTION_DOWN) down = t;
    jobject ev = (*env)->CallStaticObjectMethod(env, I.motion_class, I.motion_obtain, down, t, action, x, y, 0);
    if (!ev) return;
    (*env)->CallVoidMethod(env, ev, I.motion_set_source, SOURCE_TOUCHSCREEN);
    deliver(env, ev);
}

static void tap(JNIEnv *env, int action) { touch(env, action, CX, CY); }

static void sticks(JNIEnv *env, float x, float y, float z, float rz)
{
    if (native_active() || !I.java || !I.unity) return;
    jlong t = now_ms(env);
    jobjectArray props = (*env)->NewObjectArray(env, 1, I.props_class, NULL);
    jobjectArray coords = (*env)->NewObjectArray(env, 1, I.coords_class, NULL);
    jobject p = (*env)->NewObject(env, I.props_class, I.props_init);
    jobject c = (*env)->NewObject(env, I.coords_class, I.coords_init);
    (*env)->SetIntField(env, p, I.props_id, 0);
    (*env)->SetIntField(env, p, I.props_tool, 0);
    (*env)->CallVoidMethod(env, c, I.coords_set_axis, AXIS_X, x);
    (*env)->CallVoidMethod(env, c, I.coords_set_axis, AXIS_Y, -y); /* Android: down is positive */
    (*env)->CallVoidMethod(env, c, I.coords_set_axis, AXIS_Z, z);
    (*env)->CallVoidMethod(env, c, I.coords_set_axis, AXIS_RZ, -rz);
    (*env)->SetObjectArrayElement(env, props, 0, p);
    (*env)->SetObjectArrayElement(env, coords, 0, c);
    jobject ev = (*env)->CallStaticObjectMethod(env, I.motion_class, I.motion_obtain_full, t, t, ACTION_MOVE, 1, props,
                                                coords, 0, 0, 1.0f, 1.0f, I.gamepad_id, 0, SOURCE_JOYSTICK, 0);
    deliver(env, ev);
    (*env)->DeleteLocalRef(env, p);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, props);
    (*env)->DeleteLocalRef(env, coords);
}

static int setup(JNIEnv *env)
{
#define CLS(v, n) if (!(I.v = (*env)->NewGlobalRef(env, (*env)->FindClass(env, n)))) return 0
    CLS(key_class, "android/view/KeyEvent");
    CLS(motion_class, "android/view/MotionEvent");
    CLS(coords_class, "android/view/MotionEvent$PointerCoords");
    CLS(props_class, "android/view/MotionEvent$PointerProperties");
    CLS(clock_class, "android/os/SystemClock");
#undef CLS
    I.key_init = (*env)->GetMethodID(env, I.key_class, "<init>", "(JJIIIIIIII)V");
    I.motion_obtain = (*env)->GetStaticMethodID(env, I.motion_class, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    I.motion_obtain_full = (*env)->GetStaticMethodID(env, I.motion_class, "obtain",
        "(JJII[Landroid/view/MotionEvent$PointerProperties;[Landroid/view/MotionEvent$PointerCoords;IIFFIIII)Landroid/view/MotionEvent;");
    I.motion_set_source = (*env)->GetMethodID(env, I.motion_class, "setSource", "(I)V");
    I.coords_init = (*env)->GetMethodID(env, I.coords_class, "<init>", "()V");
    I.coords_set_axis = (*env)->GetMethodID(env, I.coords_class, "setAxisValue", "(IF)V");
    I.props_init = (*env)->GetMethodID(env, I.props_class, "<init>", "()V");
    I.props_id = (*env)->GetFieldID(env, I.props_class, "id", "I");
    I.props_tool = (*env)->GetFieldID(env, I.props_class, "toolType", "I");
    I.uptime = (*env)->GetStaticMethodID(env, I.clock_class, "uptimeMillis", "()J");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    /* a real gamepad (Unity only takes joystick input from devices Android knows) */
    jclass dc = (*env)->FindClass(env, "android/view/InputDevice");
    jmethodID ids = dc ? (*env)->GetStaticMethodID(env, dc, "getDeviceIds", "()[I") : NULL;
    jmethodID get = dc ? (*env)->GetStaticMethodID(env, dc, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    jmethodID src = dc ? (*env)->GetMethodID(env, dc, "getSources", "()I") : NULL;
    jmethodID name = dc ? (*env)->GetMethodID(env, dc, "getName", "()Ljava/lang/String;") : NULL;
    jintArray all = ids ? (*env)->CallStaticObjectMethod(env, dc, ids) : NULL;
    jint n = all ? (*env)->GetArrayLength(env, all) : 0;
    for (jint k = 0; k < n && get && src && name; k++) {
        jint id;
        (*env)->GetIntArrayRegion(env, all, k, 1, &id);
        jobject d = (*env)->CallStaticObjectMethod(env, dc, get, id);
        if (!d) continue;
        jint sources = (*env)->CallIntMethod(env, d, src);
        jstring nm = (*env)->CallObjectMethod(env, d, name);
        const char *u = nm ? (*env)->GetStringUTFChars(env, nm, NULL) : NULL;
        bridge_log(ANDROID_LOG_INFO, "controllers: Android input device %d \"%s\" sources 0x%x", id, u ? u : "?", sources);
        if ((sources & SOURCE_GAMEPAD) == SOURCE_GAMEPAD && !I.gamepad_id) I.gamepad_id = id;
        if (u) (*env)->ReleaseStringUTFChars(env, nm, u);
        (*env)->DeleteLocalRef(env, d);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    /* Unity: UnityPlayerActivity.mUnityPlayer.injectEvent(InputEvent) */
    jclass ac = (*env)->GetObjectClass(env, I.activity);
    jfieldID f = (*env)->GetFieldID(env, ac, "mUnityPlayer", "Lcom/unity3d/player/UnityPlayer;");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jobject player = f ? (*env)->GetObjectField(env, I.activity, f) : NULL;
    if (player) {
        jclass pc = (*env)->GetObjectClass(env, player);
        I.inject = (*env)->GetMethodID(env, pc, "injectEvent", "(Landroid/view/InputEvent;)Z");
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (I.inject) I.target = (*env)->NewGlobalRef(env, player), I.unity = 1;
    }
    if (!I.inject) {
        I.inject = (*env)->GetMethodID(env, ac, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
        I.target = I.activity;
    }
    return I.inject != NULL && I.key_init && I.motion_obtain && I.motion_obtain_full && I.uptime;
}

static void *poll_thread(void *arg)
{
    int gen = (int)(intptr_t)arg;
    JNIEnv *env = NULL;
    JavaVMAttachArgs a = {JNI_VERSION_1_6, "a32 controllers", NULL};
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, &a) != JNI_OK) return NULL;
    if (!I.java) I.java = setup(env);
    if (!I.java && !native_active()) {
        bridge_log(ANDROID_LOG_ERROR, "controllers: can't set up Android input events");
        (*g_vm)->DetachCurrentThread(g_vm);
        return NULL;
    }
    bridge_log(ANDROID_LOG_INFO, "controllers -> %s, gamepad device %d (trigger: hold touchpad, A: tap, right stick: "
               "swipe, B/menu: back), window %dx%d", native_active() ? "AInputQueue" : I.unity ?
               "UnityPlayer.injectEvent" : "Activity.dispatchKeyEvent", I.gamepad_id, g_window_w, g_window_h);
    uint32_t prev_buttons = 0, prev_back = 0;
    int prev_tap = 0, sticks_active = 0, logged = 0;
    int swipe_step = 0, swipe_armed = 1, tap_frames = 0;
    float swipe_dx = 0, swipe_dy = 0;
    while (I.running && I.gen == gen) {
        uint32_t buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0, trigger = 0;
        int devices = 0;
        for (uint32_t i = 0; i < 4; i++) {
            caps_header h = {0, 0};
            if (I.api.enumerate(I.mobile, i, &h) < 0) break;
            if (h.Type != TRACKED_REMOTE) continue;
            remote_caps caps;
            memset(&caps, 0, sizeof caps);
            caps.Header = h;
            if (I.api.caps(I.mobile, &caps.Header) < 0) continue;
            remote_state st;
            memset(&st, 0, sizeof st);
            st.ControllerType = TRACKED_REMOTE;
            if (I.api.state(I.mobile, h.DeviceID, &st) < 0) continue;
            devices++;
            int left = (caps.ControllerCapabilities & LEFT_HAND) != 0;
            buttons |= st.Buttons;
            if (left) lx = st.Joystick[0], ly = st.Joystick[1];
            else rx = st.Joystick[0], ry = st.Joystick[1], trigger = st.IndexTrigger;
        }
        if (devices && !logged++) bridge_log(ANDROID_LOG_INFO, "controllers: %d found", devices);
        static const struct { uint32_t bit; int code; } map[] = {
            {B_X, KEYCODE_BUTTON_X}, {B_Y, KEYCODE_BUTTON_Y},
            {B_LTHUMB, KEYCODE_BUTTON_THUMBL}, {B_RTHUMB, KEYCODE_BUTTON_THUMBR},
        };
        for (size_t k = 0; k < sizeof map / sizeof *map; k++) {
            uint32_t now = buttons & map[k].bit, was = prev_buttons & map[k].bit;
            if (now != was) key(env, now ? KEY_DOWN : KEY_UP, map[k].code, SOURCE_GAMEPAD);
        }
        if (a32_remote_in_use) {
            /* the game reads the right controller as its Gear VR Controller: only the left menu button stays (back) */
            buttons &= B_ENTER | B_X | B_Y | B_LTHUMB;
            trigger = rx = ry = 0;
            if (prev_tap && !swipe_step) tap(env, ACTION_UP), prev_tap = 0;
        }
        uint32_t back = buttons & (B_ENTER | B_B);
        if (!!back != !!prev_back) key(env, back ? KEY_DOWN : KEY_UP, KEYCODE_BACK, SOURCE_KEYBOARD);
        /* touchpad held: the trigger; A gives a short tap (~100 ms) */
        if ((buttons & B_A) && !(prev_buttons & B_A)) tap_frames = 9;
        int tapping = trigger > 0.6f || (prev_tap && trigger > 0.4f) || tap_frames > 0;
        if (tap_frames > 0) tap_frames--;
        if (tapping != prev_tap && !swipe_step) tap(env, tapping ? ACTION_DOWN : ACTION_UP);
        else if (tapping != prev_tap) tapping = prev_tap;
        /* a flick of the right stick: a swipe across the touchpad (down, 6 moves, up) */
        float mag = rx * rx + ry * ry;
        if (mag < 0.09f) swipe_armed = 1;
        if (!swipe_step && swipe_armed && mag > 0.5f && !prev_tap) {
            swipe_armed = 0;
            if (rx * rx > ry * ry) swipe_dx = rx > 0 ? 1 : -1, swipe_dy = 0;
            /* NativeActivity games get the touchpad's own axes: X runs front to back, so stick up = swipe forward,
             * down = swipe back (Gunjack: back = reload, forward = special weapon) */
            else if (native_active()) swipe_dx = ry > 0 ? -1 : 1, swipe_dy = 0;
            else swipe_dx = 0, swipe_dy = ry > 0 ? -1 : 1; /* stick forward: swipe towards the front */
            touch(env, ACTION_DOWN, CX, CY);
            swipe_step = 1;
        } else if (swipe_step) {
            float d = 50.0f * (float)swipe_step;
            if (swipe_step <= 6) touch(env, ACTION_MOVE, CX + swipe_dx * d, CY + swipe_dy * d);
            else {
                touch(env, ACTION_UP, CX + swipe_dx * 300, CY + swipe_dy * 300);
                swipe_step = -1;
            }
            swipe_step++;
        }
        int moving = lx * lx + ly * ly > 0.0025f;
        if (moving || sticks_active) sticks(env, lx, ly, 0, 0); /* one centred event after movement stops */
        sticks_active = moving;
        prev_buttons = buttons, prev_back = back, prev_tap = tapping;
        usleep(11000);
    }
    (*g_vm)->DetachCurrentThread(g_vm);
    return NULL;
}

void input_start(void *mobile, void *enumerate, void *caps, void *state, jobject activity)
{
    if (getenv("A32_NO_INPUT") || I.running || !enumerate || !caps || !state || !activity) return;
    I.mobile = mobile;
    I.api.enumerate = enumerate, I.api.caps = caps, I.api.state = state;
    I.activity = activity;
    I.running = 1;
    int gen = ++I.gen; /* a thread of an earlier VR mode that hasn't seen running == 0 yet ends */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, poll_thread, (void *)(intptr_t)gen)) I.running = 0;
    pthread_attr_destroy(&attr);
}

void input_stop(void) { I.running = 0; }
