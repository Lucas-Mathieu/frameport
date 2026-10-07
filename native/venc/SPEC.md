# fp_venc — specification

A small program that runs **on the Steam Frame** (SteamOS, aarch64 Linux with glibc) and hardware-encodes the
SteamVR "headset view" webcam to an H.264 elementary stream on stdout. FramePort's live view (PC side) pipes that
stream into ffmpeg on the Frame, which adds sound and packs fragmented MP4. The program is used for **live viewing
only**: low latency, constant bitrate, evenly spaced frames, a keyframe whenever the viewer side asks for one.

Sources: the Linux kernel's V4L2 documentation (`Documentation/userspace-api/media/v4l/dev-encoder.rst` = the
memory-to-memory stateful encoder interface, `ext-ctrls-codec.rst`, `buffer.rst`, `vidioc-*.rst`), the DRM KMS UAPI
(`drm_mode.h`), and the facts measured on the Frame listed in "Measured facts". Nothing else.

## Files
- `native/venc/convert.h`, `convert.c` — the picture converter (pure computation, no syscalls). Must compile for
  **aarch64** (NEON path + scalar path) **and x86_64** (scalar path only; used by the host unit test).
- `native/venc/sys.h` (+ `.c` if wanted) — raw Linux aarch64 syscalls (`svc #0`), `_start`, `memcpy`/`memset`/`memmove`
  (the compiler may emit calls to them), small string/number helpers (parse decimal, print decimal, write a whole buffer).
- `native/venc/fp_venc.c` — command line, source, encoder, DRM refresh query, main loop.
- `native/venc/include/sys/time.h` — minimal stub so the kernel UAPI `linux/videodev2.h` compiles without a libc
  (`struct timeval { long tv_sec; long tv_usec; };` and whatever else that header needs).
- `native/build.py` — a `build_venc(tc)` step (see "Build").

Keep it small and readable: plain C11, no clever macros, comments that explain *why*. Target ≤ ~900 lines total.

## Build
No libc exists at build time (the toolchain is the Android NDK's clang; its sysroot is bionic). The program is a fully
static, freestanding executable:

```
clang --target=aarch64-linux-gnu -ffreestanding -nostdlib -nostdlibinc -static -fno-stack-protector
      -fno-builtin -O2 -Wall -Wextra -Werror -fuse-ld=lld -Wl,--build-id=none -Wl,-z,max-page-size=65536
      -e _start -I native/venc/include -I <uapi> -ffile-prefix-map=<native dir>=native
      fp_venc.c convert.c [sys.c] -o artifacts/linux-arm64-bin/fp_venc
```
- `<uapi>`: a staging directory made by build.py under `native/.cache/uapi-arm64/` containing **only** copies of the
  NDK sysroot's kernel UAPI folders `usr/include/linux`, `usr/include/asm-generic`, `usr/include/drm` and
  `usr/include/aarch64-linux-android/asm` (as `asm/`). Never put the bionic libc headers on the include path.
- `stdint.h`, `stddef.h`, `stdbool.h` and `arm_neon.h` come from clang's own resource directory (allowed).
- Floating point is allowed but not needed; prefer integer math.
- Add `venc` to build.py's `--only` default list, the docstring, the `steps` dict and the ordered build tuple (last).
  The step creates `artifacts/linux-arm64-bin/` itself (not `linux-arm64/`: that folder is uploaded whole as the OpenXR layer). `write_sums()` then lists the new file.
- The binary must be reproducible (same source → same bytes) and contain no local paths.

## Command line
```
fp_venc [--height N] [--bitrate BPS] [--fps N] [--max-fps N] [--gop-seconds N]
        [--source PATH] [--encoder PATH] [--probe | --selftest]
```
- `--height N`: output height, scaled **down** only. Default: the source height. See "Output size".
- `--bitrate BPS`: constant bitrate in bit/s. Default 3000000.
- `--fps N`: output frame rate; overrides the automatic choice.
- `--max-fps N`: cap for the automatic choice. Default 45.
- `--gop-seconds N`: keyframe interval in seconds. Default 4.
- `--source PATH`: V4L2 capture device. Default: the `/dev/videoN` whose `/sys/class/video4linux/videoN/name` is
  exactly `SteamVR` (strip the trailing newline).
- `--encoder PATH`: the encoder node. Default: `/dev/video-enc0` if it exists, else the first
  `/sys/class/video4linux/videoN` whose `name` contains `encoder` and that passes the capability check below.
- `--probe`: do every setup step up to and including buffer allocation (no STREAMON, no frames), print the info JSON
  (below) on **stdout**, exit.
- `--selftest`: run the converter self-test (below) and exit. Needs no devices.

Exit codes: 0 ok · 1 self-test failed / runtime error after streaming started · 2 bad arguments · 3 no source
device · 4 hardware encoder not usable (missing, wrong caps, a required format refused, any setup ioctl failed).
When the environment variable `FP_VENC_DISABLE=1` is set, setup fails with exit 4 (used to test the fallback).
The environment is on the stack after argv (`_start`: `argc` at `[sp]`, `argv` at `sp+8`, `envp` after argv's NULL).

### Info JSON
One line, keys in this order, no spaces:
`{"encoder":"/dev/video23","source":"/dev/video99","src":[1920,1080],"out":[1280,720],"refresh":96,"fps":32,"bitrate":3000000}`
`refresh` is the panel rate in whole Hz (0 when unknown). In normal (streaming) mode the same line is written to
**stderr** right after setup, prefixed `fp_venc: info ` — the PC side reads it for its status line.

## Frame rate
1. Read the panel's current refresh rate through DRM (read-only, no master needed; `/dev/dri/card0` is mode 0666 on
   the Frame): open `/dev/dri/card0` (`O_RDWR|O_CLOEXEC`), `DRM_IOCTL_MODE_GETRESOURCES` once for the counts and
   once more with an array for the CRTC ids, then `DRM_IOCTL_MODE_GETCRTC` for each. Among CRTCs with `mode_valid`,
   take the one with the largest `hdisplay × vdisplay`. Rate in milli-Hz = `clock(kHz) × 1 000 000 / (htotal × vtotal)`;
   round to whole Hz. Any failure → refresh unknown.
2. `fps` = `--fps` if given; else if the refresh is known: `refresh / ceil(refresh / max_fps)` (integer division,
   rounded to nearest): 72→36, 80→40, 90→45, 96→32, 108→36, 120→40, 144→36; else 30.
Rationale: frames on an even grid of the panel's refresh look smooth; 30 fps on a 72/96 Hz panel judders.

## Output size
`out_h = min(--height or src_h, src_h)`, rounded **down** to even. `out_w = floor(src_w × out_h / src_h)` rounded down
to even. Both ≥ 2. (1920×1080 → 360: 640×360; 480: 852×480; 720: 1280×720; 1080: 1920×1080.)

## Converter (`convert.h`)
```c
#include <stdint.h>
#include <stddef.h>
void   fp_out_size(int src_w, int src_h, int max_h, int *out_w, int *out_h);   /* rule above; max_h <= 0 = src_h */
size_t fp_scratch_size(int src_w, int src_h, int out_w, int out_h);           /* bytes of scratch fp_convert needs */
/* RGB24 (R,G,B bytes) → NV12 (BT.709, limited range). Writes out_w×out_h luma and out_w×out_h/2 interleaved CbCr. */
void   fp_convert_scalar(const uint8_t *src, int src_w, int src_h, int src_stride,
                         uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                         int out_w, int out_h, void *scratch);
void   fp_convert(/* same parameters */);   /* NEON on aarch64; identical to fp_convert_scalar elsewhere */
```
Both functions must produce **bit-identical** output; the exact definition is:

1. **Scaling (box average).** For output column `i` the source columns are `[x0, x1)` with
   `x0 = i·src_w / out_w`, `x1 = (i+1)·src_w / out_w` (integer division); rows likewise with `src_h/out_h`. Each
   channel of output pixel (i, j) is `(S + n/2) / n` (integer division), `S` = sum of that channel over the block,
   `n` = block width × block height. When the size is unchanged this is the identity.
2. **Luma** from each scaled pixel (R, G, B):
   `Y = 16 + ((47·R + 157·G + 16·B + 128) >> 8)`.
3. **Chroma** per 2×2 block of scaled pixels: first average each channel, `C = (c00 + c01 + c10 + c11 + 2) >> 2`,
   then `Cb = 128 + ((−26·R − 86·G + 112·B + 128) >> 8)`, `Cr = 128 + ((112·R − 102·G − 10·B + 128) >> 8)`
   (`>>` on a signed 32-bit value = floor division by 256). Write Cb then Cr.
4. Clamp Y to [16, 235] and Cb/Cr to [16, 240] (the formulas already stay inside; clamp anyway).
The input is sRGB-encoded full-range RGB; no linearisation (video expects gamma-encoded values).

**Performance:** at 1920×1080 → 1920×1080 the aarch64 `fp_convert` should take ≤ 4 ms per frame on one core; scaled
outputs not more. Suggested shape (free to differ if output stays identical): NEON vertical accumulation of the rows of
each output row into a uint16/uint32 row buffer, horizontal block sums + division (a per-block-size reciprocal table is
fine only if it is exact for every possible sum; otherwise divide), NEON colour conversion with `vld3`/widening
multiplies. The identity size gets a direct NEON path (no box step). Exact 3:2 and 3:1 ratios (720p and 360p from
1080p) get their own NEON paths: the boxes repeat every 3 source pixels (widths 1,2 resp. 3), so the averages are
rounding shifts / an exact multiply (×7282 >> 16 = ÷9 for every possible sum). The self-test covers them, including
columns left over after the 48-pixel steps (1158×648 → 432/216).

### Self-test (`--selftest`)
Deterministic synthetic images (a 32-bit LCG noise image, a horizontal gradient, solid black/white/red/green/blue,
a 1-pixel checkerboard) at source sizes 1920×1080, 1922×1080, 333×201 and 64×64, each converted for max heights
{source height, 720, 480, 360, 100} where ≤ source height. `fp_convert` must equal `fp_convert_scalar` byte for byte
(Y and UV planes, using strides larger than the width to catch stride bugs). Print `fp_venc: selftest ok (N cases)` or
the first mismatch (size, plane, x, y, both values) on stderr; exit 0/1. Also time 50 conversions 1920×1080→1080 and
→720 and print the average ms each.

## Source (V4L2 capture, `/dev/video99`)
v4l2loopback device written by SteamVR's `v4l2cam` (single-planar `V4L2_BUF_TYPE_VIDEO_CAPTURE`).
- Open `O_RDWR|O_NONBLOCK|O_CLOEXEC`. `VIDIOC_QUERYCAP`: needs `V4L2_CAP_VIDEO_CAPTURE` + `V4L2_CAP_STREAMING`
  (check `device_caps` when `V4L2_CAP_DEVICE_CAPS` is set). `VIDIOC_G_FMT`: pixelformat must be `V4L2_PIX_FMT_RGB24`
  (else exit 4); use its width, height and `bytesperline` (0 → width×3).
- `VIDIOC_REQBUFS` 3 × `V4L2_MEMORY_MMAP`, `VIDIOC_QUERYBUF` + `mmap(PROT_READ, MAP_SHARED)` each, `VIDIOC_QBUF` all,
  `VIDIOC_STREAMON`.
- At most one dequeued buffer is held (the newest frame). When a newer one is dequeued, re-queue the held one first.
  Drain all ready buffers on each wake-up so the held one is always the newest.

## Encoder (V4L2 memory-to-memory, multi-planar)
Follow the "Initialization" and "Encoding" sections of `dev-encoder.rst`:
1. Open `O_RDWR|O_NONBLOCK|O_CLOEXEC`. `VIDIOC_QUERYCAP`: needs `V4L2_CAP_VIDEO_M2M_MPLANE` and
   `V4L2_CAP_STREAMING` (in `device_caps`).
2. **Coded format first** — `VIDIOC_S_FMT` on `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`: `pixelformat = V4L2_PIX_FMT_H264`,
   width/height = out size, `num_planes = 1`. Must come back as H264 (else exit 4). Its `sizeimage` is the size of
   the encoded-frame buffers.
3. **Raw format** — `VIDIOC_S_FMT` on `V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE`: `V4L2_PIX_FMT_NV12`, out size,
   `num_planes = 1`, `colorspace = V4L2_COLORSPACE_REC709`, `ycbcr_enc = V4L2_YCBCR_ENC_709`,
   `quantization = V4L2_QUANTIZATION_LIM_RANGE`, `xfer_func = V4L2_XFER_FUNC_709`. Use what comes back:
   `num_planes` must be 1 (else exit 4); `stride = plane_fmt[0].bytesperline`; the returned `height` is the padded
   height `H'`; the CbCr plane starts at `stride × H'` inside the same buffer; require
   `stride × H' × 3/2 ≤ plane_fmt[0].sizeimage` and `stride ≥ out_w` (else exit 4).
4. **Visible rectangle** — `VIDIOC_S_SELECTION` (type `V4L2_BUF_TYPE_VIDEO_OUTPUT`, target `V4L2_SEL_TGT_CROP`,
   rect 0,0,out_w,out_h). If the driver rejects the single-planar type, retry with the `_MPLANE` type. Failure is only
   a warning when `G_SELECTION` already reports that rectangle.
5. **Frame rate** — `VIDIOC_S_PARM` on the OUTPUT queue: `timeperframe = 1/fps` (warning on failure).
6. **Controls** — `VIDIOC_S_EXT_CTRLS` (or `VIDIOC_S_CTRL` one by one), each best-effort: a refused control prints
   `fp_venc: warning: <name> refused (<errno>)` and continues:
   - `V4L2_CID_MPEG_VIDEO_H264_PROFILE` = HIGH; `V4L2_CID_MPEG_VIDEO_H264_LEVEL` = the lowest level whose limits
     (H.264 Table A-1: MaxFS macroblocks per frame, MaxMBPS macroblocks per second, MaxBR ×1.25 for High in kbit/s)
     fit out size × fps × bitrate, among 3.1, 3.2, 4, 4.1, 4.2, 5, 5.1, 5.2;
   - `V4L2_CID_MPEG_VIDEO_BITRATE_MODE` = CBR; `V4L2_CID_MPEG_VIDEO_BITRATE` = bitrate;
     `V4L2_CID_MPEG_VIDEO_BITRATE_PEAK` = bitrate; `V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE` = 1;
   - `V4L2_CID_MPEG_VIDEO_B_FRAMES` = 0; `V4L2_CID_MPEG_VIDEO_GOP_SIZE` = fps × gop_seconds;
   - `V4L2_CID_MPEG_VIDEO_HEADER_MODE` = JOINED_WITH_1ST_FRAME; `V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR` = 1
     (every keyframe is decodable on its own — a viewer may join at any keyframe);
   - `V4L2_CID_MPEG_VIDEO_FRAME_SKIP_MODE` = DISABLED (one output frame per input frame: the PC side derives
     timestamps from the frame count).
7. **Buffers** — `VIDIOC_REQBUFS` OUTPUT_MPLANE 4 × MMAP and CAPTURE_MPLANE 4 × MMAP; `VIDIOC_QUERYBUF` + `mmap` every
   buffer (OUTPUT: read/write, fill with Y=16/CbCr=128 once so padding rows are defined; CAPTURE: read).
   (`--probe` stops here: print the JSON, exit 0.)
8. Queue all CAPTURE buffers, `VIDIOC_STREAMON` both queues.
9. Per frame: write NV12 into a free OUTPUT buffer (`bytesused` = sizeimage), `VIDIOC_QBUF`. Dequeue finished OUTPUT
   buffers (they become free). Dequeue CAPTURE buffers: write `bytesused − data_offset` bytes from
   `mem + data_offset` to stdout, re-queue. `V4L2_BUF_FLAG_ERROR` on a capture buffer: re-queue, count, don't write.
   `V4L2_BUF_FLAG_LAST`: the encoder is drained.
10. **Keyframe on request** — set `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` (button) just before queuing the next frame.
11. **Stop** — `VIDIOC_ENCODER_CMD` `V4L2_ENC_CMD_STOP`, keep dequeuing/writing CAPTURE buffers until one has
    `V4L2_BUF_FLAG_LAST` (or 1 s passes), `VIDIOC_STREAMOFF` both queues, close, exit 0.

## Main loop and timing
- Ignore `SIGPIPE` (`rt_sigaction` with `SIG_IGN`; no restorer needed for SIG_IGN) so a closed stdout is an `EPIPE`
  error → stop quietly (exit 0).
- **stdin protocol** (stdin is the SSH channel; non-blocking reads after `poll`): byte `k` = request a keyframe;
  byte `q` or end of file = stop (step 11); other bytes ignored.
- **Fixed frame grid.** `t0` = `CLOCK_MONOTONIC` when streaming starts; slot `n` is due at `t0 + n·10⁹/fps` ns.
  Exactly **one** frame is queued per slot, always: at slot time, convert the held (newest) source frame — even if it
  was already used, e.g. while the headset is in standby the source delivers ~1 fps — so the stream's frame count
  always equals elapsed time × fps. The ffmpeg side relies on that (`-framerate fps`, no timestamps in the stream).
  Before the first source frame arrives, submit black frames (Y=16, CbCr=128).
- If no OUTPUT buffer is free at slot time, wait for one (it's the encoder's latency; normal is a few ms). If the loop
  falls behind (several slots due), submit the missed slots back to back with the same picture. If more than `fps`
  slots (1 s) are missed at once (e.g. the process was stopped), log it, skip forward to the current slot and continue.
- Wait with `ppoll` on: source fd (POLLIN), encoder fd (POLLIN = capture ready, POLLOUT = output buffer done),
  stdin (POLLIN|POLLHUP), timeout = time to the next slot.
- stdout writes are blocking writes of whole packets (loop over partial writes; `EINTR`/`EAGAIN` retry).
- Every 10 s print one stderr line: `fp_venc: stats frames=<n> repeats=<frames that reused an already-sent picture>
  late=<slots submitted late> skipped=<slots skipped> kbps=<average output bitrate>`.
- All stderr lines start with `fp_venc: `. No output on stdout except the H.264 stream (or the probe JSON).

## Measured facts (the Frame, SteamOS 0.3.0 kernel 6.18, 2026-10-07)
- Encoder: `/dev/video23` (`/dev/video-enc0` → it), name `qcom-iris-encoder`, driver `iris_driver`, caps
  M2M multiplanar + streaming. OUTPUT formats: Q08C, NV12, NV21, AB24, QC24. CAPTURE formats: H264, HEVC. Frame sizes
  128×128 … 8192×8192 step 1. The `steamos` user can open it (group `video`).
- NV12 S_FMT results (stride / returned height / sizeimage): 640×360 → 640/384/368640; 854×480 → 896/480/647168;
  1280×720 → 1280/736/1413120; 1920×1080 → 1920/1088/3133440. Default OUTPUT crop = the requested size.
- Controls present: B_FRAMES (0–7), GOP_SIZE, BITRATE_MODE (VBR, CBR), BITRATE / BITRATE_PEAK (≤ 245 000 000),
  FRAME_RC_ENABLE, HEADER_MODE (separate, joined with 1st frame), FORCE_KEY_FRAME, H264 PROFILE (Baseline, Constrained
  Baseline, Main, High, Constrained High), H264 LEVEL (1 … 6.0, default 5), PREPEND_SPSPPS_TO_IDR, FRAME_SKIP_MODE,
  H264 entropy (CABAC default), I/P QP ranges, intra-refresh period, LTR, hierarchical coding.
- Source: `/dev/video99`, v4l2loopback, name `SteamVR`, single-planar capture, RGB24 only, 1920×1080, bytesperline
  5760, advertised 30 fps but frames come at the panel rate when worn, ~1.1 fps in standby.
- DRM: `/dev/dri/card0` (msm, mode 0666); one active CRTC 4320×2160 (`2*2160x2160_96`, clock 1402720 kHz,
  htotal 4448, vtotal 3285 → 96.00 Hz). The panel offers 72/80/90/96/108/120/144 Hz.
- ffmpeg's stock `h264_v4l2m2m` hangs on this encoder; that's why this program exists.
