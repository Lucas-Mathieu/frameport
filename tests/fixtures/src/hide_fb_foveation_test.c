#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openxr/openxr.h>
// Exact platform image layout; no Vulkan driver or host Vulkan SDK is needed.
typedef struct { XrStructureType type; void *next; uint64_t image; } XrSwapchainImageVulkanKHR;
#define LOG(...) ((void)0)
#include "hide_fb_foveation.h"
#include "hide_foveation_images.h"
static int fail_images;
static XrResult XRAPI_CALL enumerate(const char *layer, uint32_t capacity, uint32_t *count, XrExtensionProperties *out) {
    (void)layer;
    const char *names[] = {"XR_KHR_vulkan_enable2", "XR_FB_foveation", "XR_FB_foveation_vulkan",
        "XR_FB_foveation_configuration", "XR_META_foveation_eye_tracked", "XR_EXT_debug_utils"};
    *count = 6;
    if (capacity && capacity < 6) return XR_ERROR_SIZE_INSUFFICIENT;
    if (capacity) for (unsigned i=0;i<6;++i) {
        assert(out[i].type == XR_TYPE_EXTENSION_PROPERTIES);
        strcpy(out[i].extensionName, names[i]);
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL create(const XrInstanceCreateInfo *info, XrInstance *instance) {
    (void)instance;
    assert(info->enabledExtensionCount == 1);
    assert(!fb_is_foveation_extension(info->enabledExtensionNames[0]));
    return XR_ERROR_RUNTIME_FAILURE;
}
static XrResult XRAPI_CALL images(XrSwapchain swapchain, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *base) {
    (void)swapchain;
    XrSwapchainImageVulkanKHR *out = (void *)base;
    for (uint32_t i=0;i<capacity;++i) {
        XrBaseOutStructure *p=out[i].next;
        assert(p && p->type == (XrStructureType)123 && !p->next);
        out[i].image=1234+i;
    }
    *count=capacity;
    return fail_images ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
int main(void) {
    uint32_t count=0;
    assert(fb_hide_enumerate(enumerate,NULL,0,&count,NULL)==XR_SUCCESS && count==2);
    XrExtensionProperties out[2]={{XR_TYPE_EXTENSION_PROPERTIES},{XR_TYPE_EXTENSION_PROPERTIES}};
    assert(fb_hide_enumerate(enumerate,NULL,1,&count,out)==XR_ERROR_SIZE_INSUFFICIENT && count==2);
    assert(fb_hide_enumerate(enumerate,"layer",2,&count,out)==XR_SUCCESS);
    assert(!strcmp(out[1].extensionName,"XR_EXT_debug_utils"));
    assert(fb_hide_enumerate(enumerate,NULL,1,&count,NULL)==XR_ERROR_VALIDATION_FAILURE);
    const char *names[]={"XR_FB_foveation","XR_KHR_vulkan_enable2","XR_FB_foveation_vulkan"};
    XrInstanceCreateInfo ci={XR_TYPE_INSTANCE_CREATE_INFO}; ci.enabledExtensionNames=names; ci.enabledExtensionCount=3;
    assert(fb_hide_create_instance(create,&ci,NULL)==XR_ERROR_RUNTIME_FAILURE);
    assert(ci.enabledExtensionCount==3 && ci.enabledExtensionNames==names);
    const char *gles[]={"XR_KHR_opengl_es_enable"}; ci.enabledExtensionNames=gles; ci.enabledExtensionCount=1;
    assert(!fb_instance_uses_vulkan(&ci));
    assert(fb_hide_create_instance(create,&ci,NULL)==XR_ERROR_RUNTIME_FAILURE);
    XrBaseOutStructure fov2={XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB,NULL};
    XrBaseOutStructure unknown={(XrStructureType)123,&fov2};
    XrBaseOutStructure fov1={XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB,&unknown};
    XrSwapchainImageVulkanKHR image={XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR,&fov1,0};
    for (fail_images=0;fail_images<2;++fail_images) {
        XrResult r=fb_hide_foveation_images(images,XR_NULL_HANDLE,1,&count,(void *)&image);
        assert(r==(fail_images ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS));
        assert(image.next==&fov1 && fov1.next==&unknown && unknown.next==&fov2);
        assert(image.image==1234);
    }
    return 0;
}
