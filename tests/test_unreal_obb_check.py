"""frame.unreal_skip_obb_check (GitHub #159, Contractors): Unreal's DownloaderActivity CRC-checks the OBB when the
manifest says bVerifyOBBOnStartUp=true; its screens are invisible in Lepton, so the game seems to hang."""
from __future__ import annotations

import zipfile

from conftest import build_axml

from frameport.analysis.detect import analyze, unreal_verify_obb
from frameport.apk import axml
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.validate.triage import triage

PKG = "com.CaveManStudio.ContractorsVR"
VERIFY = "com.epicgames.ue4.GameActivity.bVerifyOBBOnStartUp"


def _manifest(verify=True, kind="bool"):
    return build_axml([
        ("start", "manifest", [("package", "str", PKG)]),
        ("start", "application", []),
        ("start", "meta-data", [("name", "str", "com.epicgames.ue4.GameActivity.bHasOBBFiles"),
                                ("value", "bool", True)]),
        ("end", "meta-data"),
        ("start", "meta-data", [("name", "str", VERIFY), ("value", kind, verify if kind == "bool" else str(verify))]),
        ("end", "meta-data"),
        ("start", "meta-data", [("name", "str", "com.epicgames.ue4.GameActivity.bShouldHideUI"),
                                ("value", "bool", True)]),
        ("end", "meta-data"),
        ("end", "application"),
        ("end", "manifest"),
    ])


def _apk(tmp_path, manifest):
    p = tmp_path / "game.apk"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("AndroidManifest.xml", manifest)
        z.writestr("classes.dex", b"dex\n035\0")
    return p


def _analysis(**extra):
    return Analysis(package=PKG, version="3.9", label="Contractors", abis=["arm64-v8a"], engine="Unreal", xr="OpenXR",
                    graphics="GLES", direct_vrapi=False, libs=["libUE4.so", "libOVRPlugin.so"],
                    launcher_activity=None, has_info_category=False, meta_permissions=[], uses_glad_gl=False,
                    unity_msaa_levels=0, oculus_os_classes=False, is_overport_output=False, debuggable=False,
                    extra={"size": 1, **extra})


def test_set_meta_data_bool_changes_only_that_value():
    m = _manifest()
    out = axml.set_meta_data_bool(m, ".GameActivity.bVerifyOBBOnStartUp", False)
    assert out is not None and len(out) == len(m)
    assert sum(a != b for a, b in zip(m, out, strict=True)) == 4  # one 32-bit boolean
    meta = axml.meta_data(out)
    assert meta[VERIFY] is False
    assert meta["com.epicgames.ue4.GameActivity.bHasOBBFiles"] is True  # the others stay
    assert meta["com.epicgames.ue4.GameActivity.bShouldHideUI"] is True
    assert axml.set_meta_data_bool(out, ".GameActivity.bVerifyOBBOnStartUp", False) is None  # already off
    assert axml.set_meta_data_bool(out, ".GameActivity.bVerifyOBBOnStartUp", True) == m  # round trip


def test_set_meta_data_bool_leaves_string_values():
    """Bundle.getBoolean reads a string "true" as false: nothing to change."""
    assert axml.set_meta_data_bool(_manifest(kind="str"), ".GameActivity.bVerifyOBBOnStartUp", False) is None


def test_analysis_flag(tmp_path):
    assert unreal_verify_obb({VERIFY: True})
    assert unreal_verify_obb({"com.epicgames.unreal.GameActivity.bVerifyOBBOnStartUp": True})  # UE5's name
    assert not unreal_verify_obb({VERIFY: False})
    assert not unreal_verify_obb({VERIFY: "true"})  # not a boolean: Unreal reads false
    assert not unreal_verify_obb({"com.epicgames.ue4.GameActivity.bHasOBBFiles": True})
    assert analyze(_apk(tmp_path, _manifest())).extra["unreal_verify_obb"] is True
    other = tmp_path / "off"
    other.mkdir()
    assert analyze(_apk(other, _manifest(verify=False))).extra["unreal_verify_obb"] is False


def test_patch_detects_applies_and_validates(tmp_path):
    base.load_all()
    patch = base.get("frame.unreal_skip_obb_check")
    assert patch.detect(_analysis(unreal_verify_obb=True)).recommended
    assert patch.detect(_analysis()) is None and not patch.applies(_analysis())
    with ApkWorkspace(_apk(tmp_path, _manifest())) as ws:
        ctx = base.ApkContext(ws, _analysis(unreal_verify_obb=True), {}, Reporter(), {patch.id: {}})
        assert patch.validate(ctx)[0][1] is False
        assert patch.apply(ctx)
        assert patch.validate(ctx) == [("OBB check at start off", True, "bVerifyOBBOnStartUp=false")]
        assert axml.meta_data(ws.read("AndroidManifest.xml"))[VERIFY] is False
        assert not patch.apply(ctx)  # nothing left to change


# the launch log of GitHub #159 (Lepton's logcat mirror; system_server lines have another pid than the game)
STUCK = f"""Boot complete!
10-09 18:02:11.120  1201  1230 I ActivityManager: Start proc 2345:{PKG}/u0a120 for pre-top-activity
10-09 18:02:12.004  2345  2345 D UE4     : [GameActivity] Target SDK is 32.  This may cause issues if below 27.
10-09 18:02:12.410  1201  1802 I ActivityTaskManager: START u0 {{flg=0x10000 cmp={PKG}/.DownloaderActivity}}
10-09 18:02:12.900  1201  1240 I ActivityTaskManager: Displayed {PKG}/.DownloaderActivity: +394ms
"""


def test_triage_flags_the_stuck_downloader():
    res = triage(STUCK, "RUNNING", PKG)
    f = next(f for f in res.findings if f.id == "unreal-obb-check-stuck")
    assert f.severity == "fatal" and f.suggest == ["frame.unreal_skip_obb_check", "frame.unreal_skip_obb_delivery"]
    assert "DownloaderActivity" in f.evidence
    assert res.verdict == "fail"  # used to pass with only "Lepton container started"


def test_triage_ignores_a_downloader_that_returned():
    later = STUCK + ("10-09 18:02:14.000  2345  2380 I FrameBridge: xrCreateInstance result=0\n"
                     "10-09 18:02:20.000  2345  2380 I FrameBridge: pacing: 72.0 fps\n")
    assert not any(f.id == "unreal-obb-check-stuck" for f in triage(later, "RUNNING", PKG).findings)
    crashed = STUCK + "10-09 18:02:14.000  2345  2380 F libc    : Fatal signal 11 (SIGSEGV), code 1\n"
    ids = [f.id for f in triage(crashed, "EXITED", PKG).findings]
    assert "unreal-obb-check-stuck" not in ids and "native-crash" in ids


def test_triage_keeps_only_the_games_own_activity_lines():
    other = STUCK.replace(f"{PKG}/.DownloaderActivity", "com.other.app/.DownloaderActivity")
    assert not any(f.id == "unreal-obb-check-stuck" for f in triage(other, "RUNNING", PKG).findings)


def test_delivery_predicates_return_true_in_place():
    import hashlib
    import struct
    import zlib

    from test_vivox_audio_route import build_dex

    from frameport.apk.dex import Dex
    from frameport.patches.frame.unreal_obb_check import has_delivery_check, patch_dex

    cls = f"L{PKG.replace('.', '/')}/DownloaderActivity;"
    obb = f"L{PKG.replace('.', '/')}/OBBData;"
    shim = "Lcom/epicgames/ue4/DownloadShim;"
    methods = [(cls, "expansionFilesDelivered", "Z", [0x0013, 5, 0x000F]),
               (obb, "expansionFilesDelivered", "Z", [0x0012, 0x000F]),
               (shim, "expansionFilesDelivered", "Z", [0x0071, 0, 0, 0x000A, 0x000F]),
               (cls, "other", "Z", [0x0012, 0x000F]),
               ("Lother/DownloaderActivity;", "expansionFilesDelivered", "Z", [0x0012, 0x000F]),
               (cls, "expansionFilesDelivered", "I", [0x0012, 0x000F])]
    raw = build_dex(methods)
    dex = Dex(raw)
    assert has_delivery_check(raw, PKG)
    assert set(patch_dex(dex, PKG)) == {cls, obb, shim}
    out = dex.finish()
    assert len(out) == len(raw)
    assert out[12:32] == hashlib.sha1(out[32:]).digest()
    assert struct.unpack_from("<I", out, 8)[0] == zlib.adler32(out[12:]) & 0xFFFFFFFF
    for idx, (_c, _n, _r, units) in enumerate(methods):
        off = next(code for i, code in dex.class_methods(_c) if i == idx)
        code = list(struct.unpack_from(f"<{len(units)}H", out, off + 16))
        expected = ([0x1012, 0x000F] + ([0] if idx == 2 else []) + units[3 if idx == 2 else 2:]
                    if idx < 3 else units)
        assert code == expected
        assert out[off:off + 16] == raw[off:off + 16]
    assert patch_dex(Dex(out), PKG) == []


def test_delivery_detection_and_multidex_apply_with_crc_already_off(tmp_path):
    from test_vivox_audio_route import build_dex

    from frameport.patches.frame.unreal_obb_check import UnrealSkipObbDelivery

    patch = UnrealSkipObbDelivery()
    path = _apk(tmp_path, _manifest(verify=False))
    raw = build_dex([(f"L{PKG.replace('.', '/')}/DownloaderActivity;", "expansionFilesDelivered", "Z",
                      [0x0012, 0x000F])])
    with zipfile.ZipFile(path, "a") as z:
        z.writestr("classes2.dex", raw)
    analysis = analyze(path)
    assert analysis.extra["analysis_version"] >= 16
    assert analysis.extra["unreal_obb_delivery"] is True
    assert patch.applies(analysis) and patch.detect(analysis) is None
    assert patch.experimental and not patch.default_on
    assert not patch.applies(_analysis())
    original = base.get("frame.unreal_skip_obb_check")
    assert original.detect(analysis) is None and not original.applies(analysis)
    assert original.revision == 1
    with ApkWorkspace(path) as ws:
        ctx = base.ApkContext(ws, analysis, {}, Reporter(), {patch.id: {}})
        assert patch.validate(ctx)[-1][1] is False
        assert patch.apply(ctx)
        assert all(ok for _title, ok, _detail in patch.validate(ctx))
        assert ws.read("AndroidManifest.xml") == _manifest(verify=False)
        assert ws.read("classes.dex") == b"dex\n035\0"
        assert len(ws.read("classes2.dex")) == len(raw)
        assert "expansionFilesDelivered=true" in ctx.notes[-1]
        assert not patch.apply(ctx)


def test_delivery_ignores_unrelated_referenced_and_unsupported_methods():
    from test_vivox_audio_route import build_dex

    from frameport.apk.dex import Dex
    from frameport.patches.frame.unreal_obb_check import has_delivery_check, patch_dex

    cls = f"L{PKG.replace('.', '/')}/DownloaderActivity;"
    raw = build_dex([("Lother/DownloaderActivity;", "expansionFilesDelivered", "Z", [0x0012, 0x000F]),
                     (cls, "expansionFilesDelivered", "I", [0x0012, 0x000F]),
                     (cls, "expansionFilesDelivered", "Z", None)])
    assert not has_delivery_check(raw, PKG)
    dex = Dex(raw)
    assert patch_dex(dex, PKG) == [] and bytes(dex.data) == raw
    raw = build_dex([(cls, "expansionFilesDelivered", "Z", [0x000F])])
    dex = Dex(raw)
    assert patch_dex(dex, PKG) == []  # too short for const/4 + return
    assert bytes(dex.data) == raw


def test_triage_does_not_blame_only_a_wrong_version_when_crc_is_off():
    log = STUCK.replace("Boot complete!", "Boot complete!\nbVerifyOBBOnStartUp=false")
    finding = next(f for f in triage(log, "RUNNING", PKG).findings if f.id == "unreal-obb-check-stuck")
    assert "skip patch already applied" in finding.diagnosis
    assert "external-storage path" in finding.diagnosis


def test_ue5_shim_and_zero_register_method():
    import struct

    from test_vivox_audio_route import build_dex

    from frameport.apk.dex import Dex
    from frameport.patches.frame.unreal_obb_check import has_delivery_check, patch_dex

    cls = "Lcom/epicgames/unreal/DownloadShim;"
    raw = build_dex([(cls, "expansionFilesDelivered", "Z", [0x0012, 0x000F])])
    assert has_delivery_check(raw, PKG)
    dex = Dex(raw)
    off = dex.code_items(cls, "expansionFilesDelivered")[0]
    struct.pack_into("<H", dex.data, off, 0)
    before = bytes(dex.data)
    assert patch_dex(dex, PKG) == [] and bytes(dex.data) == before
    dex = Dex(raw)
    assert patch_dex(dex, PKG) == [cls]


def test_crc_patch_preserves_head_output_with_delivery_dex(tmp_path):
    from test_vivox_audio_route import build_dex

    patch = base.get("frame.unreal_skip_obb_check")
    manifest = _manifest()
    raw = build_dex([(f"L{PKG.replace('.', '/')}/DownloaderActivity;", "expansionFilesDelivered", "Z",
                      [0x0012, 0x000F])])
    path = _apk(tmp_path, manifest)
    with zipfile.ZipFile(path, "a") as z:
        z.writestr("classes2.dex", raw)
    with ApkWorkspace(path) as ws:
        before = {name: ws.read(name) for name in ws.names()}
        ctx = base.ApkContext(ws, analyze(path), {}, Reporter(), {patch.id: {}})
        assert patch.detect(ctx.analysis).recommended
        assert patch.apply(ctx)
        # HEAD writes only the manifest boolean; all other APK entries stay byte-identical.
        expected = {**before, "AndroidManifest.xml": axml.set_meta_data_bool(manifest, VERIFY, False)}
        assert {name: ws.read(name) for name in ws.names()} == expected
        assert ctx.notes == ["bVerifyOBBOnStartUp=false"]
        assert patch.validate(ctx) == [("OBB check at start off", True, "bVerifyOBBOnStartUp=false")]


def test_triage_offers_delivery_when_crc_patch_already_enabled(monkeypatch):
    from dataclasses import asdict

    from frameport import pipeline

    base.load_all()
    monkeypatch.setattr(pipeline.library, "game", lambda _pkg: {
        "analysis": asdict(_analysis(unreal_verify_obb=True, unreal_obb_delivery=True)),
        "recipe": {"patches": {"frame.unreal_skip_obb_check": {}}},
    })
    assert pipeline.useful_suggestions(PKG, triage(STUCK, "RUNNING", PKG).suggestions()) == [
        "frame.unreal_skip_obb_delivery"]
