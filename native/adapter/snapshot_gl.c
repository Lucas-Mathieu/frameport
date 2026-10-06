// FramePort FrameBridge: eye-image snapshots for headless diagnostics (setting snapshot=<seconds>, GLES games only).
// Every <seconds>, at xrEndFrame, the left-eye image the game just submitted (projection layer, view 0: its swapchain
// image and array layer) is read back on the game's own GL context and written at a quarter of its size as
// /sdcard/Android/data/<package>/files/fb_snap_<n>.ppm (n = 0-7, the oldest is overwritten). Nobody wears the headset
// in a launch test, so the headset view stays black: this shows what the game itself draws (e.g. which screen it is
// stuck on). The game's framebuffer, pack-buffer and pack-alignment bindings are restored afterwards.
#define SNAP_MAX_SWAPCHAINS 16
#define SNAP_MAX_IMAGES 8
#define SNAP_SCALE 4


#define SNAP_GL_FUNCS(X) X(glGenFramebuffers) X(glDeleteFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) \
    X(glFramebufferTextureLayer) X(glCheckFramebufferStatus) X(glReadPixels) X(glGetIntegerv) X(glPixelStorei) \
    X(glBindBuffer) X(glGetError)
#define SNAP_DECLARE(fn) static __typeof__(&fn) s_##fn;
SNAP_GL_FUNCS(SNAP_DECLARE)
#undef SNAP_DECLARE
static EGLContext (*s_eglGetCurrentContext)(void);

static struct {
    XrSwapchain handle;
    uint32_t width, height, array_size, count, last_acquired;
    GLuint images[SNAP_MAX_IMAGES];
} snap_chains[SNAP_MAX_SWAPCHAINS];
static pthread_mutex_t snap_lock = PTHREAD_MUTEX_INITIALIZER;

static int snap_load_gl(void) {
    static int state;  // 0 untried, 1 ok, -1 failed
    if (state) return state > 0;
    void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL), *gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    int ok = egl && gles;
    if (ok) s_eglGetCurrentContext = (EGLContext (*)(void))dlsym(egl, "eglGetCurrentContext");
#define SNAP_RESOLVE(fn) ok &= gles && (s_##fn = (__typeof__(s_##fn))dlsym(gles, #fn)) != NULL;
    SNAP_GL_FUNCS(SNAP_RESOLVE)
#undef SNAP_RESOLVE
    state = ok && s_eglGetCurrentContext ? 1 : -1;
    if (state < 0) LOG("snapshot: OpenGL ES not available, no snapshots");
    return state > 0;
}

static int snap_find(XrSwapchain handle, int create) {
    for (int i = 0; i < SNAP_MAX_SWAPCHAINS; ++i)
        if (snap_chains[i].handle == handle) return i;
    if (!create) return -1;
    for (int i = 0; i < SNAP_MAX_SWAPCHAINS; ++i)
        if (!snap_chains[i].handle) { snap_chains[i].handle = handle; snap_chains[i].count = 0; return i; }
    return -1;
}

static void snap_on_create(XrSwapchain handle, const XrSwapchainCreateInfo *info) {
    if (!snapshot || !info) return;
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(handle, 1);
    if (i >= 0) {
        snap_chains[i].width = info->width;
        snap_chains[i].height = info->height;
        snap_chains[i].array_size = info->arraySize ? info->arraySize : 1;
    }
    pthread_mutex_unlock(&snap_lock);
}

static void snap_on_destroy(XrSwapchain handle) {
    if (!snapshot) return;
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(handle, 0);
    if (i >= 0) snap_chains[i].handle = XR_NULL_HANDLE;
    pthread_mutex_unlock(&snap_lock);
}

static void snap_on_enumerate(XrSwapchain handle, uint32_t count, const XrSwapchainImageBaseHeader *images) {
    if (!snapshot || !images || !count || images->type != (XrStructureType)1000024002) return;  // OPENGL_ES_KHR
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(handle, 1);
    if (i >= 0) {
        uint32_t n = count < SNAP_MAX_IMAGES ? count : SNAP_MAX_IMAGES;
        const emul_gles_image *gl = (const emul_gles_image *)images;
        for (uint32_t k = 0; k < n; ++k) snap_chains[i].images[k] = gl[k].image;
        snap_chains[i].count = n;
    }
    pthread_mutex_unlock(&snap_lock);
}

static void snap_on_acquire(XrSwapchain handle, uint32_t index) {
    if (!snapshot) return;
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(handle, 0);
    if (i >= 0) snap_chains[i].last_acquired = index;
    pthread_mutex_unlock(&snap_lock);
}

static void snap_write(const unsigned char *rgba, uint32_t w, uint32_t h, int n) {
    char path[320];
    snprintf(path, sizeof(path), "%s/fb_snap_%d.ppm", snap_dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) { LOG("snapshot: can't write %s", path); return; }
    uint32_t ow = w / SNAP_SCALE, oh = h / SNAP_SCALE;
    fprintf(f, "P6\n%u %u\n255\n", ow, oh);
    unsigned char *row = malloc((size_t)ow * 3);
    for (uint32_t y = 0; row && y < oh; ++y) {
        const unsigned char *src = rgba + (size_t)(h - 1 - y * SNAP_SCALE) * w * 4;  // GL rows are bottom-up
        for (uint32_t x = 0; x < ow; ++x) {
            const unsigned char *p = src + (size_t)x * SNAP_SCALE * 4;
            row[x * 3] = p[0], row[x * 3 + 1] = p[1], row[x * 3 + 2] = p[2];
        }
        fwrite(row, 1, (size_t)ow * 3, f);
    }
    free(row);
    fclose(f);
    LOG("snapshot: %s (%ux%u)", path, ow, oh);
}

static void snap_end_frame(const XrFrameEndInfo *info) {
    if (!snapshot || !*snap_dir || !info) return;
    static int64_t next_ns;
    static int taken;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t t = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
    if (t < next_ns) return;
    next_ns = t + (int64_t)snapshot * 1000000000ll;
    const XrCompositionLayerProjection *proj = NULL;
    for (uint32_t i = 0; i < info->layerCount && !proj; ++i)
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
            ((const XrCompositionLayerProjection *)info->layers[i])->viewCount)
            proj = (const XrCompositionLayerProjection *)info->layers[i];
    if (!proj || !snap_load_gl()) return;
    if (!s_eglGetCurrentContext()) {
        static int logged;
        if (!logged++) LOG("snapshot: no GL context current in xrEndFrame, no snapshot");
        return;
    }
    const XrSwapchainSubImage *sub = &proj->views[0].subImage;
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(sub->swapchain, 0);
    GLuint tex = 0;
    uint32_t w = 0, h = 0, array_size = 1;
    if (i >= 0 && snap_chains[i].count && snap_chains[i].last_acquired < snap_chains[i].count) {
        tex = snap_chains[i].images[snap_chains[i].last_acquired];
        w = snap_chains[i].width, h = snap_chains[i].height, array_size = snap_chains[i].array_size;
    }
    pthread_mutex_unlock(&snap_lock);
    if (!tex || !w || !h) return;
    GLint read_fb = 0, pack_buffer = 0, pack_alignment = 4;
    s_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    s_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    s_glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    GLuint fb = 0;
    s_glGenFramebuffers(1, &fb);
    s_glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    if (array_size > 1)
        s_glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, (GLint)sub->imageArrayIndex);
    else
        s_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    unsigned char *rgba = NULL;
    if (s_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        (rgba = malloc((size_t)w * h * 4))) {
        s_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        s_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        s_glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        static int logged;
        if (logged++ < 3) LOG("snapshot: eye image %u not readable (framebuffer incomplete)", tex);
    }
    s_glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    s_glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    s_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    s_glDeleteFramebuffers(1, &fb);
    while (s_glGetError() != GL_NO_ERROR) {}  // our calls must not leave errors for the game to find
    if (rgba) {
        snap_write(rgba, w, h, taken++ % 8);
        free(rgba);
    }
}
