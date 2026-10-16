// SPDX-License-Identifier: GPL-3.0-only
// Output chains are writable. Unlink FB nodes during enumeration and restore every
// app-owned link on both success and failure; preserve unrelated output structures.
static XrResult fb_hide_foveation_images(PFN_xrEnumerateSwapchainImages fn, XrSwapchain swapchain,
        uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    if (!images || !capacity || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
        return fn(swapchain, capacity, count, images);
    XrSwapchainImageVulkanKHR *vk_images = (void *)images;
    size_t total = 0;
    for (uint32_t i = 0; i < capacity; ++i)
        for (XrBaseOutStructure *p = vk_images[i].next; p; p = p->next)
            total += p->type == XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB;
    struct FbImageLink { XrBaseOutStructure *parent, *node; };
    struct FbImageLink *links = calloc(total ? total : 1, sizeof(*links));
    if (!links) return XR_ERROR_OUT_OF_MEMORY;
    size_t n = 0;
    for (uint32_t i = 0; i < capacity; ++i) {
        XrBaseOutStructure *parent = (void *)&vk_images[i];
        while (parent->next) {
            XrBaseOutStructure *p = parent->next;
            if (p->type == XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB) {
                links[n++] = (struct FbImageLink){parent, p};
                parent->next = p->next;
            } else parent = p;
        }
    }
    XrResult result = fn(swapchain, capacity, count, images);
    while (n) { --n; links[n].parent->next = links[n].node; }
    free(links);
    if (total) LOG("hide_fb_foveation: removed %zu Vulkan foveation image node(s)", total);
    return result;
}
