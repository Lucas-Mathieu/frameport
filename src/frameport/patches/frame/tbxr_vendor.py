"""Team Beef's TBXR ports (Lambda1VR, RTCWQuest, ...) choose their OpenXR setup by headset maker.

Their Java activity does System.loadLibrary("openxr_loader_" + Build.MANUFACTURER) and sets OPENXR_HMD to the same
name; Lepton reports the maker "valve", so the app stops at once (UnsatisfiedLinkError: libopenxr_loader_valve.so not
found). Past that, the native code takes Meta's path only where strstr(OPENXR_HMD, "meta") matches and Pico's
otherwise (XR_PICO_configs_ext and a NULL xrSetConfigPICO call). The patch adds an empty libopenxr_loader_valve.so and
turns the "meta" literal those checks compare with into "alve", which "valve" contains: the Meta path, which OVRPort
translates. Checked in Lambda1VR 1.7.3's libxash.so: all six uses of the literal are these strstr checks.
"""
from __future__ import annotations

from ...analysis import elf
from ...analysis.stubgen import build_stub_library
from ..base import ApkContext, Patch, Suggestion, register

VENDOR_LOADER = "libopenxr_loader_valve.so"


class TbxrVendor(Patch):
    id = "frame.tbxr_vendor"
    title = "Team Beef ports: treat the Frame as a Meta headset"
    description = ("Team Beef's ports (e.g. Lambda1VR, RTCWQuest) load an OpenXR library named after the headset maker "
                   "and pick their VR setup by maker; for \"valve\" they stop at start (libopenxr_loader_valve.so not "
                   "found) or take the Pico path. Adds that library and lets them take the Meta path.")
    order = 47

    def applies(self, a):
        return bool((a.extra or {}).get("tbxr_libs")) and "arm64-v8a" in a.abis

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Team Beef port: it picks its OpenXR setup by headset maker, which the Frame "
                                    "reports as \"valve\".")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        ws = ctx.ws
        if ws.abi != "arm64-v8a":
            return False
        changed = False
        for lib in (ctx.analysis.extra or {}).get("tbxr_libs") or []:
            if not ws.has(ws.lib(lib)):
                continue
            data, count = elf.replace_rodata_string(ws.read_lib(lib), "meta", "alve")
            if count:
                ws.put(ws.lib(lib), data)
                ctx.notes.append(f"{lib}: headset checks for \"meta\" match \"valve\" ({count})")
                changed = True
        if not ws.has(ws.lib(VENDOR_LOADER)):
            ws.put(ws.lib(VENDOR_LOADER), build_stub_library([], soname=VENDOR_LOADER))
            changed = True
        return changed


register(TbxrVendor)
