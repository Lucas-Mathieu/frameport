"""Unity's App UI package (com.unity.dt.app-ui) ships its own activity (com.unity3d.player.appui.AppUIGameActivity,
e.g. PianoVision, GitHub #297) whose onCreate gets the vibrator through Android 12's VibratorManager
(getSystemService("vibrator_manager") → check-cast VibratorManager → getDefaultVibrator) without checking the Android
version. Lepton runs Android 11: the class doesn't exist, onCreate throws NoClassDefFoundError and the app quits before
Unity starts. That sequence becomes "no vibrator" (its registers are set to null, the field store stays, the rest of
onCreate runs), and the activity's methods that use the vibrator (App UI's haptic feedback for its UI controls) return
at once. In-place classes.dex edit (see vivox_audio_route); nothing else changes. The Frame's Android has no vibrator
to drive anyway: controller haptics go through OpenXR, not through this."""
from __future__ import annotations

import struct

from ...apk.dex import Dex
from ..base import ApkContext, Patch, Suggestion, register

PACKAGE = "Lcom/unity3d/player/appui/"
VIBRATOR_MANAGER = "Landroid/os/VibratorManager;"
VIBRATOR = "Landroid/os/Vibrator;"
SERVICE = "vibrator_manager"
CONST_STRING, CONST16, INVOKE_VIRTUAL, MOVE_RESULT_OBJECT, CHECK_CAST, IGET_OBJECT = 0x1A, 0x13, 0x6E, 0x0C, 0x1F, 0x54


def _no_vibrator_manager(dex: Dex, code_off: int) -> bool:
    """Rewrite one `getSystemService("vibrator_manager") … getDefaultVibrator()` sequence in this code item: the
    registers it leaves set (the string, the manager, the vibrator) become null and the rest of the span nops."""
    insns = list(dex._insns(code_off))
    for k, (pos, u) in enumerate(insns):
        if u[0] & 0xFF != CONST_STRING or dex.string(u[1]) != SERVICE or k + 5 >= len(insns):
            continue
        seq = insns[k:k + 6]
        ops = [x[1][0] & 0xFF for x in seq]
        if ops != [CONST_STRING, INVOKE_VIRTUAL, MOVE_RESULT_OBJECT, CHECK_CAST, INVOKE_VIRTUAL, MOVE_RESULT_OBJECT]:
            continue
        if (dex.method(seq[1][1][1])[1] != "getSystemService" or dex.type_name(seq[3][1][1]) != VIBRATOR_MANAGER
                or dex.method(seq[4][1][1]) != (VIBRATOR_MANAGER, "getDefaultVibrator")):
            continue
        regs = []
        for unit in (seq[0][1][0], seq[2][1][0], seq[5][1][0]):  # vAA of const-string / both move-results
            if unit >> 8 not in regs:
                regs.append(unit >> 8)
        end = seq[5][0] + len(seq[5][1])
        new = [w for r in regs for w in (CONST16 | r << 8, 0)]
        new += [0] * (end - pos - len(new))
        struct.pack_into(f"<{len(new)}H", dex.data, code_off + 16 + 2 * pos, *new)
        return True
    return False


def _reads_vibrator(dex: Dex, code_off: int) -> bool:
    return any(u[0] & 0xFF == IGET_OBJECT and dex.field(u[1])[1] == VIBRATOR for _pos, u in dex._insns(code_off))


def patch_dex(dex: Dex) -> list[str]:
    """What changed in App UI's activities ("<Class>.onCreate", "<Class>.Vibrate", …); [] if nothing applies."""
    done = []
    for cls in dex.class_names():
        if not cls.startswith(PACKAGE):
            continue
        short = cls[len(PACKAGE):-1]
        methods = dex.class_methods(cls)
        fixed = [dex.method(idx)[1] for idx, code in methods if _no_vibrator_manager(dex, code)]
        if not fixed:
            continue
        done += [f"{short}.{m}" for m in fixed]
        for idx, code in methods:
            if dex.return_type(idx) == "V" and _reads_vibrator(dex, code) and dex.return_early(code, "V"):
                done.append(f"{short}.{dex.method(idx)[1]}")
    return done


class UnityAppUiVibrator(Patch):
    id = "frame.unity_appui_vibrator"
    title = "Unity App UI: no Android 12 vibrator"
    description = ("The activity of Unity's App UI package (for example PianoVision) gets the vibrator through "
                   "Android 12's VibratorManager without checking the Android version; on Lepton's Android 11 the "
                   "game quits at start with a NoClassDefFoundError. The activity then starts without a vibrator "
                   "and its UI haptic feedback does nothing (an in-place edit of classes.dex).")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("unity_appui_vibrator"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Unity App UI's activity uses Android 12's VibratorManager at start, which "
                                    "Lepton (Android 11) lacks: the game would quit at once.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if PACKAGE.encode() not in data or VIBRATOR_MANAGER.encode() not in data:
                continue
            dex = Dex(data)
            done = patch_dex(dex)
            if done:
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: Unity App UI without VibratorManager ({', '.join(done)})")
                changed = True
        return changed


register(UnityAppUiVibrator)
