/* VrApi 1.x for 32-bit games (Gear VR / Oculus Mobile SDK 1.0-1.5: vrapi_SubmitFrame with ovrFrameParms,
 * vrapi_GetPredictedTracking), translated to the 64-bit VrApi -> OpenXR bridge (native/vrapi-bridge, shipped in the
 * APK as libfp_vrapi64.so), which implements the later API (vrapi_SubmitFrame2, vrapi_GetPredictedTracking2).
 *
 * The 32-bit layouts are VrApi 1.5's (VrApi_Types.h asserts them; OVRPlugin 1.12's 1856-byte ovrFrameParms
 * matches). Host objects (ovrMobile, swap chains) are handles; EGL objects in the mode parameters are the handles the
 * guest got from the GL thunks. Each frame's layers become projection layers (the old API had no other kind). */
#include <dlfcn.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include "imports_internal.h"

/* ---------------------------------------------------------------- the 64-bit API (native/vrapi-bridge/abi.h) */

typedef struct { JavaVM *Vm; JNIEnv *Env; jobject ActivityObject; } j64;
typedef struct { int32_t Type, ProductVersion, MajorVersion, MinorVersion, PatchVersion, GraphicsAPI; j64 Java; } init64;
typedef struct { int32_t Type; uint32_t Flags; j64 Java; uint64_t Display, WindowSurface, ShareContext; } mode64;
typedef struct {
    float Orientation[4], Position[3];
    float AngularVelocity[3], LinearVelocity[3], AngularAcceleration[3], LinearAcceleration[3];
    uint32_t Padding;
    double TimeInSeconds, PredictionInSeconds;
} pose64; /* ovrRigidBodyPosef: the same in both ABIs */
typedef struct {
    uint32_t Status, Padding;
    pose64 HeadPose;
    struct { float Projection[16], View[16]; } Eye[2];
} tracking2;
typedef struct {
    int32_t Type;
    uint32_t Flags;
    float ColorScale[4];
    int32_t SrcBlend, DstBlend;
    void *Reserved;
} header2;
typedef struct {
    header2 Header;
    pose64 HeadPose;
    struct {
        void *ColorSwapChain;
        int32_t SwapChainIndex;
        float TexCoordsFromTanAngles[16];
        float TextureRect[4];
    } Textures[2];
} projection2;
typedef struct {
    uint32_t Flags, SwapInterval;
    uint64_t FrameIndex;
    double DisplayTime;
    uint8_t Pad[8];
    uint32_t LayerCount;
    const header2 *const *Layers;
} submit2;
_Static_assert(sizeof(init64) == 48 && sizeof(mode64) == 56 && sizeof(pose64) == 96, "VrApi ABI");
_Static_assert(sizeof(tracking2) == 360 && sizeof(header2) == 40 && sizeof(projection2) == 328, "VrApi ABI");
_Static_assert(sizeof(submit2) == 48, "VrApi ABI");

/* ---------------------------------------------------------------- the old 32-bit layouts (offsets) */

/* ovrFrameParms. VrApi 1.5 (Type 3, 1856 bytes): Layers at 8. What OVRPlugin 1.12 (VrApi loader 1.0.4.5) passes
 * has Type 0x10000001 and 140 more bytes before the same layers (found from a dump: the layers' swap chain handles,
 * head pose times and the trailing PerformanceParms/Java fields line up), 1996 bytes. */
typedef struct { uint32_t layers, count, flags, frame_index; } fp_layout;
static const fp_layout FP_15 = {8, 1736, 1740, 1744}, FP_104 = {148, 1876, 1880, 1884};
#define FP_LAYER_SIZE 432
#define FP_MAX_LAYERS 4
/* ovrFrameLayer: Textures[2] (200 bytes each), SpinSpeed, SpinScale, ColorScale, pad, SrcBlend, DstBlend, Flags */
#define FL_TEXTURE_SIZE 200
#define FL_COLOR_SCALE 408
#define FL_SRC_BLEND 416
#define FL_DST_BLEND 420
#define FL_FLAGS 424
/* ovrFrameLayerTexture */
#define FT_COLOR 0
#define FT_INDEX 8
#define FT_TEXCOORDS 12
#define FT_RECT 76
#define FT_HEAD_POSE 96

#define DEFAULT_SWAPCHAIN_MAX 2u /* VRAPI_DEFAULT_TEXTURE_SWAPCHAIN_BLACK / _LOADING_ICON: not real chains */

/* ---------------------------------------------------------------- the bridge */

static struct {
    void *lib;
    int (*Initialize)(const init64 *);
    void (*Shutdown)(void);
    const char *(*GetVersionString)(void);
    double (*GetTimeInSeconds)(void);
    int (*GetSystemPropertyInt)(const j64 *, int);
    float (*GetSystemPropertyFloat)(const j64 *, int);
    int (*GetSystemStatusInt)(const j64 *, int);
    void *(*EnterVrMode)(const mode64 *);
    void (*LeaveVrMode)(void *);
    double (*GetPredictedDisplayTime)(void *, int64_t);
    tracking2 (*GetPredictedTracking2)(void *, double);
    void (*RecenterPose)(void *);
    void *(*CreateTextureSwapChain2)(int32_t, int32_t, int, int, int, int);
    void (*DestroyTextureSwapChain)(void *);
    int (*GetTextureSwapChainLength)(void *);
    unsigned (*GetTextureSwapChainHandle)(void *, int);
    int (*SubmitFrame2)(void *, const submit2 *);
    void (*SetPropertyInt)(const j64 *, int, int);
    void *EnumerateInputDevices, *GetInputDeviceCapabilities, *GetCurrentInputState;
    int (*GetInputTrackingState)(void *, uint32_t, double, void *);
    void *(*CreateTextureSwapChain3)(int32_t, int64_t, int, int, int, int);
    int (*SetDisplayRefreshRate)(void *, float);
    int (*PollEvent)(void *);
    int (*GetSystemPropertyFloatArray)(const j64 *, int, float *, int);
    int (*SetTrackingSpace)(void *, int);
} B;
static jobject activity; /* the app's activity (global reference), for input.c */

/* input.c */
void input_start(void *mobile, void *enumerate, void *caps, void *state, jobject activity);
void input_stop(void);

static int bridge_ok;

static void bridge_init(void)
{
    B.lib = dlopen("libfp_vrapi64.so", RTLD_NOW | RTLD_LOCAL);
    if (!B.lib) {
        bridge_log(ANDROID_LOG_ERROR, "VrApi: the 64-bit bridge libfp_vrapi64.so is missing: %s", dlerror());
        return;
    }
#define L(f) *(void **)&B.f = dlsym(B.lib, "vrapi_" #f)
    L(Initialize); L(Shutdown); L(GetVersionString); L(GetTimeInSeconds); L(GetSystemPropertyInt);
    L(GetSystemPropertyFloat); L(GetSystemStatusInt); L(EnterVrMode); L(LeaveVrMode); L(GetPredictedDisplayTime);
    L(GetPredictedTracking2); L(RecenterPose); L(CreateTextureSwapChain2); L(DestroyTextureSwapChain);
    L(GetTextureSwapChainLength); L(GetTextureSwapChainHandle); L(SubmitFrame2); L(SetPropertyInt);
    L(EnumerateInputDevices); L(GetInputDeviceCapabilities); L(GetCurrentInputState); L(GetInputTrackingState);
    L(CreateTextureSwapChain3); L(SetDisplayRefreshRate); L(PollEvent); L(GetSystemPropertyFloatArray);
    L(SetTrackingSpace);
#undef L
    bridge_ok = B.Initialize && B.EnterVrMode && B.SubmitFrame2 && B.GetPredictedTracking2 && B.GetPredictedDisplayTime &&
                B.LeaveVrMode && B.CreateTextureSwapChain2 && B.DestroyTextureSwapChain && B.GetTextureSwapChainLength &&
                B.GetTextureSwapChainHandle;
    bridge_log(bridge_ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "VrApi 1.x -> %s",
               B.GetVersionString ? B.GetVersionString() : "?");
}

static int bridge(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, bridge_init);
    return bridge_ok;
}

/* ovrJava (32-bit: 12 bytes) -> the host's */
static j64 java_of(uint32_t g)
{
    j64 j = {g_vm, host_env(), g ? h2o(ld32(g + 8)) : NULL};
    return j;
}

static void *chain(uint32_t h) { return h <= DEFAULT_SWAPCHAIN_MAX ? (void *)(uintptr_t)h : h2ptr(h); }

/* ---------------------------------------------------------------- wrapped swap chains */

/* OVRPlugin 1.12 creates empty (0x0) swap chains and puts Unity's own eye textures into them with
 * vrapi_SetTextureSwapChainHandle. Such a chain is ours: at each submit the texture in use is copied into a real
 * bridge chain of its size (created on demand), which is what the layer then shows. */
#define WRAP_MAGIC 0x57524150u
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_TEXTURE_BINDING_2D_ 0x8069
#define GL_TEXTURE_WIDTH_ 0x1000
#define GL_TEXTURE_HEIGHT_ 0x1001

typedef struct {
    uint32_t magic;
    int type, format, count;
    unsigned tex[8];
    void *real;
    int w, h;
} wrap;

static struct {
    void (*GetIntegerv)(unsigned, int *);
    void (*BindTexture)(unsigned, unsigned);
    void (*GetTexLevelParameteriv)(unsigned, int, unsigned, int *);
    void (*CopyImageSubData)(unsigned, unsigned, int, int, int, int, unsigned, unsigned, int, int, int, int, int, int,
                             int);
    unsigned (*GetError)(void);
    void (*GenFramebuffers)(int, unsigned *);
    void (*DeleteFramebuffers)(int, const unsigned *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
} G;

static int gl_ready(void)
{
    static int done, ok;
    if (!done) {
        done = 1;
        void *gles = dlopen("libGLESv3.so", RTLD_NOW);
        void *egl = dlopen("libEGL.so", RTLD_NOW);
        void *(*gpa)(const char *) = egl ? (void *(*)(const char *))dlsym(egl, "eglGetProcAddress") : NULL;
#define G_(f, n) *(void **)&G.f = gles ? dlsym(gles, n) : NULL; if (!G.f && gpa) *(void **)&G.f = gpa(n)
        G_(GetIntegerv, "glGetIntegerv"); G_(BindTexture, "glBindTexture");
        G_(GetTexLevelParameteriv, "glGetTexLevelParameteriv"); G_(CopyImageSubData, "glCopyImageSubData");
        G_(GetError, "glGetError"); G_(GenFramebuffers, "glGenFramebuffers");
        G_(DeleteFramebuffers, "glDeleteFramebuffers"); G_(BindFramebuffer, "glBindFramebuffer");
        G_(FramebufferTexture2D, "glFramebufferTexture2D"); G_(ReadPixels, "glReadPixels");
        G_(BlitFramebuffer, "glBlitFramebuffer");
#undef G_
        ok = G.GetIntegerv && G.BindTexture && G.GetTexLevelParameteriv && G.GetError && G.GenFramebuffers &&
             G.BindFramebuffer && G.FramebufferTexture2D && G.BlitFramebuffer;
        if (!ok) bridge_log(ANDROID_LOG_ERROR, "VrApi: GLES functions for wrapped swap chains are missing");
    }
    return ok;
}

/* A32_PROBE_EYES=1: every 5 s, the average of a 64x64 block in the middle of the app's eye texture (w 0: its size) */
static void probe(unsigned tex, int w, int h)
{
    static int on = -1;
    static double next;
    if (on < 0) on = getenv("A32_PROBE_EYES") != NULL;
    if (!on || !G.GenFramebuffers || !G.ReadPixels) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (now < next) return;
    next = now + 5;
    if (!w) { /* the size of the texture itself */
        int b = 0;
        G.GetIntegerv(GL_TEXTURE_BINDING_2D_, &b);
        G.BindTexture(GL_TEXTURE_2D_, tex);
        G.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_WIDTH_, &w);
        G.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_HEIGHT_, &h);
        G.BindTexture(GL_TEXTURE_2D_, (unsigned)b);
    }
    int old = 0;
    G.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old);
    unsigned fb;
    G.GenFramebuffers(1, &fb);
    G.BindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fb);
    G.FramebufferTexture2D(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, GL_TEXTURE_2D_, tex, 0);
    static unsigned char px[64 * 64 * 4];
    G.ReadPixels(w / 2 - 32, h / 2 - 32, 64, 64, 0x1908 /* GL_RGBA */, 0x1401 /* UNSIGNED_BYTE */, px);
    G.BindFramebuffer(0x8CA8, (unsigned)old);
    G.DeleteFramebuffers(1, &fb);
    unsigned long sum[3] = {0, 0, 0};
    for (int k = 0; k < 64 * 64; k++) sum[0] += px[4 * k], sum[1] += px[4 * k + 1], sum[2] += px[4 * k + 2];
    bridge_log(ANDROID_LOG_INFO, "eye texture %u: middle average rgb %lu %lu %lu (GL error 0x%x)", tex, sum[0] / 4096,
               sum[1] / 4096, sum[2] / 4096, G.GetError());
}

static wrap *as_wrap(void *p) { return p && ((wrap *)p)->magic == WRAP_MAGIC ? p : NULL; }

/* the real chain + index showing the wrapped chain's texture `index` this frame */
static void *wrap_resolve(wrap *w, int index, int32_t *out_index)
{
    *out_index = 0;
    if (index < 0 || index >= w->count || !w->tex[index] || !gl_ready()) return NULL;
    unsigned src = w->tex[index];
    int old = 0, width = 0, height = 0;
    G.GetIntegerv(GL_TEXTURE_BINDING_2D_, &old);
    G.BindTexture(GL_TEXTURE_2D_, src);
    G.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_WIDTH_, &width);
    G.GetTexLevelParameteriv(GL_TEXTURE_2D_, 0, GL_TEXTURE_HEIGHT_, &height);
    G.BindTexture(GL_TEXTURE_2D_, (unsigned)old);
    if (width <= 0 || height <= 0) return NULL;
    if (!w->real || w->w != width || w->h != height) {
        if (w->real) B.DestroyTextureSwapChain(w->real);
        w->real = B.CreateTextureSwapChain2(0, w->format ? w->format : 5, width, height, 1, 1);
        w->w = width, w->h = height;
        bridge_log(ANDROID_LOG_INFO, "wrapped swap chain: the app's %dx%d texture is copied into %p", width, height,
                   w->real);
    }
    if (!w->real) return NULL;
    probe(src, width, height);
    unsigned dst = B.GetTextureSwapChainHandle(w->real, 0);
    /* a framebuffer blit, not glCopyImageSubData: that needs complete textures, and Unity's render textures have
     * a mipmapping filter without mipmaps (GL_INVALID_OPERATION) */
    static unsigned fbs[2];
    if (!fbs[0]) G.GenFramebuffers(2, fbs);
    int old_read = 0, old_draw = 0;
    G.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_read);
    G.GetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &old_draw);
    G.BindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbs[0]);
    G.FramebufferTexture2D(0x8CA8, 0x8CE0, GL_TEXTURE_2D_, src, 0);
    G.BindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fbs[1]);
    G.FramebufferTexture2D(0x8CA9, 0x8CE0, GL_TEXTURE_2D_, dst, 0);
    G.BlitFramebuffer(0, 0, width, height, 0, 0, width, height, 0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
    G.BindFramebuffer(0x8CA8, (unsigned)old_read);
    G.BindFramebuffer(0x8CA9, (unsigned)old_draw);
    static int errors;
    unsigned e = G.GetError();
    if (e && errors++ < 5) bridge_log(ANDROID_LOG_ERROR, "wrapped swap chain blit failed: 0x%x", e);
    return w->real;
}

/* ---------------------------------------------------------------- call statistics (every 2 s, first minute) */

enum { S_TRACK, S_DISPLAY_TIME, S_SUBMIT, S_SUBMIT_EMPTY, S_STATUS, S_PROP, S_WAIT, S_BEGIN, S_OTHER, S_N };
static const char *const stat_names[S_N] = {"GetPredictedTracking", "GetPredictedDisplayTime", "SubmitFrame",
                                            "SubmitFrame(no layers)", "GetSystemStatus", "GetSystemProperty", "WaitFrame",
                                            "BeginFrame", "other"};
static unsigned stats[S_N];
static double last_display_time;
static int64_t last_frame_index; /* the app's frame counter, as given to vrapi_GetPredictedDisplayTime */

static void stat(int k)
{
    static double next;
    static int reports;
    __atomic_add_fetch(&stats[k], 1, __ATOMIC_RELAXED);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (!next) next = now + 2;
    if (now < next || reports >= 30) return;
    next = now + 2;
    reports++;
    char buf[400];
    size_t n = 0;
    for (int i = 0; i < S_N; i++)
        if (stats[i]) n += (size_t)snprintf(buf + n, sizeof buf - n, " %s=%u", stat_names[i], stats[i]);
    bridge_log(ANDROID_LOG_INFO, "vrapi calls in 2 s:%s (last display time %.3f)", buf, last_display_time);
    memset(stats, 0, sizeof stats);
}

/* ---------------------------------------------------------------- the functions */

/* One VrApi instance per process, as on a Gear VR/Go (the system driver kept it): OVRPlugin 1.46 initializes in
 * PreInitialize and again when VR starts. Re-creating the OpenXR instance made the loader unload the runtime while
 * another thread still ran in it (SIGSEGV in UnityMain, Spunky Dungeon), so later calls reuse the first. */
static int vrapi_initialized;
static uint32_t app_api; /* the app's VrApi as major << 16 | minor (1.0.4.5 -> 0x00004, 1.1.31.0 -> 0x1001f) */

/* the app's input structs have the fields added in VrApi 1.1.<minor> (they end before them in older apps: writing
 * them overwrote AFFECTED's stack, VrApi 1.0.4) */
static int api_has(uint32_t minor) { return app_api >= (1u << 16 | minor); }

void imp_vrapi_Initialize(a32_cpu *c, argreader *R)
{
    uint32_t p = rd32(R);
    if (!bridge() || !p) {
        ret32(c, (uint32_t)-1);
        return;
    }
    if (vrapi_initialized) {
        bridge_log(ANDROID_LOG_INFO, "vrapi_Initialize again: the instance is kept");
        ret32(c, 0);
        return;
    }
    init64 in = {1, (int32_t)ld32(p + 4), 1, 50, 0, (int32_t)ld32(p + 20), java_of(p + 24)};
    if (!activity && in.Java.ActivityObject && in.Java.Env)
        activity = (*in.Java.Env)->NewGlobalRef(in.Java.Env, in.Java.ActivityObject);
    int r = B.Initialize(&in);
    if (r == 0) vrapi_initialized = 1;
    app_api = ld32(p + 8) << 16 | (ld32(p + 12) & 0xffff);
    bridge_log(ANDROID_LOG_INFO, "vrapi_Initialize(app VrApi %u.%u.%u, graphics 0x%x) -> %d", ld32(p + 8), ld32(p + 12),
               ld32(p + 16), ld32(p + 20), r);
    ret32(c, (uint32_t)r);
}

/* kept until the process ends (see vrapi_Initialize) */
void imp_vrapi_Shutdown(a32_cpu *c, argreader *R) {}

void imp_vrapi_GetVersionString(a32_cpu *c, argreader *R)
{
    ret32(c, guest_cstr("1.5.0 (FramePort a32 bridge)"));
}

void imp_vrapi_GetTimeInSeconds(a32_cpu *c, argreader *R)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    retd(c, bridge() && B.GetTimeInSeconds ? B.GetTimeInSeconds() : ts.tv_sec + ts.tv_nsec * 1e-9);
}

void imp_vrapi_GetSystemPropertyInt(a32_cpu *c, argreader *R)
{
    uint32_t jp = rd32(R);
    int prop = (int)rd32(R);
    j64 j = java_of(jp);
    int v = bridge() && B.GetSystemPropertyInt ? B.GetSystemPropertyInt(&j, prop) : 0;
    static int seen[256]; /* once per property: some games ask every frame */
    if (prop < 0 || prop >= 256 || !seen[prop]++) bridge_log(ANDROID_LOG_INFO, "vrapi_GetSystemPropertyInt(%d) -> %d", prop, v);
    ret32(c, (uint32_t)v);
}

void imp_vrapi_GetSystemPropertyFloat(a32_cpu *c, argreader *R)
{
    uint32_t jp = rd32(R);
    int prop = (int)rd32(R);
    j64 j = java_of(jp);
    float v = bridge() && B.GetSystemPropertyFloat ? B.GetSystemPropertyFloat(&j, prop) : 0.0f;
    bridge_log(ANDROID_LOG_INFO, "vrapi_GetSystemPropertyFloat(%d) -> %g", prop, (double)v);
    retf(c, v);
}

void imp_vrapi_GetSystemPropertyString(a32_cpu *c, argreader *R)
{
    rd32(R);
    int prop = (int)rd32(R);
    ret32(c, guest_cstr(prop == 15 /* VRAPI_SYS_PROP_EXT_SDCARD_PATH */ ? "/sdcard" : ""));
}

void imp_vrapi_GetSystemStatusInt(a32_cpu *c, argreader *R)
{
    stat(S_STATUS);
    uint32_t jp = rd32(R);
    int st = (int)rd32(R);
    j64 j = java_of(jp);
    int v = bridge() && B.GetSystemStatusInt ? B.GetSystemStatusInt(&j, st) : 0;
    static int seen[64];
    if (st >= 0 && st < 64 && seen[st]++ == 0) bridge_log(ANDROID_LOG_INFO, "vrapi_GetSystemStatusInt(%d) -> %d", st, v);
    ret32(c, (uint32_t)v);
}

void imp_vrapi_GetSystemStatusFloat(a32_cpu *c, argreader *R) { retf(c, 0.0f); }

void imp_vrapi_EnterVrMode(a32_cpu *c, argreader *R)
{
    uint32_t p = rd32(R);
    if (!bridge() || !p) {
        ret32(c, 0);
        return;
    }
    /* ovrModeParms (32-bit): Type, Flags, Java (12), pad, Display, WindowSurface, ShareContext (u64 each) */
    mode64 m = {2, ld32(p + 4), java_of(p + 8), (uint64_t)(uintptr_t)h2ptr((uint32_t)ld64(p + 24)),
                (uint64_t)(uintptr_t)h2ptr((uint32_t)ld64(p + 32)), (uint64_t)(uintptr_t)h2ptr((uint32_t)ld64(p + 40))};
    bridge_log(ANDROID_LOG_INFO, "vrapi_EnterVrMode: display 0x%llx -> %p, window 0x%llx -> %p, context 0x%llx -> %p",
               (unsigned long long)ld64(p + 24), (void *)(uintptr_t)m.Display, (unsigned long long)ld64(p + 32),
               (void *)(uintptr_t)m.WindowSurface, (unsigned long long)ld64(p + 40), (void *)(uintptr_t)m.ShareContext);
    {
        void *egl = dlopen("libEGL.so", RTLD_NOW);
        unsigned (*query)(void *, void *, int, int *) = egl ? dlsym(egl, "eglQueryContext") : NULL;
        int (*error)(void) = egl ? dlsym(egl, "eglGetError") : NULL;
        void *(*current)(void) = egl ? dlsym(egl, "eglGetCurrentContext") : NULL;
        void *(*current_dpy)(void) = egl ? dlsym(egl, "eglGetCurrentDisplay") : NULL;
        int id = -1;
        unsigned ok = query ? query((void *)(uintptr_t)m.Display, (void *)(uintptr_t)m.ShareContext, 0x3028, &id) : 0;
        bridge_log(ANDROID_LOG_INFO, "EnterVrMode: eglQueryContext(CONFIG_ID) ok %u id %d error 0x%x; current %p on %p", ok,
                   id, error ? error() : 0, current ? current() : NULL, current_dpy ? current_dpy() : NULL);
    }
    void *mobile = B.EnterVrMode(&m);
    bridge_log(ANDROID_LOG_INFO, "vrapi_EnterVrMode(flags 0x%x) -> %p", m.Flags, mobile);
    if (mobile) input_start(mobile, B.EnumerateInputDevices, B.GetInputDeviceCapabilities, B.GetCurrentInputState, activity);
    ret32(c, ptr2h(mobile));
}

void imp_vrapi_LeaveVrMode(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    guest_backtrace(c, "vrapi_LeaveVrMode"); /* games that leave VR by themselves: where from */
    input_stop();
    if (bridge() && m) B.LeaveVrMode(m);
}

void imp_vrapi_GetPredictedDisplayTime(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    int64_t index = (int64_t)rd64(R);
    stat(S_DISPLAY_TIME);
    last_frame_index = index;
    last_display_time = bridge() && m ? B.GetPredictedDisplayTime(m, index) : 0.0;
    retd(c, last_display_time);
}

/* ovrTracking (104 bytes) is returned through a hidden pointer (r0); it is the start of ovrTracking2 */
void imp_vrapi_GetPredictedTracking(a32_cpu *c, argreader *R)
{
    uint32_t out = rd32(R);
    void *m = h2ptr(rd32(R));
    double t = rdd(R);
    stat(S_TRACK);
    tracking2 tr;
    memset(&tr, 0, sizeof tr);
    tr.HeadPose.Orientation[3] = 1;
    if (bridge() && m) tr = B.GetPredictedTracking2(m, t);
    if (out) memcpy(g2p(out), &tr, 104);
    ret32(c, out);
}

void imp_vrapi_RecenterPose(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    if (bridge() && m && B.RecenterPose) B.RecenterPose(m);
}

static void create_chain(a32_cpu *c, argreader *R, int buffered_flag)
{
    int32_t type = (int32_t)rd32(R), format = (int32_t)rd32(R);
    int w = (int)rd32(R), h = (int)rd32(R), levels = (int)rd32(R), last = (int)rd32(R);
    /* vrapi_CreateTextureSwapChain's last argument is "buffered" (3 images), CreateTextureSwapChain2's a count */
    int count = buffered_flag ? (last ? 3 : 1) : last;
    if (w <= 0 || h <= 0) {
        /* an empty chain for the app's own textures (vrapi_SetTextureSwapChainHandle) */
        wrap *wr = calloc(1, sizeof *wr);
        wr->magic = WRAP_MAGIC;
        wr->type = type, wr->format = format, wr->count = count < 1 ? 1 : count > 8 ? 8 : count;
        ret32(c, ptr2h(wr));
        return;
    }
    void *ch = bridge() && B.CreateTextureSwapChain2 ? B.CreateTextureSwapChain2(type, format, w, h, levels, count) : NULL;
    bridge_log(ANDROID_LOG_INFO, "swap chain %dx%d type %d format %d levels %d x%d -> %p", w, h, type, format, levels,
               count, ch);
    ret32(c, ptr2h(ch));
}

void imp_vrapi_CreateTextureSwapChain(a32_cpu *c, argreader *R) { create_chain(c, R, 1); }
void imp_vrapi_CreateTextureSwapChain2(a32_cpu *c, argreader *R) { create_chain(c, R, 0); }

void imp_vrapi_DestroyTextureSwapChain(a32_cpu *c, argreader *R)
{
    uint32_t h = rd32(R);
    if (h <= DEFAULT_SWAPCHAIN_MAX || !bridge()) return;
    wrap *w = as_wrap(h2ptr(h));
    if (w) {
        if (w->real) B.DestroyTextureSwapChain(w->real);
        w->magic = 0;
        free(w);
        return;
    }
    B.DestroyTextureSwapChain(h2ptr(h));
}

void imp_vrapi_GetTextureSwapChainLength(a32_cpu *c, argreader *R)
{
    uint32_t h = rd32(R);
    wrap *w = h > DEFAULT_SWAPCHAIN_MAX ? as_wrap(h2ptr(h)) : NULL;
    ret32(c, w ? (uint32_t)w->count : h > DEFAULT_SWAPCHAIN_MAX && bridge() ? (uint32_t)B.GetTextureSwapChainLength(h2ptr(h)) : 0);
}

void imp_vrapi_GetTextureSwapChainHandle(a32_cpu *c, argreader *R)
{
    uint32_t h = rd32(R);
    int index = (int)rd32(R);
    wrap *w = h > DEFAULT_SWAPCHAIN_MAX ? as_wrap(h2ptr(h)) : NULL;
    if (w) ret32(c, index >= 0 && index < w->count ? w->tex[index] : 0);
    else ret32(c, h > DEFAULT_SWAPCHAIN_MAX && bridge() ? B.GetTextureSwapChainHandle(h2ptr(h), index) : 0);
}

void imp_vrapi_SetTextureSwapChainHandle(a32_cpu *c, argreader *R)
{
    uint32_t h = rd32(R);
    int index = (int)rd32(R);
    unsigned tex = rd32(R);
    wrap *w = h > DEFAULT_SWAPCHAIN_MAX ? as_wrap(h2ptr(h)) : NULL;
    static int logged;
    if (logged++ < 12) bridge_log(ANDROID_LOG_INFO, "vrapi_SetTextureSwapChainHandle(0x%x%s, %d, %u)", h, w ? " wrapped" : "", index, tex);
    if (w && index >= 0 && index < w->count) w->tex[index] = tex;
    else {
        static int once;
        if (!once++) bridge_log(ANDROID_LOG_WARN, "vrapi_SetTextureSwapChainHandle on a chain the bridge made (ignored)");
    }
}

void imp_vrapi_SubmitFrame(a32_cpu *c, argreader *R)
{
    uint32_t mh = rd32(R);
    void *m = h2ptr(mh);
    uint32_t fp = rd32(R);
    stat(S_SUBMIT);
    if (!bridge() || !m || !fp) {
        static int bad;
        if (bad++ < 5) bridge_log(ANDROID_LOG_WARN, "vrapi_SubmitFrame(ovr 0x%x -> %p, parms 0x%x) skipped", mh, m, fp);
        return;
    }
    static projection2 layers[FP_MAX_LAYERS];
    const header2 *list[FP_MAX_LAYERS];
    const fp_layout *F = ld32(fp) == 3 ? &FP_15 : &FP_104;
    static int announced;
    if (!announced++) bridge_log(ANDROID_LOG_INFO, "ovrFrameParms type 0x%x: %s layout", ld32(fp), F == &FP_15 ? "VrApi 1.5" : "VrApi 1.0.4");
    uint32_t n = 0, count = ld32(fp + F->count);
    if (count > FP_MAX_LAYERS) count = FP_MAX_LAYERS;
    static int frames_logged;
    if (frames_logged < 3 || (frames_logged < 600 && frames_logged % 200 == 0)) {
        for (uint32_t k = 0; k < count; k++) {
            uint32_t L = fp + F->layers + k * FP_LAYER_SIZE;
            uint32_t c0 = ld32(L + FT_COLOR), c1 = ld32(L + FL_TEXTURE_SIZE + FT_COLOR);
            bridge_log(ANDROID_LOG_INFO, "frame %llu layer %u/%u: chains 0x%x%s 0x%x%s index %u %u flags 0x%x blend %u/%u",
                       (unsigned long long)ld64(fp + F->frame_index), k, count, c0,
                       c0 > DEFAULT_SWAPCHAIN_MAX && as_wrap(h2ptr(c0)) ? "(w)" : "", c1,
                       c1 > DEFAULT_SWAPCHAIN_MAX && as_wrap(h2ptr(c1)) ? "(w)" : "", ld32(L + FT_INDEX),
                       ld32(L + FL_TEXTURE_SIZE + FT_INDEX), ld32(L + FL_FLAGS), ld32(L + FL_SRC_BLEND), ld32(L + FL_DST_BLEND));
        }
    }
    frames_logged++;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t L = fp + F->layers + k * FP_LAYER_SIZE;
        uint32_t c0 = ld32(L + FT_COLOR), c1 = ld32(L + FL_TEXTURE_SIZE + FT_COLOR);
        if (c0 <= DEFAULT_SWAPCHAIN_MAX) continue; /* empty, black or the loading icon */
        projection2 *p = &layers[n];
        memset(p, 0, sizeof *p);
        p->Header.Type = 1; /* VRAPI_LAYER_TYPE_PROJECTION2 */
        p->Header.Flags = ld32(L + FL_FLAGS);
        float scale;
        memcpy(&scale, g2p(L + FL_COLOR_SCALE), 4);
        if (scale == 0) scale = 1;
        p->Header.ColorScale[0] = p->Header.ColorScale[1] = p->Header.ColorScale[2] = p->Header.ColorScale[3] = scale;
        p->Header.SrcBlend = (int32_t)ld32(L + FL_SRC_BLEND);
        p->Header.DstBlend = (int32_t)ld32(L + FL_DST_BLEND);
        memcpy(&p->HeadPose, g2p(L + FT_HEAD_POSE), sizeof p->HeadPose);
        for (int e = 0; e < 2; e++) {
            uint32_t T = L + (uint32_t)e * FL_TEXTURE_SIZE;
            uint32_t ch = e ? (c1 > DEFAULT_SWAPCHAIN_MAX ? c1 : c0) : c0;
            if (e && c1 <= DEFAULT_SWAPCHAIN_MAX) T = L; /* one texture for both eyes */
            p->Textures[e].ColorSwapChain = chain(ch);
            p->Textures[e].SwapChainIndex = (int32_t)ld32(T + FT_INDEX);
            wrap *w = as_wrap(p->Textures[e].ColorSwapChain);
            if (w) {
                /* both eyes may show one wrapped texture: copy it once */
                if (e && ch == c0 && T == L) p->Textures[1].ColorSwapChain = p->Textures[0].ColorSwapChain,
                                              p->Textures[1].SwapChainIndex = p->Textures[0].SwapChainIndex;
                else p->Textures[e].ColorSwapChain = wrap_resolve(w, p->Textures[e].SwapChainIndex,
                                                                  &p->Textures[e].SwapChainIndex);
            }
            memcpy(p->Textures[e].TexCoordsFromTanAngles, g2p(T + FT_TEXCOORDS), 64);
            memcpy(p->Textures[e].TextureRect, g2p(T + FT_RECT), 16);
        }
        if (!p->Textures[0].ColorSwapChain || !p->Textures[1].ColorSwapChain) continue;
        list[n++] = &p->Header;
    }
    if (!n) stat(S_SUBMIT_EMPTY);
    submit2 d;
    memset(&d, 0, sizeof d);
    d.Flags = ld32(fp + F->flags) & 2; /* VRAPI_FRAME_FLAG_FLUSH: the only one the bridge takes */
    d.SwapInterval = 1;
    d.FrameIndex = ld64(fp + F->frame_index);
    if (!d.FrameIndex) d.FrameIndex = (uint64_t)last_frame_index; /* not filled in by OVRPlugin 1.12 */
    d.DisplayTime = B.GetPredictedDisplayTime(m, (int64_t)d.FrameIndex);
    d.LayerCount = n;
    d.Layers = list;
    int r = B.SubmitFrame2(m, &d);
    static int fails;
    if (r && fails++ < 10) bridge_log(ANDROID_LOG_WARN, "vrapi_SubmitFrame: frame %llu, %u layers -> %d",
                                      (unsigned long long)d.FrameIndex, n, r);
}

/* ---------------------------------------------------------------- input: the Gear VR Controller */

/* The game sees one Gear VR Controller (ovrControllerType_TrackedRemote), made from the Frame's right controller:
 *   index trigger -> the remote's trigger (ovrButton_A)        stick -> its touchpad (touched + position)
 *   stick click, A -> touchpad click (ovrButton_Enter)          B -> back (ovrButton_Back)
 *   the controller's pose -> the remote's orientation (vrapi_GetInputTrackingState)
 * VrApi 1.5 layouts (32-bit = 64-bit here): caps header {Type, DeviceID}; tracked remote caps + ControllerCaps,
 * ButtonCaps, TrackpadMaxX/Y (u16), TrackpadSizeX/Y; state header {Type, pad, double Time}; tracked remote state +
 * Buttons, TrackpadStatus, TrackpadPosition (x, y), battery, recenter count. */
#define REMOTE_ID 0x52454d31u /* the guest's device ID for it */
#define TRACKED_REMOTE 4
#define RB_A 0x00000001u
#define RB_B 0x00000002u
#define RB_RTHUMB 0x00000004u
#define RB_LTHUMB 0x00000400u
#define TURN_PULSE 0.06   /* s: one turn's click at the touchpad's edge */
#define TURN_REPEAT 0.35  /* s: the next turn while the right stick stays pushed */
#define RB_ENTER 0x00100000u
#define RB_BACK 0x00200000u
#define TRACKPAD_MAX 320

int a32_remote_in_use; /* input.c: once the game reads the remote, the right controller isn't sent as touch */

typedef struct { int32_t Type; uint32_t DeviceID; } caps_header;
typedef struct {
    caps_header Header;
    uint32_t ControllerCapabilities, ButtonCapabilities;
    uint16_t TrackpadMaxX, TrackpadMaxY;
    float TrackpadSizeX, TrackpadSizeY;
    uint32_t HapticSamplesMax, HapticSampleDurationMS, TouchCapabilities, Reserved4, Reserved5;
} remote_caps64;
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
} remote_state64;

/* the bridge's device ID of the Frame's right controller, 0xffffffff if none */
/* the bridge's Touch controller for one hand (caps bit 4 left, 8 right) */
static uint32_t hand_controller(void *m, uint32_t hand)
{
    int (*en)(void *, uint32_t, caps_header *) = B.EnumerateInputDevices;
    int (*caps)(void *, caps_header *) = B.GetInputDeviceCapabilities;
    if (!en || !caps) return 0xffffffffu;
    for (uint32_t i = 0; i < 4; i++) {
        caps_header h = {0, 0};
        if (en(m, i, &h) < 0) break;
        if (h.Type != TRACKED_REMOTE) continue;
        remote_caps64 rc;
        memset(&rc, 0, sizeof rc);
        rc.Header = h;
        if (caps(m, &rc.Header) >= 0 && (rc.ControllerCapabilities & hand)) return h.DeviceID;
    }
    return 0xffffffffu;
}

static uint32_t right_controller(void *m) { return hand_controller(m, 8u); }

void imp_vrapi_EnumerateInputDevices(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    uint32_t index = rd32(R), out = rd32(R);
    if (!bridge() || !m || index != 0 || right_controller(m) == 0xffffffffu) {
        ret32(c, (uint32_t)-1);
        return;
    }
    st32(out, TRACKED_REMOTE);
    st32(out + 4, REMOTE_ID);
    if (!a32_remote_in_use++) bridge_log(ANDROID_LOG_INFO, "the game uses the Gear VR Controller (Frame right controller)");
    ret32(c, 0);
}

void imp_vrapi_GetInputDeviceCapabilities(a32_cpu *c, argreader *R)
{
    rd32(R);
    uint32_t p = rd32(R);
    if (ld32(p) != TRACKED_REMOTE || ld32(p + 4) != REMOTE_ID) {
        ret32(c, (uint32_t)-1);
        return;
    }
    /* orientation tracking, right hand, the Oculus Go Controller, a trackpad (OVRPlugin reads the touchpad only with it) */
    st32(p + 8, 0x1u | 0x8u | 0x10u | 0x1000u);
    st32(p + 12, RB_A | RB_ENTER | RB_BACK);
    st16(p + 16, TRACKPAD_MAX);
    st16(p + 18, TRACKPAD_MAX);
    float size = 0.04f;
    memcpy(g2p(p + 20), &size, 4);
    memcpy(g2p(p + 24), &size, 4);
    if (api_has(15)) st32(p + 36, 0x10u | 0x40u); /* TouchCapabilities: trackpad, index trigger */
    ret32(c, 0);
}

void imp_vrapi_GetCurrentInputState(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    uint32_t id = rd32(R), out = rd32(R);
    uint32_t dev = bridge() && m && id == REMOTE_ID ? right_controller(m) : 0xffffffffu;
    remote_state64 st;
    memset(&st, 0, sizeof st);
    st.ControllerType = TRACKED_REMOTE;
    int (*state)(void *, uint32_t, remote_state64 *) = B.GetCurrentInputState;
    if (dev == 0xffffffffu || !state || state(m, dev, &st) < 0) {
        ret32(c, (uint32_t)-1);
        return;
    }
    /* the left stick walks and strafes (touch position), the right stick only turns: Go games turn on a click of
     * the touchpad's left/right edge (OVRInput's DpadLeft/Right), but a thumb there also strafes, so a turn is a
     * short click pulse at the edge (TURN_PULSE), repeated every TURN_REPEAT while the stick stays pushed */
    remote_state64 left;
    memset(&left, 0, sizeof left);
    left.ControllerType = TRACKED_REMOTE; /* the bridge refuses queries of any other type */
    uint32_t ldev = hand_controller(m, 4u);
    if (ldev == 0xffffffffu || state(m, ldev, &left) < 0) memset(left.Joystick, 0, sizeof left.Joystick);
    uint32_t buttons = 0;
    if (st.IndexTrigger > 0.5f) buttons |= RB_A;
    if (st.Buttons & (RB_RTHUMB | RB_A) || left.Buttons & RB_LTHUMB) buttons |= RB_ENTER;
    if (st.Buttons & RB_B) buttons |= RB_BACK;
    float x = left.Joystick[0], y = left.Joystick[1], turn = st.Joystick[0];
    if (x * x + y * y < 0.15f * 0.15f) x = y = 0; /* sticks at rest aren't exactly 0 */
    static double turn_until, next_turn;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (fabsf(turn) > 0.6f) {
        if (now >= next_turn) {
            turn_until = now + TURN_PULSE;
            next_turn = now + TURN_REPEAT;
        }
    } else if (fabsf(turn) < 0.3f) {
        next_turn = 0; /* back to the middle: the next push turns at once */
    }
    if (now < turn_until) { /* the click at the edge, alone */
        buttons |= RB_ENTER;
        x = turn > 0 ? 1.0f : turn < 0 ? -1.0f : x;
        y = 0;
    }
    int touched = x != 0 || y != 0 || (buttons & RB_ENTER);
    /* the touchpad's origin is its top-left corner: stick forward = the top edge */
    float px = (x + 1) * 0.5f * TRACKPAD_MAX, py = (1 - (y + 1) * 0.5f) * TRACKPAD_MAX;
    st32(out, TRACKED_REMOTE);
    double t = st.TimeInSeconds;
    memcpy(g2p(out + 8), &t, 8);
    st32(out + 16, buttons);
    st32(out + 20, touched ? 1 : 0);
    memcpy(g2p(out + 24), &px, 4);
    memcpy(g2p(out + 28), &py, 4);
    st8(out + 32, 100);
    st8(out + 33, 0);
    /* trigger value (VrApi 1.1.13+) and touches (1.1.15+) */
    if (api_has(13)) memcpy(g2p(out + 36), &st.IndexTrigger, 4);
    if (api_has(15)) st32(out + 44, (touched ? 0x10u /* ovrTouch_TrackPad */ : 0) | (st.IndexTrigger > 0.05f ? 0x40u : 0));
    ret32(c, 0);
}

/* (ovr, deviceID, double time, ovrTracking *): the double takes r2:r3, the pointer is on the stack */
void imp_vrapi_GetInputTrackingState(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    uint32_t id = rd32(R);
    double t = rdd(R);
    uint32_t out = rd32(R);
    uint32_t dev = bridge() && m && id == REMOTE_ID ? right_controller(m) : 0xffffffffu;
    if (dev == 0xffffffffu || !B.GetInputTrackingState || !out) {
        ret32(c, (uint32_t)-1);
        return;
    }
    static uint8_t tr[104];
    memset(tr, 0, sizeof tr);
    int r = B.GetInputTrackingState(m, dev, t, tr);
    if (r >= 0) memcpy(g2p(out), tr, 104); /* ovrTracking: the same layout in both ABIs */
    ret32(c, (uint32_t)r);
}

void imp_vrapi_RecenterInputPose(a32_cpu *c, argreader *R) {}
void imp_vrapi_SetRemoteEmulation(a32_cpu *c, argreader *R) { ret32(c, 0); }

void imp_vrapi_SetPropertyInt(a32_cpu *c, argreader *R)
{
    uint32_t jp = rd32(R);
    int prop = (int)rd32(R), value = (int)rd32(R);
    j64 j = java_of(jp);
    if (bridge() && B.SetPropertyInt) B.SetPropertyInt(&j, prop, value);
}

void imp_vrapi_SetPropertyFloat(a32_cpu *c, argreader *R) {}
void imp_vrapi_GetPropertyInt(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_ShowSystemUI(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_ShowSystemUIWithExtra(a32_cpu *c, argreader *R) { ret32(c, 0); }

void imp_vrapi_ShowFatalError(a32_cpu *c, argreader *R)
{
    rd32(R);
    uint32_t title = rd32(R), msg = rd32(R);
    bridge_log(ANDROID_LOG_ERROR, "vrapi_ShowFatalError: %s: %s", title ? (const char *)g2p(title) : "",
               msg ? (const char *)g2p(msg) : "");
}

void imp_vrapi_ReturnToHome(a32_cpu *c, argreader *R) {}

/* the loader's own preference store: nothing stored, the default comes back */
void imp_ovr_GetLocalPreferenceValueForKey(a32_cpu *c, argreader *R)
{
    rd32(R);
    ret32(c, rd32(R));
}

void imp_ovr_SetLocalPreferenceValueForKey(a32_cpu *c, argreader *R) {}

/* ---------------------------------------------------------------- VrApi 1.2x-1.4x: vrapi_SubmitFrame2 and friends */

/* 32-bit layouts (VrApi 1.42's VrApi_Types.h asserts them): ovrLayerHeader2 36 bytes {Type, Flags, ColorScale[4],
 * SrcBlend, DstBlend, Reserved}; ovrLayerProjection2 312 {header, pad, HeadPose @40, Textures @136, 88 bytes each:
 * ColorSwapChain, SwapChainIndex, TexCoordsFromTanAngles @8, TextureRect @72}; ovrLayerCylinder2 440 (textures 152
 * bytes: + TextureMatrix @88); ovrSubmitFrameDescription2 40 {Flags, SwapInterval, FrameIndex @8, DisplayTime @16,
 * Pad, LayerCount @32, Layers @36}. */
#define L2_TYPE 0
#define L2_FLAGS 4
#define L2_COLOR_SCALE 8
#define L2_SRC_BLEND 24
#define L2_DST_BLEND 28
#define L2_HEAD_POSE 40
#define L2_TEXTURES 136
#define L2_PROJ_TEX 88
#define L2_CYL_TEX 152
#define LAYER_PROJECTION2 1
#define LAYER_CYLINDER2 3
#define LAYER_LOADING_ICON2 6
#define MAX_LAYERS2 16

typedef struct {
    header2 Header;
    pose64 HeadPose;
    struct {
        void *ColorSwapChain;
        int32_t SwapChainIndex;
        float TexCoordsFromTanAngles[16];
        float TextureRect[4];
        float TextureMatrix[16];
    } Textures[2];
} cylinder2;
_Static_assert(sizeof(cylinder2) == 456, "VrApi ABI");

static void header_of(uint32_t g, header2 *h)
{
    memset(h, 0, sizeof *h);
    h->Type = (int32_t)ld32(g + L2_TYPE);
    h->Flags = ld32(g + L2_FLAGS);
    memcpy(h->ColorScale, g2p(g + L2_COLOR_SCALE), 16);
    h->SrcBlend = (int32_t)ld32(g + L2_SRC_BLEND);
    h->DstBlend = (int32_t)ld32(g + L2_DST_BLEND);
}

/* a guest swap chain handle -> what the bridge takes (wrapped chains get their app texture copied in) */
static void *chain_for_bridge(uint32_t h, int32_t *index)
{
    void *p = chain(h);
    wrap *w = as_wrap(p);
    return w ? wrap_resolve(w, *index, index) : p;
}

void imp_vrapi_SubmitFrame2(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    uint32_t d32 = rd32(R);
    stat(S_SUBMIT);
    if (!bridge() || !m || !d32) {
        ret32(c, (uint32_t)-1005);
        return;
    }
    static union { header2 h; projection2 p; cylinder2 cy; } layers[MAX_LAYERS2];
    const header2 *list[MAX_LAYERS2];
    uint32_t count = ld32(d32 + 32), arr = ld32(d32 + 36), n = 0;
    for (uint32_t k = 0; k < count && n < MAX_LAYERS2 && arr; k++) {
        uint32_t g = ld32(arr + 4 * k);
        if (!g) continue;
        int32_t type = (int32_t)ld32(g + L2_TYPE);
        if (type == LAYER_PROJECTION2) {
            projection2 *p = &layers[n].p;
            memset(p, 0, sizeof *p);
            header_of(g, &p->Header);
            memcpy(&p->HeadPose, g2p(g + L2_HEAD_POSE), sizeof p->HeadPose);
            for (int e = 0; e < 2; e++) {
                uint32_t T = g + L2_TEXTURES + (uint32_t)e * L2_PROJ_TEX;
                p->Textures[e].SwapChainIndex = (int32_t)ld32(T + 4);
                p->Textures[e].ColorSwapChain = chain_for_bridge(ld32(T), &p->Textures[e].SwapChainIndex);
                memcpy(p->Textures[e].TexCoordsFromTanAngles, g2p(T + 8), 64);
                memcpy(p->Textures[e].TextureRect, g2p(T + 72), 16);
            }
            if (!p->Textures[0].ColorSwapChain || !p->Textures[1].ColorSwapChain) continue;
            if (!as_wrap(chain(ld32(g + L2_TEXTURES))) && gl_ready() && B.GetTextureSwapChainHandle)
                probe(B.GetTextureSwapChainHandle(p->Textures[0].ColorSwapChain, p->Textures[0].SwapChainIndex), 0, 0);
        } else if (type == LAYER_CYLINDER2) {
            cylinder2 *cy = &layers[n].cy;
            memset(cy, 0, sizeof *cy);
            header_of(g, &cy->Header);
            memcpy(&cy->HeadPose, g2p(g + L2_HEAD_POSE), sizeof cy->HeadPose);
            for (int e = 0; e < 2; e++) {
                uint32_t T = g + L2_TEXTURES + (uint32_t)e * L2_CYL_TEX;
                cy->Textures[e].SwapChainIndex = (int32_t)ld32(T + 4);
                cy->Textures[e].ColorSwapChain = chain_for_bridge(ld32(T), &cy->Textures[e].SwapChainIndex);
                memcpy(cy->Textures[e].TexCoordsFromTanAngles, g2p(T + 8), 64);
                memcpy(cy->Textures[e].TextureRect, g2p(T + 72), 16);
                memcpy(cy->Textures[e].TextureMatrix, g2p(T + 88), 64);
            }
            if (!cy->Textures[0].ColorSwapChain) continue;
        } else if (type == LAYER_LOADING_ICON2) {
            header_of(g, &layers[n].h); /* the bridge shows nothing for it */
        } else {
            static int logged[16];
            if (type >= 0 && type < 16 && !logged[type]++)
                bridge_log(ANDROID_LOG_WARN, "vrapi_SubmitFrame2: layer type %d isn't supported (left out)", type);
            continue;
        }
        list[n] = &layers[n].h;
        n++;
    }
    submit2 d;
    memset(&d, 0, sizeof d);
    d.Flags = ld32(d32) & 2;
    d.SwapInterval = 1;
    d.FrameIndex = ld64(d32 + 8);
    memcpy(&d.DisplayTime, g2p(d32 + 16), 8);
    d.LayerCount = n;
    d.Layers = list;
    if (!n) stat(S_SUBMIT_EMPTY);
    int r = B.SubmitFrame2(m, &d);
    static int fails;
    if (r && fails++ < 10) bridge_log(ANDROID_LOG_WARN, "vrapi_SubmitFrame2: frame %llu, %u layers -> %d",
                                      (unsigned long long)d.FrameIndex, n, r);
    ret32(c, (uint32_t)r);
}

void imp_vrapi_CreateTextureSwapChain3(a32_cpu *c, argreader *R)
{
    int32_t type = (int32_t)rd32(R);
    int64_t format = (int64_t)rd64(R);
    int w = (int)rd32(R), h = (int)rd32(R), levels = (int)rd32(R), count = (int)rd32(R);
    /* GLES apps pass a GL internal format; the bridge takes those only through CreateTextureSwapChain2's VrApi
     * formats (its CreateTextureSwapChain3 is Vulkan only) */
    int vrformat = format == 0x8C43 ? 5 /* GL_SRGB8_ALPHA8: 8888_sRGB */ : format == 0x8058 ? 4 /* GL_RGBA8 */
                 : format == 0x881A ? 6 /* GL_RGBA16F */ : 0;
    void *ch = NULL;
    if (bridge() && vrformat && B.CreateTextureSwapChain2) ch = B.CreateTextureSwapChain2(type, vrformat, w, h, levels, count);
    else if (bridge() && B.CreateTextureSwapChain3) ch = B.CreateTextureSwapChain3(type, format, w, h, levels, count);
    bridge_log(ANDROID_LOG_INFO, "swap chain3 %dx%d type %d format 0x%llx levels %d x%d -> %p", w, h, type,
               (unsigned long long)format, levels, count, ch);
    ret32(c, ptr2h(ch));
}

/* ovrTracking2 (360 bytes, no pointers) through a hidden result pointer */
void imp_vrapi_GetPredictedTracking2(a32_cpu *c, argreader *R)
{
    uint32_t out = rd32(R);
    void *m = h2ptr(rd32(R));
    double t = rdd(R);
    stat(S_TRACK);
    tracking2 tr;
    memset(&tr, 0, sizeof tr);
    tr.HeadPose.Orientation[3] = 1;
    if (bridge() && m) tr = B.GetPredictedTracking2(m, t);
    if (out) memcpy(g2p(out), &tr, sizeof tr);
    ret32(c, out);
}

/* the bridge paces frames in vrapi_GetPredictedDisplayTime (its xrWaitFrame/xrBeginFrame) */
void imp_vrapi_WaitFrame(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    int64_t index = (int64_t)rd64(R);
    stat(S_WAIT);
    last_frame_index = index;
    if (bridge() && m) last_display_time = B.GetPredictedDisplayTime(m, index);
    ret32(c, 0);
}

void imp_vrapi_BeginFrame(a32_cpu *c, argreader *R)
{
    stat(S_BEGIN);
    ret32(c, 0);
}

void imp_vrapi_SetDisplayRefreshRate(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    float hz = rdf(R);
    ret32(c, bridge() && m && B.SetDisplayRefreshRate ? (uint32_t)B.SetDisplayRefreshRate(m, hz) : 0);
}

/* events have no pointers: the same layout in both ABIs */
void imp_vrapi_PollEvent(a32_cpu *c, argreader *R)
{
    uint32_t ev = rd32(R);
    ret32(c, bridge() && ev && B.PollEvent ? (uint32_t)B.PollEvent(g2p(ev)) : (uint32_t)-1005);
}

void imp_vrapi_GetSystemPropertyFloatArray(a32_cpu *c, argreader *R)
{
    uint32_t jp = rd32(R);
    int prop = (int)rd32(R);
    uint32_t values = rd32(R);
    int count = (int)rd32(R);
    j64 j = java_of(jp);
    ret32(c, bridge() && B.GetSystemPropertyFloatArray && values
                 ? (uint32_t)B.GetSystemPropertyFloatArray(&j, prop, g2p(values), count) : 0);
}

static int tracking_space = 1; /* VRAPI_TRACKING_SPACE_LOCAL */

void imp_vrapi_SetTrackingSpace(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
    int space = (int)rd32(R);
    int r = bridge() && m && B.SetTrackingSpace ? B.SetTrackingSpace(m, space) : 0;
    if (!r) tracking_space = space;
    ret32(c, (uint32_t)r);
}

void imp_vrapi_GetTrackingSpace(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)tracking_space); }

/* ovrPosef (28 bytes) through a hidden result pointer: the identity (spaces share their origin here) */
void imp_vrapi_LocateTrackingSpace(a32_cpu *c, argreader *R)
{
    uint32_t out = rd32(R);
    static const float identity[7] = {0, 0, 0, 1, 0, 0, 0};
    if (out) memcpy(g2p(out), identity, sizeof identity);
    ret32(c, out);
}

/* no Guardian boundary is reported (the Frame's play area isn't exposed to VrApi apps) */
void imp_vrapi_GetBoundaryGeometry(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_GetBoundaryOrientedBoundingBox(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_TestPointIsInBoundary(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_GetBoundaryTriggerState(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_RequestBoundaryVisible(a32_cpu *c, argreader *R) { ret32(c, 0); }

void imp_vrapi_GetBoundaryVisible(a32_cpu *c, argreader *R)
{
    rd32(R);
    uint32_t out = rd32(R);
    if (out) st8(out, 0);
    ret32(c, 0);
}

/* the Gear VR/Go controllers had no vibration */
void imp_vrapi_SetHapticVibrationSimple(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_SetHapticVibrationBuffer(a32_cpu *c, argreader *R) { ret32(c, 0); }

void imp_vrapi_SetClockLevels(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_SetPerfThread(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_SetExtraLatencyMode(a32_cpu *c, argreader *R) { ret32(c, 0); }

/* not yet: Android surface swap chains (video layers) and Vulkan (32-bit games here use GLES) */
void imp_vrapi_CreateAndroidSurfaceSwapChain(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_CreateAndroidSurfaceSwapChain2(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_GetTextureSwapChainAndroidSurface(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_CreateSystemVulkan(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1050); }
void imp_vrapi_DestroySystemVulkan(a32_cpu *c, argreader *R) {}
void imp_vrapi_GetInstanceExtensionsVulkan(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1050); }
void imp_vrapi_GetDeviceExtensionsVulkan(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1050); }
void imp_vrapi_GetTextureSwapChainBufferVulkan(a32_cpu *c, argreader *R) { ret32(c, 0); }
void imp_vrapi_GetTextureSwapChainBufferFoveationVulkan(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1050); }
