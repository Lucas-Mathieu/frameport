"""Google Play services' client libraries ask com.google.android.gms.common.util.PlatformVersion before using newer
Android APIs. In an app built for Android 12+ only (Quest builds declare minSdkVersion 32), the R8 shrinker folds
PlatformVersion.isAtLeastS()/isAtLeastSv2() into `return true`. OVRPort lowers the minimum so the app installs on
Lepton's Android 11, but the folded checks stay true: play-services-basement then calls
StrictMode.VmPolicy.Builder.permitUnsafeIntentLaunch() (Android 12) when it binds to Play services and the app dies
on its GoogleApiHandler thread with NoSuchMethodError (Titan Isles, GitHub #310), whether or not Play services are
there. The checks for Android versions newer than 11 that were folded to a constant `true` return false again (same
size, in place; see vivox_audio_route); Play services then take their Android 11 path (and, as there are no Play
services in Lepton, report them missing the normal way). Only constant bodies change: a real SDK_INT check stays."""
from __future__ import annotations

import struct

from ...apk.dex import Dex
from ..base import ApkContext, Patch, Suggestion, register

PLATFORM_VERSION = "Lcom/google/android/gms/common/util/PlatformVersion;"
# the checks for Android versions above Lepton's Android 11 (API 30)
NEWER = ("isAtLeastS", "isAtLeastSv2", "isAtLeastT", "isAtLeastU", "isAtLeastV", "isAtLeastB", "isAtLeastBaklava")
TRUE_BODY = [0x1012, 0x000F]  # const/4 v0, #1 ; return v0


def folded_checks(dex: Dex) -> list[tuple[str, int]]:
    """(name, code offset) of PlatformVersion's checks for Android 12+ that R8 folded to `return true`."""
    out = []
    for idx, code in dex.class_methods(PLATFORM_VERSION):
        name = dex.method(idx)[1]
        if name not in NEWER or dex.return_type(idx) != "Z":
            continue
        n = struct.unpack_from("<I", dex.data, code + 12)[0]
        if n == 2 and list(struct.unpack_from("<2H", dex.data, code + 16)) == TRUE_BODY:
            out.append((name, code))
    return out


def has_folded_checks(dexes: list[bytes]) -> bool:
    return any(PLATFORM_VERSION.encode() in d and b"isAtLeastS" in d and folded_checks(Dex(d)) for d in dexes)


def patch_dex(dex: Dex) -> list[str]:
    """PlatformVersion checks that now return false; [] if nothing applies."""
    return [name for name, code in folded_checks(dex) if dex.return_early(code, "Z")]


class GmsPlatformVersion(Patch):
    id = "frame.gms_platform_version"
    title = "Play services: Android 11 version checks"
    description = ("Google Play services' libraries in games built for Android 12+ only (for example Titan Isles) "
                   "had their Android 12 checks folded into 'always true' when the game was built; on Lepton's "
                   "Android 11 they then call an Android 12 method and the game crashes at start (FATAL EXCEPTION: "
                   "GoogleApiHandler). Those checks answer 'no' again (an in-place edit of classes.dex).")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("gms_folded_sdk_checks"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Google Play services' Android 12 checks were built in as 'always true'; on "
                                    "Lepton (Android 11) the game would crash at start.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if PLATFORM_VERSION.encode() not in data:
                continue
            dex = Dex(data)
            done = patch_dex(dex)
            if done:
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: PlatformVersion.{', '.join(done)} return false")
                changed = True
        return changed


register(GmsPlatformVersion)
