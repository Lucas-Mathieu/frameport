"""Play services' PlatformVersion checks folded to `true` crashed Titan Isles at start on Android 11 (GitHub #310)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.gms_platform_version import PLATFORM_VERSION, GmsPlatformVersion, patch_dex
from frameport.validate.triage import triage

TRUE = [0x1012, 0x000F]                                    # const/4 v0, #1 ; return v0
REAL = [0x0060, 0x0000, 0x0113, 0x0021, 0x1034, 0x0004,  # sget SDK_INT; const/16 v1, 33; if-lt …
        0x1012, 0x000F, 0x0012, 0x000F]


def _dex(sv2=TRUE) -> bytes:
    return build_dex([(PLATFORM_VERSION, "isAtLeastR", "Z", TRUE),
                      (PLATFORM_VERSION, "isAtLeastS", "Z", TRUE),
                      (PLATFORM_VERSION, "isAtLeastSv2", "Z", sv2),
                      (PLATFORM_VERSION, "isAtLeastT", "Z", REAL)])


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(PLATFORM_VERSION, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_folded_android12_checks_return_false():
    raw = _dex()
    dex = Dex(raw)
    assert patch_dex(dex) == ["isAtLeastS", "isAtLeastSv2"]
    assert _code(dex, "isAtLeastS") == _code(dex, "isAtLeastSv2") == [0x0012, 0x000F]
    assert _code(dex, "isAtLeastR") == TRUE  # Android 11 is R: true is right
    assert _code(dex, "isAtLeastT") == REAL  # a real SDK_INT check stays
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_detect_and_triage(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", _dex())
    a = detect.analyze(path, deep=False)
    assert a.extra["gms_folded_sdk_checks"] is True
    assert GmsPlatformVersion().applies(a) and GmsPlatformVersion().detect(a).recommended
    with zipfile.ZipFile(path, "w") as z:  # real checks only: nothing to do
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", build_dex([(PLATFORM_VERSION, "isAtLeastS", "Z", REAL)]))
    assert detect.analyze(path, deep=False).extra["gms_folded_sdk_checks"] is False
    log = ("10-16 03:59:27.344  1218  1241 E AndroidRuntime: FATAL EXCEPTION: GoogleApiHandler\n"
           "10-16 03:59:27.344  1218  1241 E AndroidRuntime: java.lang.NoSuchMethodError: No virtual method "
           "permitUnsafeIntentLaunch()Landroid/os/StrictMode$VmPolicy$Builder; in class "
           "Landroid/os/StrictMode$VmPolicy$Builder; or its super classes\n")
    r = triage(log, "EXITED", "com.PsytecGames.TitanIsles")
    assert [f.id for f in r.findings] == ["gms-android12-call"]
    assert r.findings[0].suggest == ["frame.gms_platform_version"]
