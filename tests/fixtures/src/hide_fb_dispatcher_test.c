#include <assert.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <openxr/openxr.h>
static int enabled, requested;
static int query(void) { return enabled; }
static PFN_xrVoidFunction extension(const char *name) {
    return !strcmp(name,"framebridge_hide_fb_foveation") ? (PFN_xrVoidFunction)query : NULL;
}
static XrResult XRAPI_CALL enumerate(const char *layer,uint32_t capacity,uint32_t *count,XrExtensionProperties *out) {
    (void)layer; *count=2;
    if (capacity && capacity<2) return XR_ERROR_SIZE_INSUFFICIENT;
    if (capacity) {
        strcpy(out[0].extensionName,"XR_FB_foveation");
        strcpy(out[1].extensionName,"XR_KHR_vulkan_enable");
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL create(const XrInstanceCreateInfo *info,XrInstance *instance) {
    (void)instance; requested=info->enabledExtensionCount; return XR_SUCCESS;
}
static XrResult XRAPI_CALL gipa(XrInstance instance,const char *name,PFN_xrVoidFunction *fn) {
    (void)instance;
    if (!strcmp(name,"xrCreateInstance")) *fn=(PFN_xrVoidFunction)create;
    else if (!strcmp(name,"xrEnumerateInstanceExtensionProperties")) *fn=(PFN_xrVoidFunction)enumerate;
    else return XR_ERROR_FUNCTION_UNSUPPORTED;
    return XR_SUCCESS;
}
static void *fake_dlopen(const char *name,int flags) { (void)name;(void)flags;return (void *)(uintptr_t)1; }
static void *fake_dlsym(void *handle,const char *name) {
    (void)handle;
    if (!strcmp(name,"xrGetInstanceProcAddr")) return (void *)gipa;
    if (!strcmp(name,"framebridge_extension_proc")) return (void *)extension;
    return NULL;
}
int __android_log_print(int priority,const char *tag,const char *format,...) {
    (void)priority;(void)tag;(void)format;return 0;
}
#define dlopen fake_dlopen
#define dlsym fake_dlsym
#include "xrshim.c"
int main(void) {
    PFN_xrVoidFunction fn=NULL;
    assert(xrGetInstanceProcAddr(XR_NULL_HANDLE,"xrEnumerateInstanceExtensionProperties",&fn)==XR_SUCCESS);
    PFN_xrEnumerateInstanceExtensionProperties e=(PFN_xrEnumerateInstanceExtensionProperties)fn;
    const char *names[]={"XR_FB_foveation","XR_KHR_vulkan_enable"};
    XrInstanceCreateInfo ci={XR_TYPE_INSTANCE_CREATE_INFO};ci.enabledExtensionNames=names;ci.enabledExtensionCount=2;
    for (enabled=0;enabled<2;++enabled) {
        uint32_t count=0;
        assert(e(NULL,0,&count,NULL)==XR_SUCCESS && count==(enabled ? 1u : 2u));
        XrExtensionProperties out[2]={{XR_TYPE_EXTENSION_PROPERTIES},{XR_TYPE_EXTENSION_PROPERTIES}};
        assert(e(NULL,2,&count,out)==XR_SUCCESS && count==(enabled ? 1u : 2u));
        if (enabled) assert(!strcmp(out[0].extensionName,"XR_KHR_vulkan_enable"));
        assert(xrGetInstanceProcAddr(XR_NULL_HANDLE,"xrCreateInstance",&fn)==XR_SUCCESS);
        assert(((PFN_xrCreateInstance)fn)(&ci,NULL)==XR_SUCCESS);
        assert(requested==(enabled ? 1 : 2) && ci.enabledExtensionCount==2);
    }
    return 0;
}
