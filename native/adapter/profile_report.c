// SPDX-License-Identifier: GPL-3.0-only
// Included by session_fixes.c (after TOUCH_PROFILE and app_suggested_touch): what xrGetCurrentInteractionProfile
// tells the game about each hand. Uses controller_fix, profile_remap, active_instance, lookup and LOG.
//  * controller_fix: SteamVR's own profiles (valve/..., khr/generic_controller) are reported as oculus/touch_controller,
//    the profile Quest games have bindings for.
//  * profile_remap: when the game suggested bindings only for one of Meta's newer profiles (Touch Plus/Pro) and
//    hook_xrSuggestInteractionProfileBindings suggested them again for Touch, the runtime reports
//    oculus/touch_controller, a profile the game never suggested. Unity's OpenXR plugin creates its input devices by
//    the reported profile, finds none for Touch and leaves the controllers unconnected: hands lying at the play-space
//    origin on the floor, no buttons (e.g. Hotel Infinity, GitHub #327). Such a hand is reported with the game's own
//    profile instead (the last one remapped: the Touch bindings the runtime uses are that profile's).

static char remapped_profile[XR_MAX_PATH_LENGTH];  // set by hook_xrSuggestInteractionProfileBindings

static void current_profile_fix(XrPath user, XrInteractionProfileState *state) {
    if ((!controller_fix && !profile_remap) || !state->interactionProfile || !active_instance) return;
    PFN_xrPathToString to_string = (PFN_xrPathToString)lookup(active_instance, "xrPathToString");
    PFN_xrStringToPath to_path = (PFN_xrStringToPath)lookup(active_instance, "xrStringToPath");
    char name[XR_MAX_PATH_LENGTH];
    uint32_t size = 0;
    if (!to_string || !to_path ||
        XR_FAILED(to_string(active_instance, state->interactionProfile, sizeof(name), &size, name)))
        return;
    int touch = !strcmp(name, TOUCH_PROFILE);
    if (controller_fix && (strstr(name, "/valve/") || strstr(name, "/khr/generic_controller")) &&
        XR_SUCCEEDED(to_path(active_instance, TOUCH_PROFILE, &state->interactionProfile)))
        touch = 1;
    if (!touch || !profile_remap || !*remapped_profile || app_suggested_touch ||
        XR_FAILED(to_path(active_instance, remapped_profile, &state->interactionProfile)))
        return;
    static int logged;
    if (logged++ < 4) {
        char who[XR_MAX_PATH_LENGTH] = "?";
        to_string(active_instance, user, sizeof(who), &size, who);
        LOG("profile_remap: %s reported as %s (the app's profile) instead of oculus/touch_controller", who,
            remapped_profile + strlen("/interaction_profiles/"));
    }
}
