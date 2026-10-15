// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c (after layer_emul_gl.c, whose GL entry points it shares). cube_standin (default on):
// the Frame's runtime has no cube layers (XR_KHR_composition_layer_cube) and refuses cube swapchains (faceCount 6)
// with XR_ERROR_RUNTIME_FAILURE. OVRPlugin doesn't check: it goes on with "CreateSwapchain for eye 0: 0x0, 0 stages"
// and writes past its empty image list in the next ovrp_EndFrame4 (Unity's render thread SIGSEGV in memset, e.g.
// Budget Cuts Ultimate's cube-map overlay, GitHub #107). Only when the runtime refused one, a cube swapchain is served
// by the adapter instead: one GL cube-map texture or Vulkan cube-compatible image with the requested format,
// size and mip levels. Acquire/wait/release succeed at once. The app renders or copies into it as usual; layers
// that use it are dropped in xrEndFrame (the runtime couldn't show them anyway).
#define STANDIN_MAX 8
typedef struct {
    XrSwapchain handle;
    GLuint tex;
    VkDevice device;
    VkImage image;
    VkDeviceMemory memory;
} standin_swapchain;
static standin_swapchain standins[STANDIN_MAX];
static pthread_mutex_t standin_lock = PTHREAD_MUTEX_INITIALIZER;

static int standin_find(XrSwapchain handle) {
    if (!handle) return -1;
    int slot = -1;
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX && slot < 0; ++i)
        if (standins[i].handle == handle) slot = i;
    pthread_mutex_unlock(&standin_lock);
    return slot;
}

static int is_standin(XrSwapchain handle) { return cube_standin && standin_find(handle) >= 0; }

// Uses the app's Vulkan queue, with a private pool so the blitter's command buffer stays untouched.
static int standin_vk_create(const XrSwapchainCreateInfo *info, standin_swapchain *s) {
    if (info->sampleCount != 1 || !info->mipCount || !info->arraySize ||
        (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return 0;
    int ok = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    pthread_mutex_lock(&flip_lock);
    if (!vk_ready()) goto done;
    PFN_vkDestroyCommandPool destroy_pool = (PFN_vkDestroyCommandPool)vk.GetDeviceProcAddr(vk.device, "vkDestroyCommandPool");
    PFN_vkQueueWaitIdle wait_idle = (PFN_vkQueueWaitIdle)vk.GetDeviceProcAddr(vk.device, "vkQueueWaitIdle");
    if (!destroy_pool || !wait_idle) goto done;
    s->device = vk.device;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_MND) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        VK_IMAGE_TYPE_2D, (VkFormat)info->format, {info->width, info->height, 1}, info->mipCount, 6,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.CreateImage(s->device, &ci, NULL, &s->image) != VK_SUCCESS) goto done;
    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(s->device, s->image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || vk.AllocateMemory(s->device, &ai, NULL, &s->memory) != VK_SUCCESS ||
        vk.BindImageMemory(s->device, s->image, s->memory, 0) != VK_SUCCESS) goto done;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vk.family};
    if (vk.CreateCommandPool(s->device, &pi, NULL, &pool) != VK_SUCCESS) goto done;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.AllocateCommandBuffers(s->device, &alloc, &cmd) != VK_SUCCESS) goto done;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (vk.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) goto done;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, s->image,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, info->mipCount, 0, 6}};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &b);
    if (vk.EndCommandBuffer(cmd) != VK_SUCCESS) goto done;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) goto done;
    ok = wait_idle(vk.queue) == VK_SUCCESS;
done:
    if (pool) destroy_pool(s->device, pool, NULL);
    if (!ok) {
        if (s->image) vk.DestroyImage(s->device, s->image, NULL);
        if (s->memory) vk.FreeMemory(s->device, s->memory, NULL);
        s->device = VK_NULL_HANDLE;
        s->image = VK_NULL_HANDLE;
        s->memory = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&flip_lock);
    return ok;
}

// After the runtime refused a swapchain: serve a cube stand-in if the graphics API can supply it.
static XrResult standin_after_failure(const XrSwapchainCreateInfo *info, XrSwapchain *out, XrResult result) {
    if (XR_SUCCEEDED(result) || !cube_standin || !info || !out || info->faceCount != 6) return result;
    if (info->width != info->height || !info->width || info->arraySize > 1) return result;
    int gles = (p_eglCreateContext || emul_load_gl()) && p_eglGetCurrentContext() != EGL_NO_CONTEXT;
    int slot = -1;
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX && slot < 0; ++i)
        if (!standins[i].handle) slot = i;
    if (slot >= 0) standins[slot].handle = (XrSwapchain)1;  // reserved
    pthread_mutex_unlock(&standin_lock);
    if (slot < 0) { LOG("cube_standin: too many cube swapchains, the runtime's error stays"); return result; }
    if (!gles) {
        if (!standin_vk_create(info, &standins[slot])) {
            pthread_mutex_lock(&standin_lock);
            standins[slot].handle = XR_NULL_HANDLE;
            pthread_mutex_unlock(&standin_lock);
            LOG("cube_standin: couldn't make a Vulkan cube image, the runtime's error stays");
            return result;
        }
        pthread_mutex_lock(&standin_lock);
        *out = standins[slot].handle = FAKE_HANDLE(XrSwapchain);
        pthread_mutex_unlock(&standin_lock);
        LOG("cube_standin: runtime refused a %ux%u cube swapchain (result=%d): served by FrameBridge "
            "(Vulkan cube image, %u levels); its cube layers are dropped", info->width, info->height, result, info->mipCount);
        return XR_SUCCESS;
    }
    GLsizei levels = info->mipCount > 1 ? (GLsizei)info->mipCount : 1;
    for (int i = 0; i < 8 && p_glGetError() != GL_NO_ERROR; ++i) {}
    GLint previous = 0;
    p_glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &previous);
    GLuint tex = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    p_glTexStorage2D(GL_TEXTURE_CUBE_MAP, levels, (GLenum)info->format, (GLsizei)info->width, (GLsizei)info->height);
    GLenum err = p_glGetError();
    p_glBindTexture(GL_TEXTURE_CUBE_MAP, (GLuint)previous);  // the app's binding, untouched
    if (err != GL_NO_ERROR) {
        p_glDeleteTextures(1, &tex);
        pthread_mutex_lock(&standin_lock);
        standins[slot].handle = XR_NULL_HANDLE;
        pthread_mutex_unlock(&standin_lock);
        LOG("cube_standin: couldn't make a %ux%u cube map (GL 0x%x), the runtime's error stays", info->width,
            info->height, err);
        return result;
    }
    XrSwapchain handle = FAKE_HANDLE(XrSwapchain);
    pthread_mutex_lock(&standin_lock);
    standins[slot].handle = handle;
    standins[slot].tex = tex;
    pthread_mutex_unlock(&standin_lock);
    *out = handle;
    LOG("cube_standin: runtime refused a %ux%u cube swapchain (result=%d): served by FrameBridge (GL cube map %u, "
        "%d levels); its cube layers are dropped", info->width, info->height, result, tex, (int)levels);
    return XR_SUCCESS;
}

static XrResult standin_enumerate(XrSwapchain handle, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (!capacity) return XR_SUCCESS;
    if (!images) return XR_ERROR_VALIDATION_FAILURE;
    int slot = standin_find(handle);
    if (slot < 0) return XR_ERROR_HANDLE_INVALID;
    if (standins[slot].image) {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        ((XrSwapchainImageVulkanKHR *)images)[0].image = standins[slot].image;
    } else {
        if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) return XR_ERROR_VALIDATION_FAILURE;
        ((emul_gles_image *)images)[0].image = standins[slot].tex;
    }
    return XR_SUCCESS;
}

static void standin_destroy(XrSwapchain handle) {
    pthread_mutex_lock(&standin_lock);
    for (int i = 0; i < STANDIN_MAX; ++i)
        if (standins[i].handle == handle) {
            if (standins[i].image) {
                vk.DestroyImage(standins[i].device, standins[i].image, NULL);
                vk.FreeMemory(standins[i].device, standins[i].memory, NULL);
            } else if (p_eglGetCurrentContext && p_eglGetCurrentContext() != EGL_NO_CONTEXT) p_glDeleteTextures(1, &standins[i].tex);
            memset(&standins[i], 0, sizeof(standins[i]));
        }
    pthread_mutex_unlock(&standin_lock);
}

// A layer that shows a stand-in: never passed to the runtime (whatever layer_fix says).
static int layer_uses_standin(const XrCompositionLayerBaseHeader *layer) {
    if (!cube_standin || !layer) return 0;
    switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        return is_standin(((const XrCompositionLayerCubeKHR *)layer)->swapchain);
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
        return is_standin(((const XrCompositionLayerQuad *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        return is_standin(((const XrCompositionLayerCylinderKHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        return is_standin(((const XrCompositionLayerEquirectKHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        return is_standin(((const XrCompositionLayerEquirect2KHR *)layer)->subImage.swapchain);
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)layer;
        for (uint32_t v = 0; v < p->viewCount; ++v)
            if (is_standin(p->views[v].subImage.swapchain)) return 1;
        return 0;
    }
    default:
        return 0;
    }
}

// XR_FB_swapchain_update_state on a stand-in: nothing to update (only hooked when the runtime has the function).
static PFN_xrUpdateSwapchainFB real_update_swapchain;
static PFN_xrGetSwapchainStateFB real_get_swapchain_state;
static XRAPI_ATTR XrResult XRAPI_CALL standin_update_swapchain(XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB *state) {
    if (is_standin(swapchain)) return XR_SUCCESS;
    return real_update_swapchain ? real_update_swapchain(swapchain, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XRAPI_ATTR XrResult XRAPI_CALL standin_get_swapchain_state(XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB *state) {
    if (is_standin(swapchain)) return XR_ERROR_VALIDATION_FAILURE;  // no state to report (apps treat it as "unsupported")
    return real_get_swapchain_state ? real_get_swapchain_state(swapchain, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
