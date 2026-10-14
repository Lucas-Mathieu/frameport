"""Verified xLua/LuaJIT engines whose lightuserdata is limited to 47-bit pointers."""
from __future__ import annotations

import hashlib

# xLua API 105, LuaJIT 2.1 beta3, GC64. The bindings use the public Lua API;
# xlua_gl reads lua_State.glref at offset 16, unchanged in the replacement.
# Match the complete original library: custom builds need their own ABI review.
XLUA_47BIT_SHA256 = frozenset({
    "6d5860e4a4f1cc2b05c600c63297ed467369739666edca79413fff4374ff2684",
})


def xlua_sha256(data: bytes) -> str | None:
    """Diagnostics can identify an unreviewed engine without sharing the library itself."""
    return hashlib.sha256(data).hexdigest() if data else None


def xlua_47bit(data: bytes) -> bool:
    return xlua_sha256(data) in XLUA_47BIT_SHA256
