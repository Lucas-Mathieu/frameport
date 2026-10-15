#!/usr/bin/env python3
"""Disassembles an A32_DUMP file written by a32run: guest instructions (arm-none-eabi-objdump of the guest
binary) interleaved with the host A64 code (llvm-mc).

    A32_DUMP=/tmp/d.txt a32run prog; python tools/jitdump.py /tmp/d.txt prog [--pc 1d7a0]
"""
import argparse
import re
import subprocess


def guest_disasm(exe):
    out = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", exe], capture_output=True,
                         text=True).stdout
    res = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if m:
            res[int(m.group(1), 16)] = m.group(2).strip()
    return res


def host_disasm(words):
    src = "\n".join(" ".join(f"0x{(w >> s) & 0xff:02x}" for s in (0, 8, 16, 24)) for w in words)
    out = subprocess.run(["llvm-mc", "-disassemble", "-triple=aarch64", "-mattr=+v8a,+neon,+crypto"], input=src,
                         capture_output=True, text=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith(".text")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("exe")
    ap.add_argument("--pc", help="only the block starting at this guest pc (hex)")
    a = ap.parse_args()
    g = guest_disasm(a.exe)
    blocks, cur = [], None
    for line in open(a.dump):
        line = line.strip()
        if line.startswith("block"):
            _, pc, thumb, n = line.split()
            cur = {"pc": int(pc, 16), "thumb": thumb, "items": []}
            blocks.append(cur)
        elif line.startswith("@"):
            cur["items"].append(("g", int(line[2:], 16)))
        else:
            cur["items"].append(("h", int(line, 16)))
    for b in blocks:
        if a.pc and b["pc"] != int(a.pc, 16):
            continue
        hw = [v for k, v in b["items"] if k == "h"]
        dis = iter(host_disasm(hw))
        print(f"== block {b['pc']:08x} ({'Thumb' if b['thumb'] == '1' else 'ARM'}), {len(hw)} host instructions")
        for k, v in b["items"]:
            if k == "g":
                print(f"  {v:08x}: {g.get(v, '?')}")
            else:
                print(f"        {next(dis, '?')}")


if __name__ == "__main__":
    main()
