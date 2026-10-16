"""Unity App UI's activity quit PianoVision at start on the Frame: Android 12's VibratorManager in onCreate (GitHub
#297)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.unity_appui_vibrator import UnityAppUiVibrator, patch_dex
from frameport.validate.triage import triage

ACT = "Lcom/unity3d/player/appui/AppUIGameActivity;"
VM = "Landroid/os/VibratorManager;"
# method 0 getSystemService, 1 getDefaultVibrator (referenced only); field 0 m_Vibrator; string 0 / type 0 as below
ON_CREATE = [0x001A, 0x0000,          # const-string v0, "vibrator_manager"
             0x206E, 0x0000, 0x0003,  # invoke-virtual {v3, v0}, getSystemService
             0x000C,                  # move-result-object v0
             0x001F, 0x0000,          # check-cast v0, VibratorManager
             0x106E, 0x0001, 0x0000,  # invoke-virtual {v0}, getDefaultVibrator
             0x000C,                  # move-result-object v0
             0x305B, 0x0000,          # iput-object v0, v3, m_Vibrator
             0x000E]
VIBRATE = [0x3054, 0x0000, 0x000E]    # iget-object v0, v3, m_Vibrator; return-void
OTHER = [0x0012, 0x000F]


def _dex() -> bytes:
    return build_dex([("Landroid/app/Activity;", "getSystemService", "Ljava/lang/Object;", None),
                      (VM, "getDefaultVibrator", "Landroid/os/Vibrator;", None),
                      (ACT, "onCreate", "V", ON_CREATE),
                      (ACT, "Vibrate", "V", VIBRATE),
                      (ACT, "IsNightModeDefined", "Z", OTHER)],
                     strings_first=("vibrator_manager",), types_first=(VM,),
                     fields=((ACT, "Landroid/os/Vibrator;", "m_Vibrator"),))


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(ACT, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_activity_starts_without_vibrator_manager():
    raw = _dex()
    dex = Dex(raw)
    assert dex.field(0) == (ACT, "Landroid/os/Vibrator;", "m_Vibrator")
    assert patch_dex(dex) == ["AppUIGameActivity.onCreate", "AppUIGameActivity.Vibrate"]
    # const/16 v0, 0 (null) in place of the sequence; the field store and the rest of onCreate stay
    assert _code(dex, "onCreate") == [0x0013, 0] + [0] * 10 + ON_CREATE[12:]
    assert _code(dex, "Vibrate") == [0x000E, 0, 0x000E]
    assert _code(dex, "IsNightModeDefined") == OTHER
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []  # same size; a second run changes nothing


def test_other_sequences_are_left_alone():
    changed = list(ON_CREATE)
    changed[6:8] = [0x001F, 0x0001]  # check-cast to another type
    dex = Dex(build_dex([("Landroid/app/Activity;", "getSystemService", "Ljava/lang/Object;", None),
                         (VM, "getDefaultVibrator", "Landroid/os/Vibrator;", None),
                         (ACT, "onCreate", "V", changed), (ACT, "Vibrate", "V", VIBRATE)],
                        strings_first=("vibrator_manager",), types_first=(VM, "Landroid/os/Vibrator;"),
                        fields=((ACT, "Landroid/os/Vibrator;", "m_Vibrator"),)))
    assert patch_dex(dex) == []


def test_detect_and_triage(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", _dex())
    a = detect.analyze(path, deep=False)
    assert a.extra["unity_appui_vibrator"] is True
    assert UnityAppUiVibrator().applies(a) and UnityAppUiVibrator().detect(a).recommended
    log = ("10-16 00:57:56.321  1196  1196 E AndroidRuntime: FATAL EXCEPTION: main\n"
           "10-16 00:57:56.321  1196  1196 E AndroidRuntime: java.lang.NoClassDefFoundError: Failed resolution of: "
           "Landroid/os/VibratorManager;\n"
           "10-16 00:57:56.321  1196  1196 E AndroidRuntime: \tat com.unity3d.player.appui.AppUIGameActivity.onCreate"
           "(AppUIGameActivity.java:123)\n")
    r = triage(log, "EXITED", "com.ZarApps.PianoVision")
    assert [f.id for f in r.findings] == ["unity-appui-vibrator"]
    assert r.findings[0].suggest == ["frame.unity_appui_vibrator"]
