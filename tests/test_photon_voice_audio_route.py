"""frame.photon_voice_audio_route (GitHub #362 Table Troopers, #325): Photon Voice's AudioInAEC.updateAudioDevice calls
Android 12 AudioManager methods with no version check; it returns at once (in-place dex edit, as for Vivox)."""
from __future__ import annotations

import hashlib
import os
import struct
import zipfile
import zlib
from pathlib import Path

import pytest
from test_vivox_audio_route import CHECK, build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.apk.workspace import ApkWorkspace
from frameport.core.events import Reporter
from frameport.patches import base
from frameport.patches.frame import vivox_audio_route
from frameport.patches.frame.photon_voice_audio_route import AUDIO_IN, PhotonVoiceAudioRoute, patch_dex
from frameport.validate.triage import triage

AM = "Landroid/media/AudioManager;"
RUNNER = "Lcom/exitgames/photon/audioinaec/AudioInAEC$1;"
# method indices: 0 getAvailableCommunicationDevices, 1 setCommunicationDevice, 2 access$000 (see _dex)
UPDATE = [0x1054, 0x0000, 0x106E, 0, 0x0000, 0x000C, 0x000E]  # iget-object; invoke; move-result-object; return-void
ACCESS = [0x1070, 3, 0x0000, 0x000E]                          # invoke-direct {v0} updateAudioDevice; return-void
RUN = [0x1071, 2, 0x0000, 0x000E]                             # invoke-static {v0} access$000; return-void
START = [0x0012, 0x000F]                                      # const/4 v0, 0; return v0


def _dex(vivox: bool = True) -> bytes:
    return build_dex([(AM, "getAvailableCommunicationDevices", "Ljava/util/List;", None),
                      (AM, "setCommunicationDevice", "Z", None),
                      (AUDIO_IN, "access$000", "V", ACCESS),
                      (AUDIO_IN, "updateAudioDevice", "V", UPDATE),
                      (AUDIO_IN, "Start", "Z", START),
                      (RUNNER, "run", "V", RUN),
                      *[(vivox_audio_route.LISTENER, "checkAudioRouteAndApplyChanges", "V", CHECK)] * vivox])


def _code(dex: Dex, cls: str, name: str) -> list[int]:
    off = dex.code_items(cls, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_update_audio_device_returns_at_once():
    raw = _dex()
    dex = Dex(raw)
    assert patch_dex(dex) == ["updateAudioDevice"]
    assert _code(dex, AUDIO_IN, "updateAudioDevice") == [0x000E, 0] + UPDATE[2:]  # iget-object (2 units) -> return-void
    assert _code(dex, AUDIO_IN, "access$000") == ACCESS  # callers stay as they are: the mic thread goes on
    assert _code(dex, AUDIO_IN, "Start") == START
    assert _code(dex, RUNNER, "run") == RUN
    assert _code(dex, vivox_audio_route.LISTENER, "checkAudioRouteAndApplyChanges") == CHECK  # Vivox is its own patch
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_vivox_patch_leaves_photon_alone():
    dex = Dex(_dex())
    assert vivox_audio_route.patch_dex(dex) == ["checkAudioRouteAndApplyChanges"]
    assert _code(dex, AUDIO_IN, "updateAudioDevice") == UPDATE


def test_apply_through_the_workspace(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", _dex(vivox=False))
    a = detect.analyze(path, deep=False)
    assert a.extra["photon_voice_api31"] is True and a.extra["vivox_api31"] is False
    assert a.extra["analysis_version"] == detect.ANALYSIS_VERSION >= 21
    patch = base.get("frame.photon_voice_audio_route")
    assert patch.applies(a) and patch.detect(a).recommended and not patch.needs_vr
    assert not base.get("frame.vivox_audio_route").applies(a)
    with ApkWorkspace(path) as ws:
        ctx = base.ApkContext(ws, a, {}, Reporter(), {})
        assert patch.apply(ctx)
        assert ctx.notes == ["classes.dex: Photon Voice AudioInAEC returns at once in updateAudioDevice"]
        assert _code(Dex(ws.read("classes.dex")), AUDIO_IN, "updateAudioDevice")[0] == 0x000E
        assert not patch.apply(ctx)  # once


def test_detect_needs_the_flag():
    class A:
        extra = {"photon_voice_api31": True}
    assert PhotonVoiceAudioRoute().detect(A()).recommended
    A.extra = {"vivox_api31": True}
    assert PhotonVoiceAudioRoute().detect(A()) is None


# the crash of GitHub #362 (Table Troopers, the tutorial's start)
HEAD = ("10-16 16:22:38.975  1238  1413 E AndroidRuntime: FATAL EXCEPTION: Thread-23\n"
        "10-16 16:22:38.975  1238  1413 E AndroidRuntime: Process: com.cosmorama.tabletroopers, PID: 1238\n"
        "10-16 16:22:38.975  1238  1413 E AndroidRuntime: java.lang.NoSuchMethodError: No virtual method "
        "getAvailableCommunicationDevices()Ljava/util/List; in class Landroid/media/AudioManager; or its super classes "
        "(declaration of 'android.media.AudioManager' appears in /system/framework/framework.jar!classes2.dex)\n")
PHOTON = ("10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat com.exitgames.photon.audioinaec.AudioInAEC."
          "updateAudioDevice(AudioInAEC.java:107)\n"
          "10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat com.exitgames.photon.audioinaec.AudioInAEC."
          "access$000(AudioInAEC.java:23)\n"
          "10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat com.exitgames.photon.audioinaec.AudioInAEC$1."
          "run(AudioInAEC.java:185)\n"
          "10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat java.lang.Thread.run(Thread.java:923)\n")
VIVOX = ("10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat com.vivox.sdk.AudioChangeListener."
         "checkAudioRouteAndApplyChanges(AudioChangeListener.java:80)\n")
OTHER = "10-16 16:22:38.975  1238  1413 E AndroidRuntime: \tat com.example.voice.Router.pick(Router.java:12)\n"


@pytest.mark.parametrize("stack, finding, suggest", [
    (PHOTON, "photon-voice-api31", ["frame.photon_voice_audio_route"]),
    (VIVOX, "vivox-api31", ["frame.vivox_audio_route"]),
    (OTHER, "audio-api31", []),
])
def test_triage_names_the_sdk_in_the_stack(stack, finding, suggest):
    r = triage(HEAD + stack, "EXITED", "com.cosmorama.tabletroopers")
    assert [f.id for f in r.findings] == [finding]  # java-crash and the generic one are superseded
    assert r.findings[0].suggest == suggest
    assert r.findings[0].report is (finding == "audio-api31")


def _table_troopers_apk() -> Path:
    root = os.environ.get("FRAMEPORT_GAMES")
    if not root:
        pytest.skip("set FRAMEPORT_GAMES")
    found = sorted(Path(root).glob("*/com.cosmorama.tabletroopers.apk")) or sorted(
        Path(root).glob("**/com.cosmorama.tabletroopers.apk"))
    if not found:
        pytest.skip("no Table Troopers APK in FRAMEPORT_GAMES")
    return found[0]


@pytest.mark.games
def test_real_table_troopers_dex():
    with zipfile.ZipFile(_table_troopers_apk()) as z:
        raw = z.read("classes.dex")
    assert vivox_audio_route.patch_dex(Dex(raw)) == []  # no Vivox here: that patch was a no-op (#362)
    dex = Dex(raw)
    off = dex.code_items(AUDIO_IN, "updateAudioDevice")[0]
    assert patch_dex(dex) == ["updateAudioDevice"]
    out = dex.finish()
    assert len(out) == len(raw)
    # only the first instruction changed (const/16 -> return-void; nop), nothing else in the file
    start = off + 16
    assert raw[32:start] == out[32:start] and raw[start + 4:] == out[start + 4:]
    assert struct.unpack_from("<2H", out, start) == (0x000E, 0x0000)
    # header: SHA-1 signature and Adler-32 checksum match the new bytes
    assert out[12:32] == hashlib.sha1(out[32:]).digest()
    assert struct.unpack_from("<I", out, 8)[0] == zlib.adler32(out[12:]) & 0xFFFFFFFF
    # the microphone thread and every other method of the plugin are unchanged
    again, orig = Dex(out), Dex(raw)
    for cls in (AUDIO_IN, RUNNER):
        for idx, code in orig.class_methods(cls):
            if orig.method(idx)[1] != "updateAudioDevice":
                n = struct.unpack_from("<I", raw, code + 12)[0]
                assert raw[code:code + 16 + 2 * n] == out[code:code + 16 + 2 * n]
    assert patch_dex(again) == []
