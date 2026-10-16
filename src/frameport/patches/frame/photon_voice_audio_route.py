"""Photon Voice (Exit Games' Unity voice chat, e.g. Table Troopers, GitHub #362/#325) crashes in Lepton when voice
chat starts: its Java microphone plugin (com.exitgames.photon.audioinaec.AudioInAEC) picks the audio route in
updateAudioDevice with Android 12 AudioManager methods (getAvailableCommunicationDevices, setCommunicationDevice)
and has no Android version check left (it logs "Start: SDK >= 31, using BroadcastReceiver" on every Android), so on
Lepton's Android 11 the microphone thread dies with java.lang.NoSuchMethodError and takes the app with it. The
AudioInAEC methods that call them (updateAudioDevice: void) return at once instead, the same in-place classes.dex edit
as frame.vivox_audio_route. The microphone thread then goes on as on Android 12 (communication mode, echo canceller /
gain control / noise suppressor, AudioRecord) on Android's default route; only the device pick is skipped."""
from __future__ import annotations

from ..base import ApkContext, Patch, Suggestion, register
from .vivox_audio_route import patch_class, patch_dexes

AUDIO_IN = "Lcom/exitgames/photon/audioinaec/AudioInAEC;"


def patch_dex(dex) -> list[str]:
    """Methods of Photon Voice's AudioInAEC that now return at once; [] if nothing applies."""
    return patch_class(dex, AUDIO_IN)


class PhotonVoiceAudioRoute(Patch):
    id = "frame.photon_voice_audio_route"
    title = "Photon Voice: skip Android 12 audio routing"
    description = ("Photon Voice's microphone plugin (for example Table Troopers) picks the audio device with "
                   "Android 12 methods (AudioManager communication devices) without checking the Android version; "
                   "Lepton runs Android 11, so the game crashes with a NoSuchMethodError when voice chat starts. "
                   "That method returns at once instead (an in-place edit of classes.dex); the microphone keeps "
                   "Android's default audio route.")
    order = 44
    needs_vr = False

    def applies(self, a):
        return bool((a.extra or {}).get("photon_voice_api31"))

    def detect(self, a):
        if self.applies(a):
            return Suggestion(True, "Photon Voice calls Android 12 audio methods, which Lepton (Android 11) lacks, "
                                    "and crashes when voice chat starts.")
        return None

    def apply(self, ctx: ApkContext) -> bool:
        return patch_dexes(ctx, AUDIO_IN, "Photon Voice AudioInAEC")


register(PhotonVoiceAudioRoute)
