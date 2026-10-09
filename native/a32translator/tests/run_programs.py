#!/usr/bin/env python3
"""Builds the guest test programs (NDK armv7a clang, static bionic) in ARM and Thumb variants and compares the
output of a32run (JIT and --interp) with qemu-arm's.

    python tests/run_programs.py [--only basic,neon] [--modes jit,interp] [--runner "qemu-aarch64"] [--keep]

The host side runs a32run under qemu-aarch64 on x86_64 hosts (or natively on AArch64 with --runner ""). Every run
happens in a fresh user+pid namespace (unshare -Urpf): 32-bit bionic refuses pids above 65535.
"""
import argparse
import hashlib
import shlex
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NDK_BIN = ROOT.parent / ".cache/ndk-27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin"
BUILD = ROOT / "out" / "tests"
VARIANTS = {"arm": ["-marm", "-O2"], "thumb": ["-mthumb", "-O2"], "thumb-O0": ["-mthumb", "-O0"]}
PROGRAMS = {  # name -> (source, variants, args)
    "basic": ("basic.c", ["arm", "thumb", "thumb-O0"], []),
    "memstr": ("memstr.c", ["arm", "thumb"], []),
    "neon": ("neon.c", ["arm", "thumb"], []),
    "threads": ("threads.c", ["arm", "thumb"], []),
    "signals": ("signals.c", ["arm", "thumb"], []),
    "files": ("files.c", ["thumb"], []),
    "cxx": ("cxx.cpp", ["arm", "thumb"], []),
    "jitguest": ("jitguest.c", ["arm", "thumb"], []),
    "exec": ("exec.c", ["arm", "thumb"], []),
    "bench": ("bench.c", ["arm", "thumb"], []),
}
JIT_ONLY = {"bench"}  # far too slow for the interpreter under emulation


def build(name, src, variant):
    out = BUILD / f"{name}-{variant}"
    srcp = HERE / "programs" / src
    if out.exists() and out.stat().st_mtime > srcp.stat().st_mtime:
        return out
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = NDK_BIN / ("armv7a-linux-androideabi29-clang++" if src.endswith(".cpp") else "armv7a-linux-androideabi29-clang")
    cmd = [str(cc), *VARIANTS[variant], "-mfpu=neon-vfpv4", "-static", "-g", str(srcp), "-lm", "-o", str(out)]
    if src.endswith(".cpp"):
        cmd += ["-static-libstdc++", "-std=c++17"]
    subprocess.run(cmd, check=True)
    return out


def no_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # qemu-user writes guest core files into the cwd


def run(cmd, timeout):
    t = time.time()
    try:
        p = subprocess.run(["unshare", "-Urpf", "--kill-child", *cmd], capture_output=True, timeout=timeout,
                           preexec_fn=no_core)
        return p.returncode, p.stdout, p.stderr, time.time() - t
    except subprocess.TimeoutExpired:
        return "timeout", b"", b"", time.time() - t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--modes", default="jit,interp")
    ap.add_argument("--runner", default="qemu-aarch64")
    ap.add_argument("--a32run", default=str(ROOT / "out" / "a32run"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    names = a.only.split(",") if a.only else list(PROGRAMS)
    runner = shlex.split(a.runner)
    fails = 0
    for name in names:
        src, variants, args = PROGRAMS[name]
        for v in variants:
            exe = build(name, src, v)
            cache = BUILD / f"{exe.name}.{hashlib.sha1(exe.read_bytes()).hexdigest()[:12]}.expected"
            if cache.exists():
                exp_rc, exp_out = cache.read_bytes().split(b"\n", 1)
                exp_rc = int(exp_rc)
            else:
                exp_rc, exp_out, err, _ = run(["qemu-arm", str(exe), *args], a.timeout)
                if exp_rc == "timeout":
                    print(f"SKIP {name}-{v}: qemu-arm timed out (no reference output)")
                    continue
                cache.write_bytes(f"{exp_rc}\n".encode() + exp_out)
            for mode in a.modes.split(","):
                if mode == "interp" and name in JIT_ONLY:
                    continue
                flags = ["--interp"] if mode == "interp" else []
                rc, out, err, dt = run([*runner, a.a32run, *flags, str(exe), *args], a.timeout)
                ok = rc == exp_rc and out == exp_out
                fails += not ok
                print(f"{'PASS' if ok else 'FAIL'} {name}-{v} [{mode}] {dt:.2f}s", flush=True)
                if not ok or a.v:
                    if rc != exp_rc:
                        print(f"  exit {rc}, expected {exp_rc}")
                    if out != exp_out:
                        el, gl = exp_out.decode(errors="replace").splitlines(), out.decode(errors="replace").splitlines()
                        for k in range(max(len(el), len(gl))):
                            e = el[k] if k < len(el) else "<missing>"
                            g = gl[k] if k < len(gl) else "<missing>"
                            if e != g:
                                print(f"  line {k + 1}: expected {e!r}\n           got      {g!r}")
                                break
                    if err:
                        print("  stderr: " + err.decode(errors="replace")[-1500:].replace("\n", "\n  "))
    print(f"{fails} failure(s)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
