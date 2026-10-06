"""Unity 2017-era built-in Oculus support starts its VR device only on a Quest/Go: libunity.so asks Android's package
manager for com.oculus.systemactivities (Meta's system UI) and, when the lookup fails, quietly falls back to its "None"
VR device. Lepton's Android has no such package, so the game ran as a plain 2D app that Lepton never shows (only the
Android home screen, e.g. Accounting+, Unity 2017.4). The package name in libunity.so is pointed at "android" (always
installed): a same-length, in-place string edit; everything else about the check stays as it is."""
from __future__ import annotations

import os

from ...analysis import elf
from ..base import ApkContext, Patch, Suggestion, register
from . import artifact

PACKAGE = "com.oculus.systemactivities"
ALWAYS_THERE = "android"
# Unity's legacy frame loop never calls ovrp_WaitToBeginFrame: its ovrp_Update2 lookup goes to native/ovrpshim
SHIM = "libfp_ovrp.so"
UPDATE, SHIM_UPDATE = "ovrp_Update2", "fpov_Update2"
# input diagnostics (off): the C# P/Invoke names in libil2cpp.so pointed at the shim's wrappers, which log what they
# return, report input focus as true and release buttons held > 2 s. Accounting+ still didn't pass "press any button"
# with them (input reached the game cleanly), so they stay off; switch on to investigate another game.
INPUT_PROBE = os.environ.get("FRAMEPORT_INPUT_PROBE") == "1"  # diagnostic builds only
INPUT_CALLS = ("ovrp_GetConnectedControllers", "ovrp_GetControllerState4", "ovrp_GetControllerState2",
               "ovrp_GetAppHasInputFocus")
UNITY_INPUT_CALLS = ("ovrp_GetControllerState", "ovrp_GetControllerState2")


class UnityOculusCheck(Patch):
    id = "frame.unity_oculus_check"
    title = "Unity: start VR without Meta's system apps"
    description = ("Unity's built-in Oculus support (Unity 2017–2019) only starts VR when Android has Meta's "
                   "com.oculus.systemactivities package; without it the game runs as a 2D app (the Android home "
                   "screen or a black window, e.g. Accounting+, BattleSisters). Points that package name in "
                   "libunity.so at \"android\", which always exists. Games on Unity's built-in VR (2017–2018, and 2019 "
                   "without the Oculus XR Plugin) also get the frame wait their legacy frame loop never makes "
                   "(libfp_ovrp.so calls ovrp_WaitToBeginFrame before ovrp_Update2; without it no frame starts, the "
                   "dashboard freezes or the GPU hangs). The shim also counts a newly pressed trigger or A/B/X/Y as a "
                   "mouse click (Input.GetMouseButtonDown), which Go-era screens wait for (e.g. Accounting+'s motion "
                   "warning) and which Lepton never delivers.")
    order = 45
    # 2: frame wait also for Unity 2019 without the Oculus XR Plugin; 3: controller presses as mouse clicks (ovrpshim)
    revision = 3

    @staticmethod
    def _major(a) -> int:
        return int(str((a.extra or {}).get("unity_version") or "0").split(".")[0] or 0)

    @classmethod
    def legacy_loop(cls, a) -> bool:
        """Unity's built-in Oculus VR drives OVRPlugin without waiting for frames: Unity 2017-2018, and 2019 games
        without the Oculus XR Plugin (BattleSisters flooded "outside of frame bounds" and hung the GPU). With
        libOculusXRPlugin.so (XR Plugin Management, e.g. Lucky's Tale) the plugin waits itself."""
        major = cls._major(a)
        return 0 < major < 2019 or (major == 2019 and "libOculusXRPlugin.so" not in a.libs)

    def applies(self, a):
        # any Unity with built-in Oculus support whose libunity.so has the check (BattleSisters, Unity 2019.4, stayed
        # a 2D app without it); Lucky's Tale (2019.4) runs either way
        return a.engine == "Unity" and "libOVRPlugin.so" in a.libs and bool((a.extra or {}).get("unity_oculus_check"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Unity with built-in Oculus support: it checks for Meta's system apps before "
                                    "starting VR, else it runs as a 2D app (e.g. Accounting+, BattleSisters).")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        name = ws.lib("libunity.so")
        if not ws.has(name):
            return False
        data, count = elf.replace_rodata_string(ws.read(name), PACKAGE, ALWAYS_THERE)
        if not count:
            return False
        data, loops = elf.replace_rodata_string(data, UPDATE, SHIM_UPDATE) if self.legacy_loop(ctx.analysis) \
            else (data, 0)
        plugin = ws.lib("libOVRPlugin.so")
        if loops and ws.abi == "arm64-v8a" and ws.has(plugin):
            ovrp = ws.read(plugin)
            if SHIM.encode() not in ovrp:
                ws.put(plugin, elf.add_needed(ovrp, SHIM))
            ws.put(ws.lib(SHIM), artifact(ws.abi, SHIM))
            ctx.notes.append(f"libunity.so: {UPDATE} -> {SHIM_UPDATE} ({SHIM} waits for each frame)")
        il2cpp = ws.lib("libil2cpp.so")
        if loops and INPUT_PROBE and ws.has(il2cpp):  # diagnostics: the game's C# input calls go through the shim
            code, probes = ws.read(il2cpp), 0
            for real in INPUT_CALLS:
                code, n = elf.replace_rodata_string(code, real, "fpov_" + real[5:])
                probes += n
            if probes:
                ws.put(il2cpp, code)
                ctx.notes.append(f"libil2cpp.so: {probes} input calls logged by {SHIM}")
        if loops and INPUT_PROBE:  # Unity's own input reads too
            for real in UNITY_INPUT_CALLS:
                data, n = elf.replace_rodata_string(data, real, "fpov_" + real[5:])
                if n:
                    ctx.notes.append(f"libunity.so: {real} logged by {SHIM}")
        ws.put(name, data)
        ctx.notes.append(f"libunity.so: {PACKAGE} -> {ALWAYS_THERE} ({count}x)")
        return True


register(UnityOculusCheck)
