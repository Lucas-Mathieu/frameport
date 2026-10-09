# a32translator: AArch32 → AArch64 dynamic binary translator

`a32run` runs 32-bit ARM Linux programs (ARM, Thumb-2, VFPv4, NEON, plus the ARMv8 AArch32 additions) on an
AArch64 host without AArch32 support, such as the Steam Frame's SteamOS. It is a user-mode translator (like
`qemu-arm`). It decodes guest instructions, runs them through a JIT that emits native A64 code, and translates
system calls to the host kernel.

Status: proof of concept. It runs static bionic (Android NDK) programs, including C++ with exceptions, pthreads,
signals, fork, self-modifying code and NEON. It is **not** yet an Android native bridge: running 32-bit-only Quest
APKs inside Lepton additionally needs `libnativebridge` integration and JNI/GL/OpenXR thunks (see "Next steps").

```
python native/a32translator/build.py          # -> native/a32translator/out/a32run (static AArch64, NDK clang)
a32run [--interp] [--strace] [--trace] [--sysroot DIR] [-v] program [args...]
```

Environment: `A32_STATS=1` (block/chain statistics at exit), `A32_DUMP=file` (translated code, see
`tools/jitdump.py`), `A32_EXACT_ADDR=1` (guest address arithmetic modulo 2^32, see below), `A32_CACHE_KB=n`
(small code cache, exercises flushes), `A32_INTERP=1`, `A32_STRACE=1`.

32-bit bionic refuses pids above 65535, which hosts with a large `pid_max` (SteamOS: 4194304) exceed. Start such
programs in a fresh pid namespace: `unshare -Urpf --kill-child a32run prog`.

## How it works

```
 guest bytes ─┬─ a32_decode.c (A32) ───┐
              └─ t32_decode.c (T32) ───┼─► insn_t ─┬─► interp*.c  (reference semantics, fallback)
   vfp_neon_decode.c (VFP/NEON) ◄──────┘           └─► jit.c / jit_vfp.c (A64 code) ─► code cache
```

- **One instruction form, two backends.** Both decoders produce `insn_t` (`insn.h`). T32 VFP/NEON encodings are
  rewritten to their A32 form first, so one decoder serves both instruction sets. The interpreter (`interp.c`,
  `interp_vfp.c`, `interp_neon.c`) defines the semantics of everything. The JIT translates the common
  instructions natively and calls the interpreter for the rest (`jx_call_helper`), so every instruction the
  interpreter knows also works in JIT mode.
- **Register mapping.** Guest r0-r14 are pinned in x0-x14 (upper halves always zero), q0-q15 in v16-v31, and the
  guest NZCV lives in the host NZCV. x19 points to the `a32_cpu` state. The guest PC is a translation-time
  constant. Scratch registers: x15-x17, x20-x23, v0-v7. Within a block, scalar VFP values are cached in v8-v15:
  writing a lane of a pinned Q register would chain loop iterations through the merge. The cache is flushed at
  exits, helper calls and branches.
- **Flags.** A64 ADDS/SUBS/ADCS/SBCS set NZCV exactly like AArch32. Logical flag-setting instructions get N/Z
  from the result, C from the shifter and V unchanged (`set_nzc`). Conditional instructions branch over their
  code (the condition encodings match).
- **Floating point.** Guest FP operations run as the identical A64 instructions with the guest's FPSCR control
  bits in FPCR (FPCR/FPSR share FPSCR's bit layout), so rounding, flush-to-zero, default NaN and exception flags
  match by construction. Advanced SIMD floating point runs with the "standard FPSCR value" (DN, FZ, round to
  nearest). Blocks switch FPCR only around such operations. VMLA/VMLS stay unfused (multiply, then add) and
  VRECPS/VRSQRTS go to the interpreter (non-fused in AArch32, fused in A64). The interpreter uses inline A64
  instructions as well.
- **Blocks.** A block (a superblock) ends at the first unconditional branch or other PC write, an SVC/UDF, or
  after 96 instructions. Conditional branches and CBZ/CBNZ are side exits: translation continues on the
  fall-through path, so a loop with an early exit is still one block. Blocks are keyed by (pc, Thumb, ITSTATE), so
  IT blocks may span block boundaries. Direct exits are patchable `b` instructions that get chained to their
  target. Indirect branches (BX LR, POP {pc}, LDR pc, TBB) look the target up inline in a direct-mapped table. A
  branch back to the block's own start is a plain `b`/`b.cond`. Every block starts with a check of
  `cpu->interrupt`, which signals and code flushes set, so chained loops still stop.
- **Code quality.** Conditional data-processing instructions that don't set flags become compute + `csel`
  (branchless). VCMP followed by VMRS APSR_nzcv leaves the compare's flags in NZCV directly. LDREX/STREX (all
  sizes) are an inline compare-and-swap loop (`ldxr`/`stxr`). Thumb's flag-setting register shifts
  (LSLS/LSRS/ASRS/RORS Rdn, Rm) compute the AArch32 carry for 0-255 bit amounts inline. In the benchmark below,
  no instruction goes to the interpreter (`A32_STATS=1` lists the ones that do).
- **Memory.** Guest addresses are host addresses: the low 4 GiB are reserved at start-up (`mem.c`) and the
  guest's mappings live there, so loads and stores need no translation and most syscalls pass pointers through.
  Translated code uses A64 addressing modes (`[xn, #imm]`). They differ from AArch32 only when guest address
  arithmetic wraps past 4 GiB: such an access then faults above 4 GiB, and the fault is still delivered to the
  guest. `A32_EXACT_ADDR=1` computes every address modulo 2^32 instead. Unaligned LDRD/LDM/VLDn are allowed
  (newer arm64 kernels fix them up for AArch32 too); qemu raises SIGBUS. The host page size must be 4 KiB.
- **Self-modifying code.** `cacheflush` (what `__builtin___clear_cache` calls on ARM), `mprotect`, `munmap` and
  `mremap` invalidate the translated blocks of the affected pages and unlink chains into them. A full cache
  (128 MiB) is flushed once every thread has left translated code.
- **Faults.** A guest memory fault in translated code is mapped back to the guest instruction (per-instruction
  host offsets, plus the VFP cache state). The guest registers are rebuilt from the signal context, and the
  guest's handler gets a proper ARM `rt_sigframe` (VFP state included). Returning from it restarts the
  instruction.
- **Syscalls** (`syscall.c`): ARM EABI numbers (generated from the NDK headers, `tools/gen_syscalls.py`) map to
  AArch64 ones. Structures whose 32-bit layout differs are converted: stat64, statfs64, timespec/timeval, iovec,
  rusage, sysinfo, rlimit, flock, msghdr with control messages, getdents. The guest persona (`PER_LINUX32`) is
  emulated, because hosts without AArch32 refuse it. ARM private calls (set_tls, cacheflush) and kuser helpers
  (0xffff0fxx) are implemented. `execve` of a 32-bit ARM ELF re-executes `a32run`.
- **Signals** (`signals.c`): host handlers queue signals; delivery happens at block boundaries with the ARM
  kernel's frame layouts (`sigframe`/`rt_sigframe`, `sigaltstack`, SA_RESTORER, SA_NODEFER, SA_RESETHAND).
  Guest SIG_DFL/SIG_IGN dispositions stay on the host, so the kernel applies default actions itself.
- **Threads** (`threads.c`): `clone` with CLONE_VM|CLONE_THREAD starts a host pthread running its own `a32_cpu`.
  Other clones become `fork`. The code cache is shared (a mutex guards translation; lookups are lock-free).
  LDREX/STREX are emulated as compare-and-swap on the exclusive value; ABA changes are not detected.

Not supported: big-endian data (SETEND BE), ARMv8.1+ AArch32 extensions (FP16 arithmetic, RDM, JSCVT, dot
product), VFP short vectors (Len/Stride, absent on ARMv8 anyway), robust futex lists, and the iWMMXt/ThumbEE
relics. FPSCR exception *traps* are not emulated (flags are).

## Testing

All of these run on an x86-64 PC through `qemu-aarch64`, with `qemu-arm` as the reference, or natively on an
AArch64 machine with `--runner ""`.

- `tests/test_a64.py`: every A64 encoder in `src/a64.h` against `llvm-mc` (400 encodings).
- `tests/fuzz.py`: differential instruction fuzzer. Random encodings per class (`arm-dp`, `arm-ldst`,
  `arm-media`, `arm-ldm`, `arm-vfp`, `arm-neon`, `arm-neon-ldst`, `arm-v8`, `t16`, `t32`, `t32-neon`,
  `t32-vfp`) are filtered with `arm-none-eabi-objdump` and with the translator's own UNPREDICTABLE flag
  (`tools/a32dis.c`). Each runs once in a harness that loads random registers, flags, FPSCR and NEON state, and
  dumps them afterwards together with a hash of the touched memory. The harness's own signal handler records
  UNDEF and memory faults per test, so UNDEF decoding is compared too. Runs `qemu-arm -cpu max`, then `a32run
  --interp`, then `a32run`. Known, deliberate differences are filtered: qemu's alignment faults, ARMv8.1+
  features, and encodings in ARMv7's unallocated space that qemu executes.
- `tests/run_programs.py`: C/C++ programs built with the NDK for ARM and Thumb (static bionic), compared with
  `qemu-arm` in both modes: libc/libm/printf, memory and string routines, NEON intrinsics, pthreads and atomics,
  signals (handlers, SIGSEGV recovery, itimers, fork), file system calls, C++ exceptions/RTTI/STL/std::thread,
  run-time generated code (`jitguest`) and a benchmark. `A32_CACHE_KB=300` exercises cache flushes.

Benchmark (`tests/programs/bench.c 10`) on the Steam Frame, medians of three runs (the headset's clocks vary by
up to ±30 % between runs), against the same source built natively for AArch64:

| | native AArch64 | a32run, ARM build | a32run, Thumb build |
|---|---|---|---|
| sieve | 0.11 s | 0.12 s | 0.10 s |
| crc32 + sha256 | 0.39 s | 0.42 s | 0.57 s |
| qsort | 0.15 s | 0.21 s | 0.25 s |
| float matmul | 0.017 s (auto-vectorized) | 0.27 s (scalar VFP: ARMv7 compilers don't vectorize float) | 0.27 s |
| double mandelbrot | 0.21 s | 0.44 s | 0.41 s |
| libm | 0.06 s | 0.25 s | 0.27 s |

## Next steps

1. **Android native bridge.** Expose the translator through the `NativeBridgeCallbacks` interface (as
   libhoudini and libndk_translation do) so ART can load 32-bit ARM libraries in a 64-bit process. This needs:
   loading guest `.so` files with a 32-bit linker in the low 4 GiB, trampolines for JNI in both directions
   (`JNIEnv` and every callback marshalled between the 32-bit and 64-bit ABIs), and guest-side stubs for the host
   libraries a game calls (libc, libGLESv3/EGL, Vulkan, OpenXR, OpenSL/AAudio).
2. Check that Lepton accepts `ro.dalvik.vm.native.bridge` and the 32-bit ABI lists; until then, PackageManager
   rejects 32-bit-only APKs before any code runs.
3. JIT: VLDn single-lane/all-lanes forms, flag liveness (skip unused C/V computations), keeping the VFP cache
   across a loop's back edge, and VRECPS/VRSQRTS/VCVTR natively.
