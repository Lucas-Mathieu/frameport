"""Bypass UE4 OculusHMD's independent loading renderer (Tales from the Galaxy's Edge, GitHub #61).

During map travel the game thread flushes rendering while FSplash waits for a frame the normal renderer hasn't
begun. Neither thread can proceed. Returning from the splash's RenderFrame_RenderThread leaves loading on the
last submitted image and lets map travel finish. The game's normal renderer and menu layers still run.
Only the exported void function with the tested size and prologue is changed; other engine builds are refused.
"""
from __future__ import annotations

import struct

from ...analysis import elf
from ..base import ApkContext, Patch, register

ENGINE_LIBS = ("libUE4.so", "libUnreal.so")
RENDER = "_ZN9OculusHMD7FSplash24RenderFrame_RenderThreadER24FRHICommandListImmediate"
SIZE = 2864
# Tales 1.1.9+734762.cl.439699: stack allocation followed by the callee-saved register pairs.
PROLOGUE = bytes.fromhex("ffc304d1 fc6f0da9 fa670ea9 f85f0fa9 f65710a9 f44f11a9 fd7b12a9 fd830491")
RET = struct.pack("<I", 0xD65F03C0)


def splash_site(data: bytes) -> tuple[int, bool] | None:
    symbols = elf.find_symbols(data, (RENDER,))
    if RENDER not in symbols:
        return None
    va, size = symbols[RENDER]
    off = elf.vaddr_to_offset(elf.load_segments(data), va)
    if size != SIZE or off is None or off + size > len(data):
        return None
    head = data[off:off + len(PROLOGUE)]
    if head not in (PROLOGUE, RET + PROLOGUE[4:]):
        return None
    return off, head.startswith(RET)


class UnrealSplashBypass(Patch):
    id = "frame.unreal_splash_bypass"
    title = "Unreal: bypass the stalled loading splash"
    description = (
        "Some UE4 Oculus games stall during map loading: the game waits for the render thread, and the separate "
        "loading splash waits for an unbegun VR frame. Bypasses the splash renderer so loading finishes; the last "
        "image stays visible during loading. The normal environment and menu rendering continue. Verified in "
        "Tales from the Galaxy's Edge 1.1.9; changes one instruction only when the function size and prologue match.")
    order = 48

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
        site = splash_site(data)
        if not site:
            ctx.notes.append(f"{self.id}: {lib} doesn't match the tested splash renderer, left unchanged")
            return False
        off, done = site
        if done:
            return False
        ws.put(ws.lib(lib), data[:off] + RET + data[off + 4:])
        ctx.notes.append(f"Oculus loading splash renderer bypassed ({lib} file offset {off:#x})")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
        if not lib or ws.abi != "arm64-v8a":
            return []
        site = splash_site(ws.read_lib(lib))
        return [("Oculus loading splash renderer bypassed", bool(site and site[1]),
                 "" if site else "code not found (different build)")]


register(UnrealSplashBypass)
