#!/usr/bin/env python3
"""Runs the bridge test harness (build it first: python bridge/build.py --test).

    python bridge/test/run.py [--runner "qemu-aarch64"] [--interp] [--strace] [--timeout 300]

On x86_64 the harness runs under qemu-aarch64; on an AArch64 Linux machine pass --runner "". Like the program tests,
it runs in a fresh user+pid namespace (32-bit bionic refuses pids above 65535). Exit code 0 = every check passed.
"""
import argparse
import os
import resource
import shlex
import subprocess
import sys
from pathlib import Path

DIR = Path(__file__).resolve().parents[2] / "out" / "bridge" / "test"


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # qemu-user hangs writing a multi-threaded core


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", default="qemu-aarch64")
    ap.add_argument("--interp", action="store_true", help="interpreter only (A32_INTERP=1)")
    ap.add_argument("--strace", action="store_true", help="log guest syscalls (A32_STRACE=1)")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()
    if not (DIR / "harness").exists():
        sys.exit(f"{DIR / 'harness'} missing: run python bridge/build.py --test")
    env = dict(os.environ)
    if a.interp:
        env["A32_INTERP"] = "1"
    if a.strace:
        env["A32_STRACE"] = "1"
    cmd = ["timeout", "-s", "KILL", str(a.timeout), "unshare", "-Urpf", "--kill-child", *shlex.split(a.runner),
           "./harness", "."]
    p = subprocess.run(cmd, cwd=DIR, env=env, preexec_fn=no_core)
    sys.exit(p.returncode)


if __name__ == "__main__":
    main()
