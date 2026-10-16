// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c. Emulates XR_FB_composition_layer_image_layout's VERTICAL_FLIP for quad
// layers of Vulkan apps when the runtime lacks the extension (Frame): the quad's rectangle is turned upside down in
// the app's own image (through a scratch image of ours), once per acquired image. An extra runtime swapchain per
// flipped quad (the first version) ran into the Frame's swapchain limit: AC Nexus has more UI panels than it allows,
// so some stayed upside down and the game's own later panels failed with XR_ERROR_LIMIT_REACHED (GitHub #138).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr_platform.h>

#define XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB_VALUE 1000040000
#define MAX_TRACKED_SWAPCHAINS 128
#define MAX_IMAGES 8

static int flip_emul = 1;             // setting


static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family, index;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    int state;  // 0 = not initialized, 1 = ready, -1 = failed
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdClearColorImage CmdClearColorImage;  // fade_quad.c
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateBuffer CreateBuffer;  // flip_dump
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    VkPhysicalDeviceMemoryProperties memory;
    // scratch image for in-place flips: grown to the largest rectangle, one format at a time
    VkImage scratch;
    VkDeviceMemory scratch_memory;
    VkFormat scratch_format;
    uint32_t scratch_w, scratch_h;
} vk;

typedef struct {
    XrSwapchain handle;
    XrSwapchainCreateInfo info;
    VkImage images[MAX_IMAGES];
    uint32_t image_count;
    uint32_t last_acquired;
    uint32_t acquired[MAX_IMAGES], acquired_count, waited_count;
    // rectangles already turned upside down in each image since the app last acquired it (static quads are
    // submitted again without a new acquire: flipping them again would undo it)
    struct { XrRect2Di rect; uint32_t layer; } flipped[MAX_IMAGES][4];
    uint32_t flipped_count[MAX_IMAGES];
    uint32_t alpha_inverted[MAX_IMAGES];  // alpha_blend_vk.c: array layers whose alpha was inverted since the acquire
    int dumped;  // flip_dump saved this swapchain's quad
    int logged;  // its first in-place flip was logged
} tracked_swapchain;

static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static tracked_swapchain tracked[MAX_TRACKED_SWAPCHAINS];

static tracked_swapchain *find_tracked(XrSwapchain handle, int create) {
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; ++i)
        if (tracked[i].handle == handle) return &tracked[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_TRACKED_SWAPCHAINS; ++i)
        if (tracked[i].handle == XR_NULL_HANDLE) {
            memset(&tracked[i], 0, sizeof(tracked[i]));
            tracked[i].handle = handle;
            return &tracked[i];
        }
    return NULL;
}

static void flip_on_create_session(const XrSessionCreateInfo *info) {
    for (const XrBaseInStructure *p = info ? (const XrBaseInStructure *)info->next : NULL; p; p = p->next)
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            const XrGraphicsBindingVulkanKHR *b = (const XrGraphicsBindingVulkanKHR *)p;
            vk.instance = b->instance;
            vk.physical = b->physicalDevice;
            vk.device = b->device;
            vk.family = b->queueFamilyIndex;
            vk.index = b->queueIndex;
            vk.state = 0;
            vk.scratch = VK_NULL_HANDLE;  // belonged to the previous session's device
            vk.scratch_memory = VK_NULL_HANDLE;
            vk.scratch_w = vk.scratch_h = 0;
            LOG("flip: Vulkan session (queue family %u index %u)", vk.family, vk.index);
        }
}

static void flip_on_create_swapchain(XrSwapchain handle, const XrSwapchainCreateInfo *info) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 1);
    if (t) { t->info = *info; t->info.next = NULL; }
    pthread_mutex_unlock(&flip_lock);
}

static void flip_on_enumerate_images(XrSwapchain handle, uint32_t count, const XrSwapchainImageBaseHeader *images) {
    if (!images || !count || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return;
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) {
        const XrSwapchainImageVulkanKHR *v = (const XrSwapchainImageVulkanKHR *)images;
        t->image_count = count < MAX_IMAGES ? count : MAX_IMAGES;
        for (uint32_t i = 0; i < t->image_count; ++i) t->images[i] = v[i].image;
    }
    pthread_mutex_unlock(&flip_lock);
}

static void flip_on_acquire(XrSwapchain handle, uint32_t index) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) {
        t->last_acquired = index;
        if (index < MAX_IMAGES) t->flipped_count[index] = 0;  // new content: upside down again
        if (index < MAX_IMAGES) t->alpha_inverted[index] = 0;
        if(t->acquired_count<MAX_IMAGES)t->acquired[t->acquired_count++]=index;
    }
    pthread_mutex_unlock(&flip_lock);
}

static void flip_on_destroy(XrSwapchain handle) {
    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(handle, 0);
    if (t) memset(t, 0, sizeof(*t));
    pthread_mutex_unlock(&flip_lock);
}

// xrCreateSwapchain: single-image colour swapchains of a Vulkan session (quad candidates) also get transfer usage,
// so the in-place flip may blit from and back into them.
static int flip_wants_transfer(const XrSwapchainCreateInfo *info) {
    return flip_emul && vk.device && !runtime_has_image_layout && info->arraySize == 1 && info->faceCount == 1 &&
           info->sampleCount == 1 && (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
}

static int vk_ready(void) {
    if (vk.state) return vk.state > 0;
    vk.state = -1;
    if (!vk.device) return 0;
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) return 0;
    vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(vk.instance, "vkGetDeviceProcAddr");
    if (!vk.GetDeviceProcAddr) return 0;
#define DEV(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!vk.name) return 0;
    DEV(GetDeviceQueue) DEV(CreateCommandPool) DEV(AllocateCommandBuffers) DEV(BeginCommandBuffer)
    DEV(EndCommandBuffer) DEV(ResetCommandBuffer) DEV(CmdPipelineBarrier) DEV(CmdBlitImage) DEV(CmdClearColorImage) DEV(QueueSubmit)
    DEV(CreateFence) DEV(WaitForFences) DEV(ResetFences) DEV(CreateImage) DEV(DestroyImage)
    DEV(GetImageMemoryRequirements) DEV(AllocateMemory) DEV(FreeMemory) DEV(BindImageMemory)
    DEV(CreateBuffer) DEV(DestroyBuffer) DEV(GetBufferMemoryRequirements) DEV(BindBufferMemory) DEV(MapMemory)
    DEV(UnmapMemory) DEV(CmdCopyImageToBuffer)
#undef DEV
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(vk.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!memory_properties) return 0;
    memory_properties(vk.physical, &vk.memory);
    vk.GetDeviceQueue(vk.device, vk.family, vk.index, &vk.queue);
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk.family};
    if (vk.CreateCommandPool(vk.device, &pool, NULL, &vk.pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk.pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.AllocateCommandBuffers(vk.device, &alloc, &vk.cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    if (vk.CreateFence(vk.device, &fence, NULL, &vk.fence) != VK_SUCCESS) return 0;
    vk.state = 1;
    LOG("flip: Vulkan blitter ready");
    return 1;
}

static void barrier(VkImage image, uint32_t layer, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src_access, dst_access, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1}};
    vk.CmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL,
                          0, NULL, 1, &b);
}

static int wants_vertical_flip(const XrCompositionLayerQuad *q) {
    for (const XrBaseInStructure *n = (const XrBaseInStructure *)q->next; n; n = n->next)
        if (n->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB_VALUE)
            return (((const XrCompositionLayerImageLayoutFB *)n)->flags & XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB) != 0;
    return 0;
}

// A scratch image at least w x h in this format (device-local, transfer source + destination).
static int scratch_fit(VkFormat format, uint32_t w, uint32_t h) {
    if (vk.scratch && vk.scratch_format == format && vk.scratch_w >= w && vk.scratch_h >= h) return 1;
    if (vk.scratch) {
        vk.DestroyImage(vk.device, vk.scratch, NULL);
        vk.FreeMemory(vk.device, vk.scratch_memory, NULL);
        vk.scratch = VK_NULL_HANDLE;
        vk.scratch_memory = VK_NULL_HANDLE;
    }
    if (vk.scratch_format == format) {  // grow, don't shrink: panels of one game come in several sizes
        if (vk.scratch_w > w) w = vk.scratch_w;
        if (vk.scratch_h > h) h = vk.scratch_h;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.CreateImage(vk.device, &ci, NULL, &vk.scratch) != VK_SUCCESS) { vk.scratch = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(vk.device, vk.scratch, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (vk.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if (req.memoryTypeBits & (1u << i)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || vk.AllocateMemory(vk.device, &ai, NULL, &vk.scratch_memory) != VK_SUCCESS ||
        vk.BindImageMemory(vk.device, vk.scratch, vk.scratch_memory, 0) != VK_SUCCESS) {
        if (vk.scratch_memory) vk.FreeMemory(vk.device, vk.scratch_memory, NULL);
        vk.DestroyImage(vk.device, vk.scratch, NULL);
        vk.scratch = VK_NULL_HANDLE;
        vk.scratch_memory = VK_NULL_HANDLE;
        return 0;
    }
    vk.scratch_format = format;
    vk.scratch_w = w;
    vk.scratch_h = h;
    LOG("flip: scratch image %ux%u format %d", w, h, (int)format);
    return 1;
}

// flip_dump=N (diagnostics): the quad's rectangle as the app drew it ("raw") and as submitted after the flip
// ("upright"), saved as files/fb_flip_<n>_<raw|upright>.ppm (8-bit RGBA formats only).
static int dump_rect(VkImage app, uint32_t layer, XrRect2Di rect, int64_t format, int n, const char *what) {
    int written = 0;
    if (!*snap_dir || (format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM &&
                       format != VK_FORMAT_B8G8R8A8_SRGB && format != VK_FORMAT_B8G8R8A8_UNORM)) return 0;
    uint32_t w = (uint32_t)rect.extent.width, h = (uint32_t)rect.extent.height;
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vk.CreateBuffer(vk.device, &bi, NULL, &buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vk.GetBufferMemoryRequirements(vk.device, buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || vk.AllocateMemory(vk.device, &ai, NULL, &mem) != VK_SUCCESS ||
        vk.BindBufferMemory(vk.device, buf, mem, 0) != VK_SUCCESS) goto out;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vk.ResetCommandBuffer(vk.cmd, 0);
    vk.BeginCommandBuffer(vk.cmd, &begin);
    barrier(app, layer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                                {rect.offset.x, rect.offset.y, 0}, {w, h, 1}};
    vk.CmdCopyImageToBuffer(vk.cmd, app, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    barrier(app, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vk.EndCommandBuffer(vk.cmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &vk.cmd, 0, NULL};
    int ok = vk.QueueSubmit(vk.queue, 1, &submit, vk.fence) == VK_SUCCESS &&
             vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 1000000000ull) == VK_SUCCESS;
    vk.ResetFences(vk.device, 1, &vk.fence);
    void *mapped = NULL;
    if (!ok || vk.MapMemory(vk.device, mem, 0, size, 0, &mapped) != VK_SUCCESS) goto out;
    const unsigned char *bytes = mapped;
    VkDeviceSize nonzero = 0;
    for (VkDeviceSize i = 0; i < size && !nonzero; ++i) nonzero = bytes[i];
    char path[320];
    snprintf(path, sizeof(path), "%s/fb_flip_%d_%s.ppm", snap_dir, n, what);
    FILE *f = nonzero ? fopen(path, "wb") : NULL;  // (an empty panel, e.g. before its first content: not counted)
    if (f) {
        int bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const unsigned char *px = mapped;
        for (VkDeviceSize i = 0; i < (VkDeviceSize)w * h; ++i) {
            unsigned char rgb[3] = {px[i * 4 + (bgra ? 2 : 0)], px[i * 4 + 1], px[i * 4 + (bgra ? 0 : 2)]};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        written = 1;
        LOG("flip_dump: wrote %s (%ux%u)", path, w, h);
    }
    vk.UnmapMemory(vk.device, mem);
out:
    if (mem) vk.FreeMemory(vk.device, mem, NULL);
    vk.DestroyBuffer(vk.device, buf, NULL);
    return written;
}

// Turns the quad's rectangle upside down in the app's own image, once per acquire of that image. Always returns
// NULL: the original quad (same swapchain, same rectangle) is submitted and now shows upright.
static const XrCompositionLayerQuad *flip_quad(XrSession session, const XrCompositionLayerQuad *q,
                                               XrCompositionLayerQuad *out) {
    (void)session; (void)out;
    if (!flip_emul || runtime_has_image_layout || !wants_vertical_flip(q)) return NULL;
    if (!vk_ready()) { static int warned; if (!warned++) LOG("flip: needed but Vulkan blitter unavailable (GLES app?)"); return NULL; }

    pthread_mutex_lock(&flip_lock);
    tracked_swapchain *t = find_tracked(q->subImage.swapchain, 0);
    const XrRect2Di rect = q->subImage.imageRect;
    uint32_t layer = q->subImage.imageArrayIndex, index = t ? t->last_acquired : 0;
    if (!t || !t->image_count || index >= t->image_count || rect.extent.width <= 0 || rect.extent.height <= 0) {
        static int skipped;
        if (skipped++ < 10)
            LOG("flip: skipped quad swapchain=%p tracked=%d images=%u last=%u rect=%dx%d", (void *)(uintptr_t)q->subImage.swapchain,
                t != NULL, t ? t->image_count : 0, index, rect.extent.width, rect.extent.height);
        goto done;
    }
    for (uint32_t i = 0; i < t->flipped_count[index]; ++i)
        if (t->flipped[index][i].layer == layer && !memcmp(&t->flipped[index][i].rect, &rect, sizeof rect)) goto done;
    if (t->flipped_count[index] >= 4) goto done;  // never seen: more than 4 quads on one image
    if (!(t->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {  // created before the Vulkan session was known
        static int warned;
        if (warned++ < 5) LOG("flip: quad swapchain=%p has no transfer usage, left as it is", (void *)(uintptr_t)q->subImage.swapchain);
        goto done;
    }
    uint32_t w = (uint32_t)rect.extent.width, h = (uint32_t)rect.extent.height;
    if (!scratch_fit((VkFormat)t->info.format, w, h)) {
        static int failed;
        if (failed++ < 5) LOG("flip: no scratch image for %ux%u format %lld", w, h, (long long)t->info.format);
        goto done;
    }
    VkImage app = t->images[index];
    static int dumps;  // one non-empty image per quad swapchain, at most flip_dump of them
    int dump = 0;
    if (dumps < flip_dump && !t->dumped && dump_rect(app, layer, rect, t->info.format, dumps + 1, "raw")) {
        dump = ++dumps;
        t->dumped = 1;
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vk.ResetCommandBuffer(vk.cmd, 0);
    vk.BeginCommandBuffer(vk.cmd, &begin);
    barrier(app, layer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(vk.scratch, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit down = {  // the rectangle into the scratch image, rows reversed
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
        {{rect.offset.x, rect.offset.y, 0}, {rect.offset.x + rect.extent.width, rect.offset.y + rect.extent.height, 1}},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {{0, (int32_t)h, 0}, {(int32_t)w, 0, 1}},
    };
    vk.CmdBlitImage(vk.cmd, app, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &down, VK_FILTER_NEAREST);
    barrier(vk.scratch, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(app, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit back = {  // and back into the app's image as it is
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
        {{rect.offset.x, rect.offset.y, 0}, {rect.offset.x + rect.extent.width, rect.offset.y + rect.extent.height, 1}},
    };
    vk.CmdBlitImage(vk.cmd, vk.scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, app, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &back, VK_FILTER_NEAREST);
    barrier(app, layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vk.EndCommandBuffer(vk.cmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &vk.cmd, 0, NULL};
    int ok = vk.QueueSubmit(vk.queue, 1, &submit, vk.fence) == VK_SUCCESS &&
             vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 100000000ull) == VK_SUCCESS;
    vk.ResetFences(vk.device, 1, &vk.fence);
    if (!ok) goto done;
    if (dump) dump_rect(app, layer, rect, t->info.format, dump, "upright");
    t->flipped[index][t->flipped_count[index]].rect = rect;
    t->flipped[index][t->flipped_count[index]++].layer = layer;
    if (!t->logged++)  // once per quad swapchain
        LOG("flip: turning quad swapchain=%p (%ux%u) upright in place", (void *)(uintptr_t)q->subImage.swapchain, w, h);
done:
    pthread_mutex_unlock(&flip_lock);
    return NULL;
}
