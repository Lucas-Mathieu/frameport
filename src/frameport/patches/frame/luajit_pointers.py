"""Use a full-pointer Lua engine while retaining the game's xLua bindings and custom helpers."""
from __future__ import annotations

from ...analysis import elf
from ...analysis.luajit import xlua_47bit
from ..base import ApkContext, Patch, Suggestion, register
from . import artifact

LOADER = "libxlua.so"
RUNTIME = "libfp_luajit.so"


class LuaJitPointers(Patch):
    id = "frame.luajit_pointers"
    title = "Fix Lua startup crashes on Frame"
    description = ("Older xLua/LuaJIT engines reject native pointers outside their 47-bit range, causing a startup "
                   "error and sometimes a second crash during Unity shutdown (e.g. BattleTalent). Uses a Lua engine "
                   "that preserves full pointer values while keeping the game's original xLua bindings and native "
                   "helpers. Applied only to the verified compatible library build.")
    order = 48

    def applies(self, a):
        return "arm64-v8a" in a.abis and bool((a.extra or {}).get("xlua_47bit"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "The bundled Lua engine rejects Frame's wider native pointers; use the "
                                    "compatible full-pointer engine while retaining the game's bindings.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        if ws.abi != "arm64-v8a" or not ws.has(ws.lib(LOADER)):
            return False
        original = ws.read_lib(LOADER)
        runtime = artifact(ws.abi, RUNTIME)
        if RUNTIME in elf.needed(original):
            if ws.has(ws.lib(RUNTIME)) and ws.read_lib(RUNTIME) == runtime:
                return False
            ws.put(ws.lib(RUNTIME), runtime)
            return True
        if not xlua_47bit(original):
            raise ValueError("Lua pointer fix refused: this xLua library has not been verified compatible")
        names = {name for name in elf.dyn_symbols(original, True) & elf.dyn_symbols(runtime, True)
                 if name.startswith(("lua_", "luaL_", "luaJIT_", "luaopen_"))}
        patched, redirected = elf.redirect_exports(original, names)
        if not redirected:
            raise ValueError("Lua pointer fix could not redirect the verified engine's API")
        ws.put(ws.lib(LOADER), elf.add_needed(patched, RUNTIME))
        ws.put(ws.lib(RUNTIME), runtime)
        ctx.notes.append(f"{LOADER}: {len(redirected)} Lua engine functions redirected; original bindings retained")
        return True

    def validate(self, ctx: ApkContext):
        ws = ctx.ws
        if not ws.has(ws.lib(LOADER)):
            return []
        data = ws.read_lib(LOADER)
        exported = elf.dyn_symbols(data, True)
        ready = (ws.has(ws.lib(RUNTIME)) and RUNTIME in elf.needed(data)
                 and {"lua_pushlightuserdata", "lua_touserdata"} <= elf.dyn_symbols(data, False)
                 and {"xlua_get_lib_version", "xlua_gl", "luaopen_perflib"} <= exported)
        return [("Full-pointer Lua engine with original xLua bindings", ready, RUNTIME)]


register(LuaJitPointers)
