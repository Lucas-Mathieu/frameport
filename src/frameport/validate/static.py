"""Offline checks on a built APK (no device needed)."""
from __future__ import annotations

import zipfile
from pathlib import Path

from ..analysis import elf
from ..analysis.detect import missing_ovr_symbols
from ..apk import axml, sign

# Libraries Android/Lepton provides (a NEEDED entry outside the APK must be one of these).
SYSTEM_LIBS = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
    "libGLESv3.so", "libvulkan.so", "libOpenSLES.so", "libOpenMAXAL.so", "libaaudio.so", "libmediandk.so", "libz.so",
    "libjnigraphics.so", "libcamera2ndk.so", "libnativewindow.so", "libstdc++.so", "libbinder_ndk.so", "libsync.so",
    "libneuralnetworks.so", "libamidi.so", "libicu.so", "libdl_android.so", "libsurfaceflinger.so",
}


def check_apk(apk: Path, package: str | None = None, expect_adapter: bool = True) -> list[dict]:
    """Returns [{name, ok (True/False/None=warning), detail}]."""
    checks: list[dict] = []

    def add(name, ok, detail=""):
        checks.append({"name": name, "ok": ok, "detail": detail})

    ok, text = sign.verify(apk)
    add("Signature (v1/v2/v3)", ok, "" if ok else text[-300:])
    problems = sign.alignment_problems(apk)
    add("Zip alignment (4 B, .so 16 KiB)", not problems, "; ".join(problems[:3]))
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        manifest = z.read("AndroidManifest.xml")
        abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 2})
        abi = next((a for a in ("arm64-v8a", "armeabi-v7a") if a in abis), None)
        add("64-bit (arm64-v8a) libraries", "arm64-v8a" in abis or not abis,
            "32-bit only: the Steam Frame has no AArch32 support" if abi == "armeabi-v7a" else ", ".join(abis))
        libs = {}
        if abi:
            prefix = f"lib/{abi}/"
            for n in names:
                if n.startswith(prefix) and n.endswith(".so"):
                    info = z.getinfo(n)
                    data = z.read(info) if info.file_size < 400 * 2**20 else b""
                    libs[n[len(prefix):]] = data
            stored = [n for n in names if n.startswith(prefix) and z.getinfo(n).compress_type != zipfile.ZIP_STORED]
            add("Native libraries stored (uncompressed)", None if stored else True,
                f"{len(stored)} compressed (allowed, but slower)" if stored else "")
    try:
        cats = axml.categories(manifest)
        add("Manifest parses", True)
        add("Launcher activity (category LAUNCHER)", axml.LAUNCHER in cats, "Lepton needs category LAUNCHER")
    except Exception as exc:  # noqa: BLE001
        add("Manifest parses", False, str(exc))
    if abi:
        unresolved = set()
        for data in libs.values():
            if elf.is_elf(data):
                unresolved |= {n for n in elf.needed(data) if n not in libs and n not in SYSTEM_LIBS}
        add("Library dependencies resolvable", None if unresolved else True,
            ("not in the APK or the system list: " + ", ".join(sorted(unresolved))) if unresolved else "")
        missing = missing_ovr_symbols({k: v for k, v in libs.items() if elf.is_elf(v)})
        add("Meta platform functions resolvable", not missing, ", ".join(sorted(missing)[:6]))
        vrapi = libs.get("libvrapi.so")
        if vrapi and elf.is_elf(vrapi):  # BlazeRush needed 4 the VrApi bridge lacked (GitHub #57): no start at all
            have = elf.dyn_symbols(vrapi, True)
            lacking = sorted({s for n, d in libs.items() if n != "libvrapi.so" and elf.is_elf(d)
                              for s in elf.dyn_symbols(d, False) if s.startswith("vrapi_")} - have)
            add("VrApi functions resolvable", not lacking, ", ".join(lacking[:6]))
        if expect_adapter:
            have = {"libopenxr_loader_generic.so", "libopenxr_loader_original.so", "libframe_settings.so"} <= set(libs)
            add("FrameBridge adapter", have, "" if have else "adapter/original loader/settings missing")
    if package:
        from ..tools import overport as ov

        ks = ov.keystore(package)
        add("Signing key kept for updates", ks.exists(), str(ks))
    return checks
