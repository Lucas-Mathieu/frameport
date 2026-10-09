#!/usr/bin/env python3
"""Build the Android media-service plugin on Linux with NDK r27c and a Lepton rootfs.

The runtime's private SoftOMX ABI is deliberately pinned. This plugin is mounted
only when the tested library fingerprint matches; it never replaces system files.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARTIFACTS = HERE.parents[1] / "artifacts/hevc"
FFMPEG_SHA = "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"
RUNTIME_SHA = "456e912c75cd389abcf6a63bc80e2a53bdc334371d00b200c93680388ae955e2"
NDK_REVISION = "27.2.12479018"


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--lepton-root", type=Path, required=True)
    args = parser.parse_args()
    ndk_root = args.ndk.resolve()
    if not any(line.strip() == f"Pkg.Revision = {NDK_REVISION}"
               for line in (ndk_root / "source.properties").read_text().splitlines()):
        parser.error(f"use Android NDK r27c ({NDK_REVISION})")
    ndk = ndk_root / "toolchains/llvm/prebuilt/linux-x86_64"
    if not (ndk / "bin/clang++").exists():
        parser.error("use the Linux NDK r27c (Windows: run this builder inside WSL)")
    runtime = args.lepton_root.resolve()
    if hashlib.sha256((runtime / "vendor/lib64/libstagefright_softomx.so").read_bytes()).hexdigest() != RUNTIME_SHA:
        parser.error("unverified SoftOMX ABI; validate and update the fingerprint before rebuilding")
    cache = HERE.parent / ".cache/hevc"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "ffmpeg-7.1.1.tar.xz"
    if not archive.exists():
        urllib.request.urlretrieve("https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz", archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != FFMPEG_SHA:
        raise RuntimeError("FFmpeg source checksum mismatch")
    with tarfile.open(archive) as tar:
        tar.extractall(cache, filter="data")
    source = cache / "ffmpeg-7.1.1"
    install = cache / "ffmpeg-install"
    # Discard previous objects/configuration so a host compiler or changed
    # configure flags cannot silently survive a rebuild.
    if (source / "config.mak").exists():
        run(["make", "distclean"], cwd=source)
    if install.exists():
        shutil.rmtree(install)
    env = dict(os.environ, PATH=str(ndk / "bin") + os.pathsep + os.environ["PATH"])
    prefix_maps = [f"-ffile-prefix-map={HERE.parents[1]}=.", f"-ffile-prefix-map={ndk_root}=android-ndk-r27c",
                   f"-ffile-prefix-map={runtime}=lepton-rootfs"]
    run([
        source / "configure", f"--prefix={install}", "--target-os=android", "--arch=aarch64",
        "--enable-cross-compile", "--cc=aarch64-linux-android30-clang", "--cxx=aarch64-linux-android30-clang++",
        "--ld=aarch64-linux-android30-clang", "--ar=llvm-ar", "--nm=llvm-nm", "--ranlib=llvm-ranlib",
        "--extra-cflags=" + " ".join(prefix_maps),
        "--extra-cxxflags=" + " ".join(prefix_maps),
        "--disable-everything", "--disable-autodetect", "--enable-v4l2-m2m", "--disable-programs",
        "--disable-doc", "--enable-pic", "--enable-static", "--disable-shared",
        "--enable-decoder=hevc_v4l2m2m", "--enable-parser=hevc", "--enable-bsf=hevc_mp4toannexb",
        "--enable-demuxer=mov", "--enable-protocol=file", "--enable-avcodec", "--enable-avformat",
        "--enable-avutil", "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
        "--disable-swresample", "--disable-postproc",
    ], cwd=source, env=env)
    # FFmpeg embeds its configure command as a runtime diagnostic string.
    # Prefix-map flags cannot rewrite string literals, so normalize that
    # generated string too, retaining the options without host paths.
    config = source / "config.h"
    text = config.read_text()
    for path, replacement in ((ndk_root, "android-ndk-r27c"), (runtime, "lepton-rootfs"), (HERE.parents[1], ".")):
        text = text.replace(str(path), replacement)
    config.write_text(text)
    run(["make", "-j4"], cwd=source, env=env)
    run(["make", "install"], cwd=source, env=env)
    # Use the platform libc++ namespace. Do not change the NDK's own headers or
    # distribute another libc++, which would create a conflicting private ABI.
    cpp = cache / "cpp"
    shutil.copytree(ndk / "sysroot/usr/include/c++/v1", cpp, dirs_exist_ok=True)
    site = cpp / "__config_site"
    site.write_text(site.read_text().replace("_LIBCPP_ABI_NAMESPACE __ndk1", "_LIBCPP_ABI_NAMESPACE __1"))
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    run([
        ndk / "bin/aarch64-linux-android30-clang++", "-std=gnu++17", "-O3", "-fPIC", "-shared",
        *prefix_maps,
        "-fno-rtti", "-fno-exceptions", "-nostdlib++", "-nostdinc++", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-private-field", "-isystem", cpp, "-I", HERE / "platform",
        "-I", HERE / "platform/media/openmax", "-I", install / "include", HERE / "frameport_hevc.cpp",
        "-L", install / "lib", "-lavcodec", "-lavutil", "-L", runtime / "vendor/lib64",
        "-L", runtime / "system/lib64", "-lstagefright_softomx", "-lstagefright_foundation", "-lutils",
        "-llog", "-lnativewindow", "-lyuv", "-l:libc++.so", "-lm", "-ldl", "-Wl,--no-undefined",
        "-Wl,-z,max-page-size=16384",
        "-Wl,-soname,libstagefrighthw.so", "-o", ARTIFACTS / "libstagefrighthw.so",
    ], env=env)
    for name in ("podman.py", "media_codecs_frameport.xml"):
        shutil.copyfile(HERE / name, ARTIFACTS / (name + ".txt" if name == "podman.py" else name))
    shutil.copyfile(source / "COPYING.LGPLv2.1", ARTIFACTS / "COPYING.FFmpeg")
    files = {}
    for name in ("libstagefrighthw.so", "podman.py", "media_codecs_frameport.xml", "COPYING.FFmpeg"):
        path = ARTIFACTS / (name + ".txt" if name == "podman.py" else name)
        files[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = {"runtime_sha256": RUNTIME_SHA, "files": files,
                "build": {"ndk_revision": NDK_REVISION, "ffmpeg_source_sha256": FFMPEG_SHA}}
    (ARTIFACTS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
