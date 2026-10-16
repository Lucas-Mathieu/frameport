#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <openxr/openxr.h>
#include "foveation_storage.h"
int main(void) {
    FbStorageNode nodes[16];
    XrBaseInStructure tail = {(XrStructureType)123456, NULL};
    FbVulkanSwapchainCreateInfo meta = {XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, &tail, 0xc000, 0x123};
    XrSwapchainCreateInfoFoveationFB fov = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB, &meta, 1};
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO, &fov};
    assert(fb_foveation_storage(&ci, nodes) == 1);
    const XrSwapchainCreateInfoFoveationFB *f = ci.next;
    const FbVulkanSwapchainCreateInfo *m = f->next;
    assert(f != &fov && f->flags == 1 && m->additionalCreateFlags == 0x8000);
    assert(m->additionalUsageFlags == 0x123 && m->next == &tail);
    assert(meta.additionalCreateFlags == 0xc000 && fov.next == &meta);
    // With foveation_fix too (device.foveation, GitHub #303): the leading foveation copies are skipped afterwards
    // and the runtime still gets the META node without SUBSAMPLED.
    ci.next = &fov;
    assert(fb_foveation_storage(&ci, nodes) == 1);
    const XrBaseInStructure *p = ci.next;
    while (p && p->type == XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB) p = p->next;
    m = (const FbVulkanSwapchainCreateInfo *)p;
    assert(m && m->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META && m != &meta);
    assert(m->additionalCreateFlags == 0x8000 && m->next == &tail);
    ci.next = &meta;
    assert(fb_foveation_storage(&ci, nodes) == 1);
    meta.additionalCreateFlags = 0x8000; ci.next = &meta;
    assert(fb_foveation_storage(&ci, nodes) == 0 && ci.next == &meta);
    tail.next = (const XrBaseInStructure *)&meta; ci.next = &tail;
    assert(fb_foveation_storage(&ci, nodes) == -1 && ci.next == &tail);
    ci.next = NULL; assert(fb_foveation_storage(&ci, nodes) == 0);
    // An invalid cyclic chain is bounded and left untouched.
    fov.next = &fov; ci.next = &fov;
    assert(fb_foveation_storage(&ci, nodes) == -1 && ci.next == &fov);
    return 0;
}
