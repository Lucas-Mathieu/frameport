"""kshoji's USB MIDI watcher crashed PianoVision on the Frame: Lepton has no USB service (GitHub #297)."""
from __future__ import annotations

import struct
import zipfile

from test_vivox_audio_route import build_dex

from frameport.analysis import detect
from frameport.apk.dex import Dex
from frameport.patches.frame.usb_midi_watcher import WATCH_THREAD, UsbMidiWatcher, patch_dex
from frameport.validate.triage import triage

CHECK = [0x061D, 0x6054, 0x0000, 0x000E]  # monitor-enter v6; iget-object …usbManager; return-void
RUN = [0x000E]


def _dex() -> bytes:
    return build_dex([(WATCH_THREAD, "checkConnectedDevices", "V", CHECK), (WATCH_THREAD, "run", "V", RUN)])


def _code(dex: Dex, name: str) -> list[int]:
    off = dex.code_items(WATCH_THREAD, name)[0]
    n = struct.unpack_from("<I", dex.data, off + 12)[0]
    return list(struct.unpack_from(f"<{n}H", dex.data, off + 16))


def test_poll_returns_at_once():
    raw = _dex()
    dex = Dex(raw)
    assert patch_dex(dex) == ["checkConnectedDevices"]
    assert _code(dex, "checkConnectedDevices") == [0x000E] + CHECK[1:]
    assert _code(dex, "run") == RUN
    out = dex.finish()
    assert len(out) == len(raw) and patch_dex(Dex(out)) == []


def test_detect_and_triage(tmp_path, quest_manifest):
    path = tmp_path / "game.apk"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", quest_manifest)
        z.writestr("classes.dex", _dex())
    a = detect.analyze(path, deep=False)
    assert a.extra["usb_midi_watcher"] is True
    assert UsbMidiWatcher().applies(a) and UsbMidiWatcher().detect(a).recommended
    log = ("10-16 04:17:10.829  1223  1344 E AndroidRuntime: FATAL EXCEPTION: MidiDeviceConnectionWatchThread\n"
           "10-16 04:17:10.829  1223  1344 E AndroidRuntime: java.lang.NullPointerException: Attempt to invoke virtual "
           "method 'java.util.HashMap android.hardware.usb.UsbManager.getDeviceList()' on a null object reference\n"
           "10-16 04:17:10.829  1223  1344 E AndroidRuntime: \tat jp.kshoji.driver.midi.device.MidiDeviceConnection"
           "Watcher$MidiDeviceConnectionWatchThread.checkConnectedDevices(MidiDeviceConnectionWatcher.java:304)\n")
    r = triage(log, "EXITED", "com.ZarApps.PianoVision")
    assert [f.id for f in r.findings] == ["usb-midi-watcher"]
    assert r.findings[0].suggest == ["frame.usb_midi_watcher"]
