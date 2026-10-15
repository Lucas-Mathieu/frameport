"""Unreal (UE4/UE5) games with an OBB start through Epic's DownloaderActivity (GameActivity.onResume starts it for a
result before the engine's main init). It checks that every OBB exists with the name and size compiled into the APK
(OBBData), and with the manifest meta-data `com.epicgames.ue4.GameActivity.bVerifyOBBOnStartUp` = true it then reads
the whole OBB and compares each zip entry's CRC32 (once: the result is cached in files/cacheFile.txt by the OBB's
modified time). A CRC mismatch shows an error screen that waits for a button, and the check itself shows a progress
screen while it reads gigabytes; Lepton runs VR apps headless, so neither is visible and the game seems to hang on
the downloader (GitHub #159, Contractors). Setting the meta-data to false skips only that CRC pass: the name + size
check stays (a missing or wrong-size OBB still stops at the downloader). One 4-byte manifest value changes."""
from __future__ import annotations

import struct

from ...analysis.detect import UE_VERIFY_OBB_KEY
from ...apk import axml
from ...apk.dex import Dex
from ..base import ApkContext, Patch, Suggestion, register

MANIFEST = "AndroidManifest.xml"
DELIVERY_LOG = "Checking for file : "


def obfuscated_delivery_methods(dex: Dex, package: str) -> list[tuple[int, int]]:
    # MoH keeps the activity name and Epic's log text, but renames the predicates and OBBData (GitHub #159).
    cls = f"L{package.replace('.', '/')}/DownloaderActivity;"
    methods = dex.class_methods(cls)
    called = {i for idx, code in methods if dex.method(idx)[1] == "onCreate" for i in dex.invoked(code)}
    found = []
    for idx, code in methods:
        if idx not in called or dex.return_type(idx) != "Z":
            continue
        strings = {dex.string(u[1] if u[0] & 0xFF == 0x1A else u[1] | u[2] << 16)
                   for _pos, u in dex._insns(code) if u[0] & 0xFF in (0x1A, 0x1B)}
        helpers = {dex.return_type(i) for i in dex.invoked(code)
                   if dex.method(i)[0].startswith("Lcom/google/android/vending/expansion/downloader/")}
        if DELIVERY_LOG in strings and {"Z", "Ljava/lang/String;"} <= helpers:
            found.append((idx, code))
    return found


def delivery_methods(dex: Dex, package: str) -> list[tuple[int, int]]:
    classes = {f"L{package.replace('.', '/')}/{name};" for name in ("DownloaderActivity", "OBBData")}
    classes.update(f"Lcom/epicgames/{engine}/{name};" for engine in ("ue4", "unreal")
                   for name in ("DownloadShim", "GameActivity"))
    named = [(idx, code) for cls in dex.class_names() if cls in classes
            for idx, code in dex.class_methods(cls)
            if dex.method(idx)[1] == "expansionFilesDelivered" and dex.return_type(idx) == "Z"]
    return list(dict.fromkeys(named + obfuscated_delivery_methods(dex, package)))


def has_delivery_check(data: bytes, package: str) -> bool:
    if b"expansionFilesDelivered" not in data and DELIVERY_LOG.encode() not in data:
        return False
    return bool(delivery_methods(Dex(data), package))


def patch_dex(dex: Dex, package: str) -> list[str]:
    return [dex.method(idx)[0] for idx, code in delivery_methods(dex, package)
            if dex.return_early(code, "Z", boolean_value=True)]


class UnrealSkipObbCheck(Patch):
    id = "frame.unreal_skip_obb_check"
    title = "Unreal: start without the OBB check"
    description = ("Unreal games can check their whole data file (.obb) before starting (bVerifyOBBOnStartUp). On "
                   "the Frame that check's progress and error screens are invisible (Lepton runs VR games without "
                   "an Android window), so the game seems to hang on its DownloaderActivity, for example Contractors. "
                   "This turns the check off in the manifest; Unreal still checks that the OBB is there with the "
                   "right size, so a missing or wrong-version OBB still stops the game.")
    order = 23
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("unreal_verify_obb"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Unreal checks the OBB's contents before starting (bVerifyOBBOnStartUp); on the "
                                    "Frame a failed or slow check can't be seen and the game seems to hang.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        fixed = axml.set_meta_data_bool(ctx.ws.read(MANIFEST), UE_VERIFY_OBB_KEY, False)
        if fixed:
            ctx.ws.put(MANIFEST, fixed)
            ctx.notes.append("bVerifyOBBOnStartUp=false")
        return bool(fixed)

    def validate(self, ctx):
        meta = axml.meta_data(ctx.ws.read(MANIFEST))
        on = [k for k, v in meta.items() if k.endswith(UE_VERIFY_OBB_KEY) and v is True]
        return [("OBB check at start off", not on, ", ".join(on) or "bVerifyOBBOnStartUp=false")]


register(UnrealSkipObbCheck)


class UnrealSkipObbDelivery(Patch):
    id = "frame.unreal_skip_obb_delivery"
    title = "Unreal: skip the OBB delivery check"
    description = ("Unreal's Java OBB delivery check can leave a game on an invisible DownloaderActivity screen "
                   "even with the manifest CRC check off (e.g. Into The Radius). Makes known Unreal "
                   "delivery predicates return true, including recognized obfuscated versions, bypassing the Java "
                   "name and size gate. "
                   "Experimental: the native engine still needs complete, matching, readable OBB data.")
    order = 24
    needs_vr = False
    experimental = True

    def applies(self, a):
        return bool((a.extra or {}).get("unreal_obb_delivery"))

    def detect(self, a):
        return None  # triage-only: most games pass the delivery check

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if not has_delivery_check(data, ctx.analysis.package):
                continue
            dex = Dex(data)
            done = patch_dex(dex, ctx.analysis.package)
            if done:
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: expansionFilesDelivered=true in {', '.join(sorted(done))}")
                changed = True
        return changed

    def validate(self, ctx):
        checks = []
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if not has_delivery_check(data, ctx.analysis.package):
                continue
            dex = Dex(data)
            for idx, code in delivery_methods(dex, ctx.analysis.package):
                ready = (struct.unpack_from("<H", data, code)[0] > 0
                         and struct.unpack_from("<I", data, code + 12)[0] >= 2
                         and struct.unpack_from("<2H", data, code + 16) == (0x1012, 0x000F))
                checks.append(("OBB delivery check bypassed", ready, f"{name}: {dex.method(idx)[0]}"))
        return checks


register(UnrealSkipObbDelivery)
