"""The live view's picture converter (native/venc/convert.c: RGB24 → NV12 with a box downscale, BT.709 limited
range), compiled for this host and compared byte for byte with a Python reference written from native/venc/SPEC.md.
The NEON path is checked against the scalar one on the Frame itself (`fp_venc --selftest`).
Opt-in (FRAMEPORT_NATIVE_TESTS=1, -m native): needs the NDK in native/.cache (from native/build.py)."""
import ctypes as C
import os
import platform
import random
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CLANG = next(iter((ROOT / "native/.cache").glob("ndk-*/toolchains/llvm/prebuilt/*/bin/clang")), None)
pytestmark = [pytest.mark.native, pytest.mark.skipif(
    not os.environ.get("FRAMEPORT_NATIVE_TESTS") or not CLANG or platform.machine() != "x86_64",
    reason="set FRAMEPORT_NATIVE_TESTS=1 (needs the NDK from native/build.py on an x86_64 host)")]


@pytest.fixture(scope="module")
def conv(tmp_path_factory):
    out = tmp_path_factory.mktemp("venc") / "convert.so"
    subprocess.run([str(CLANG), "--target=x86_64-linux-gnu", "-ffreestanding", "-nostdlibinc", "-fno-stack-protector",
                    "-fno-builtin", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-nostdlib",
                    "-fuse-ld=lld", str(ROOT / "native/venc/convert.c"), "-o", str(out)], check=True)
    lib = C.CDLL(str(out))
    lib.fp_scratch_size.restype = C.c_size_t
    return lib


# ---------------------------------------------------------------- reference (SPEC.md "Output size", "Converter")
def out_size(src_w, src_h, max_h):
    h = min(max_h if max_h > 0 else src_h, src_h) & ~1
    return (src_w * h // src_h) & ~1, h


def reference(rgb: bytes, sw: int, sh: int, ow: int, oh: int):
    def px(x, y):
        i = (y * sw + x) * 3
        return rgb[i], rgb[i + 1], rgb[i + 2]

    scaled = []
    for j in range(oh):
        y0, y1 = j * sh // oh, (j + 1) * sh // oh
        row = []
        for i in range(ow):
            x0, x1 = i * sw // ow, (i + 1) * sw // ow
            n = (x1 - x0) * (y1 - y0)
            s = [0, 0, 0]
            for y in range(y0, y1):
                for x in range(x0, x1):
                    p = px(x, y)
                    s[0] += p[0]
                    s[1] += p[1]
                    s[2] += p[2]
            row.append(tuple((v + n // 2) // n for v in s))
        scaled.append(row)
    clamp = lambda v, lo, hi: max(lo, min(hi, v))  # noqa: E731
    y_plane = bytes(clamp(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8), 16, 235)
                    for row in scaled for r, g, b in row)
    uv = bytearray()
    for j in range(0, oh, 2):
        for i in range(0, ow, 2):
            r, g, b = ((scaled[j][i][c] + scaled[j][i + 1][c] + scaled[j + 1][i][c] + scaled[j + 1][i + 1][c] + 2) >> 2
                       for c in range(3))
            uv.append(clamp(128 + ((-26 * r - 86 * g + 112 * b + 128) >> 8), 16, 240))
            uv.append(clamp(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8), 16, 240))
    return y_plane, bytes(uv)


def run(conv, rgb: bytes, sw: int, sh: int, max_h: int, fn="fp_convert"):
    ow, oh = C.c_int(), C.c_int()
    conv.fp_out_size(sw, sh, max_h, C.byref(ow), C.byref(oh))
    ow, oh = ow.value, oh.value
    assert (ow, oh) == out_size(sw, sh, max_h)
    pad = 7  # strides wider than the picture, to catch stride bugs
    src_stride, ys, uvs = sw * 3 + 5, ow + pad, ow + pad
    src = C.create_string_buffer(b"".join(rgb[r * sw * 3:(r + 1) * sw * 3] + b"\xee" * 5 for r in range(sh)))
    y = C.create_string_buffer(b"\x01" * (ys * oh))
    uv = C.create_string_buffer(b"\x01" * (uvs * oh // 2))
    scratch = C.create_string_buffer(max(1, conv.fp_scratch_size(sw, sh, ow, oh)))
    getattr(conv, fn)(src, sw, sh, src_stride, y, ys, uv, uvs, ow, oh, scratch)
    yb, uvb = y.raw, uv.raw
    y_plane = b"".join(yb[r * ys:r * ys + ow] for r in range(oh))
    uv_plane = b"".join(uvb[r * uvs:r * uvs + ow] for r in range(oh // 2))
    assert all(yb[r * ys + ow:(r + 1) * ys] == b"\x01" * pad for r in range(oh))  # padding untouched
    return (ow, oh), y_plane, uv_plane


def solid(w, h, rgb):
    return bytes(rgb) * (w * h)


def noise(w, h, seed=1):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(w * h * 3))


@pytest.mark.parametrize("rgb,y,cb,cr", [((0, 0, 0), 16, 128, 128), ((255, 255, 255), 235, 128, 128),
                                         ((255, 0, 0), 63, 102, 240), ((0, 255, 0), 172, 42, 26),
                                         ((0, 0, 255), 32, 240, 118)])
def test_solid_colours(conv, rgb, y, cb, cr):
    (ow, oh), yp, uvp = run(conv, solid(16, 8, rgb), 16, 8, 0)
    assert set(yp) == {y} and set(uvp[0::2]) == {cb} and set(uvp[1::2]) == {cr}
    assert reference(solid(16, 8, rgb), 16, 8, ow, oh) == (yp, uvp)


@pytest.mark.parametrize("sw,sh,max_h", [(64, 48, 0), (64, 48, 24), (64, 48, 32), (64, 48, 10), (37, 23, 0),
                                         (37, 23, 16), (66, 40, 30), (30, 90, 36)])
@pytest.mark.parametrize("fn", ["fp_convert", "fp_convert_scalar"])
def test_matches_reference(conv, sw, sh, max_h, fn):
    rgb = noise(sw, sh, seed=sw * sh + max_h)
    (ow, oh), yp, uvp = run(conv, rgb, sw, sh, max_h, fn)
    assert (yp, uvp) == reference(rgb, sw, sh, ow, oh)


def test_box_average_of_two_by_two(conv):
    # a 4x2 image of 2x2 blocks: left block black/white checker → grey 128 (rounded), right block pure red
    rgb = bytes([0, 0, 0, 255, 255, 255, 255, 0, 0, 255, 0, 0,
                 255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 0, 0])
    (ow, oh), yp, uvp = run(conv, rgb, 4, 2, 2)  # max height 2 = unchanged size
    assert (ow, oh) == (4, 2) and (yp, uvp) == reference(rgb, 4, 2, 4, 2)
    big = b"".join(rgb[r * 12:(r + 1) * 12] for r in (0, 1)) * 2  # 4x4: rows 0,1,0,1
    (ow, oh), yp, uvp = run(conv, big, 4, 4, 2)
    assert (ow, oh) == (2, 2)
    assert yp[0] == 16 + ((47 * 128 + 157 * 128 + 16 * 128 + 128) >> 8) and yp[1] == 63


def test_output_sizes(conv):
    for (sw, sh, mh), want in {(1920, 1080, 360): (640, 360), (1920, 1080, 480): (852, 480),
                               (1920, 1080, 720): (1280, 720), (1920, 1080, 1080): (1920, 1080),
                               (1920, 1080, 0): (1920, 1080), (1920, 1080, 4000): (1920, 1080),
                               (1921, 1081, 0): (1918, 1080)}.items():
        ow, oh = C.c_int(), C.c_int()
        conv.fp_out_size(sw, sh, mh, C.byref(ow), C.byref(oh))
        assert (ow.value, oh.value) == want == out_size(sw, sh, mh)
