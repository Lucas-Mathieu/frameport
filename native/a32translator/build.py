#!/usr/bin/env python3
"""Builds a32run (the AArch32 -> AArch64 translator) with the NDK's clang as a static AArch64 executable.

    python native/a32translator/build.py [--debug] [--ndk PATH] [--out DIR]

The NDK comes from native/.cache (python native/build.py downloads it). The binary is static bionic, linked at a
high image base so the guest's low 4 GiB stay free; it runs on any AArch64 Linux (SteamOS, Android) or under
qemu-aarch64.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NDK_DEFAULT = HERE.parent / ".cache" / "ndk-27.2.12479018"
SOURCES = ["main.c", "dispatch.c", "mem.c", "loader.c", "syscall.c", "signals.c", "threads.c", "interp.c",
           "interp_vfp.c", "interp_neon.c", "a32_decode.c", "t32_decode.c", "vfp_neon_decode.c", "jit.c",
           "jit_vfp.c", "jit_glue.S"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--ndk", default=str(NDK_DEFAULT))
    ap.add_argument("--out", default=str(HERE / "out"))
    a = ap.parse_args()
    cc = Path(a.ndk) / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    if not cc.exists():
        sys.exit(f"{cc} not found (run python native/build.py once to fetch the NDK)")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    srcs = [HERE / "src" / s for s in SOURCES if (HERE / "src" / s).exists()]
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
             "-march=armv8-a+crypto", f"-ffile-prefix-map={HERE}=a32translator", "-fno-strict-aliasing"]
    flags += ["-O0", "-g"] if a.debug else ["-O2", "-g"]
    cmd = [str(cc), *flags, *map(str, srcs), "-static", "-Wl,--image-base=0x7000000000", "-lm",
           "-o", str(out / "a32run")]
    print(" ".join(cmd[:1] + ["..."] + cmd[-2:]))
    r = subprocess.run(cmd)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
