"""32-bit games through the a32 bridge (frame.a32_bridge): APK conversion, recipe, analysis, checks, triage."""
import zipfile

from conftest import build_axml

from frameport.analysis import detect
from frameport.apk import axml
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.core.models import Analysis
from frameport.patches import base
from frameport.patches.frame import a32_bridge
from frameport.recommend import engine
from frameport.validate.triage import triage

UNITY = b"\x7fELF-unity\0com.oculus.systemactivities\0rest"


def _manifest(target_sdk=21, headtracking=None):
    """headtracking: (version, required) of a uses-feature android.hardware.vr.headtracking, None for none"""
    feature = [] if headtracking is None else [
        ("start", "uses-feature", [("name", "str", "android.hardware.vr.headtracking"),
                                   ("required", "bool", headtracking[1]), ("version", "int", headtracking[0])]),
        ("end", "uses-feature")]
    return build_axml([
        ("start", "manifest", [("package", "str", "com.example.gearvr")]),
        ("start", "uses-sdk", [("minSdkVersion", "int", 19), ("targetSdkVersion", "int", target_sdk)]),
        ("end", "uses-sdk"),
        *feature,
        ("start", "application", [("extractNativeLibs", "bool", False)]),
        ("start", "activity", [("name", "str", "com.unity3d.player.UnityPlayerActivity")]),
        ("start", "intent-filter", []),
        ("start", "action", [("name", "str", "android.intent.action.MAIN")]),
        ("end", "action"),
        ("start", "category", [("name", "str", "android.intent.category.LAUNCHER")]),
        ("end", "category"),
        ("end", "intent-filter"),
        ("end", "activity"),
        ("start", "meta-data", [("name", "str", "com.samsung.android.vr.application.mode"),
                                ("value", "str", "vr_only")]),
        ("end", "meta-data"),
        ("end", "application"),
        ("end", "manifest"),
    ])


def _apk(tmp_path, platform=True, manifest=None):
    p = tmp_path / "gearvr.apk"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("AndroidManifest.xml", manifest or _manifest())
        z.writestr("classes.dex", b"dex\n035\0")
        z.writestr("META-INF/CERT.RSA", b"old signature")
        z.writestr("lib/armeabi-v7a/libmain.so", b"\x7fELF-main32")
        z.writestr("lib/armeabi-v7a/libunity.so", UNITY)
        z.writestr("lib/armeabi-v7a/libvrapi.so", b"\x7fELF-oculus-vrapi32")
        z.writestr("lib/x86/libmain.so", b"\x7fELF-x86")
        if platform:
            z.writestr("lib/armeabi-v7a/libovrplatformloader.so", b"\x7fELF-meta-loader32")
    return p


def _prebuilt(name):
    return {"liba32stub.so": b"STUB", "liba32bridge.so": b"BRIDGE", "liba32rt.so": b"RUNTIME",
            "libvrapi.so": b"VRAPI64", "libopenxr_loader_generic.so": b"FRAMEBRIDGE"}[name]


def _ovrport(name, abi):
    return f"OVRPORT {abi} {name}".encode()


def _signed_cert_pkcs7():
    """A PKCS#7 signature block (like META-INF/CERT.RSA) with a self-signed certificate; returns (block, cert DER)."""
    import datetime

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.serialization import pkcs7
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Game Studio")])
    now = datetime.datetime(2016, 1, 1, tzinfo=datetime.UTC)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(1).not_valid_before(now).not_valid_after(now + datetime.timedelta(days=9000))
            .sign(key, hashes.SHA256()))
    block = (pkcs7.PKCS7SignatureBuilder().set_data(b"SF").add_signer(cert, key, hashes.SHA256())
             .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature]))
    return block, cert.public_bytes(serialization.Encoding.DER)


def test_original_signature_for_unreal(tmp_path):
    """UE4's AndroidThunkJava_GetSignature = Base64.DEFAULT(SHA-1(signing certificate DER)); a re-signed APK has
    another, so the conversion keeps the original's for the bridge to answer with (Gunjack quits otherwise)."""
    import base64
    import hashlib

    block, der = _signed_cert_pkcs7()
    p = tmp_path / "ue4.apk"
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("AndroidManifest.xml", _manifest())
        z.writestr("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n")
        z.writestr("META-INF/CERT.RSA", block)
        z.writestr("lib/armeabi-v7a/libUE4.so", b"\x7fELF-ue4")
    with ApkWorkspace(p) as ws:
        a32_bridge.convert(ws, _prebuilt, _ovrport)
        assert ws.read(a32_bridge.SIGNATURE_ASSET) == base64.b64encode(hashlib.sha1(der).digest()) + b"\n"
    with ApkWorkspace(_apk(tmp_path)) as ws:  # not Unreal (and an unreadable signature block): nothing added
        a32_bridge.convert(ws, _prebuilt, _ovrport)
        assert not ws.has(a32_bridge.SIGNATURE_ASSET)


def test_convert_layout(tmp_path):
    with ApkWorkspace(_apk(tmp_path)) as ws:
        converted = a32_bridge.convert(ws, _prebuilt, _ovrport)
        assert (ws.abi, ws.libdir) == ("arm64-v8a", "lib/arm64-v8a/")  # later patches see the arm64 layout
        out = ws.write(tmp_path / "out.apk")
    assert converted == ["libmain.so", "libovrplatformloader.so", "libunity.so"]  # libvrapi.so: translated instead
    with zipfile.ZipFile(out) as z:
        names = set(z.namelist())
        lib = {n.split("/")[-1]: z.read(n) for n in names if n.startswith("lib/arm64-v8a/")}
        manifest = z.read("AndroidManifest.xml")
    assert not any(n.startswith(("lib/armeabi-v7a/", "lib/x86/")) for n in names)  # Lepton would pick those
    assert lib["libmain.a32.so"] == b"\x7fELF-main32" and lib["libmain.so"] == b"STUB"
    assert b"systemactivities" not in lib["libunity.a32.so"] and b"\0android\0" in lib["libunity.a32.so"]
    assert len(lib["libunity.a32.so"]) == len(UNITY)
    assert lib["libovrplatformloader.a32.so"] == b"OVRPORT armeabi-v7a libovrplatformloader.so"
    assert lib["libovrplatformloader_meta.a32.so"] == b"OVRPORT armeabi-v7a libovrplatformloader_meta.so"
    assert "libovrplatformloader_meta.so" not in lib  # only the guest dispatcher loads it: no stub
    assert "libvrapi.a32.so" not in lib and lib["libvrapi.so"].startswith(b"\x7fELF\x02")  # Java's: an empty arm64 one
    assert lib["libfp_vrapi64.so"] == b"VRAPI64"
    assert lib["libopenxr_loader.so"] == b"OVRPORT arm64-v8a libopenxr_loader.so"
    assert lib["libopenxr_loader_original.so"] == b"OVRPORT arm64-v8a libopenxr_loader_generic.so"
    assert lib["libopenxr_loader_generic.so"] == b"FRAMEBRIDGE"
    assert lib["liba32bridge.so"] == b"BRIDGE" and lib["liba32rt.so"] == b"RUNTIME"
    assert axml.Axml(manifest).get_bool("application", "extractNativeLibs")
    x = axml.Axml(manifest)
    names_ = x.strings()
    sdk = {names_[a.name]: a.value for el in x.elements() if el.name == "uses-sdk" for a in el.attrs}
    assert sdk == {"minSdkVersion": 19, "targetSdkVersion": 23}  # no "older Android" dialog taking the focus


def test_patch_with_adapter(tmp_path, monkeypatch):
    """frame.a32_bridge, then frame.adapter as in a build: the adapter only adds its settings."""
    from frameport.tools import overport

    def fake_runtime(name, abi, reporter):
        f = tmp_path / f"{abi}-{name}"
        f.write_bytes(_ovrport(name, abi))
        return f

    monkeypatch.setattr(overport, "ensure_runtime_lib", fake_runtime)
    a = _analysis()
    recipe = {"frame.a32_bridge": {}, "frame.adapter": {}}
    with ApkWorkspace(_apk(tmp_path, platform=False)) as ws:
        for pid in ("frame.a32_bridge", "frame.adapter"):
            ctx = base.ApkContext(ws, a, {}, Reporter(), recipe)
            base.get(pid).apply(ctx)
            assert all(ok for _, ok, _ in base.get(pid).validate(ctx))
        assert not base.get("frame.a32_bridge").apply(base.ApkContext(ws, a, {}, Reporter(), recipe))  # once
        out = ws.write(tmp_path / "out.apk")
    with zipfile.ZipFile(out) as z:
        assert z.read("lib/arm64-v8a/libframe_settings.so").startswith(b"scale=")
        assert z.read("lib/arm64-v8a/libopenxr_loader_original.so") == _ovrport("libopenxr_loader_generic.so",
                                                                                 "arm64-v8a")


def _analysis(**kw):
    d = dict(package="com.example.gearvr", version="1.0", label="Gear VR Game", abis=["armeabi-v7a", "x86"],
             engine="Unity", xr="VrApi", graphics="GLES or unknown (no Vulkan declaration)", direct_vrapi=False,
             libs=["libmain.so", "libunity.so", "libvrapi.so", "libOVRPlugin.so"], launcher_activity=None,
             has_info_category=False, meta_permissions=[], uses_glad_gl=False, unity_msaa_levels=0,
             oculus_os_classes=False, is_overport_output=False, debuggable=False,
             extra={"size": 1, "vr_kind": "quest", "headset": "gearvr", "unity_oculus_check": True})
    d.update(kw)
    return Analysis(**d)


def test_recipe_for_a_32bit_game():
    r = engine.suggest(_analysis(), use_catalog=False)
    assert r.status != "unsupported" and "32-bit bridge" in r.notes
    assert not r.overport and not r.alt_patches  # OVRPort handles 64-bit apps only
    assert {"frame.a32_bridge", "frame.adapter", "device.text_input_window"} <= set(r.patches)
    assert not [p for p in r.patches if base.get(p).category == "overport"]
    # the 64-bit library fixes can't apply to translated code
    assert not {"frame.unity_oculus_check", "frame.ovrstubs", "adapter.haptic_fix"} & set(r.patches)
    shown, _ = engine.visible_patches(_analysis(), r)
    assert not [p for p in shown if p.category == "overport"]


def test_64bit_games_keep_their_recipe():
    a = _analysis(abis=["arm64-v8a"], extra={"size": 1, "vr_kind": "quest", "headset": "quest"})
    r = engine.suggest(a, use_catalog=False)
    assert r.overport and "frame.a32_bridge" not in r.patches


def test_headset():
    gear = {"com.samsung.android.vr.application.mode": "vr_only"}
    assert detect.headset(gear, ["armeabi-v7a"], "quest") == "gearvr"
    assert detect.headset({"com.oculus.supportedDevices": "go"}, ["armeabi-v7a"], "quest") == "go"
    assert detect.headset({"com.oculus.supportedDevices": "quest|quest2"}, ["armeabi-v7a"], "quest") == "quest"
    assert detect.headset(gear, ["arm64-v8a", "armeabi-v7a"], "quest") == "quest"  # a Quest build kept the entry
    assert detect.headset({}, ["armeabi-v7a"], "none") == ""
    # a 32-bit Santa Cruz build (Sports Scramble) kept Samsung's VR mode but requires 6DoF head tracking
    assert detect.headset(gear, ["armeabi-v7a"], "quest", six_dof=True) == "quest"


def test_requires_6dof():
    assert detect.requires_6dof(_manifest(headtracking=(1, True)))
    assert not detect.requires_6dof(_manifest(headtracking=(1, False)))  # optional: a Go app may run on a Quest too
    assert not detect.requires_6dof(_manifest(headtracking=(0, True)))  # version 0 = 3DoF
    assert not detect.requires_6dof(_manifest())


def test_quest_build_gets_touch_controllers(tmp_path):
    """The bridge passes the Frame's controllers through as Touch controllers for Quest builds (fp_a32_headset.txt),
    and keeps the Gear VR remote emulation for the others."""
    with ApkWorkspace(_apk(tmp_path, manifest=_manifest(headtracking=(1, True)))) as ws:
        a32_bridge.convert(ws, _prebuilt, _ovrport)
        assert ws.read(a32_bridge.HEADSET_ASSET) == b"quest"
    with ApkWorkspace(_apk(tmp_path)) as ws:
        a32_bridge.convert(ws, _prebuilt, _ovrport)
        assert not ws.has(a32_bridge.HEADSET_ASSET)


def test_static_checks_skip_guest_libraries(tmp_path, monkeypatch):
    from frameport.analysis import elf
    from frameport.analysis.stubgen import build_stub_library
    from frameport.apk import sign
    from frameport.validate.static import check_apk

    monkeypatch.setattr(sign, "verify", lambda apk: (True, ""))
    monkeypatch.setattr(sign, "alignment_problems", lambda apk: [])

    apk = tmp_path / "bridged.apk"
    guest = elf.add_needed(build_stub_library(["main"], soname="libmain.so"), "libvrapi.so")  # the guest runtime's
    with zipfile.ZipFile(apk, "w") as z:
        z.writestr("AndroidManifest.xml", _manifest(23))
        z.writestr("lib/arm64-v8a/libmain.a32.so", guest)  # (not really 32-bit: only its file name matters)
        z.writestr("lib/arm64-v8a/libmain.so", build_stub_library(["stub"], soname="libmain.so"))
    checks = {c["name"]: c for c in check_apk(apk, expect_adapter=False)}
    assert checks["Library dependencies resolvable"]["ok"] is True


def test_triage_doesnt_suggest_the_64bit_vrapi_bridge():
    log = """
10-10 15:59:39.927  1300  1324 I a32bridge: vrapi_Initialize(app VrApi 1.31.0, graphics 0x10200) -> 0
10-10 15:59:42.000  1300  1324 I FrameBridge: pacing: 72.2 fps, displayTime vs predicted: avg 0.00 ms
"""
    r = triage(log, "RUNNING", "com.example.gearvr")
    assert "direct-vrapi" not in {f.id for f in r.findings}
    r = triage("I Unity: vrapi_Initialize\n", "RUNNING", "com.example.game")
    assert "direct-vrapi" in {f.id for f in r.findings}
