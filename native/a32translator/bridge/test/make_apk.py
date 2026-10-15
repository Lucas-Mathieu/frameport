#!/usr/bin/env python3
"""Builds the on-device test APK org.frameport.a32test: a NativeActivity without Java code whose library is 32-bit
ARM code (libapktest.a32.so) run by the bridge. Build the bridge first (python bridge/build.py --test).

    uv run python native/a32translator/bridge/test/make_apk.py [--out FILE]

The APK is signed with FramePort's key for the package (created on first use). Install it with
`frameport install org.frameport.a32test --apk FILE` after `frameport scan` of its folder, or by hand.
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "out" / "bridge"
NDK_BIN = ROOT.parent / ".cache/ndk-27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin"
PACKAGE = "org.frameport.a32test"
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ATTR_IDS = {  # android: attributes used here (framework resource IDs)
    "label": 0x01010001, "name": 0x01010003, "hasCode": 0x0101000C, "debuggable": 0x0101000F,
    "exported": 0x01010010, "value": 0x01010024, "minSdkVersion": 0x0101020C, "versionCode": 0x0101021B,
    "versionName": 0x0101021C, "targetSdkVersion": 0x01010270, "extractNativeLibs": 0x010104EA,
}
T_STRING, T_INT, T_BOOL = 0x03, 0x10, 0x12

# (element, [(attribute, value)], children); android attributes are the ATTR_IDS keys, others have no namespace
MANIFEST = ("manifest", [("package", PACKAGE), ("versionCode", 1), ("versionName", "1.0")], [
    ("uses-sdk", [("minSdkVersion", 24), ("targetSdkVersion", 30)], []),
    ("application", [("label", "A32 bridge test"), ("hasCode", False), ("debuggable", True),
                     ("extractNativeLibs", True)], [
        ("activity", [("name", "android.app.NativeActivity"), ("label", "A32 bridge test"), ("exported", True)], [
            ("meta-data", [("name", "android.app.lib_name"), ("value", "apktest")], []),
            ("intent-filter", [], [
                ("action", [("name", "android.intent.action.MAIN")], []),
                ("category", [("name", "android.intent.category.LAUNCHER")], []),
            ]),
        ]),
    ]),
])


def axml(root) -> bytes:
    """Android binary XML for the tree above (UTF-8 string pool; attribute names with IDs first, as aapt does)."""
    attr_names = sorted({a for _, attrs, _ in walk(root) for a, _ in attrs if a in ATTR_IDS}, key=ATTR_IDS.get)
    strings = list(attr_names)

    def idx(s):
        if s not in strings:
            strings.append(s)
        return strings.index(s)

    for s in ("android", ANDROID_NS):
        idx(s)
    body = bytearray()
    ns_chunk = struct.pack("<IIII", 1, 0xFFFFFFFF, idx("android"), idx(ANDROID_NS))
    body += struct.pack("<HHI", 0x0100, 16, 24) + ns_chunk

    def emit(node):
        name, attrs, children = node
        recs = []
        for a, v in sorted(attrs, key=lambda av: ATTR_IDS.get(av[0], 0)):
            ns = idx(ANDROID_NS) if a in ATTR_IDS else 0xFFFFFFFF
            if isinstance(v, bool):
                raw, typ, data = 0xFFFFFFFF, T_BOOL, 0xFFFFFFFF if v else 0
            elif isinstance(v, int):
                raw, typ, data = 0xFFFFFFFF, T_INT, v
            else:
                raw, typ, data = idx(v), T_STRING, idx(v)
            recs.append(struct.pack("<IIIHBBI", ns, idx(a), raw, 8, 0, typ, data))
        n = idx(name)
        start = struct.pack("<IIIIHHHHHH", 1, 0xFFFFFFFF, 0xFFFFFFFF, n, 20, 20, len(recs), 0, 0, 0)
        body.extend(struct.pack("<HHI", 0x0102, 16, 8 + len(start) + 20 * len(recs)) + start + b"".join(recs))
        for c in children:
            emit(c)
        body.extend(struct.pack("<HHI", 0x0103, 16, 24) + struct.pack("<IIII", 1, 0xFFFFFFFF, 0xFFFFFFFF, n))

    emit(root)
    body += struct.pack("<HHI", 0x0101, 16, 24) + ns_chunk

    data = bytearray()
    offsets = []
    for s in strings:
        offsets.append(len(data))
        b = s.encode()
        assert len(s) < 0x80 and len(b) < 0x80
        data += bytes([len(s), len(b)]) + b + b"\0"
    while len(data) % 4:
        data += b"\0"
    hdr = 28
    pool = struct.pack("<HHIIIIII", 0x0001, hdr, hdr + 4 * len(strings) + len(data), len(strings), 0, 0x100,
                       hdr + 4 * len(strings), 0)
    pool += struct.pack(f"<{len(offsets)}I", *offsets) + data
    resmap = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(attr_names)) + b"".join(
        struct.pack("<I", ATTR_IDS[a]) for a in attr_names)
    total = 8 + len(pool) + len(resmap) + len(body)
    return struct.pack("<HHI", 0x0003, 8, total) + pool + resmap + bytes(body)


def walk(node):
    yield node
    for c in node[2]:
        yield from walk(c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(OUT / "a32test.apk"))
    a = ap.parse_args()
    test = OUT / "test"
    cc = NDK_BIN / "armv7a-linux-androideabi30-clang"
    subprocess.run([str(cc), "-O2", "-g", "-mthumb", "-shared", "-fPIC", str(HERE / "libapktest.c"), "-landroid",
                    "-llog", "-ldl", "-o", str(test / "libapktest.a32.so")], check=True)
    libs = {
        "libapktest.so": OUT / "liba32stub.so",  # the stub, under the 32-bit library's name
        "liba32bridge.so": OUT / "liba32bridge.so",
        "liba32rt.so": OUT / "liba32rt.so",
        "libapktest.a32.so": test / "libapktest.a32.so",
        "libbtest3.a32.so": test / "libbtest3.a32.so",
    }
    manifest = axml(MANIFEST)
    sys.path.insert(0, str(ROOT.parents[1] / "src"))
    from frameport.apk import axml as fp_axml
    from frameport.apk import sign

    x = fp_axml.Axml(manifest)  # FramePort's own parser must read it
    assert fp_axml.MAIN in x.strings() and fp_axml.LAUNCHER in x.strings()
    assert fp_axml.categories(manifest) == {fp_axml.LAUNCHER}
    with tempfile.TemporaryDirectory() as td:
        unsigned = Path(td) / "unsigned.apk"
        with zipfile.ZipFile(unsigned, "w") as z:
            z.writestr(zipfile.ZipInfo("AndroidManifest.xml", (2026, 1, 1, 0, 0, 0)), manifest, zipfile.ZIP_DEFLATED)
            for name, src in libs.items():
                z.write(src, f"lib/arm64-v8a/{name}", zipfile.ZIP_STORED)
        sign.sign(unsigned, Path(a.out), PACKAGE)
    print(a.out)


if __name__ == "__main__":
    main()
