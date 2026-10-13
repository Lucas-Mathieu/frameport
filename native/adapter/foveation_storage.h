// SPDX-License-Identifier: GPL-3.0-only
// Copy the known prefix instead of writing into application-owned const chains.
// The META node must reach the runtime without SUBSAMPLED before shared allocation.
typedef struct FbVulkanSwapchainCreateInfo {
    XrStructureType type;
    const void *next;
    uint32_t additionalCreateFlags;
    uint32_t additionalUsageFlags;
} FbVulkanSwapchainCreateInfo;

typedef union FbStorageNode {
    XrBaseInStructure base;
    FbVulkanSwapchainCreateInfo vk;
    XrSwapchainCreateInfoFoveationFB foveation;
} FbStorageNode;

// Returns 1 when changed, 0 when unnecessary, -1 for an unsupported prefix.
static int fb_foveation_storage(XrSwapchainCreateInfo *info, FbStorageNode nodes[16]) {
    const XrBaseInStructure *p = info->next;
    unsigned n = 0;
    while (p && n < 16) {
        if (p->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META) {
            const FbVulkanSwapchainCreateInfo *vk = (const void *)p;
            if (!(vk->additionalCreateFlags & 0x4000u)) return 0;
            nodes[n].vk = *vk;
            nodes[n].vk.additionalCreateFlags &= ~0x4000u; // VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT
            for (unsigned i = 0; i < n; ++i) nodes[i].base.next = &nodes[i + 1].base;
            info->next = &nodes[0];
            return 1;
        }
        if (p->type != XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) return -1;
        nodes[n++].foveation = *(const XrSwapchainCreateInfoFoveationFB *)p;
        p = p->next;
    }
    return p ? -1 : 0;
}
