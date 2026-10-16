// SPDX-License-Identifier: GPL-3.0-only
// Included by session_fixes.c: Meta's detached controllers (XR_META_detached_controllers: a controller put down while
// hand tracking runs) on a runtime without that extension. Uses profile_remap, runtime_has_detached_controllers,
// active_instance, lookup and LOG.
//
// Meta's Unity OpenXR features add "Detached ... Controller" devices for /user/detached_controller_meta/left|right.
// Unity merges same-named actions of every enabled device into one action, so the first action with such a subaction
// path (e.g. "thumbstick") fails with XR_ERROR_PATH_UNSUPPORTED on the Frame's runtime, Unity attaches no action set
// at all and neither Unity nor OVRPlugin get controller input: hands lying at the play-space origin on the floor, no
// buttons (Spatial Zombies, GitHub #328). With profile_remap on and the runtime lacking the extension, these paths are
// left out of the game's actions and suggested bindings, and xrGetCurrentInteractionProfile answers "no profile"
// for them (a detached controller that never shows up) instead of XR_ERROR_PATH_UNSUPPORTED.

#define DETACHED_PREFIX "/user/detached_controller_meta/"

static int detached_filter_on(void) { return profile_remap && !runtime_has_detached_controllers && active_instance; }

// 1 if `path` is a detached controller's top-level path or one of its input/output paths
static int detached_path(XrPath path) {
    PFN_xrPathToString to_string = (PFN_xrPathToString)lookup(active_instance, "xrPathToString");
    char name[XR_MAX_PATH_LENGTH];
    uint32_t size = 0;
    return path && to_string && XR_SUCCEEDED(to_string(active_instance, path, sizeof(name), &size, name)) &&
           !strncmp(name, DETACHED_PREFIX, strlen(DETACHED_PREFIX));
}

// xrCreateAction: `info` without the detached subaction paths, in *copy (paths in `kept`, room for all of them).
// Returns `info` itself when there is nothing to leave out.
static const XrActionCreateInfo *detached_strip_action(const XrActionCreateInfo *info, XrActionCreateInfo *copy,
                                                       XrPath *kept) {
    if (!info || !info->countSubactionPaths || !info->subactionPaths || !kept || !detached_filter_on()) return info;
    uint32_t n = 0;
    for (uint32_t i = 0; i < info->countSubactionPaths; ++i)
        if (!detached_path(info->subactionPaths[i])) kept[n++] = info->subactionPaths[i];
    if (n == info->countSubactionPaths) return info;
    *copy = *info;
    copy->countSubactionPaths = n;
    copy->subactionPaths = kept;
    static int logged;
    if (logged++ < 3)
        LOG("profile_remap: action %s without %u detached-controller path(s) (the runtime has no "
            "XR_META_detached_controllers)", info->actionName, info->countSubactionPaths - n);
    return copy;
}

// xrSuggestInteractionProfileBindings: `suggested` without bindings of detached controllers, in *copy (bindings in
// `kept`, room for all of them). Returns `suggested` itself when there is nothing to leave out.
static const XrInteractionProfileSuggestedBinding *detached_strip_bindings(
        const XrInteractionProfileSuggestedBinding *suggested, XrInteractionProfileSuggestedBinding *copy,
        XrActionSuggestedBinding *kept) {
    if (!suggested || !suggested->countSuggestedBindings || !suggested->suggestedBindings || !kept ||
        !detached_filter_on())
        return suggested;
    uint32_t n = 0;
    for (uint32_t i = 0; i < suggested->countSuggestedBindings; ++i)
        if (!detached_path(suggested->suggestedBindings[i].binding)) kept[n++] = suggested->suggestedBindings[i];
    if (n == suggested->countSuggestedBindings) return suggested;
    *copy = *suggested;
    copy->countSuggestedBindings = n;
    copy->suggestedBindings = kept;
    static int logged;
    if (logged++ < 8) {
        PFN_xrPathToString to_string = (PFN_xrPathToString)lookup(active_instance, "xrPathToString");
        char profile[XR_MAX_PATH_LENGTH] = "?";
        uint32_t size = 0;
        if (to_string) to_string(active_instance, suggested->interactionProfile, sizeof(profile), &size, profile);
        LOG("profile_remap: %s bindings without %u detached-controller path(s)", profile,
            suggested->countSuggestedBindings - n);
    }
    return copy;
}

// xrGetCurrentInteractionProfile: a detached controller's path the runtime doesn't know has no profile.
static XrResult detached_current_profile(XrPath user, XrInteractionProfileState *state, XrResult result) {
    if (result != XR_ERROR_PATH_UNSUPPORTED || !state || !detached_filter_on() || !detached_path(user)) return result;
    state->interactionProfile = XR_NULL_PATH;
    return XR_SUCCESS;
}
