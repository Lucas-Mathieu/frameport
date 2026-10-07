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

The native surface renderer and stereo composition are documented in
[Surface video playback](../../docs/SURFACE_VIDEO.md). The original decoded
panorama is retained; projected eye views are generated on the GPU.

Build on Linux/WSL with Android NDK r27c and the tested Lepton rootfs:

```
python native/hevc/build.py --ndk /path/to/android-ndk-r27c --lepton-root /path/to/Lepton/images/rootfs
```

AOSP headers in `platform/` come from `android-11.0.0_r48` and retain their
original license notices. `fetch_headers.py` records the upstream paths.
The builder downloads unmodified FFmpeg 7.1.1 source from
https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz and verifies SHA256
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`.
Its LGPL build configuration is recorded in `build.py` and the license is
included in the APK/per-game codec directory as `COPYING.FFmpeg`.

`omx_decode_probe.cpp` and `media_codec_probe.cpp` exercise decoder selection,
original-size output, timestamps, flush/seek, EOS and teardown on the headset.
The native surface fixtures cover buffer ownership, GPU projection and
composition. See the surface documentation for validation and limitations.
