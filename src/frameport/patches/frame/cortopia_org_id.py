"""Cortopia Studios' Meta platform start-up waits forever for an org-scoped user id (Escaping Wonderland, GitHub #360).

Cortopia's games start Meta's platform in `Cortopia.Scripts.Platform.Meta.OculusPlatform.Initialize` (an async
method, `<Initialize>d__13.MoveNext`): Core.AsyncInitialize, Entitlements.IsUserEntitledToApplication,
Users.GetLoggedInUser, and then, only when `Application.internetReachability` is ReachableViaLocalAreaNetwork,
Users.GetOrgScopedID(user id), whose answer only becomes the game's AnalyticsID. OVRPort's platform library has no
`ovr_User_GetOrgScopedID` (an export that returns request id 0, runtimes 3.4.3-23204ea and -aa54c3f); Meta's C#
wrapper then logs "An unknown error occurred. Request failed." and never calls the callback, so the awaited task never
finishes and the game stays on its loading screen with frames still paced.

The game's own code skips that request when the device isn't on Wi-Fi/LAN (then the analytics id stays empty, as it
does for offline players). This patch takes that path always: the `b.ne` after `cmp w0, #2` that follows the call to
`get_internetReachability` in that method becomes an unconditional `b`. The licence check and the logged-in user come
before it and are untouched. Methods are found with Cpp2IL; the site must match exactly once, with the
GetOrgScopedID call inside the skipped part, or nothing is changed."""
from __future__ import annotations

from ..base import ApkContext, Patch, Suggestion, register
from .unity_text_input import ALL_TARGETS, METADATA, Il2cppReturnPatch, _executable, can_rewrite_il2cpp

PLATFORM = "Assembly-CSharp/Cortopia/Scripts/Platform/Meta/OculusPlatform.cs"
APPLICATION = "UnityEngine.CoreModule/UnityEngine/Application.cs"
USERS = "Oculus.Platform/Oculus/Platform/Users.cs"
WANTED = {PLATFORM: ["MoveNext"], APPLICATION: ["get_internetReachability"], USERS: ["GetOrgScopedID"]}
for _cls, _names in WANTED.items():  # one Cpp2IL run (and cache entry) serves every IL2CPP patch
    ALL_TARGETS.setdefault(_cls, [])
    ALL_TARGETS[_cls] += [n for n in _names if n not in ALL_TARGETS[_cls]]

CMP_W0_2 = 0x7100081F  # cmp w0, #2 (subs wzr, w0, #2): NetworkReachability.ReachableViaLocalAreaNetwork


def _word(data: bytes | bytearray, off: int) -> int:
    return int.from_bytes(data[off:off + 4], "little")


def _bl_target(word: int, off: int) -> int | None:
    if word & 0xFC000000 != 0x94000000:
        return None
    imm = word & 0x3FFFFFF
    return off + ((imm - (1 << 26) if imm & (1 << 25) else imm) << 2)


def skip_org_id(lib: bytes, method: tuple[int, int], reachability: int, org_scoped_id: int
                ) -> tuple[bytes | None, list[str]]:
    """In the method at (file offset, length): `bl get_internetReachability; cmp w0, #2; b.ne X` -> `b X`, when the
    skipped code [b.ne + 4, X) calls GetOrgScopedID. Offsets are file offsets (one code segment: bl's relative
    displacement is the same in the file). Returns (patched library or None, notes); raises when the code differs."""
    start, length = method
    if start % 4 or not any(a <= start and start + length <= b for a, b in _executable(lib)):
        raise RuntimeError(f"OculusPlatform.Initialize: offset {start:#x} isn't code in libil2cpp.so")
    sites, done = [], []
    for off in range(start, start + length - 8, 4):
        if _bl_target(_word(lib, off), off) != reachability or _word(lib, off + 4) != CMP_W0_2:
            continue
        br = off + 8
        w = _word(lib, br)
        if w & 0xFF00001F == 0x54000001:  # b.ne
            imm = (w >> 5) & 0x7FFFF
            target = br + ((imm - (1 << 19) if imm & (1 << 18) else imm) << 2)
            if br < target <= start + length and any(_bl_target(_word(lib, o), o) == org_scoped_id
                                                     for o in range(br + 4, target, 4)):
                sites.append((br, target))
        elif w & 0xFC000000 == 0x14000000:  # already an unconditional b
            done.append(br)
    if not sites:
        if done:
            return None, ["OculusPlatform.Initialize: org-scoped id request already skipped"]
        raise RuntimeError("OculusPlatform.Initialize: the network check before GetOrgScopedID wasn't found")
    if len(sites) > 1:
        raise RuntimeError(f"OculusPlatform.Initialize: {len(sites)} candidate checks; not guessing which one")
    br, target = sites[0]
    out = bytearray(lib)
    out[br:br + 4] = (0x14000000 | (((target - br) >> 2) & 0x3FFFFFF)).to_bytes(4, "little")
    return bytes(out), [f"OculusPlatform.Initialize: skips Users.GetOrgScopedID (b.ne -> b at {br:#x})"]


class CortopiaOrgId(Patch):
    id = "frame.cortopia_org_id"
    title = "Cortopia games: don't wait for Meta's org-scoped id"
    description = ("Cortopia Studios' games (e.g. Escaping Wonderland) ask Meta's platform for an org-scoped user id "
                   "at start when on Wi-Fi, only to use it as their analytics id. OVRPort's platform library doesn't "
                   "answer that request (\"An unknown error occurred. Request failed.\"), so the game waits forever "
                   "on its loading screen. Makes the game take its own offline path for that one request (the "
                   "analytics id stays empty); the licence check and the logged-in user before it are unchanged. "
                   "Found with Cpp2IL in libil2cpp.so.")
    order = 49

    def applies(self, a):
        return (a.engine == "Unity" and "libil2cpp.so" in a.libs and "arm64-v8a" in a.abis
                and bool((a.extra or {}).get("cortopia_org_id")))

    def detect(self, a):
        if self.applies(a) and can_rewrite_il2cpp(a):
            return Suggestion(True, "Cortopia's Meta platform start-up waits for an org-scoped user id that "
                                    "OVRPort's platform library never answers (stuck loading screen).")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        from ...analysis.il2cpp import find_methods

        ws = ctx.ws
        lib_name = ws.lib("libil2cpp.so")
        if ws.abi != "arm64-v8a" or not ws.has(lib_name) or not ws.has(METADATA):
            return False
        check = "Cortopia platform start-up"
        version = (ctx.analysis.extra or {}).get("unity_version") or Il2cppReturnPatch._unity_version(ws)
        if not version:
            ctx.reporter.check(check, False, "not fixed: unknown Unity version (rescan the game)")
            return False
        lib = ws.read(lib_name)
        ctx.reporter.log(f"{check}: finding the code (Cpp2IL; the first time can take a minute)")
        try:
            found = find_methods(lib, ws.read(METADATA), version, ALL_TARGETS)
            method = found.get(PLATFORM, {}).get("MoveNext")
            reach = found.get(APPLICATION, {}).get("get_internetReachability")
            org = found.get(USERS, {}).get("GetOrgScopedID")
            if not (method and reach and org):
                ctx.notes.append(f"{check}: code not found in this game")
                return False
            patched, notes = skip_org_id(lib, method, reach[0], org[0])
        except Exception as exc:  # noqa: BLE001 - optional fix: the game still builds, unchanged
            ctx.reporter.check(check, False, f"not fixed: {exc}")
            return False
        ctx.notes.extend(notes)
        if patched is not None:
            ws.put(lib_name, patched)
        return patched is not None


register(CortopiaOrgId)
