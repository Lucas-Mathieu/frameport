#!/usr/bin/env python3
"""Differential instruction fuzzer: a32run (interpreter and JIT) against qemu-arm.

Random encodings of one instruction class are disassembled with arm-none-eabi-objdump; UNPREDICTABLE ones and
those that branch or write SP/PC are dropped, memory operations get base/index registers that point into a scratch
area. Each test is assembled into a harness that loads random r0-r12/r14, APSR (NZCVQ, GE), FPSCR and d0-d31,
executes the instruction (Thumb ones optionally inside an IT block), then dumps all of that state plus a hash of the
scratch memory. The harness runs under qemu-arm (reference) and a32run; differing tests are reported with their
disassembly and the differing fields.

    python tests/fuzz.py --class arm-dp --count 2000 [--seed 1] [--modes interp,jit] [--runner qemu-aarch64]
    python tests/fuzz.py --class all --count 500
"""
import argparse
import os
import random
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

# (instruction set, mask, value, weight); remaining bits random
CLASSES = {
    "arm-dp": ("arm", [(0x0c000000, 0x00000000)]),
    "arm-ldst": ("arm", [(0x0e000000, 0x04000000), (0x0e000010, 0x06000000)]),
    "arm-media": ("arm", [(0x0e000010, 0x06000010)]),
    "arm-ldm": ("arm", [(0x0e000000, 0x08000000)]),
    "arm-vfp": ("arm", [(0x0f000e10, 0x0e000a00), (0x0f000e10, 0x0e000a10), (0x0e000e00, 0x0c000a00)]),
    "arm-neon": ("arm", [(0xfe000000, 0xf2000000)]),
    "arm-neon-ldst": ("arm", [(0xff100000, 0xf4000000)]),
    "arm-v8": ("arm", [(0xff000e10, 0xfe000a00)]),
    "t16": ("thumb16", [(0x0000, 0x0000)]),
    "t32": ("thumb32", [(0xe0000000, 0xe0000000)]),
    "t32-neon": ("thumb32", [(0xef000000, 0xef000000)]),
    "t32-vfp": ("thumb32", [(0xef000e10, 0xee000a00), (0xef000e10, 0xee000a10), (0xee000e00, 0xec000a00)]),
}

BRANCHY = re.compile(r"^(b|bl|blx|bx|bxj|cbz|cbnz|tbb|tbh|svc|bkpt|udf|hlt|smc|hvc|eret|rfe\w*|srs\w*|cps\w*|"
                     r"setend|wfi|wfe|sev|sevl|it\w*|dbg|mcr2?|mrc2?|mcrr2?|mrrc2?|ldc2?l?|stc2?l?|cdp2?|"
                     r"subs|movs)(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.[nw])?$")
NO_SP_PC = re.compile(r"\b(pc|r15)\b")

SCRATCH_SIZE = 16384
SCRATCH_BASE = 0x400000  # fixed (section .fuzzbss), so dropping a test does not move it
SCRATCH_MID = SCRATCH_BASE + SCRATCH_SIZE // 2
NSTATE = 14 + 2 + 64  # r0-r12, r14, apsr, fpscr, d0-d31 (words)
RECORD = 4 + NSTATE * 4 + 4  # index, state, scratch hash


def tool(name):
    return "arm-none-eabi-" + name


def random_word(rng, cls):
    iset, pats = CLASSES[cls]
    mask, val = rng.choice(pats)
    if iset == "thumb16":
        while True:
            w = rng.getrandbits(16)
            if (w >> 11) not in (0x1d, 0x1e, 0x1f) and (w & 0xff00) != 0xbf00 or (w & 0xff0f) == 0xbf00:
                return w
    w = (rng.getrandbits(32) & ~mask) | val
    if iset == "arm" and val >> 28 != 0xf:
        # mostly unconditional, some conditional
        cond = 0xe if rng.random() < 0.75 else rng.randrange(0, 14)
        w = (w & 0x0fffffff) | cond << 28
    if iset == "thumb32" and (w >> 27) not in (0x1d, 0x1e, 0x1f):
        w |= 0xe8000000
    return w


def disassemble(words, iset, workdir):
    """objdump -d of raw words; returns list of (text) per word ('' if unknown)."""
    src = workdir / "dis.s"
    lines = [".syntax unified", ".thumb" if iset.startswith("thumb") else ".arm"]
    for w in words:
        if iset == "thumb16":
            lines.append(f".inst.n 0x{w:04x}")
        elif iset == "thumb32":
            lines.append(f".inst.w 0x{w:08x}")
        else:
            lines.append(f".inst 0x{w:08x}")
    src.write_text("\n".join(lines) + "\n")
    obj = workdir / "dis.o"
    subprocess.run([tool("as"), "-march=armv8-a+crc+simd+crypto", "-mfpu=crypto-neon-fp-armv8", str(src), "-o",
                    str(obj)], check=True)
    out = subprocess.run([tool("objdump"), "-d", "-M", "force-thumb" if iset.startswith("thumb") else "no-force-thumb",
                          str(obj)], capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{4,8} ?)+)\s+(.*)$", line)
        if m:
            res.append(m.group(2).strip())
    return res


MEM_RE = re.compile(r"\[(r\d+|sp|ip|fp|lr|pc)(?:, *#?(-?)(r\d+|ip|fp|lr|0x[0-9a-f]+|\d+)?(?:, *(lsl|lsr|asr|ror) *#(\d+))?)?\]"
                    r"(!)?(?:, *(-?)(r\d+|ip|fp|lr|#-?\d+|#0x[0-9a-f]+)(?:, *(lsl|lsr|asr|ror) *#(\d+))?)?")
REGMAP = {"ip": 12, "fp": 11, "lr": 14, "sp": 13, "pc": 15}


def regnum(r):
    return REGMAP.get(r, int(r[1:]) if r.startswith("r") and r[1:].isdigit() else -1)


def classify(text):
    """Returns None to drop, else dict(mem=bool, base=[regs], index=[regs], align=int)."""
    if not text or "UNDEFINED" in text or "unpredictable" in text.lower() or "illegal" in text or "undefined" in text:
        return None
    if "<" in text and "UNPRED" in text.upper():
        return None
    parts = text.split(None, 1)
    mn = parts[0]
    ops = parts[1].split(";")[0].strip() if len(parts) > 1 else ""
    if BRANCHY.match(mn):
        return None
    # ARMv8.1+ features qemu's "max" CPU has and the translator does not claim (FP16 arithmetic, RDM, JSCVT, ...)
    if (".f16" in mn and not mn.startswith(("vcvtb", "vcvtt")) and mn not in ("vcvt.f16.f32", "vcvt.f32.f16")) or \
            mn.startswith(("vqrdmlah", "vqrdmlsh", "vjcvt", "vins", "vmovx", "vcmla", "vcadd", "vsdot", "vudot",
                           "vfmal", "vfmsl", "vmmla", "vsmmla", "vummla", "vusdot", "vsudot", "vdot", "vfmab",
                           "vfmat", "vldr.16", "vstr.16", "vusmmla")) or ".bf16" in mn:
        return None
    if mn.startswith(("ldm", "pop", "stm", "push", "vpush", "vpop", "ldr", "str", "pld", "pli", "ld", "st", "vld",
                      "vst", "swp", "lda", "stl", "tst")) is False:
        pass
    is_mem = "[" in ops or mn.startswith(("ldm", "stm", "pop", "push", "vldm", "vstm", "vpush", "vpop", "swp"))
    # registers written: first operand for most; reject sp/pc destinations and writeback to sp
    first = ops.split(",")[0].strip().strip("{}") if ops else ""
    if first in ("sp", "pc", "r13", "r15") and not mn.startswith(("cmp", "cmn", "tst", "teq", "str", "vst", "pld",
                                                                   "pli", "stl")):
        return None
    if mn.startswith(("push", "pop", "vpush", "vpop")):
        return None
    # LDRD/LDREXD Rt also write Rt+1, which objdump may not print: r12 -> SP, r14 -> PC
    if mn.startswith(("ldrd", "ldrexd", "ldaexd")) and first in ("ip", "r12", "lr", "r14"):
        return None
    # instructions with two destination registers (RdLo/RdHi, Rt/Rt2) must not write SP/PC either
    two = [t.strip() for t in ops.split(",")[:2]]
    if mn.startswith(("umull", "smull", "umlal", "smlal", "umaal", "ldrd", "ldrex", "ldaex", "vmov", "mrrc")) and \
            any(t in ("sp", "pc", "r13", "r15") for t in two):
        return None
    if mn.startswith(("ldm", "stm", "vldm", "vstm")) and ops.startswith(("sp", "r13")):
        return None
    if re.search(r"\b(sp|r13)\b.*!", ops) or re.search(r"\[sp\], ", ops):
        return None
    if mn.startswith(("ldm", "pop")) and re.search(r"\b(pc|sp)\b", ops.split("{", 1)[-1]):
        return None
    if mn.startswith(("ldr", "ldrex", "lda")) and re.match(r"(pc|r15)\b", first):
        return None
    if mn.startswith(("mrs", "msr")) and ("spsr" in ops or "cpsr_c" in ops or "cpsr_x" in ops or "cpsr_s" in ops
                                          or "_c" in ops.split(",")[0] or "_x" in ops or "_s" in ops):
        return None
    if mn.startswith(("vmrs", "vmsr")) and "fpscr" not in ops and "APSR" not in ops:
        return None
    if mn.startswith(("vmsr",)):
        return None  # writes FPSCR control bits: harness compares them anyway, but skip trap-enable fun
    if is_mem and re.search(r"\[(pc|r15)", ops) and mn.startswith(("st", "vst")):
        return None
    if is_mem and mn.startswith(("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "vldr")) and re.search(r"\[(pc|r15)", ops):
        # PC-relative loads read the harness code; a register index must stay small
        m = MEM_RE.search(ops)
        idx = []
        for g, sh in ((m.group(3), m.group(5)), (m.group(8), m.group(10))) if m else ():
            if g and (g.startswith("r") or g in REGMAP):
                if sh and int(sh) > 4:
                    return None
                idx.append(regnum(g))
        return {"mem": False, "base": [], "index": idx, "text": text}
    info = {"mem": is_mem, "base": [], "index": [], "text": text}
    if is_mem:
        if mn.startswith(("ldm", "stm", "vldm", "vstm")):
            b = ops.split(",")[0].strip().rstrip("!")
            info["base"].append(regnum(b))
            if mn.startswith(("ldm", "stm")) and "!" in ops.split(",")[0] and regnum(b) >= 0:
                lst = ops.split("{", 1)[1].split("}")[0]
                if re.search(rf"\b{b}\b", lst):
                    return None
        else:
            m = MEM_RE.search(ops)
            if not m:
                return None
            info["base"].append(regnum(m.group(1)))
            for g, sh, kind in ((m.group(3), m.group(5), m.group(4)), (m.group(8), m.group(10), m.group(9))):
                if g and (g.startswith("r") or g in REGMAP):
                    info["index"].append(regnum(g))
                    if sh and int(sh) > 4:
                        return None
                    if kind == "ror":  # keep the rotated index small: its low bits become the top bits
                        info["index_ror"] = int(sh)
            # base == index: the address is not near the scratch area (layout-dependent faults)
            if info["base"][0] in info["index"]:
                return None
            # writeback base must not be a destination
            dests = [regnum(x.strip()) for x in ops.split("[")[0].split(",") if x.strip()]
            if (m.group(6) or m.group(8)) and info["base"][0] in dests:
                return None
        if any(r in (13, 15) for r in info["index"]):
            return None
        if 13 in info["base"]:
            return None  # below SP lie earlier tests' signal frames, whose contents legitimately differ
    else:
        # register-shifted-register forms and loads via pc are fine; nothing else to constrain
        pass
    return info


FP_SPECIALS = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fa00000,
               0xffc00001, 0x00000001, 0x807fffff, 0x7f7fffff, 0x4b000000, 0xcf000000, 0x4f800000, 0x3f000000]
FP64_SPECIALS = [0, 1 << 63, 0x3ff0000000000000, 0x7ff0000000000000, 0x7ff8000000000000, 0x7ff4000000000000,
                 0x0000000000000001, 0x800fffffffffffff, 0x41e0000000000000, 0xc1e0000000200000, 0x7fefffffffffffff]
INT_SPECIALS = [0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0xff, 0xffff, 0x8000, 0x7fff, 31, 32, 33, 0x100]


def rand_reg(rng):
    r = rng.random()
    if r < 0.25:
        return rng.choice(INT_SPECIALS)
    if r < 0.35:
        return rng.randrange(0, 64)
    return rng.getrandbits(32)


def rand_d(rng):
    r = rng.random()
    if r < 0.2:
        return rng.choice(FP_SPECIALS) | rng.choice(FP_SPECIALS) << 32
    if r < 0.3:
        return rng.choice(FP64_SPECIALS)
    if r < 0.45:  # "normal" floats
        def f():
            return struct.unpack("<I", struct.pack("<f", rng.uniform(-1e4, 1e4)))[0]
        return f() | f() << 32
    return rng.getrandbits(64)


def make_state(rng, info, scratch_mid):
    regs = [rand_reg(rng) for _ in range(14)]  # r0-r12, r14
    for r in info["index"]:
        if 0 <= r <= 14:
            regs[r if r < 13 else 13] = (rng.randrange(0, 32) * 4) << info.get("index_ror", 0)
    for r in info["base"]:
        if 0 <= r <= 14 and r != 13:
            regs[r if r < 13 else 13] = scratch_mid + rng.randrange(-64, 64) * 8
    apsr = rng.getrandbits(5) << 27 | rng.getrandbits(4) << 16
    fpscr = rng.getrandbits(4) << 28 | rng.getrandbits(5) | (rng.getrandbits(1) << 27)
    if rng.random() < 0.3:
        fpscr |= rng.getrandbits(4) << 22  # RMode, FZ, DN
    if rng.random() < 0.1:
        fpscr |= 1 << 26  # AHP
    ds = [rand_d(rng) for _ in range(32)]
    return regs, apsr, fpscr, ds


def build_harness(tests, iset, workdir, name):
    """tests: list of (word, info, state, itcond or None)"""
    a = [".syntax unified", ".arch armv8-a", ".fpu crypto-neon-fp-armv8", ".text", ".arm", ".global _start",
         "_start:", "    ldr r0, =scratch_pristine", "    ldr r1, =scratch", f"    mov r2, #{SCRATCH_SIZE}",
         "0:  ldr r3, [r0], #4", "    str r3, [r1], #4", "    subs r2, r2, #4", "    bne 0b"]
    # faults (UNDEF, bad memory) are recorded per test: the handler writes a crash record and resumes at the
    # next test by rewriting the saved pc/cpsr in the signal frame
    for sig in (4, 5, 7, 8, 11):
        a += [f"    mov r0, #{sig}", "    ldr r1, =sigact", "    mov r2, #0", "    mov r3, #8", "    mov r7, #174",
              "    svc 0"]
    for k, (w, info, (regs, apsr, fpscr, ds), itc) in enumerate(tests):
        a += [f"start_{k}:", f"    ldr r0, =start_{k + 1}", "    ldr r1, =resume_at", "    str r0, [r1]",
              f"    ldr r0, ={k}", "    ldr r1, =cur_index", "    str r0, [r1]",
              f"    @ test {k}: {info['text']}",
              "    bl reset_scratch" if info["mem"] else "",
              f"    ldr sp, =state_{k}",
              "    vldmia sp!, {d0-d15}", "    vldmia sp!, {d16-d31}",
              "    ldr r0, [sp], #4", "    vmsr fpscr, r0",
              "    ldr r0, [sp], #4", "    msr APSR_nzcvqg, r0",
              "    ldmia sp, {r0-r12, r14}",
              "    ldr sp, =stack_top",
              f"    ldr pc, =test_{k}" + ("+1" if iset != "arm" else "")]
        a += [".thumb" if iset != "arm" else ".arm", ".align 2", f"test_{k}:"]
        if itc is not None:
            a.append(f"    .inst.n 0x{0xbf08 | itc << 4:04x}")
        if iset == "thumb16":
            a.append(f"    .inst.n 0x{w:04x}")
        elif iset == "thumb32":
            a.append(f"    .inst.w 0x{w:08x}")
        else:
            a.append(f"    .inst 0x{w:08x}")
        a += [f"    ldr pc, =save_{k}", "    .ltorg", ".arm", ".align 2", f"save_{k}:",
              "    push {r0-r12, r14}", "    mrs r0, apsr", "    ldr r1, =0xf80f0000", "    and r0, r0, r1",
              "    push {r0}", "    vmrs r0, fpscr", "    push {r0}", "    vpush {d16-d31}", "    vpush {d0-d15}",
              f"    mov r0, #{k & 0xff}", f"    orr r0, r0, #{k & 0xff00}", f"    mov r1, #{1 if info['mem'] else 0}",
              "    b dump_" + str(k), "    .ltorg", f"dump_{k}:", "    bl dump"]
    a += [f"start_{len(tests)}:", "    mov r0, #0", "    mov r7, #1", "    svc 0",
          # crash handler: r0 = signal, r2 = ucontext
          "on_fault:", "    ldr r4, =crashbuf", "    ldr r5, =cur_index", "    ldr r5, [r5]",
          "    orr r5, r5, #0x80000000", "    str r5, [r4]", "    str r0, [r4, #4]", "    mov r6, r2",
          "    mov r0, #1", "    mov r1, r4", f"    mov r2, #{RECORD}", "    mov r7, #4", "    svc 0",
          "    ldr r0, =resume_at", "    ldr r0, [r0]", "    str r0, [r6, #92]",
          "    ldr r0, [r6, #96]", "    ldr r1, =0xf9ff03df", "    and r0, r0, r1", "    str r0, [r6, #96]",
          "    bx lr",
          "reset_scratch:", "    ldr r0, =scratch_pristine", "    ldr r1, =scratch", f"    mov r2, #{SCRATCH_SIZE}",
          "1:  ldr r3, [r0], #4", "    str r3, [r1], #4", "    subs r2, r2, #4", "    bne 1b", "    bx lr",
          # dump: r0 = index, r1 = hash scratch?; state block at sp: d0-d31, fpscr, apsr, r0-r12, r14 (push order)
          "dump:", "    mov r4, sp", "    ldr r5, =outbuf", "    str r0, [r5]",
          # reorder into: r0-r12, r14, apsr, fpscr, d0-d31
          "    add r6, r4, #264", "    add r7, r5, #4", "    mov r8, #14",
          "2:  ldr r9, [r6], #4", "    str r9, [r7], #4", "    subs r8, r8, #1", "    bne 2b",
          "    ldr r9, [r4, #260]", "    str r9, [r7], #4", "    ldr r9, [r4, #256]", "    str r9, [r7], #4",
          "    mov r6, r4", "    mov r8, #64",
          "3:  ldr r9, [r6], #4", "    str r9, [r7], #4", "    subs r8, r8, #1", "    bne 3b",
          "    mov r9, #0", "    cmp r1, #0", "    beq 5f",
          "    ldr r6, =scratch", f"    mov r8, #{SCRATCH_SIZE // 4}", "    ldr r9, =0x811c9dc5", "    ldr r10, =0x01000193",
          "4:  ldr r11, [r6], #4", "    eor r9, r9, r11", "    mul r9, r9, r10", "    subs r8, r8, #1", "    bne 4b",
          "5:  str r9, [r7], #4",
          "    mov r0, #1", "    mov r1, r5", f"    mov r2, #{RECORD}", "    mov r7, #4", "    svc 0",
          "    ldr sp, =stack_top", "    bx lr", ".ltorg"]
    a += [".data", ".align 4", "sigact:", "    .word on_fault, 0x40000004, 0, 0, 0", "resume_at:", "    .word 0",
          "cur_index:", "    .word 0", "crashbuf:", f"    .space {RECORD}"]
    for k, (w, info, (regs, apsr, fpscr, ds), itc) in enumerate(tests):
        a.append(f"state_{k}:")
        a.append("    .quad " + ", ".join(f"0x{d:016x}" for d in ds))
        a.append(f"    .word 0x{fpscr:08x}, 0x{apsr:08x}")
        a.append("    .word " + ", ".join(f"0x{r:08x}" for r in regs))
    a += [".align 12", "scratch_pristine:"]
    rng = random.Random(1234)
    for _ in range(SCRATCH_SIZE // 64):
        a.append("    .word " + ", ".join(f"0x{rng.getrandbits(32):08x}" for _ in range(16)))
    a += ['.section .fuzzbss, "aw", %nobits', "scratch:", f"    .space {SCRATCH_SIZE}", "outbuf:",
          f"    .space {RECORD + 64}", "    .space 65536", "stack_top:", "    .space 64"]
    src = workdir / f"{name}.s"
    src.write_text("\n".join(x for x in a if x != "") + "\n")
    obj = workdir / f"{name}.o"
    exe = workdir / name
    subprocess.run([tool("as"), "-march=armv8-a+crc+simd+crypto", "-mfpu=crypto-neon-fp-armv8", str(src), "-o",
                    str(obj)], check=True)
    subprocess.run([tool("ld"), "-N", "-Ttext=0x10000", f"--section-start=.fuzzbss=0x{SCRATCH_BASE:x}", "-e",
                    "_start", str(obj), "-o", str(exe)], check=True)
    return exe


def no_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(cmd, timeout=120):
    try:
        p = subprocess.run(["unshare", "-Urpf", "--kill-child", *cmd], capture_output=True, timeout=timeout,
                           preexec_fn=no_core)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""


def records(out):
    res = {}
    for off in range(0, len(out) - RECORD + 1, RECORD):
        rec = out[off:off + RECORD]
        res[struct.unpack_from("<I", rec)[0]] = rec[4:]
    return res


def describe(exp, got):
    e = struct.unpack(f"<{NSTATE + 1}I", exp)
    g = struct.unpack(f"<{NSTATE + 1}I", got)
    names = [f"r{i}" for i in range(13)] + ["r14", "apsr", "fpscr"] + [f"d{i // 2}.{'lh'[i % 2]}" for i in range(64)] + ["mem"]
    diffs = []
    for k, (x, y) in enumerate(zip(e, g)):
        if x != y:
            diffs.append(f"{names[k]}: want {x:08x} got {y:08x}")
    return "; ".join(diffs)


A32DIS = Path(tempfile.mkdtemp(prefix="a32dis-")) / "a32dis"  # per process: parallel fuzzers rebuild it


def main():
    subprocess.run(["cc", "-O1", "-o", str(A32DIS), str(ROOT / "tools/a32dis.c"),
                    *(str(ROOT / "src" / f) for f in ("a32_decode.c", "t32_decode.c", "vfp_neon_decode.c"))],
                   check=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--class", dest="cls", default="arm-dp")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--modes", default="interp,jit")
    ap.add_argument("--runner", default="qemu-aarch64")
    ap.add_argument("--a32run", default=str(ROOT / "out" / "a32run"))
    ap.add_argument("--batch", type=int, default=400)
    ap.add_argument("--max-report", type=int, default=30)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    classes = list(CLASSES) if a.cls == "all" else a.cls.split(",")
    total_bad = 0
    for cls in classes:
        total_bad += fuzz_class(cls, a)
    sys.exit(1 if total_bad else 0)


def fuzz_class(cls, a):
    rng = random.Random(f"{a.seed}-{cls}")
    iset = CLASSES[cls][0]
    workdir = Path(tempfile.mkdtemp(prefix=f"a32fuzz-{cls}-"))
    # generate and filter candidates
    tests, seen = [], set()
    tries = 0
    while len(tests) < a.count and tries < 40:
        tries += 1
        words = []
        while len(words) < max(256, a.count):
            w = random_word(rng, cls)
            if w not in seen:
                seen.add(w)
                words.append(w)
        texts = disassemble(words, iset, workdir)
        if len(texts) != len(words):
            print(f"objdump returned {len(texts)} lines for {len(words)} words", file=sys.stderr)
            texts = (texts + [""] * len(words))[:len(words)]
        cands = []
        for w, t in zip(words, texts):
            info = classify(t)
            if info is None:
                continue
            itc = None
            if iset.startswith("thumb") and rng.random() < 0.25 and "it" not in t.split()[0]:
                itc = rng.randrange(0, 14)
            cands.append((w, info, itc))
        # drop encodings our decoder considers UNPREDICTABLE (qemu often UNDEFs them, hardware executes them)
        q = "".join(f"{'a' if iset == 'arm' else 't'} {w:0{4 if iset == 'thumb16' else 8}x} {-1 if c is None else c}\n"
                    for w, _, c in cands)
        flags = subprocess.run([str(A32DIS)], input=q, capture_output=True, text=True,
                               check=True).stdout.split("\n")
        for (w, info, itc), f in zip(cands, flags):
            if f.split()[0] == "1":
                continue
            tests.append((w, info, itc))
            if len(tests) >= a.count:
                break
    print(f"[{cls}] {len(tests)} tests ({workdir})", flush=True)
    runner = shlex.split(a.runner)
    bad = 0
    reported = 0
    for start in range(0, len(tests), a.batch):
        batch = tests[start:start + a.batch]
        st_rng = random.Random(f"{a.seed}-{cls}-{start}")
        full = [(w, info, make_state(st_rng, info, SCRATCH_MID), itc) for (w, info, itc) in batch]
        exe = build_harness(full, iset, workdir, f"h{start}")
        results = {}
        for mode in ["qemu"] + a.modes.split(","):
            if mode == "qemu":
                cmd = ["qemu-arm", "-cpu", "max", str(exe)]
            else:
                cmd = [*runner, a.a32run, *(["--interp"] if mode == "interp" else []), str(exe)]
            rc, out, err = run(cmd)
            recs = records(out)
            if rc != 0:
                print(f"  {mode}: harness exit {rc} after {len(recs)} records: {err.decode(errors='replace')[-400:]}")
            for k, r in recs.items():
                results.setdefault(k & 0x7fffffff, {})[mode] = r if not k & 0x80000000 else b"signal %d" % r[0]
        for k in range(len(full)):
            r = results.get(k, {})
            q = r.get("qemu")
            if q is None or q == b"signal 7":
                continue  # qemu's alignment faults (LDRD/LDM/VLDn at unaligned addresses): a32run allows them
            for mode in a.modes.split(","):
                g = r.get(mode)
                if g == q:
                    continue
                bad += 1
                if reported < a.max_report:
                    reported += 1
                    w, info, st, itc = full[k]
                    if g is None:
                        what = "no result"
                    elif q.startswith(b"signal") or g.startswith(b"signal"):
                        what = f"qemu {q[:9].decode() if q.startswith(b'signal') else 'ok'}, a32run {g[:9].decode() if g.startswith(b'signal') else 'ok'}"
                    else:
                        what = describe(q, g)
                    it = f" (in IT {itc})" if itc is not None else ""
                    print(f"  MISMATCH [{mode}] {w:0{4 if iset == 'thumb16' else 8}x} {info['text']}{it}: {what}")
    print(f"[{cls}] {bad} mismatch(es)", flush=True)
    if not a.keep and not bad:
        subprocess.run(["rm", "-rf", str(workdir)])
    return bad


if __name__ == "__main__":
    main()
