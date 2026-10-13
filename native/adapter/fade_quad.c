// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c. fade_quad (per game, off by default; GitHub #197): games that fade to black by dimming
// their eye image with XR_KHR_composition_layer_color_scale_bias (Batman: Arkham Shadow's loading screens: scale 0 ...
// 1 on its projection layer) showed the level loading on the Frame instead of black. With the setting, FrameBridge
// draws the fade itself: a black, alpha-blended quad layer right in front of the eyes (VIEW space, 1 m ahead, far
// larger than the field of view), whose opacity is 1 - the layer's colour scale. The Frame draws quad layers above all
// projection layers, which is what a fade needs; it is inserted right after the dimmed projection layer, so the game's
// own quads (loading icons, subtitles) stay on top of it.
//  * No drawing per frame: one tiny swapchain, refilled only when the fade level changes (at most FADE_LEVELS small
//    clears per fade: Vulkan vkCmdClearColorImage on the app's queue through flip_vk.c's command buffer; GLES glClear
//    through a temporary framebuffer in the app's current context). A swapchain's last released image stays valid
//    for every later frame. (One swapchain per level hit XR_ERROR_LIMIT_REACHED in Batman: the Frame's runtime
//    allows only a few swapchains per app.)
//  * Set up on the first frame that needs it; any failure turns the fade off for the session (logged once).
//  * fade_quad=2 also removes the layer's own colour scale (as strip_color_bias=1), for runtimes that do apply it
//    (the fade would then be doubled).

#include "fade_level.h"
#define FADE_GLES_BINDING ((XrStructureType)1000024001)  // XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR
#define FADE_GLES_IMAGE ((XrStructureType)1000024002)    // XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR

static struct {
    XrSession session;
    int state;  // 0 = not set up, 1 = ready, -1 = failed this session
    int gles;   // the session's graphics API, from its binding
    XrSpace view;
    XrSwapchain swapchain;
    uint32_t size, image_count;
    XrSwapchainImageVulkanKHR vk_images[8];
    emul_gles_image gl_images[8];
    int shown;  // the level the last released image shows (0 = none yet)
} fade;

// GLES entry points for the one-time fill (resolved at run time like layer_emul_gl.c's: Vulkan games never load GL)
#define FADE_GL_FUNCS(X) X(glGenFramebuffers) X(glDeleteFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) \
    X(glCheckFramebufferStatus) X(glClearColor) X(glClear) X(glGetIntegerv) X(glGetFloatv) X(glGetBooleanv) \
    X(glIsEnabled) X(glDisable) X(glEnable) X(glColorMask) X(glFinish)
#define FADE_DECLARE(fn) static __typeof__(&fn) fade_##fn;
FADE_GL_FUNCS(FADE_DECLARE)
#undef FADE_DECLARE
static __typeof__(&eglGetCurrentContext) fade_eglGetCurrentContext;

static int fade_load_gl(void) {
    void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL), *gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gles) return 0;
    int ok = (fade_eglGetCurrentContext = (__typeof__(fade_eglGetCurrentContext))dlsym(egl, "eglGetCurrentContext")) != NULL;
#define FADE_RESOLVE(fn) ok &= (fade_##fn = (__typeof__(fade_##fn))dlsym(gles, #fn)) != NULL;
    FADE_GL_FUNCS(FADE_RESOLVE)
#undef FADE_RESOLVE
    return ok;
}

static void fade_on_create_session(XrSession session, const XrSessionCreateInfo *info) {
    memset(&fade, 0, sizeof(fade));  // a new session owns new swapchains (the old ones went with its session)
    fade.session = session;
    for (const XrBaseInStructure *p = info ? (const XrBaseInStructure *)info->next : NULL; p; p = p->next)
        if (p->type == FADE_GLES_BINDING) fade.gles = 1;
}

// Clear one swapchain image to black at `alpha`.
static int fade_fill_vulkan(VkImage image, float alpha) {
    if (!vk_ready() || !vk.CmdClearColorImage) return 0;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue black = {.float32 = {0.0f, 0.0f, 0.0f, alpha}};
    vk.ResetCommandBuffer(vk.cmd, 0);
    vk.BeginCommandBuffer(vk.cmd, &begin);
    barrier(image, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vk.CmdClearColorImage(vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    barrier(image, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vk.EndCommandBuffer(vk.cmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &vk.cmd, 0, NULL};
    int ok = vk.QueueSubmit(vk.queue, 1, &submit, vk.fence) == VK_SUCCESS &&
             vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 100000000ull) == VK_SUCCESS;
    vk.ResetFences(vk.device, 1, &vk.fence);
    return ok;
}

static int fade_fill_gles(GLuint texture, float alpha) {
    if (!fade_load_gl() || !fade_eglGetCurrentContext()) return 0;  // xrEndFrame runs with the app's context current
    GLint old_fbo = 0;
    GLfloat old_clear[4];
    GLboolean old_mask[4], scissor = fade_glIsEnabled(GL_SCISSOR_TEST);
    fade_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
    fade_glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
    fade_glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
    GLuint fbo = 0;
    fade_glGenFramebuffers(1, &fbo);
    fade_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    fade_glDisable(GL_SCISSOR_TEST);
    fade_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    fade_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    int ok = fade_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    fade_glClearColor(0.0f, 0.0f, 0.0f, alpha);
    if (ok) fade_glClear(GL_COLOR_BUFFER_BIT);
    fade_glFinish();  // done before the image is released to the runtime
    fade_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_fbo);
    fade_glDeleteFramebuffers(1, &fbo);
    fade_glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    fade_glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    if (scissor) fade_glEnable(GL_SCISSOR_TEST);
    return ok;
}

static int fade_setup(XrSession session) {
    PFN_xrCreateReferenceSpace space = (PFN_xrCreateReferenceSpace)lookup(active_instance, "xrCreateReferenceSpace");
    PFN_xrCreateSwapchain create = (PFN_xrCreateSwapchain)lookup(active_instance, "xrCreateSwapchain");
    PFN_xrEnumerateSwapchainImages enumerate =
        (PFN_xrEnumerateSwapchainImages)lookup(active_instance, "xrEnumerateSwapchainImages");
    if (!space || !create || !enumerate) return 0;
    XrReferenceSpaceCreateInfo sci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_VIEW,
                                      {{0, 0, 0, 1}, {0, 0, 0}}};
    if (XR_FAILED(space(session, &sci, &fade.view))) return 0;
    static const uint32_t sizes[] = {8, 64, 256};  // in case the runtime refuses tiny images
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO, NULL, 0,
                                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                    fade.gles ? 0x8C43 /* GL_SRGB8_ALPHA8 */ : 43 /* VK_FORMAT_R8G8B8A8_SRGB */,
                                    1, sizes[i], sizes[i], 1, 1, 1};
        r = create(session, &ci, &fade.swapchain);
        LOG("fade_quad: xrCreateSwapchain %ux%u format=%lld result=%d", sizes[i], sizes[i], (long long)ci.format, r);
        if (XR_SUCCEEDED(r)) { fade.size = sizes[i]; break; }
    }
    if (XR_FAILED(r)) { fade.swapchain = XR_NULL_HANDLE; return 0; }
    for (int i = 0; i < 8; ++i) {
        fade.vk_images[i] = (XrSwapchainImageVulkanKHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, NULL, VK_NULL_HANDLE};
        fade.gl_images[i] = (emul_gles_image){FADE_GLES_IMAGE, NULL, 0};
    }
    r = enumerate(fade.swapchain, 8, &fade.image_count, fade.gles ? (XrSwapchainImageBaseHeader *)fade.gl_images
                                                                    : (XrSwapchainImageBaseHeader *)fade.vk_images);
    if (XR_FAILED(r) || !fade.image_count) { LOG("fade_quad: xrEnumerateSwapchainImages failed %d", r); return 0; }
    return 1;
}

// Show `level` in the fade swapchain (a new image only when it changes).
static int fade_show(int level) {
    if (level == fade.shown) return 1;
    PFN_xrAcquireSwapchainImage acquire = (PFN_xrAcquireSwapchainImage)lookup(active_instance, "xrAcquireSwapchainImage");
    PFN_xrWaitSwapchainImage wait = (PFN_xrWaitSwapchainImage)lookup(active_instance, "xrWaitSwapchainImage");
    PFN_xrReleaseSwapchainImage release = (PFN_xrReleaseSwapchainImage)lookup(active_instance, "xrReleaseSwapchainImage");
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, 100000000};
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL};
    uint32_t index = 0;
    if (!acquire || !wait || !release || XR_FAILED(acquire(fade.swapchain, &ai, &index)) || index >= fade.image_count)
        return 0;
    if (XR_FAILED(wait(fade.swapchain, &wi))) { release(fade.swapchain, &ri); return 0; }
    float alpha = (float)level / FADE_LEVELS;
    int ok = fade.gles ? fade_fill_gles(fade.gl_images[index].image, alpha)
                       : fade_fill_vulkan(fade.vk_images[index].image, alpha);
    release(fade.swapchain, &ri);
    if (ok) fade.shown = level;
    return ok;
}

// The fade level of a projection layer: 0 = none, else 1..FADE_LEVELS (black at level / FADE_LEVELS).
static int fade_level(const XrCompositionLayerBaseHeader *layer) {
    if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) return 0;
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)layer->next; n; n = n->next)
        if (n->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
            const XrCompositionLayerColorScaleBiasKHR *cb = (const XrCompositionLayerColorScaleBiasKHR *)n;
            return fade_level_of(cb->colorScale.r, cb->colorScale.g, cb->colorScale.b);
        }
    return 0;
}

// The quad that shows `level` (filled into *out), or NULL when there is nothing to draw or the fade isn't available.
static const XrCompositionLayerBaseHeader *fade_layer(XrSession session, int level, uint32_t layers_so_far,
                                                      XrCompositionLayerQuad *out) {
    if (!fade_quad || level <= 0) return NULL;
    if (session != fade.session) { fade.session = session; fade.state = 0; }
    if (!fade.state) {
        fade.state = fade_setup(session) ? 1 : -1;
        LOG("fade_quad: %s (%s, %d levels)", fade.state > 0 ? "drawing fades over the picture" : "unavailable",
            fade.gles ? "GLES" : "Vulkan", FADE_LEVELS);
    }
    if (fade.state < 0) return NULL;
    if (!fade_show(level)) {
        fade.state = -1;
        LOG("fade_quad: filling the fade image failed (%s); off for this session", fade.gles ? "GLES" : "Vulkan");
        return NULL;
    }
    if (layers_so_far + 1 > runtime_max_layers) {
        static int logged;
        if (!logged++) LOG("fade_quad: no room for the fade layer (%u layers)", layers_so_far);
        return NULL;
    }
    *out = (XrCompositionLayerQuad){XR_TYPE_COMPOSITION_LAYER_QUAD, NULL, XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                    fade.view, XR_EYE_VISIBILITY_BOTH,
                                    {fade.swapchain, {{0, 0}, {(int32_t)fade.size, (int32_t)fade.size}}, 0},
                                    {{0, 0, 0, 1}, {0, 0, -1.0f}}, {6.0f, 6.0f}};
    static int last = -1;
    if (layer_debug && level != last) LOG("fade_quad: level %d/%d", level, FADE_LEVELS);
    last = level;
    return (const XrCompositionLayerBaseHeader *)out;
}
