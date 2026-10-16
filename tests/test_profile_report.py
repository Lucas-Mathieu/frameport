"""What FrameBridge's xrGetCurrentInteractionProfile reports (native/adapter/profile_report.c): SteamVR's profiles as
Touch (controller_fix) and, after profile_remap moved a game's Touch Plus/Pro bindings to Touch, the game's own profile
(GitHub #327: Unity's OpenXR input found no device for oculus/touch_controller, the hands lay on the floor). Compiled
for this host with a small harness standing in for the adapter and the runtime.
Opt-in (FRAMEPORT_NATIVE_TESTS=1, -m native): needs the OpenXR headers in native/.cache (from native/build.py)."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
INC = next(iter((ROOT / "native/.cache").glob("openxr-*")), None)
CC = shutil.which("cc") or shutil.which("clang")
pytestmark = [pytest.mark.native, pytest.mark.skipif(
    not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not INC or not CC,
    reason="set FRAMEPORT_NATIVE_TESTS=1 (needs the OpenXR headers from native/build.py and a host C compiler)")]

HARNESS = r"""
#include <openxr/openxr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static XrInstance active_instance = (XrInstance)(uintptr_t)1;
static int controller_fix = 1, profile_remap = 1, app_suggested_touch;
static const char *const TOUCH_PROFILE = "/interaction_profiles/oculus/touch_controller";
static char paths[16][XR_MAX_PATH_LENGTH];
static int path_count;
static XRAPI_ATTR XrResult XRAPI_CALL fake_string_to_path(XrInstance i, const char *s, XrPath *out) {
    (void)i;
    for (int k = 0; k < path_count; ++k) if (!strcmp(paths[k], s)) { *out = (XrPath)(k + 1); return XR_SUCCESS; }
    snprintf(paths[path_count], XR_MAX_PATH_LENGTH, "%s", s);
    *out = (XrPath)++path_count;
    return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL fake_path_to_string(XrInstance i, XrPath p, uint32_t cap, uint32_t *n,
                                                          char *out) {
    (void)i;
    if (!p || p > (XrPath)path_count) return XR_ERROR_PATH_INVALID;
    *n = (uint32_t)snprintf(out, cap, "%s", paths[p - 1]) + 1;
    return XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance i, const char *name) {
    (void)i;
    if (!strcmp(name, "xrPathToString")) return (PFN_xrVoidFunction)fake_path_to_string;
    if (!strcmp(name, "xrStringToPath")) return (PFN_xrVoidFunction)fake_string_to_path;
    return NULL;
}
#include "profile_report.c"
static XrPath P(const char *s) { XrPath p; fake_string_to_path(0, s, &p); return p; }
static const char *answer(const char *hand, const char *runtime) {
    XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE, NULL, runtime ? P(runtime) : XR_NULL_PATH};
    current_profile_fix(P(hand), &state);
    return state.interactionProfile ? paths[state.interactionProfile - 1] : "(none)";
}
int main(void) {
    const char *touch = "/interaction_profiles/oculus/touch_controller";
    const char *frame = "/interaction_profiles/valve/frame_controller";
    const char *plus = "/interaction_profiles/meta/touch_controller_plus";
    // no remap: SteamVR's profile becomes Touch, Touch stays Touch, none stays none
    printf("%s\n", answer("/user/hand/left", frame));
    printf("%s\n", answer("/user/hand/left", touch));
    printf("%s\n", answer("/user/hand/left", NULL));
    // the game's Touch Plus bindings were moved to Touch: Touch (and SteamVR's profile) are reported as Touch Plus
    snprintf(remapped_profile, sizeof(remapped_profile), "%s", plus);
    printf("%s\n", answer("/user/hand/left", touch));
    printf("%s\n", answer("/user/hand/right", frame));
    printf("%s\n", answer("/user/hand/right", NULL));
    printf("%s\n", answer("/user/hand/right", "/interaction_profiles/htc/vive_controller"));
    // controller_fix off: SteamVR's own profile is passed on unchanged, Touch is still reported as the game's
    controller_fix = 0;
    printf("%s\n", answer("/user/hand/left", frame));
    printf("%s\n", answer("/user/hand/left", touch));
    // the game suggested Touch bindings itself: Touch is reported as Touch
    controller_fix = 1;
    app_suggested_touch = 1;
    printf("%s\n", answer("/user/hand/left", touch));
    // profile_remap off: Touch as Touch
    app_suggested_touch = 0;
    profile_remap = 0;
    printf("%s\n", answer("/user/hand/left", touch));
    return 0;
}
"""

STDOUT = [
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/oculus/touch_controller",
    "(none)",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/meta/touch_controller_plus",
    "(none)",
    "/interaction_profiles/htc/vive_controller",
    "/interaction_profiles/valve/frame_controller",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/oculus/touch_controller",
]


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    d = tmp_path_factory.mktemp("profile_report")
    (d / "harness.c").write_text(HARNESS)
    subprocess.run([CC, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-missing-field-initializers", "-I", str(INC), "-I", str(ROOT / "native/adapter"),
                    str(d / "harness.c"), "-o", str(d / "harness")], check=True)
    return d / "harness"


def test_reports_the_games_profile_after_a_remap(harness):
    p = subprocess.run([str(harness)], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    assert p.stdout.splitlines() == STDOUT
    logged = [line for line in p.stderr.splitlines() if line.startswith("profile_remap:")]
    assert logged[:2] == [
        "profile_remap: /user/hand/left reported as meta/touch_controller_plus (the app's profile) instead of "
        "oculus/touch_controller",
        "profile_remap: /user/hand/right reported as meta/touch_controller_plus (the app's profile) instead of "
        "oculus/touch_controller",
    ]
    assert len(logged) == 3  # once per report (at most 4)


DETACHED = r"""
#include <openxr/openxr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static XrInstance active_instance = (XrInstance)(uintptr_t)1;
static int profile_remap = 1, runtime_has_detached_controllers;
static char paths[32][XR_MAX_PATH_LENGTH];
static int path_count;
static XRAPI_ATTR XrResult XRAPI_CALL fake_string_to_path(XrInstance i, const char *s, XrPath *out) {
    (void)i;
    for (int k = 0; k < path_count; ++k) if (!strcmp(paths[k], s)) { *out = (XrPath)(k + 1); return XR_SUCCESS; }
    snprintf(paths[path_count], XR_MAX_PATH_LENGTH, "%s", s);
    *out = (XrPath)++path_count;
    return XR_SUCCESS;
}
static XRAPI_ATTR XrResult XRAPI_CALL fake_path_to_string(XrInstance i, XrPath p, uint32_t cap, uint32_t *n,
                                                          char *out) {
    (void)i;
    if (!p || p > (XrPath)path_count) return XR_ERROR_PATH_INVALID;
    *n = (uint32_t)snprintf(out, cap, "%s", paths[p - 1]) + 1;
    return XR_SUCCESS;
}
static PFN_xrVoidFunction lookup(XrInstance i, const char *name) {
    (void)i;
    if (!strcmp(name, "xrPathToString")) return (PFN_xrVoidFunction)fake_path_to_string;
    return NULL;
}
#include "detached_controllers.c"
static XrPath P(const char *s) { XrPath p; fake_string_to_path(0, s, &p); return p; }
static void show_action(const XrActionCreateInfo *info) {
    printf("%s:", info->actionName);
    for (uint32_t i = 0; i < info->countSubactionPaths; ++i) printf(" %s", paths[info->subactionPaths[i] - 1]);
    printf("\n");
}
int main(void) {
    XrPath sub[4] = {P("/user/hand/left"), P("/user/hand/right"), P("/user/detached_controller_meta/left"),
                     P("/user/detached_controller_meta/right")}, kept[4];
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO, NULL, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, 4, sub,
                               "Thumbstick"}, copy;
    show_action(detached_strip_action(&info, &copy, kept));
    info.countSubactionPaths = 2;  // hands only: passed on as it is
    printf("%d\n", detached_strip_action(&info, &copy, kept) == &info);

    XrActionSuggestedBinding b[3] = {{(XrAction)(uintptr_t)7, P("/user/hand/left/input/thumbstick")},
                                     {(XrAction)(uintptr_t)7, P("/user/detached_controller_meta/left/input/thumbstick")},
                                     {(XrAction)(uintptr_t)8, P("/user/hand/right/input/a/click")}}, kb[3];
    XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL,
                                              P("/interaction_profiles/oculus/touch_controller"), 3, b}, scopy;
    const XrInteractionProfileSuggestedBinding *u = detached_strip_bindings(&s, &scopy, kb);
    for (uint32_t i = 0; i < u->countSuggestedBindings; ++i) printf("%s\n", paths[u->suggestedBindings[i].binding - 1]);

    XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE, NULL, 5};
    XrResult r = detached_current_profile(sub[2], &st, XR_ERROR_PATH_UNSUPPORTED);
    printf("%d %llu\n", r, (unsigned long long)st.interactionProfile);
    printf("%d\n", detached_current_profile(sub[0], &st, XR_ERROR_PATH_UNSUPPORTED));  // hands: unchanged
    printf("%d\n", detached_current_profile(sub[2], &st, XR_ERROR_ACTIONSET_NOT_ATTACHED));  // other errors stay

    runtime_has_detached_controllers = 1;  // the runtime knows them: nothing changes
    info.countSubactionPaths = 4;
    printf("%d %d %d\n", detached_strip_action(&info, &copy, kept) == &info,
           detached_strip_bindings(&s, &scopy, kb) == &s,
           detached_current_profile(sub[2], &st, XR_ERROR_PATH_UNSUPPORTED));
    runtime_has_detached_controllers = 0;
    profile_remap = 0;  // off: nothing changes
    printf("%d %d\n", detached_strip_action(&info, &copy, kept) == &info, detached_strip_bindings(&s, &scopy, kb) == &s);
    return 0;
}
"""


def test_detached_controller_paths_left_out(tmp_path):
    (tmp_path / "detached.c").write_text(DETACHED)
    subprocess.run([CC, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-missing-field-initializers", "-I", str(INC), "-I", str(ROOT / "native/adapter"),
                    str(tmp_path / "detached.c"), "-o", str(tmp_path / "detached")], check=True)
    p = subprocess.run([str(tmp_path / "detached")], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    assert p.stdout.splitlines() == [
        "thumbstick: /user/hand/left /user/hand/right",
        "1",
        "/user/hand/left/input/thumbstick",
        "/user/hand/right/input/a/click",
        "0 0",
        str(-22),  # XR_ERROR_PATH_UNSUPPORTED
        str(-46),  # XR_ERROR_ACTIONSET_NOT_ATTACHED
        "1 1 -22",
        "1 1",
    ]
    assert p.stderr.splitlines() == [
        "profile_remap: action thumbstick without 2 detached-controller path(s) (the runtime has no "
        "XR_META_detached_controllers)",
        "profile_remap: /interaction_profiles/oculus/touch_controller bindings without 1 detached-controller path(s)",
    ]
