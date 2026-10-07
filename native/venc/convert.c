/* RGB24 -> NV12 converter (see convert.h and SPEC.md "Converter"). The scalar code is the definition; the NEON code
 * must produce the same bytes (fp_venc --selftest compares them). */
#include "convert.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

void fp_out_size(int src_w, int src_h, int max_h, int *out_w, int *out_h)
{
    int h = (max_h <= 0 || max_h > src_h) ? src_h : max_h;
    h &= ~1;
    int w = (int)((int64_t)src_w * h / src_h) & ~1;
    *out_w = w < 2 ? 2 : w;
    *out_h = h < 2 ? 2 : h;
}

static size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

/* Scratch layout for scaled output: per-column channel sums of the current output row's source rows (uint32, or
 * uint16 in the NEON path), the source column where each output column starts, and two scaled RGB rows (one NV12
 * row pair: chroma needs both). */
size_t fp_scratch_size(int src_w, int src_h, int out_w, int out_h)
{
    (void)src_h;
    (void)out_h;
    return align16((size_t)src_w * 3 * 4) + align16(((size_t)out_w + 1) * 4) + align16((size_t)out_w * 6 + 16);
}

static uint8_t clamp_u8(int v, int lo, int hi) { return (uint8_t)(v < lo ? lo : v > hi ? hi : v); }

/* BT.709 limited range. `>>` on a negative int is an arithmetic (floor) shift with clang/gcc on every target we
 * build for, which is what the spec's formula means. */
static uint8_t luma(int r, int g, int b) { return clamp_u8(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8), 16, 235); }
static uint8_t cb_of(int r, int g, int b) { return clamp_u8(128 + ((-26 * r - 86 * g + 112 * b + 128) >> 8), 16, 240); }
static uint8_t cr_of(int r, int g, int b) { return clamp_u8(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8), 16, 240); }

/* One NV12 row pair from two RGB rows, columns [x, w) (w even). */
static void pair_scalar(const uint8_t *r0, const uint8_t *r1, int x, int w, uint8_t *y0, uint8_t *y1, uint8_t *uv)
{
    for (; x < w; x += 2) {
        const uint8_t *a = r0 + x * 3, *b = r1 + x * 3;
        y0[x] = luma(a[0], a[1], a[2]);
        y0[x + 1] = luma(a[3], a[4], a[5]);
        y1[x] = luma(b[0], b[1], b[2]);
        y1[x + 1] = luma(b[3], b[4], b[5]);
        int r = (a[0] + a[3] + b[0] + b[3] + 2) >> 2;
        int g = (a[1] + a[4] + b[1] + b[4] + 2) >> 2;
        int bl = (a[2] + a[5] + b[2] + b[5] + 2) >> 2;
        uv[x] = cb_of(r, g, bl);
        uv[x + 1] = cr_of(r, g, bl);
    }
}

struct layout { uint32_t *colsum; int32_t *xb; uint8_t *rgb; };

static struct layout split(void *scratch, int src_w, int out_w)
{
    struct layout l;
    uint8_t *p = scratch;
    l.colsum = (uint32_t *)p;
    p += align16((size_t)src_w * 3 * 4);
    l.xb = (int32_t *)p;
    p += align16(((size_t)out_w + 1) * 4);
    l.rgb = p;
    for (int i = 0; i <= out_w; i++) l.xb[i] = (int32_t)((int64_t)i * src_w / out_w);
    return l;
}

void fp_convert_scalar(const uint8_t *src, int src_w, int src_h, int src_stride,
                       uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                       int out_w, int out_h, void *scratch)
{
    if (out_w == src_w && out_h == src_h) {
        for (int j = 0; j < out_h; j += 2)
            pair_scalar(src + (size_t)j * src_stride, src + (size_t)(j + 1) * src_stride, 0, out_w,
                        y + (size_t)j * y_stride, y + (size_t)(j + 1) * y_stride, uv + (size_t)(j / 2) * uv_stride);
        return;
    }
    struct layout l = split(scratch, src_w, out_w);
    int cols = src_w * 3;
    for (int j = 0; j < out_h; j++) {
        int ya = (int)((int64_t)j * src_h / out_h), yb = (int)((int64_t)(j + 1) * src_h / out_h);
        for (int k = 0; k < cols; k++) l.colsum[k] = 0;
        for (int r = ya; r < yb; r++) {
            const uint8_t *row = src + (size_t)r * src_stride;
            for (int k = 0; k < cols; k++) l.colsum[k] += row[k];
        }
        uint8_t *out = l.rgb + (size_t)(j & 1) * out_w * 3;
        for (int i = 0; i < out_w; i++) {
            int xa = l.xb[i], xe = l.xb[i + 1];
            uint64_t n = (uint64_t)(xe - xa) * (uint64_t)(yb - ya);
            for (int c = 0; c < 3; c++) {
                uint64_t s = 0;
                for (int x = xa; x < xe; x++) s += l.colsum[x * 3 + c];
                out[i * 3 + c] = (uint8_t)((s + n / 2) / n);
            }
        }
        if (j & 1)
            pair_scalar(l.rgb, l.rgb + (size_t)out_w * 3, 0, out_w, y + (size_t)(j - 1) * y_stride,
                        y + (size_t)j * y_stride, uv + (size_t)(j / 2) * uv_stride);
    }
}

#if defined(__aarch64__)

/* 16 pixels of 2 rows -> 32 luma bytes + 8 CbCr pairs. All intermediates fit: luma sums <= 220*255+128 < 2^16
 * (uint16), chroma partial sums stay within +-28688 (int16). vrshrn_n_u16(x, 8) == (x + 128) >> 8 and
 * vrshrq_n_u16(x, 2) == (x + 2) >> 2 exactly (the rounding add is done without overflow). */
static inline uint8x16_t luma16(uint8x16x3_t p)
{
    uint16x8_t lo = vmull_u8(vget_low_u8(p.val[0]), vdup_n_u8(47));
    lo = vmlal_u8(lo, vget_low_u8(p.val[1]), vdup_n_u8(157));
    lo = vmlal_u8(lo, vget_low_u8(p.val[2]), vdup_n_u8(16));
    uint16x8_t hi = vmull_u8(vget_high_u8(p.val[0]), vdup_n_u8(47));
    hi = vmlal_u8(hi, vget_high_u8(p.val[1]), vdup_n_u8(157));
    hi = vmlal_u8(hi, vget_high_u8(p.val[2]), vdup_n_u8(16));
    uint8x16_t yv = vaddq_u8(vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8)), vdupq_n_u8(16));
    return vminq_u8(vmaxq_u8(yv, vdupq_n_u8(16)), vdupq_n_u8(235));
}

static inline uint8x8_t chroma8(int16x8_t r, int16x8_t g, int16x8_t b, int16_t kr, int16_t kg, int16_t kb)
{
    int16x8_t v = vmulq_n_s16(r, kr);
    v = vmlaq_n_s16(v, g, kg);
    v = vmlaq_n_s16(v, b, kb);
    v = vshrq_n_s16(vaddq_s16(v, vdupq_n_s16(128)), 8);              /* arithmetic = floor */
    v = vaddq_s16(v, vdupq_n_s16(128));
    v = vminq_s16(vmaxq_s16(v, vdupq_n_s16(16)), vdupq_n_s16(240));
    return vqmovun_s16(v);
}

static void pair_neon(const uint8_t *r0, const uint8_t *r1, int w, uint8_t *y0, uint8_t *y1, uint8_t *uv)
{
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        uint8x16x3_t a = vld3q_u8(r0 + x * 3), b = vld3q_u8(r1 + x * 3);
        vst1q_u8(y0 + x, luma16(a));
        vst1q_u8(y1 + x, luma16(b));
        int16x8_t rr = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[0]), b.val[0]), 2));
        int16x8_t gg = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[1]), b.val[1]), 2));
        int16x8_t bb = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[2]), b.val[2]), 2));
        uint8x8x2_t c;
        c.val[0] = chroma8(rr, gg, bb, -26, -86, 112);
        c.val[1] = chroma8(rr, gg, bb, 112, -102, -10);
        vst2_u8(uv + x, c);
    }
    pair_scalar(r0, r1, x, w, y0, y1, uv);
}

/* Box sums of one output row's source rows, as uint16 (callers ensure rows * 255 fits). */
static void vsum16(const uint8_t *src, int src_stride, int ya, int yb, int cols, uint16_t *cs)
{
    const uint8_t *row = src + (size_t)ya * src_stride;
    int k = 0;
    for (; k + 16 <= cols; k += 16) {
        uint8x16_t v = vld1q_u8(row + k);
        vst1q_u16(cs + k, vmovl_u8(vget_low_u8(v)));
        vst1q_u16(cs + k + 8, vmovl_high_u8(v));
    }
    for (; k < cols; k++) cs[k] = row[k];
    for (int r = ya + 1; r < yb; r++) {
        row = src + (size_t)r * src_stride;
        k = 0;
        for (; k + 16 <= cols; k += 16) {
            uint8x16_t v = vld1q_u8(row + k);
            vst1q_u16(cs + k, vaddw_u8(vld1q_u16(cs + k), vget_low_u8(v)));
            vst1q_u16(cs + k + 8, vaddw_high_u8(vld1q_u16(cs + k + 8), v));
        }
        for (; k < cols; k++) cs[k] = (uint16_t)(cs[k] + row[k]);
    }
}

#define MAX_RECIP_BW 64

void fp_convert(const uint8_t *src, int src_w, int src_h, int src_stride,
                uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                int out_w, int out_h, void *scratch)
{
    if (out_w == src_w && out_h == src_h) {
        for (int j = 0; j < out_h; j += 2)
            pair_neon(src + (size_t)j * src_stride, src + (size_t)(j + 1) * src_stride, out_w,
                      y + (size_t)j * y_stride, y + (size_t)(j + 1) * y_stride, uv + (size_t)(j / 2) * uv_stride);
        return;
    }
    int bh_max = (src_h + out_h - 1) / out_h + 1;
    if (bh_max > 257) {   /* uint16 column sums would overflow: extreme downscale, not worth a NEON path */
        fp_convert_scalar(src, src_w, src_h, src_stride, y, y_stride, uv, uv_stride, out_w, out_h, scratch);
        return;
    }
    struct layout l = split(scratch, src_w, out_w);
    uint16_t *cs = (uint16_t *)l.colsum;
    int cols = src_w * 3;
    for (int j = 0; j < out_h; j++) {
        int ya = (int)((int64_t)j * src_h / out_h), yb = (int)((int64_t)(j + 1) * src_h / out_h);
        int bh = yb - ya;
        vsum16(src, src_stride, ya, yb, cols, cs);
        /* (S + n/2) / n as a multiply: with m = ceil(2^32 / n) and e = m*n - 2^32 < n, floor(x*m / 2^32) ==
         * floor(x / n) whenever x*e < 2^32. Here x < 256*n, so n <= 4096 is enough. Otherwise divide. */
        uint64_t recip[MAX_RECIP_BW + 1];
        for (int bw = 1; bw <= MAX_RECIP_BW; bw++) {
            uint64_t n = (uint64_t)bw * bh;
            recip[bw] = n <= 4096 ? ((1ULL << 32) + n - 1) / n : 0;
        }
        uint8_t *out = l.rgb + (size_t)(j & 1) * out_w * 3;
        for (int i = 0; i < out_w; i++) {
            int xa = l.xb[i], bw = l.xb[i + 1] - xa;
            const uint16_t *c = cs + xa * 3;
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int x = 0; x < bw; x++, c += 3) sr += c[0], sg += c[1], sb += c[2];
            uint64_t n = (uint64_t)bw * bh, half = n / 2;
            uint64_t m = bw <= MAX_RECIP_BW ? recip[bw] : 0;
            if (m) {
                out[i * 3] = (uint8_t)(((sr + half) * m) >> 32);
                out[i * 3 + 1] = (uint8_t)(((sg + half) * m) >> 32);
                out[i * 3 + 2] = (uint8_t)(((sb + half) * m) >> 32);
            } else {
                out[i * 3] = (uint8_t)((sr + half) / n);
                out[i * 3 + 1] = (uint8_t)((sg + half) / n);
                out[i * 3 + 2] = (uint8_t)((sb + half) / n);
            }
        }
        if (j & 1)
            pair_neon(l.rgb, l.rgb + (size_t)out_w * 3, out_w, y + (size_t)(j - 1) * y_stride,
                      y + (size_t)j * y_stride, uv + (size_t)(j / 2) * uv_stride);
    }
}

#else

void fp_convert(const uint8_t *src, int src_w, int src_h, int src_stride,
                uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                int out_w, int out_h, void *scratch)
{
    fp_convert_scalar(src, src_w, src_h, src_stride, y, y_stride, uv, uv_stride, out_w, out_h, scratch);
}

#endif
