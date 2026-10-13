// SPDX-License-Identifier: GPL-3.0-only
// Optional actual-fragment-density visualization. Loaded only by test-mode APKs.
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
extern int __android_log_print(int,const char*,const char*,...);
#define ANDROID_LOG_INFO 4
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "FoveationDarken"
#define EXPORT __attribute__((visibility("default")))
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LAYER_NAME "VK_LAYER_FRAMEPORT_fdm_darken"


#include "fdm_darken.h"
static int lock,nfixes,dump;
static void take_lock(int*p){while(__atomic_exchange_n(p,1,__ATOMIC_ACQUIRE)){}}
static void drop_lock(int*p){__atomic_store_n(p,0,__ATOMIC_RELEASE);}
#define MAX_HANDLES 16
static struct { void *key; VkInstance instance; PFN_vkGetInstanceProcAddr gipa; } instances[MAX_HANDLES];
static struct { void *key; VkDevice device; PFN_vkGetDeviceProcAddr gdpa; PFN_vkCreateShaderModule create_shader; } devices[MAX_HANDLES];

static void *key_of(const void *handle) { return handle ? *(void *const *)handle : NULL; }

static PFN_vkGetInstanceProcAddr instance_gipa(const void *handle, VkInstance *instance) {
    void *key = key_of(handle);
    PFN_vkGetInstanceProcAddr found = NULL, last = NULL;
    take_lock(&lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!instances[i].gipa) continue;
        last = instances[i].gipa;
        if (instance) *instance = instances[i].instance;
        if (instances[i].key == key) { found = instances[i].gipa; break; }
    }
    drop_lock(&lock);
    return found ? found : last;  // a physical device: its instance (or the only one there is)
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkInstance *instance) {
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, a, instance);
    if (r != VK_SUCCESS) return r;
    take_lock(&lock);
    int slot = 0;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!instances[i].gipa || instances[i].key == key_of(*instance)) { slot = i; break; }
    instances[slot].key = key_of(*instance);
    instances[slot].instance = *instance;
    instances[slot].gipa = next;
    drop_lock(&lock);
    static int logged;
    if (logged++ < 4) LOG("foveation diagnostic: active on instance %p (%d fix(es))", (void *)*instance, nfixes);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                         const VkAllocationCallbacks *a, VkDevice *device) {
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkInstance instance = VK_NULL_HANDLE;
    instance_gipa(pd, &instance);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)next_gipa(instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    for(const VkBaseInStructure*b=ci->pNext;b;b=b->pNext)if(b->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT){const VkPhysicalDeviceFragmentDensityMapFeaturesEXT*q=(void*)b;LOG("REQUEST FDM=%u dynamic=%u nonsubsampled=%u",q->fragmentDensityMap,q->fragmentDensityMapDynamic,q->fragmentDensityMapNonSubsampledImages);}
    PFN_vkGetPhysicalDeviceFeatures2 gf=(void*)next_gipa(instance,"vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fd={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 fs={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&fd};if(gf){gf(pd,&fs);LOG("SUPPORT FDM=%u dynamic=%u nonsubsampled=%u",fd.fragmentDensityMap,fd.fragmentDensityMapDynamic,fd.fragmentDensityMapNonSubsampledImages);}
    VkResult r = create(pd, ci, a, device);
    if (r != VK_SUCCESS) return r;
    take_lock(&lock);
    int slot = 0;
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!devices[i].gdpa || devices[i].key == key_of(*device)) { slot = i; break; }
    devices[slot].key = key_of(*device);
    devices[slot].device=*device;
    devices[slot].gdpa = next_gdpa;
    devices[slot].create_shader = (PFN_vkCreateShaderModule)next_gdpa(*device, "vkCreateShaderModule");
    drop_lock(&lock);
    return r;
}

static void *device_entry(VkDevice device, int shader) {
    void *key = key_of(device), *fn = NULL;
    take_lock(&lock);
    for (int i = 0; i < MAX_HANDLES && !fn; i++)
        if (devices[i].gdpa && devices[i].key == key) fn = shader ? (void *)devices[i].create_shader : (void *)devices[i].gdpa;
    drop_lock(&lock);
    return fn;
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                               const VkAllocationCallbacks *a, VkShaderModule *module) {
    PFN_vkCreateShaderModule next = (PFN_vkCreateShaderModule)device_entry(device, 1);
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    size_t words=0;
    uint32_t *code=fdm_darken(ci->pCode,ci->codeSize/4,&words);
    VkShaderModuleCreateInfo patched=*ci;
    if(code){patched.pCode=code;patched.codeSize=words*4;}
    VkResult r=next(device,&patched,a,module);
    static int count;
    if(code && __atomic_fetch_add(&count,1,__ATOMIC_RELAXED)<12)LOG("DARKEN shader %zu -> %zu bytes result=%d",ci->codeSize,words*4,r);
    free(code);return r;
}


static PFN_vkVoidFunction own(const char *name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction fn = own(name);
    if (fn) return fn;
    PFN_vkGetDeviceProcAddr next = (PFN_vkGetDeviceProcAddr)device_entry(device, 0);
    return next ? next(device, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction fn = own(name);
    if (fn) return fn;
    PFN_vkGetInstanceProcAddr next = instance ? instance_gipa(instance, NULL) : NULL;
    return next ? next(instance, name) : NULL;
}

// ---------------------------------------------------------------- layer enumeration (what the loader asks for)

static const VkLayerProperties properties = {LAYER_NAME, VK_MAKE_VERSION(1, 1, 0), 1, "FramePort coarse-fragment darkening"};

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
    if (!props) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    props[0] = properties;
    *count = 1;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                             VkExtensionProperties *props) {
    (void)props;
    if (!layer || strcmp(layer, LAYER_NAME)) return VK_ERROR_LAYER_NOT_PRESENT;
    *count = 0;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice pd, uint32_t *count,
                                                                       VkLayerProperties *props) {
    (void)pd;
    return vkEnumerateInstanceLayerProperties(count, props);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                           uint32_t *count, VkExtensionProperties *props) {
    (void)pd;
    return vkEnumerateInstanceExtensionProperties(layer, count, props);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_FRAMEPORT_fdm_darkenGetInstanceProcAddr(VkInstance i, const char *n) {
    return layer_GetInstanceProcAddr(i, n);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_FRAMEPORT_fdm_darkenGetDeviceProcAddr(VkDevice d, const char *n) {
    return layer_GetDeviceProcAddr(d, n);
}

static PFN_vkVoidFunction own(const char *name) {
    if (!name) return NULL;
#define OWN(fn, impl) if (!strcmp(name, fn)) return (PFN_vkVoidFunction)impl;
    OWN("vkGetInstanceProcAddr", layer_GetInstanceProcAddr)
    OWN("vkGetDeviceProcAddr", layer_GetDeviceProcAddr)
    OWN("vkCreateInstance", layer_CreateInstance)
    OWN("vkCreateDevice", layer_CreateDevice)
    OWN("vkCreateShaderModule", layer_CreateShaderModule)
    OWN("vkEnumerateInstanceLayerProperties", vkEnumerateInstanceLayerProperties)
    OWN("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties)
    OWN("vkEnumerateDeviceLayerProperties", vkEnumerateDeviceLayerProperties)
#undef OWN
    return NULL;
}

// ---------------------------------------------------------------- activation (GraphicsEnv debug layers)

// libc++ std::string as Android 11's platform libc++ lays it out (little endian, short-string flag in bit 0)
struct cxx_string { size_t cap; size_t size; char *data; };

static const char *cxx_chars(const void *s, size_t *len) {
    const unsigned char *b = s;
    if (b[0] & 1) {
        const struct cxx_string *l = s;
        *len = l->size;
        return l->data;
    }
    *len = b[0] >> 1;
    return (const char *)b + 1;
}

#define GET_INSTANCE "_ZN7android11GraphicsEnv11getInstanceEv"
#define GET_LAYERS "_ZN7android11GraphicsEnv14getDebugLayersEv"
#define SET_LAYERS "_ZN7android11GraphicsEnv14setDebugLayersENSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE"

#include "climb_compat.h"

__attribute__((constructor)) static void activate(void) {
    climb_stereo_fix();
    const char *enabled = getenv("FRAMEPORT_FOVEATION_TEST");
    if (!enabled || strcmp(enabled, "1")) return;
    /* This library is added only when device.foveation.test is selected. */
    /* The launcher opt-in also keeps imported test builds dormant during normal play. */
    void *vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    void *(*get_instance)(void) = vk ? (void *(*)(void))dlsym(vk, GET_INSTANCE) : NULL;
    const void *(*get_layers)(void *) = vk ? (const void *(*)(void *))dlsym(vk, GET_LAYERS) : NULL;
    void (*set_layers)(void *, struct cxx_string *) = vk ? (void (*)(void *, struct cxx_string *))dlsym(vk, SET_LAYERS) : NULL;
    if (!get_instance || !get_layers || !set_layers) {
        LOG("foveation diagnostic: NOT active, Android's GraphicsEnv functions not found (%s%s%s)", get_instance ? "" : "getInstance ",
            get_layers ? "" : "getDebugLayers ", set_layers ? "" : "setDebugLayers");
        return;
    }
    void *env = get_instance();
    size_t len = 0;
    const char *current = env ? cxx_chars(get_layers(env), &len) : NULL;
    if (!env || (len && !current) || len > 4096) {
        LOG("foveation diagnostic: NOT active, unexpected GraphicsEnv state");
        return;
    }
    char old[4097];
    memcpy(old, current ? current : "", len);
    old[len] = 0;
    if (strstr(old, LAYER_NAME)) return;  // loaded twice (the loader's own dlopen): already in the list
    size_t n = strlen(LAYER_NAME) + (len ? 1 + len : 0);
    size_t cap = (n + 16) & ~(size_t)15;
    char *data = malloc(cap);
    if (!data) return;
    snprintf(data, cap, "%s%s%s", LAYER_NAME, len ? ":" : "", old);
    // passed by value: the callee gets a pointer to our temporary and copies it; freed below unless it was moved
    struct cxx_string tmp = {cap | 1, n, data};
    set_layers(env, &tmp);
    if ((tmp.cap & 1) && tmp.data == data) free(data);
    const char *now = cxx_chars(get_layers(env), &len);
    LOG("foveation diagnostic: debug layers now \"%.*s\" (%d fix(es)%s)", (int)len, now ? now : "", nfixes,
        dump ? ", dumping modules" : "");
}
