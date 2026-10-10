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
    L(EnumerateInputDevices); L(GetInputDeviceCapabilities); L(GetCurrentInputState);
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

/* A32_PROBE_EYES=1: every 5 s, the average of a 64x64 block in the middle of the app's eye texture */
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

enum { S_TRACK, S_DISPLAY_TIME, S_SUBMIT, S_SUBMIT_EMPTY, S_STATUS, S_PROP, S_OTHER, S_N };
static const char *const stat_names[S_N] = {"GetPredictedTracking", "GetPredictedDisplayTime", "SubmitFrame",
                                            "SubmitFrame(no layers)", "GetSystemStatus", "GetSystemProperty", "other"};
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

void imp_vrapi_Initialize(a32_cpu *c, argreader *R)
{
    uint32_t p = rd32(R);
    if (!bridge() || !p) {
        ret32(c, (uint32_t)-1);
        return;
    }
    init64 in = {1, (int32_t)ld32(p + 4), 1, 50, 0, (int32_t)ld32(p + 20), java_of(p + 24)};
    if (!activity && in.Java.ActivityObject && in.Java.Env)
        activity = (*in.Java.Env)->NewGlobalRef(in.Java.Env, in.Java.ActivityObject);
    int r = B.Initialize(&in);
    bridge_log(ANDROID_LOG_INFO, "vrapi_Initialize(app VrApi %u.%u.%u, graphics 0x%x) -> %d", ld32(p + 8), ld32(p + 12),
               ld32(p + 16), ld32(p + 20), r);
    ret32(c, (uint32_t)r);
}

void imp_vrapi_Shutdown(a32_cpu *c, argreader *R)
{
    if (bridge() && B.Shutdown) B.Shutdown();
}

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
    bridge_log(ANDROID_LOG_INFO, "vrapi_GetSystemPropertyInt(%d) -> %d", prop, v);
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
    ret32(c, bridge() && B.GetSystemStatusInt ? (uint32_t)B.GetSystemStatusInt(&j, st) : 0);
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
    void *mobile = B.EnterVrMode(&m);
    bridge_log(ANDROID_LOG_INFO, "vrapi_EnterVrMode(flags 0x%x) -> %p", m.Flags, mobile);
    if (mobile) input_start(mobile, B.EnumerateInputDevices, B.GetInputDeviceCapabilities, B.GetCurrentInputState, activity);
    ret32(c, ptr2h(mobile));
}

void imp_vrapi_LeaveVrMode(a32_cpu *c, argreader *R)
{
    void *m = h2ptr(rd32(R));
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

/* Input: the Gear VR controller API. Its structures changed after 1.5, so no devices are reported for now (the
 * headset's touchpad and back button arrive as Android key/touch events). */
void imp_vrapi_EnumerateInputDevices(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_GetInputDeviceCapabilities(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_GetCurrentInputState(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
void imp_vrapi_GetInputTrackingState(a32_cpu *c, argreader *R) { ret32(c, (uint32_t)-1005); }
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
