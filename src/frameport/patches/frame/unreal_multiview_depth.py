"""Refresh UE4's cached mobile multiview depth size (Tales from the Galaxy's Edge, GitHub #61).

The initial Android window is shorter than the VR eye texture. Keeping its dimensions for the depth attachment
clips the environment while separately submitted menu layers remain intact. Always copy the current scene
BufferSize into the depth-size cache before allocation. Multiview and multisampling remain enabled.
Only the tested exported function and instruction sequence are accepted; no engine addresses are hardcoded.
"""
from __future__ import annotations

import struct

from ...analysis import elf
from ..base import ApkContext, Patch, register

ENGINE_LIBS = ("libUE4.so", "libUnreal.so")
ALLOC = "_ZN19FSceneRenderTargets25AllocMobileMultiViewDepthER15FRHICommandListi"
SIZE = 560
SEQUENCE_OFFSET = 0x150
# ldr cached width; cbnz (skip refresh); load/store BufferSize.X/Y; check existing depth target.
SEQUENCE = bytes.fromhex("e81a41b9 a8000035 a8f642b9 e81a01b9 a9fa42b9 e91e01b9 890240f9 690500b5")
NOP = struct.pack("<I", 0xD503201F)
FIXED_SEQUENCE = SEQUENCE[:4] + NOP + SEQUENCE[8:]


def depth_site(data: bytes) -> tuple[int, bool] | None:
    symbols = elf.find_symbols(data, (ALLOC,))
    if ALLOC not in symbols:
        return None
    va, size = symbols[ALLOC]
    off = elf.vaddr_to_offset(elf.load_segments(data), va)
    if size != SIZE or off is None or off + size > len(data):
        return None
    start = off + SEQUENCE_OFFSET
    code = data[start:start + len(SEQUENCE)]
    if code not in (SEQUENCE, FIXED_SEQUENCE):
        return None
    return start + 4, code == FIXED_SEQUENCE


class UnrealMultiviewDepth(Patch):
    id = "frame.unreal_multiview_depth"
    title = "Unreal: match multiview depth to the eye size"
    description = (
        "Some UE4 games keep the initial Android window size for their stereo depth buffer, clipping the upper "
        "part of the environment while menu panels look correct. Refreshes the cached depth dimensions from the "
        "current eye render target before allocation. Preserves multiview and multisampling. Verified in "
        "Tales from the Galaxy's Edge 1.1.9; changes one branch only when the function size and surrounding code "
        "match.")
    order = 49

    def applies(self, a):
        return a.engine == "Unreal" and "arm64-v8a" in a.abis

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        if ws.abi != "arm64-v8a":
            return False
        lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
        if not lib:
            return False
        data = ws.read_lib(lib)
        site = depth_site(data)
        if not site:
            ctx.notes.append(f"{self.id}: {lib} doesn't match the tested depth allocator, left unchanged")
            return False
        off, done = site
        if done:
            return False
        ws.put(ws.lib(lib), data[:off] + NOP + data[off + 4:])
        ctx.notes.append(f"Mobile multiview depth size refreshed from eye size ({lib} file offset {off:#x})")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
        if not lib or ws.abi != "arm64-v8a":
            return []
        site = depth_site(ws.read_lib(lib))
        return [("Mobile multiview depth matches eye size", bool(site and site[1]),
                 "" if site else "code not found (different build)")]


register(UnrealMultiviewDepth)
