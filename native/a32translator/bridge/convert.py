#!/usr/bin/env python3
"""Converts a 32-bit-only APK to run on arm64 through the bridge (prototype of the FramePort patch):

  lib/armeabi-v7a/libX.so -> lib/arm64-v8a/libX.a32.so (the guest code) + lib/arm64-v8a/libX.so (the stub)
  + lib/arm64-v8a/liba32bridge.so, liba32rt.so; other ABIs dropped; extractNativeLibs forced on; signed with
  FramePort's key for the package; INFO-only start activities get LAUNCHER (Lepton needs it); targetSdkVersion
  raised to 23 (older ones get a dialog that takes the focus).

    uv run python native/a32translator/bridge/convert.py IN.apk OUT.apk [--replace libvrapi.so=FILE ...]

Build the bridge first (python native/a32translator/bridge/build.py). --replace puts FILE (a 32-bit library) in
place of the APK's own copy.
"""
import argparse
import os
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "out" / "bridge"
ABIS32 = ("armeabi-v7a", "armeabi")
REPO = HERE.parents[2]
# VrApi 1.x games: the runtime translates VrApi itself (host/vrapi.c) to the 64-bit VrApi -> OpenXR bridge, which
# needs OVRPort's OpenXR loader chain: dispatcher -> FrameBridge adapter -> OVRPort's generic loader
VRAPI_BRIDGE = Path(os.environ.get("A32_VRAPI_BRIDGE") or REPO / "artifacts/arm64-v8a/libvrapi.so")  # env: a test build
FRAMEBRIDGE = REPO / "artifacts/arm64-v8a/libopenxr_loader_generic.so"
OVERPORT_CONFIG = (b'{"version":1,"database":[{"app_id":"__current__","products":[],"assets":[]}],'
                   b'"patched":{"by":"FramePort a32 bridge","patches":[]}}')
FRAME_SETTINGS = b"scale=1.0\nfoveation_fix=1\ncontroller_fix=1\n" + os.environ.get("A32_FRAME_SETTINGS", "").replace(";", "\n").encode()


def overport_libraries() -> Path:
    """OVRPort's runtime libraries (downloaded by FramePort's toolchain into its workspace)."""
    from frameport.tools import overport as overport_tool

    roots = sorted(Path(overport_tool.workspace()).glob("libraries/*/lib/arm64-v8a"))
    if not roots:
        sys.exit("OVRPort's libraries are missing: build any Quest game with FramePort once (frameport tools install)")
    return roots[-1]


def unity_oculus_check(data: bytes) -> bytes:
    """Unity's built-in Oculus support starts VR only when com.oculus.systemactivities is installed: point the lookup
    at "android" (as FramePort's frame.unity_oculus_check does for 64-bit Unity)."""
    old = b"\0com.oculus.systemactivities\0"
    new = b"\0android" + b"\0" * (len(old) - 8)
    return data.replace(old, new)


def raise_target_sdk(manifest: bytes, axml, minimum: int = 23) -> bytes:
    """Android 11 shows a "built for an older version of Android" dialog for targetSdkVersion < 23, which takes the
    window focus (Unity then never starts rendering)."""
    import struct
    x = axml.Axml(manifest)
    names = x.strings()
    for el in x.elements():
        if el.name == "uses-sdk":
            for at in el.attrs:
                if at.name < len(names) and names[at.name] == "targetSdkVersion" and at.dtype == 0x10 and at.value < minimum:
                    struct.pack_into("<I", x.data, at.offset + 16, minimum)
    return x.bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("out")
    ap.add_argument("--replace", action="append", default=[], metavar="LIB=FILE")
    a = ap.parse_args()
    sys.path.insert(0, str(HERE.parents[2] / "src"))
    from pyaxmlparser import APK

    from frameport.apk import axml, sign

    package = APK(a.apk).package
    replace = dict(r.split("=", 1) for r in a.replace)
    with zipfile.ZipFile(a.apk) as z:
        names = z.namelist()
        abi = next((x for x in ABIS32 if any(n.startswith(f"lib/{x}/") for n in names)), None)
        if not abi:
            sys.exit("no 32-bit ARM libraries in this APK")
        if any(n.startswith("lib/arm64-v8a/") for n in names):
            sys.exit("the APK has 64-bit libraries already")
        libs = [n for n in names if n.startswith(f"lib/{abi}/") and n.endswith(".so")]
        with tempfile.TemporaryDirectory() as td:
            unsigned = Path(td) / "unsigned.apk"
            with zipfile.ZipFile(unsigned, "w", zipfile.ZIP_DEFLATED) as o:
                for info in z.infolist():
                    n = info.filename
                    if n.startswith("lib/") or (n.startswith("META-INF/") and n.split("/")[-1].split(".")[-1]
                                                in ("SF", "RSA", "DSA", "EC", "MF")):
                        continue
                    data = z.read(info)
                    if n == "AndroidManifest.xml":
                        data = axml.set_bool_attr(data, "application", "extractNativeLibs", True) or data
                        data = axml.fix_launcher(data) or data  # Lepton starts the LAUNCHER activity
                        data = raise_target_sdk(data, axml)
                    o.writestr(info, data, info.compress_type)
                vrapi = f"lib/{abi}/libvrapi.so" in libs
                for n in libs:
                    base = n.split("/")[-1]
                    if base == "libvrapi.so" and base not in replace:
                        continue  # the Oculus loader: VrApi is translated instead
                    data = Path(replace[base]).read_bytes() if base in replace else z.read(n)
                    if base == "libunity.so":
                        data = unity_oculus_check(data)
                    o.writestr(f"lib/arm64-v8a/{base[:-3]}.a32.so", data)
                    o.write(OUT / "liba32stub.so", f"lib/arm64-v8a/{base}")
                for base, path in replace.items():
                    if f"lib/{abi}/{base}" not in libs:  # an addition
                        o.write(path, f"lib/arm64-v8a/{base[:-3]}.a32.so")
                        o.write(OUT / "liba32stub.so", f"lib/arm64-v8a/{base}")
                if vrapi:
                    ovr = overport_libraries()
                    o.write(VRAPI_BRIDGE, "lib/arm64-v8a/libfp_vrapi64.so")
                    o.write(ovr / "libopenxr_loader.so", "lib/arm64-v8a/libopenxr_loader.so")
                    o.write(ovr / "libopenxr_loader_generic.so", "lib/arm64-v8a/libopenxr_loader_original.so")
                    o.write(FRAMEBRIDGE, "lib/arm64-v8a/libopenxr_loader_generic.so")
                    o.writestr("lib/arm64-v8a/liboverport.config.so", OVERPORT_CONFIG)
                    o.writestr("lib/arm64-v8a/libframe_settings.so", FRAME_SETTINGS)
                o.write(OUT / "liba32bridge.so", "lib/arm64-v8a/liba32bridge.so")
                o.write(OUT / "liba32rt.so", "lib/arm64-v8a/liba32rt.so")
            sign.sign(unsigned, Path(a.out), package)
    print(f"{a.out}: {package}, {len(libs)} libraries from {abi}")


if __name__ == "__main__":
    main()
