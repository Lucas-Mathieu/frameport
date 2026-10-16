"""Unreal's Android memory reservations without commit-charge accounting (GitHub #349, Transformers Beyond Reality).

Unreal reserves virtual memory on Android with `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1,
0)` (FPlatformVirtualMemoryBlock::AllocateVirtual: Android has no separate commit step). Games built with
FMallocBinned3 reserve the whole allocator range up front while libUnreal.so loads, 76 GiB in Transformers Beyond
Reality. Quest's kernel lets that through; the Frame runs with the kernel's heuristic overcommit (mode 0), which
refuses any single writable private mapping larger than RAM + swap (16 + 8 GiB), so the reservation fails, Unreal's
out-of-memory handler runs before its log exists and dereferences a null pointer (SIGSEGV, fault addr 0x10, from a
static constructor inside System.loadLibrary). Adding MAP_NORESERVE to those mmap calls gives them the Quest
behaviour: pages are still only backed when touched, the mapping just isn't charged up front.

Each site is `mov w3, #0x22` (MAP_PRIVATE|MAP_ANONYMOUS) a few instructions before a `bl` to mmap's PLT stub, with
`mov x0, xzr` and `mov w2, #3` in the same window; the one instruction becomes `mov w3, #0x4022`. Nothing else changes
and nothing moves; only arm64 engine libraries."""
from __future__ import annotations

import io
import struct

from ...analysis import elf
from ..base import ApkContext, Patch, Suggestion, register

ENGINE_LIBS = ("libUE4.so", "libUnreal.so")
MOV_W3_FLAGS = 0x52800443      # mov w3, #0x22   MAP_PRIVATE|MAP_ANONYMOUS
MOV_W3_NORESERVE = 0x52880443  # mov w3, #0x4022 MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE
MOV_W2_RW = 0x52800062         # mov w2, #3      PROT_READ|PROT_WRITE
MOV_X0_NULL = 0xAA1F03E0       # mov x0, xzr
WINDOW_BEFORE, WINDOW_AFTER = 8, 9


def _mmap_slots(data: bytes) -> set[int]:
    """GOT slots of mmap/mmap64 (R_AARCH64_JUMP_SLOT relocations)."""
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection

    e = ELFFile(io.BytesIO(data))
    dynsym = elf.dynsym_section(e)
    if dynsym is None:
        return set()
    slots = set()
    for sec in e.iter_sections():
        if not isinstance(sec, RelocationSection) or sec["sh_type"] != "SHT_RELA":
            continue
        for r in sec.iter_relocations():
            sym = r["r_info_sym"]
            if sym and dynsym.get_symbol(sym).name in ("mmap", "mmap64"):
                slots.add(r["r_offset"])
    return slots


def _plt_slot(data: bytes, segs, va: int) -> int | None:
    """The GOT slot a PLT stub at `va` jumps through: adrp x16, page; ldr x17, [x16, #off]."""
    off = elf.vaddr_to_offset(segs, va)
    if off is None or off + 8 > len(data):
        return None
    adrp, ldr = struct.unpack_from("<II", data, off)
    if adrp & 0x9F00001F != 0x90000010 or ldr & 0xFFC003FF != 0xF9400211:
        return None
    imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7FFFF) << 2
    if imm & 1 << 20:
        imm -= 1 << 21
    return (va & ~0xFFF) + (imm << 12) + ((ldr >> 10) & 0xFFF) * 8


def mmap_sites(data: bytes) -> tuple[list[int], list[int]]:
    """File offsets of the flag instructions of anonymous read/write mmap calls: (still MAP_PRIVATE|MAP_ANONYMOUS,
    already MAP_NORESERVE)."""
    if not elf.is_elf(data) or not elf.is_64bit(data) or data[5] != 1:
        return [], []
    slots = _mmap_slots(data)
    if not slots:
        return [], []
    segs = elf.load_segments(data)
    stubs: dict[int, bool] = {}
    todo, done = [], []
    for word, out in ((MOV_W3_FLAGS, todo), (MOV_W3_NORESERVE, done)):
        needle = struct.pack("<I", word)
        at = data.find(needle)
        while at >= 0:
            va = next((at - o + v for v, o, s in segs if o <= at < o + s), None) if at % 4 == 0 else None
            if va is not None:
                for k in range(1, WINDOW_AFTER + 1):
                    if at + 4 * k + 4 > len(data):
                        break
                    w = struct.unpack_from("<I", data, at + 4 * k)[0]
                    if w >> 26 != 0b100101:  # bl
                        continue
                    imm = w & 0x3FFFFFF
                    target = va + 4 * k + ((imm - (1 << 26)) if imm & 1 << 25 else imm) * 4
                    if target not in stubs:
                        stubs[target] = _plt_slot(data, segs, target) in slots
                    if stubs[target]:
                        window = {struct.unpack_from("<I", data, at + 4 * j)[0]
                                  for j in range(-WINDOW_BEFORE, k) if at + 4 * j >= 0}
                        if MOV_W2_RW in window and MOV_X0_NULL in window:
                            out.append(at)
                    break
            at = data.find(needle, at + 1)
    return todo, done


def patch_lib(data: bytes) -> tuple[bytes, int]:
    todo, _done = mmap_sites(data)
    if not todo:
        return data, 0
    buf = bytearray(data)
    for at in todo:
        struct.pack_into("<I", buf, at, MOV_W3_NORESERVE)
    return bytes(buf), len(todo)


class UnrealNoReserve(Patch):
    id = "frame.unreal_noreserve"
    title = "Unreal: reserve memory like on Quest"
    description = ("Unreal reserves its memory ranges on Android with ordinary read/write mappings. Games built with "
                   "Unreal's Binned3 allocator reserve tens of gigabytes at once while the engine loads (76 GiB in "
                   "Transformers Beyond Reality); Quest allows that, the Frame's kernel refuses mappings larger "
                   "than its memory, and the game crashes before its first frame (SIGSEGV at fault addr 0x10 in "
                   "libUnreal.so, inside System.loadLibrary). This marks those mappings MAP_NORESERVE, as Quest "
                   "effectively treats them: memory is still only used when the game touches it.")
    order = 49
    needs_vr = False

    def applies(self, a):
        return a.engine == "Unreal" and "arm64-v8a" in a.abis

    def detect(self, a):
        if self.applies(a) and (a.extra or {}).get("unreal_binned3"):
            return Suggestion(True, "Unreal's Binned3 allocator reserves more memory at start than the Frame's "
                                    "kernel allows in one piece; without this the game crashes while it loads.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        if ws.abi != "arm64-v8a":
            return False
        lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
        if not lib:
            return False
        data, count = patch_lib(ws.read_lib(lib))
        if not count:
            if not mmap_sites(data)[1]:
                ctx.notes.append(f"{self.id}: no anonymous read/write mmap calls found in {lib}, left unchanged")
            return False
        ws.put(ws.lib(lib), data)
        ctx.notes.append(f"{count} mmap call(s) in {lib} reserve with MAP_NORESERVE")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
        if not lib or ws.abi != "arm64-v8a":
            return []
        todo, done = mmap_sites(ws.read_lib(lib))
        return [("Unreal memory reservations use MAP_NORESERVE", bool(done) and not todo,
                 f"{len(done)} rewritten, {len(todo)} left")]


register(UnrealNoReserve)
