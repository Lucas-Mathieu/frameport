# Hardware HEVC for Lepton

`frameport_hevc.cpp` implements `OMX.frameport.hevc.decoder` using the Iris
stateful V4L2 decoder through FFmpeg's LGPL hardware wrapper. The FFmpeg build
does not contain a software HEVC decoder. Byte-buffer clients receive full-size
planar frames. Native surface clients receive YUV in Android hardware buffers,
with acquire/release fences. The hardware decoder's NV12 planes are copied to
the Android buffer in disjoint parallel row bands without CPU colour conversion.
The same persistent worker pool handles native YUV copies and legacy RGBA
conversion; all workers finish before the buffer is unlocked or the decoded
frame is released. Legacy software surfaces
retain parallel NEON RGBA conversion. Input timestamps, dynamic dimensions, EOS and
seek/flush are preserved.

This uses the tested Lepton Android 11 SoftOMX ABI. FramePort stores the plugin,
codec XML and a narrow Podman wrapper per game. The launcher adds this wrapper
to its child process's PATH. Only the matching game's `podman run` gains a
read-only plugin/XML mount and `/dev/video-dec0`. All other Podman operations
pass through. Shared Lepton, drivers and original MP4 assets are unchanged.
Unknown runtime ABIs retain the stock codecs and log why. A future native
Lepton hardware plugin takes precedence.

Packaging is limited to the validated arm64 Batman package (`com.camouflaj.manta`).
Unrelated games and arm32 APKs do not receive these assets or the native video
setting. Rebuilding an older unrelated test APK removes its previous codec
assets, and installation disables its old per-game wrapper. The presence of
an MP4 alone does not establish compatible video-surface or overlay semantics.

The plugin probes admission of an 8192x4096 decoder session before advertising
its component. On the tested SteamOS kernel, another active decoder (including
Steam's web helper) can cause Iris to reject that session. In that condition it
leaves Android's stock software decoder available. This fallback preserves
playback, but does not provide hardware performance. Admission can change after
the probe; this is not a fix for the kernel's session-accounting defect.

On the tested headset, disabling **hardware video decoding in Steam's interface**
and restarting Steam removed that conflicting session. This setting does not
disable the game's Iris decoder. It is a SteamOS-driver workaround, not a kernel
fix; FramePort does not silently change the global Steam setting.

Batman uses an internal FrameBridge GPU surface path: an AImageReader retains
the decoder's actual 8192x4096 buffer. GLES converts and projects the visible
stereo view in one pass, sampling that original YUV buffer directly. Vulkan
copies only the finished eye view to the runtime. There is no full-panorama RGB
intermediate, picture readback or CPU colour conversion/upload. The default
projection remains 2048x2048 per eye; an explicit equirect_res takes precedence.
Shared output buffers are reusable only after Vulkan returns foreign ownership
and its fence signals. The decoder image remains acquired while GLES may sample
it for head movement, including during a paused video.

Projection poses use the application's submitted views when available, then an
exact session/time/space cache match, then the runtime at the submitted display
time. Each completed eye view carries the pose and space used to render it; a
late worker result is never labelled with a newer head pose. Video arrivals are
coalesced into application view requests, preventing video-fps + headset-fps
rendering. No video files, UI wording, or UI controls are changed.

Build on Linux/WSL with Android NDK r27c and a copy of the tested Lepton rootfs:

```
python native/hevc/build.py --ndk /path/to/android-ndk-r27c --lepton-root /path/to/Lepton/images/rootfs
```

AOSP headers in `platform/` come from `android-11.0.0_r48` and retain their
Apache 2.0 notices. `fetch_headers.py` records the upstream paths. FFmpeg 7.1.1
source is downloaded from https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz and
verified against SHA256
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`.
Its unmodified LGPL source, build configuration and license are supplied by
that archive and this builder; `artifacts/hevc/COPYING.FFmpeg` accompanies the
binary and is included in the APK/per-game codec directory. The integration
source follows FramePort's GPL-3.0 license.

`omx_decode_probe.cpp` exercises the actual component with an MP4: dynamic
output allocation, ordered timestamps, 600 original 8K frames, EOS, a full
flush, replay of 120 frames and clean teardown. This passed on SM8650 at
roughly 93–99 fps for planar delivery and 68–74 fps for RGBA delivery without
a Surface. The probe needs `libavformat` and the same Android headers and
runtime libraries. The NDK MediaCodec probe also passed default decoder
selection, 600 original 8K native-surface images, a flush/seek, 120 replayed
frames, and shutdown. YUV native delivery ran at roughly 82–85 fps. A separate
full-size GLES conversion test and a production Vulkan import/projection fixture
passed on the headset GPU. Revision 7 game logs confirmed 8192x4096 delivery and
roughly 59–60 delivered video frames/s once settled, while game submissions ran
roughly 46–48 fps (also around 48 fps before video). The user reported much better
detail and playback but remaining stalls and head-turn jitter. Revision 8 removes
the extra panorama copy and aligns projection poses where application views are
available. After the user's pause/resume, revision 8 submissions rose from about
47 to 96 fps while video delivery remained near 60 fps. Revision 9 replaces the
two GPU stages with the fused pass. On the same original clip its GPU draw/wait
averaged 3.38 ms, compared with 4.37 ms for the older full-RGB conversion alone.
The complete production surface worker passed 600 frames, a pause/resume,
flush/seek and 120 replay frames with concurrent head-view requests. These
isolated results still require an in-game VR replay.

Batman adapter revision 10 raises the projected target to 2560x2560 per eye with
a 12% tangent-space margin for compositor head-turn correction. It reuses held
pictures while both current visible eye frusta remain covered, renders fresh
video pictures, and recentres when coverage is exhausted. The codec/source video
is unchanged. Full-resolution playback/seek and the production worker passed
isolated gentle/rapid head-movement tests; visual equivalence to Quest 3 and
in-game frame pacing still need a VR comparison.

The `fallback` argument to the NDK probe checks stock-decoder selection and a
short original 8K surface playback/seek when hardware admission is rejected.
The packaged installation passed that check (six frames, then two replayed
frames). This verifies the fallback's functionality, not sustained frame rate.
