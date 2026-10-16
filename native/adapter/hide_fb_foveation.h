// SPDX-License-Identifier: GPL-3.0-only
// Shared by the adapter and its shim above OVRPort's synthetic extension list.
static int fb_is_foveation_extension(const char *name) {
    return !strcmp(name, "XR_FB_foveation_vulkan") || !strcmp(name, "XR_FB_foveation") ||
        !strcmp(name, "XR_FB_foveation_configuration") || !strcmp(name, "XR_META_foveation_eye_tracked");
}

static int fb_instance_uses_vulkan(const XrInstanceCreateInfo *info) {
    for (uint32_t i = 0; info && i < info->enabledExtensionCount; ++i)
        if (!strcmp(info->enabledExtensionNames[i], "XR_KHR_vulkan_enable") ||
            !strcmp(info->enabledExtensionNames[i], "XR_KHR_vulkan_enable2")) return 1;
    return 0;
}

static inline XrResult fb_hide_enumerate(PFN_xrEnumerateInstanceExtensionProperties fn, const char *layer,
        uint32_t capacity, uint32_t *count, XrExtensionProperties *properties) {
    if (!count || (capacity && !properties)) return XR_ERROR_VALIDATION_FAILURE;
    uint32_t total = 0;
    XrResult r = fn(layer, 0, &total, NULL);
    if (XR_FAILED(r)) return r;
    XrExtensionProperties *all = calloc(total ? total : 1, sizeof(*all));
    if (!all) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < total; ++i) all[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    r = fn(layer, total, &total, all);
    if (XR_SUCCEEDED(r)) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < total; ++i) {
            if (fb_is_foveation_extension(all[i].extensionName)) continue;
            if (kept < capacity) properties[kept] = all[i];
            ++kept;
        }
        *count = kept;
        if (capacity && capacity < kept) r = XR_ERROR_SIZE_INSUFFICIENT;
    }
    free(all);
    return r;
}

static inline XrResult fb_hide_create_instance(PFN_xrCreateInstance fn, const XrInstanceCreateInfo *info,
        XrInstance *instance) {
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!fb_instance_uses_vulkan(info)) return fn(info, instance);
    const char **names = calloc(info->enabledExtensionCount ? info->enabledExtensionCount : 1, sizeof(*names));
    if (!names) return XR_ERROR_OUT_OF_MEMORY;
    XrInstanceCreateInfo fixed = *info;
    fixed.enabledExtensionCount = 0;
    fixed.enabledExtensionNames = names;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (!fb_is_foveation_extension(info->enabledExtensionNames[i]))
            names[fixed.enabledExtensionCount++] = info->enabledExtensionNames[i];
    XrResult r = fn(&fixed, instance);
    free(names);
    return r;
}
