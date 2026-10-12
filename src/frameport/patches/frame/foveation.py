"""APK support for device.foveation; runtime configuration lives in settings.py.

Shared injection needs no additional APK edits in RE4/Deadpool. Batman's tested
OVRPlugin build needs ordinary shared allocation; never apply those offsets to
another build. The optional diagnostic instruments actual coarse fragments.
"""
from __future__ import annotations

import hashlib
import io
import struct

from elftools.elf.elffile import ELFFile

from ...analysis import elf
from . import artifact
from .zink_shader_fix import ENGINE_LIBS, NOT_ENGINE

LAYER = "libVkLayer_fdm_darken.so"
BATMAN_ORIGINAL = "76cafa25c2243c2bc065e30d83d7083b8546153dc6a3c39227235fbabe13c969"
CLIMB_RENDERER = "31a121dbfae692fdc0e54853e0640eed820f07826893fa0d905767c9620023a0"
CLIMB_BRIDGE = "libvrapi_foveation.so"
STORAGE_WORDS = ((0x21c778, 0x5298000b, 0x5290000b),
                 (0x21c77c, 0x5288000c, 0x5280000c),
                 (0x21df50, 0x52980009, 0x52900009))


def ordinary_storage(data: bytes) -> bytes:
    """Accept only the verified original, or exactly its already-patched form."""
    image = ELFFile(io.BytesIO(data))
    original, patched = bytearray(data), bytearray(data)
    for va, old, new in STORAGE_WORDS:
        seg = next((s for s in image.iter_segments() if s["p_type"] == "PT_LOAD"
                    and s["p_vaddr"] <= va and va + 4 <= s["p_vaddr"] + s["p_filesz"]), None)
        if seg is None:
            raise ValueError("Batman foveation: unsupported OVRPlugin build; allocation fix not applied")
        offset = va - seg["p_vaddr"] + seg["p_offset"]
        if struct.unpack_from("<I", data, offset)[0] not in (old, new):
            raise ValueError("Batman foveation: unsupported OVRPlugin instructions")
        struct.pack_into("<I", original, offset, old)
        struct.pack_into("<I", patched, offset, new)
    if hashlib.sha256(original).hexdigest() != BATMAN_ORIGINAL:
        raise ValueError("Batman foveation: this OVRPlugin version has no verified allocation fix")
    return bytes(patched)


def engine_lib(ws):
    lib = next((n for n in ENGINE_LIBS if ws.has(ws.lib(n))), None)
    if lib:
        return lib
    for name in ws.libs():
        if name.startswith(NOT_ENGINE):
            continue
        data = ws.read_lib(name)
        if (elf.is_elf(data) and {"vkGetInstanceProcAddr", "vkCreateInstance", "eglInitialize"}
                & elf.dyn_symbols(data, False)):
            return name
    return None


def apply(ctx):
    from ..settings import foveation_options

    mode, _radius, test = foveation_options(ctx.params)
    if mode not in ("tracked", "fixed"):
        return False
    ws = ctx.ws
    if ws.abi != "arm64-v8a":
        raise ValueError("Shared foveation currently supports arm64-v8a games only")
    changed = False
    if ctx.analysis.package == "com.camouflaj.manta":
        data = ws.read_lib("libOVRPlugin.so")
        fixed = ordinary_storage(data)
        if fixed != data:
            ws.put(ws.lib("libOVRPlugin.so"), fixed)
            ctx.notes.append("verified Batman ordinary shared-image allocation fix")
            changed = True
    climb = ctx.analysis.package == "com.crytek.climb2"
    if climb:
        if (not ws.has(ws.lib("libCryRenderVulkan.so")) or
                hashlib.sha256(ws.read_lib("libCryRenderVulkan.so")).hexdigest() != CLIMB_RENDERER):
            raise ValueError("The Climb 2 foveation: unsupported renderer build")
        bridge = elf.add_needed(artifact(ws.abi, CLIMB_BRIDGE), LAYER)
        if not ws.has(ws.lib("libvrapi.so")) or ws.read_lib("libvrapi.so") != bridge:
            ws.put(ws.lib("libvrapi.so"), bridge)
            changed = True
        ctx.notes.append("Climb private bridge: stereo gaze, subpass and held-focus compatibility")
    if test or climb:
        lib = "libvrapi.so" if climb else engine_lib(ws)
        if not lib:
            raise ValueError("Foveation test: no supported renderer library found")
        data = ws.read_lib(lib)
        if LAYER not in elf.needed(data):
            ws.put(ws.lib(lib), elf.add_needed(data, LAYER))
            changed = True
        layer = artifact(ws.abi, LAYER)
        if not ws.has(ws.lib(LAYER)) or ws.read_lib(LAYER) != layer:
            ws.put(ws.lib(LAYER), layer)
            changed = True
        if test:
            ctx.notes.append("coarse-fragment darkening diagnostic; not for benchmarking")
    return changed


def validate(ctx):
    from ..settings import foveation_options

    mode, _radius, test = foveation_options(ctx.params)
    checks = []
    if mode in ("tracked", "fixed") and ctx.analysis.package == "com.camouflaj.manta":
        data = ctx.ws.read_lib("libOVRPlugin.so")
        checks.append(("Batman shared allocation fix", ordinary_storage(data) == data, "verified OVRPlugin"))
    if mode in ("tracked", "fixed") and (test or ctx.analysis.package == "com.crytek.climb2"):
        lib = "libvrapi.so" if ctx.analysis.package == "com.crytek.climb2" else engine_lib(ctx.ws)
        checks.append(("Foveation darkening layer loaded", bool(lib) and ctx.ws.has(ctx.ws.lib(LAYER))
                       and LAYER in elf.needed(ctx.ws.read_lib(lib)), lib or "no renderer"))
    return checks
