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

// Strip FB nodes behind known Vulkan allocation nodes without changing const app memory.
// An unknown prefix may be retained only when its entire tail is free of FB create info.
static int fb_hide_swapchain_foveation(XrSwapchainCreateInfo *info, FbStorageNode nodes[16]) {
    const XrBaseInStructure *p = info->next;
    unsigned n = 0;
    int removed = 0;
    unsigned visited = 0;
    while (p) {
        if (++visited > 256) return -1;
        if (p->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) {
            removed = 1;
            p = p->next;
            continue;
        }
        if (p->type != XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META || n == 16) {
            for (const XrBaseInStructure *tail = p; tail; tail = tail->next) {
                if (++visited > 256 || tail->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) return -1;
            }
            break;
        }
        nodes[n].vk = *(const FbVulkanSwapchainCreateInfo *)p;
        // Foveation-only subsampled allocations are invalid without FB foveation.
        nodes[n].vk.additionalCreateFlags &= ~0x4000u;
        if (n) nodes[n - 1].base.next = &nodes[n].base;
        ++n;
        p = p->next;
    }
    if (n) nodes[n - 1].base.next = p;
    info->next = n ? &nodes[0] : (const void *)p;
    return removed;
}
