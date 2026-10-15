#!/usr/bin/env python3
"""Checks the JIT's A64 encoders (src/a64.h) against llvm-mc. Builds tests/unit/a64_enc.c with the host cc."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    exe = Path(tempfile.mkdtemp()) / "a64_enc"
    subprocess.run(["cc", "-O1", "-Wall", str(HERE / "unit/a64_enc.c"), "-o", str(exe)], check=True)
    lines = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines()
    cases = [l.split("\t") for l in lines if not l.startswith("#")]
    for l in lines:
        if l.startswith("#"):
            print(l)
    src = "\n".join(c[0] for c in cases) + "\n"
    p = subprocess.run(["llvm-mc", "-triple=aarch64", "-mattr=+v8a,+fp-armv8,+neon", "-show-encoding"], input=src,
                       capture_output=True, text=True)
    if p.returncode:
        print(p.stderr)
        sys.exit(1)
    encs = []
    for line in p.stdout.splitlines():
        m = re.search(r"encoding: \[([^\]]*)\]", line)
        if m:
            b = [int(x, 16) for x in m.group(1).split(",")]
            encs.append(b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24)
    if len(encs) != len(cases):
        print(f"llvm-mc gave {len(encs)} encodings for {len(cases)} cases")
        sys.exit(1)
    bad = 0
    for (text, ours), want in zip(cases, encs):
        if int(ours, 16) != want:
            bad += 1
            print(f"MISMATCH {text:40s} ours {ours} llvm {want:08x}")
    print(f"{len(cases) - bad}/{len(cases)} encodings match")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
