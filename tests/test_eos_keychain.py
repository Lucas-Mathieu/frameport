"""Epic Online Services' keychain loop (GitHub #214) and the lost internal storage behind it; CryEngine from Climb 1's
monolithic launcher library (GitHub #238)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.eos_keychain_loop import EOSSDK, EosKeychainLoop, patch_dex
from frameport.validate.triage import triage

RESET = [0x0071, 0x0000, 0x0000, 0x000E]  # invoke-static {} createKeychain; return-void
CREATE = [0x0071, 0x0001, 0x0000, 0x000E]


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(EOSSDK, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_reset_keychain_returns_at_once():
    raw = build_dex([(EOSSDK, "resetKeychain", "V", RESET), (EOSSDK, "createKeychain", "V", CREATE)])
    dex = Dex(raw)
    assert patch_dex(dex) == ["resetKeychain"]
    assert _code(dex, "resetKeychain") == [0x000E, 0, 0, 0x000E]
    assert _code(dex, "createKeychain") == CREATE
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_detect_and_engine(tmp_path, quest_manifest):
    dex = build_dex([(EOSSDK, "resetKeychain", "V", RESET)])
    path = tmp_path / "eos.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", b"dex\n035\0" + dex)
        z.writestr("lib/arm64-v8a/libAndroidLauncher.so", b"\x7fELF")
        z.writestr("assets/game.cryproject", b'{"type": "CRYENGINE Project"}')
    a = detect.analyze(path, deep=False)
    assert a.extra["eos_keychain"] is True and a.engine == "CryEngine"
    assert EosKeychainLoop().applies(a) and EosKeychainLoop().detect(a) is None  # offered by triage only


def test_triage_names_the_drive_problem_and_the_loop():
    log = ("ln: cannot create symbolic link from 'Frame' to '/data/data/com.AtlasXR.BeyondSandbox': File exists\n"
           "10-11 17:01:45.745  1212  1269 E SharedPreferencesImpl: Couldn't create directory for SharedPreferences "
           "file /data/user/0/com.AtlasXR.BeyondSandbox/shared_prefs/EOS_KeychainPreferences.xml\n"
           "10-11 17:01:45.747  1212  1212 E EOSSDK  : LogEos: Resetting EOS SDK keychain: Failed to create: Failed "
           "to write to SharedPreferences\n")
    ids = [f.id for f in triage(log, "RUNNING", "com.AtlasXR.BeyondSandbox").findings]
    assert "lepton-data-link-failed" in ids and "eos-keychain-loop" in ids
    r = triage(log.split("\n", 2)[2], "RUNNING", "com.AtlasXR.BeyondSandbox")
    assert [f.suggest for f in r.findings if f.id == "eos-keychain-loop"] == [["frame.eos_keychain_loop"]]
