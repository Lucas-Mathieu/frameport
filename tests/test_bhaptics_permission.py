"""bHaptics' Bluetooth permission request froze Yupitergrad's loading screen on the Frame (GitHub #235)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.bhaptics_permission import UTILS, BhapticsPermission, patch_dex
from frameport.validate.triage import triage

REQUEST = [0x0071, 0x0001, 0x0000, 0x000E]  # invoke-static {} …requestPermissions; return-void
OTHER = [0x0012, 0x000F]


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(UTILS, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_request_returns_at_once():
    raw = build_dex([(UTILS, "requestBluetoothPermission", "V", REQUEST), (UTILS, "hasPermission", "Z", OTHER)])
    dex = Dex(raw)
    assert patch_dex(dex) == ["requestBluetoothPermission"]
    assert _code(dex, "requestBluetoothPermission") == [0x000E, 0, 0, 0x000E]
    assert _code(dex, "hasPermission") == OTHER
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_detect_and_triage(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", b"dex\n035\0" + build_dex([(UTILS, "requestBluetoothPermission", "V", REQUEST)]))
    a = detect.analyze(path, deep=False)
    assert a.extra["bhaptics_permission"] is True
    assert BhapticsPermission().applies(a) and BhapticsPermission().detect(a).recommended
    log = ("10-11 23:19:07.107   470   470 V GrantPermissionsActivity: Permission grant result requestId=-80 "
           "callingUid=10014 callingPackage=com.Gamedust.Yupitergrad permission=android.permission.BLUETOOTH_CONNECT "
           "isImplicit=false result=1\n")
    r = triage(log, "RUNNING", "com.Gamedust.Yupitergrad")
    assert [f.suggest for f in r.findings if f.id == "bhaptics-permission-prompt"] == [["frame.bhaptics_permission"]]
