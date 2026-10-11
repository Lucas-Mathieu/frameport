#!/usr/bin/env python3
"""Converts a 32-bit-only APK to run on arm64 through the bridge, for bridge development (FramePort builds do the
same with the patch frame.a32_bridge, src/frameport/patches/frame/a32_bridge.py, which this script uses):

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
REPO = HERE.parents[2]
# test builds instead of the artifacts: A32_VRAPI_BRIDGE (the 64-bit VrApi bridge), A32_FRAME_SETTINGS (FrameBridge
# settings, "key=value;..."); the bridge itself always comes from out/bridge (this script is for bridge development)
VRAPI_BRIDGE = Path(os.environ.get("A32_VRAPI_BRIDGE") or REPO / "artifacts/arm64-v8a/libvrapi.so")
FRAME_SETTINGS = (b"scale=1.0\nfoveation_fix=1\ncontroller_fix=1\n"
                  + os.environ.get("A32_FRAME_SETTINGS", "").replace(";", "\n").encode())

def extract_original_cert(apk_path: Path) -> bytes:
    """Trekt de binaire inhoud van het originele RSA/DSA/EC certificaat uit de APK."""
    with zipfile.ZipFile(apk_path, 'r') as z:
        cert_files = [f for f in z.namelist() if f.startswith('META-INF/') and f.endswith(('.RSA', '.DSA', '.EC'))]
        if cert_files:
            # Neem het eerste certificaatbestand dat we tegenkomen
            return z.read(cert_files[0])
    return b""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("out")
    ap.add_argument("--replace", action="append", default=[], metavar="LIB=FILE")
    a = ap.parse_args()
    sys.path.insert(0, str(REPO / "src"))
    from pyaxmlparser import APK

    from frameport.apk import axml, sign
    from frameport.apk.workspace import ApkWorkspace
    from frameport.core.events import Reporter
    from frameport.patches.frame import a32_bridge
    from frameport.tools import overport

    package = APK(a.apk).package
    replace = {k: Path(v).read_bytes() for k, v in (r.split("=", 1) for r in a.replace)}

    def prebuilt(name: str) -> bytes:
        if name == "libvrapi.so":
            return VRAPI_BRIDGE.read_bytes()
        return (OUT / name if name.startswith("liba32") else REPO / "artifacts/arm64-v8a" / name).read_bytes()

    def ovrport(name: str, abi: str) -> bytes:
        return overport.ensure_runtime_lib(name, abi, Reporter()).read_bytes()

    with ApkWorkspace(Path(a.apk)) as ws, tempfile.TemporaryDirectory() as td:
        # ---- DYNAMISCH CERTIFICAAT EXTRAHEREN ----
        orig_cert_bytes = extract_original_cert(Path(a.apk))
        if orig_cert_bytes:
            ws.put("assets/orig_cert.bin", orig_cert_bytes)
            print(f"[FramePort] Dynamisch origineel certificaat toegevoegd aan assets ({len(orig_cert_bytes)} bytes)")
        else:
            print("[FramePort] WAARSCHUWING: Geen origineel certificaat gevonden in META-INF!")
        # ------------------------------------------

        libs = a32_bridge.convert(ws, prebuilt, ovrport, replace)
        manifest = ws.read("AndroidManifest.xml")
        ws.put("AndroidManifest.xml", axml.fix_launcher(manifest) or manifest)  # Lepton starts the LAUNCHER activity
        if ws.has(ws.lib("libopenxr_loader_generic.so")):  # FrameBridge's settings (frame.adapter in a build)
            ws.put(ws.lib("libframe_settings.so"), FRAME_SETTINGS)
        unsigned = Path(td) / "unsigned.apk"
        ws.write(unsigned)
        sign.sign(unsigned, Path(a.out), package)
    print(f"{a.out}: {package}, {len(libs)} libraries converted")

if __name__ == "__main__":
    main()
