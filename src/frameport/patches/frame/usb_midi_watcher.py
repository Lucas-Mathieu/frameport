"""kshoji's USB MIDI driver for Android (jp.kshoji.driver.midi, used by Unity MIDI plugins, e.g. PianoVision for USB
pianos, GitHub #297) polls Android's UsbManager for connected MIDI devices on its own thread. Lepton's Android has no
USB service (getSystemService("usb") is null), so the first poll throws a NullPointerException on
MidiDeviceConnectionWatchThread and the app dies a few seconds after start. The poll
(MidiDeviceConnectionWatchThread.checkConnectedDevices) returns at once instead: the watcher then never sees a USB
device, which on the Frame it couldn't reach anyway (in-place classes.dex edit, see vivox_audio_route)."""
from __future__ import annotations

from ...apk.dex import Dex
from ..base import ApkContext, Patch, Suggestion, register

WATCH_THREAD = "Ljp/kshoji/driver/midi/device/MidiDeviceConnectionWatcher$MidiDeviceConnectionWatchThread;"
METHOD = "checkConnectedDevices"


def patch_dex(dex: Dex) -> list[str]:
    """Methods of the watcher thread that now return at once; [] if nothing applies."""
    done = []
    for idx, code in dex.class_methods(WATCH_THREAD):
        if dex.method(idx)[1] == METHOD and dex.return_type(idx) == "V" and dex.return_early(code, "V"):
            done.append(METHOD)
    return done


class UsbMidiWatcher(Patch):
    id = "frame.usb_midi_watcher"
    title = "USB MIDI: don't poll for USB devices"
    description = ("kshoji's USB MIDI driver (Unity MIDI plugins, for example PianoVision) polls Android's USB "
                   "service for MIDI devices; the Frame's Android has no USB service, so the app crashes a few "
                   "seconds after start (NullPointerException on MidiDeviceConnectionWatchThread). The poll returns "
                   "at once instead, so no USB MIDI device is ever found (an in-place edit of classes.dex).")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("usb_midi_watcher"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "kshoji's USB MIDI driver polls Android's USB service, which the Frame's "
                                    "Android lacks: the app would crash a few seconds after start.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        changed = False
        for name in sorted(n for n in ctx.ws.names() if n.startswith("classes") and n.endswith(".dex")):
            data = ctx.ws.read(name)
            if WATCH_THREAD.encode() not in data:
                continue
            dex = Dex(data)
            if patch_dex(dex):
                ctx.ws.put(name, dex.finish())
                ctx.notes.append(f"{name}: USB MIDI watcher's {METHOD} returns at once")
                changed = True
        return changed


register(UsbMidiWatcher)
