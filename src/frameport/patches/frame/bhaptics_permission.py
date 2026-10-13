"""bHaptics' Android SDK (haptic vests, e.g. Yupitergrad, GitHub #235) asks for android.permission.BLUETOOTH_CONNECT at
start (com.bhaptics.commons.PermissionUtils.requestBluetoothPermission). That permission only exists from Android 12;
Lepton runs Android 11, so it can't have been granted at install and the request opens Android's permission screen,
which answers at once but takes the foreground for a moment: the game is paused and resumed, and Meta's OVRPlugin
rebuilds its GLES display objects ("Unable to find EGLConfig for configId 0") and stops drawing (a frozen loading
screen). The request method returns at once instead (in-place classes.dex edit, see vivox_audio_route); without a
paired vest, which can't reach the app in Lepton anyway, nothing else changes."""
from __future__ import annotations

from ...apk.dex import Dex
from ..base import ApkContext, Patch, Suggestion, register

UTILS = "Lcom/bhaptics/commons/PermissionUtils;"
METHOD = "requestBluetoothPermission"


def patch_dex(dex: Dex) -> list[str]:
    """Methods of bHaptics' PermissionUtils that now return at once; [] if nothing applies."""
    done = []
    for idx, code in dex.class_methods(UTILS):
        if dex.method(idx)[1] == METHOD and dex.return_early(code, dex.return_type(idx)):
            done.append(METHOD)
    return done


class BhapticsPermission(Patch):
    id = "frame.bhaptics_permission"
    title = "bHaptics: skip the Bluetooth permission request"
    description = ("bHaptics' vest SDK asks for an Android 12 Bluetooth permission at start; on Lepton's Android 11 "
                   "the permission screen takes the foreground for a moment and Meta's OVRPlugin stops drawing "
                   "(for example Yupitergrad freezing on its loading screen). The request returns at once instead "
                   "(an in-place edit of classes.dex).")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("bhaptics_permission"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "bHaptics asks for an Android 12 Bluetooth permission at start; the permission "
                                    "screen interrupts the game on the Frame.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if UTILS.encode() not in data:
                continue
            dex = Dex(data)
            if patch_dex(dex):
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: bHaptics PermissionUtils.{METHOD} returns at once")
                changed = True
        return changed


register(BhapticsPermission)
