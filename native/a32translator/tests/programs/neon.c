/* Advanced SIMD intrinsics: hashes the results of a wide set of operations on pseudo-random inputs. */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t h = 2166136261u;
static void mixb(const void *p, size_t n) { const uint8_t *b = p; for (size_t k = 0; k < n; k++) h = (h ^ b[k]) * 16777619u; }
#define M(v) do { __typeof__(v) _t = (v); mixb(&_t, sizeof _t); } while (0)

static uint8_t buf[256] __attribute__((aligned(16)));
static uint32_t rs = 99;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void)
{
    for (int round = 0; round < 200; round++) {
        for (int i = 0; i < 256; i++) buf[i] = (uint8_t)rnd();
        if (round % 3 == 0) for (int i = 0; i < 256; i += 7) buf[i] = 0x80; /* saturation corner cases */
        int8x16_t a8 = vld1q_s8((int8_t *)buf), b8 = vld1q_s8((int8_t *)buf + 16);
        uint8x16_t ua8 = vreinterpretq_u8_s8(a8), ub8 = vreinterpretq_u8_s8(b8);
        int16x8_t a16 = vld1q_s16((int16_t *)(buf + 32)), b16 = vld1q_s16((int16_t *)(buf + 48));
        int32x4_t a32 = vld1q_s32((int32_t *)(buf + 64)), b32 = vld1q_s32((int32_t *)(buf + 80));
        int64x2_t a64 = vld1q_s64((int64_t *)(buf + 96)), b64 = vld1q_s64((int64_t *)(buf + 112));
        uint16x8_t ua16 = vreinterpretq_u16_s16(a16), ub16 = vreinterpretq_u16_s16(b16);
        uint32x4_t ua32 = vreinterpretq_u32_s32(a32), ub32 = vreinterpretq_u32_s32(b32);
        M(vaddq_s8(a8, b8)); M(vsubq_s16(a16, b16)); M(vmulq_s32(a32, b32)); M(vaddq_s64(a64, b64));
        M(vqaddq_s8(a8, b8)); M(vqaddq_u8(ua8, ub8)); M(vqsubq_s16(a16, b16)); M(vqsubq_u32(ua32, ub32));
        M(vqaddq_s64(a64, b64)); M(vqsubq_u64(vreinterpretq_u64_s64(a64), vreinterpretq_u64_s64(b64)));
        M(vhaddq_s8(a8, b8)); M(vrhaddq_u16(ua16, ub16)); M(vhsubq_s32(a32, b32));
        M(vabdq_s8(a8, b8)); M(vabaq_u16(ua16, ub16, ua16)); M(vmaxq_s16(a16, b16)); M(vminq_u32(ua32, ub32));
        M(vpadd_s16(vget_low_s16(a16), vget_high_s16(b16))); M(vpmax_u8(vget_low_u8(ua8), vget_high_u8(ub8)));
        M(vpmin_s32(vget_low_s32(a32), vget_high_s32(b32)));
        M(vmlaq_s16(a16, b16, a16)); M(vmlsq_u32(ua32, ub32, ua32)); M(vmulq_p8(vreinterpretq_p8_s8(a8), vreinterpretq_p8_s8(b8)));
        M(vqdmulhq_s16(a16, b16)); M(vqrdmulhq_s32(a32, b32)); M(vqdmulh_n_s16(vget_low_s16(a16), (int16_t)rnd()));
        M(vshlq_s8(a8, vshrq_n_s8(b8, 4))); M(vshlq_u16(ua16, vshrq_n_s16(b16, 11))); M(vqshlq_s32(a32, vshrq_n_s32(b32, 26)));
        M(vrshlq_s64(a64, vshrq_n_s64(b64, 58))); M(vqrshlq_u8(ua8, vshrq_n_s8(b8, 5)));
        M(vshrq_n_u8(ua8, 3)); M(vshrq_n_s16(a16, 15)); M(vsraq_n_s32(a32, b32, 7)); M(vrshrq_n_u64(vreinterpretq_u64_s64(a64), 63));
        M(vrsraq_n_s8(a8, b8, 8)); M(vshlq_n_s16(a16, 9)); M(vqshlq_n_u32(ua32, 3)); M(vqshluq_n_s8(a8, 2));
        M(vsliq_n_u16(ua16, ub16, 5)); M(vsriq_n_u32(ua32, ub32, 31)); M(vsli_n_u64(vget_low_u64(vreinterpretq_u64_s64(a64)), vget_high_u64(vreinterpretq_u64_s64(b64)), 40));
        M(vshrn_n_s32(a32, 9)); M(vrshrn_n_u16(ua16, 3)); M(vqshrn_n_s16(a16, 2)); M(vqrshrun_n_s32(a32, 13)); M(vqshrun_n_s64(a64, 30));
        M(vshll_n_s8(vget_low_s8(a8), 3)); M(vmovl_u16(vget_high_u16(ua16))); M(vshll_n_u8(vget_low_u8(ua8), 8));
        M(vmovn_s64(a64)); M(vqmovn_s32(a32)); M(vqmovn_u16(ua16)); M(vqmovun_s16(a16));
        M(vaddl_s8(vget_low_s8(a8), vget_high_s8(b8))); M(vsubw_u16(ua32, vget_low_u16(ub16))); M(vaddhn_s32(a32, b32));
        M(vraddhn_u64(vreinterpretq_u64_s64(a64), vreinterpretq_u64_s64(b64))); M(vrsubhn_s16(a16, b16));
        M(vabdl_u8(vget_low_u8(ua8), vget_high_u8(ub8))); M(vabal_s16(a32, vget_low_s16(a16), vget_low_s16(b16)));
        M(vmull_s16(vget_low_s16(a16), vget_high_s16(b16))); M(vmlal_u32(vreinterpretq_u64_s64(a64), vget_low_u32(ua32), vget_high_u32(ub32)));
        M(vmlsl_s8(a16, vget_low_s8(a8), vget_low_s8(b8))); M(vqdmull_s32(vget_low_s32(a32), vget_high_s32(b32)));
        M(vqdmlal_s16(a32, vget_low_s16(a16), vget_high_s16(b16))); M(vqdmlsl_s32(a64, vget_low_s32(a32), vget_low_s32(b32)));
        M(vmull_p8(vreinterpret_p8_s8(vget_low_s8(a8)), vreinterpret_p8_s8(vget_high_s8(b8))));
        M(vmulq_lane_s16(a16, vget_low_s16(b16), 3)); M(vmlaq_lane_s32(a32, b32, vget_high_s32(a32), 1));
        M(vmull_lane_u16(vget_low_u16(ua16), vget_high_u16(ub16), 2)); M(vqdmulhq_lane_s32(a32, vget_low_s32(b32), 0));
        M(vqrdmulh_lane_s16(vget_low_s16(a16), vget_high_s16(b16), 1)); M(vqdmull_lane_s16(vget_low_s16(a16), vget_low_s16(b16), 2));
        M(vandq_s8(a8, b8)); M(vorrq_u16(ua16, ub16)); M(veorq_s32(a32, b32)); M(vbicq_s64(a64, b64)); M(vornq_u8(ua8, ub8));
        M(vbslq_u8(ua8, ub8, vreinterpretq_u8_s16(a16))); M(vmvnq_u8(ua8));
        M(vceqq_s8(a8, b8)); M(vcgtq_s16(a16, b16)); M(vcgeq_u32(ua32, ub32)); M(vcltq_s8(a8, b8)); M(vtstq_u16(ua16, ub16));
        M(vceqq_s8(a8, vdupq_n_s8(0))); M(vcgtq_s32(a32, vdupq_n_s32(0)));
        M(vabsq_s8(a8)); M(vnegq_s16(a16)); M(vqabsq_s32(a32)); M(vqnegq_s8(a8)); M(vclzq_u16(ua16)); M(vclsq_s32(a32)); M(vcntq_u8(ua8));
        M(vpaddlq_s8(a8)); M(vpadalq_u16(ua32, ub16)); M(vpaddl_u32(vget_low_u32(ua32)));
        M(vrev64q_s8(a8)); M(vrev32q_u16(ua16)); M(vrev16q_u8(ua8)); M(vrev64q_s32(a32));
        M(vextq_s8(a8, b8, 5)); M(vextq_s16(a16, b16, 7)); M(vext_s32(vget_low_s32(a32), vget_high_s32(b32), 1));
        M(vzipq_s8(a8, b8)); M(vuzpq_u16(ua16, ub16)); M(vtrnq_s32(a32, b32)); M(vzip_u32(vget_low_u32(ua32), vget_high_u32(ub32)));
        M(vdupq_lane_s8(vget_low_s8(a8), 5)); M(vdupq_n_u16((uint16_t)rnd())); M(vdup_lane_s32(vget_high_s32(a32), 1));
        uint8x8x2_t t2 = {{vget_low_u8(ua8), vget_high_u8(ub8)}};
        M(vtbl2_u8(t2, vand_u8(vget_low_u8(ub8), vdup_n_u8(0x1f))));
        uint8x8x4_t t4 = {{vget_low_u8(ua8), vget_high_u8(ua8), vget_low_u8(ub8), vget_high_u8(ub8)}};
        M(vtbx4_u8(vget_low_u8(ua8), t4, vget_high_u8(ub8)));
        M(vmovq_n_s32(-7)); M(vmov_n_u8(200)); M(vmvnq_s16(vdupq_n_s16(0x7f)));
        M(vgetq_lane_s16(a16, 5)); M(vsetq_lane_u32(77, ua32, 3)); M(vgetq_lane_s64(a64, 1));
        M(vrecpeq_u32(ua32)); M(vrsqrteq_u32(ua32));
        /* floating point */
        float fa[4], fb[4];
        for (int i = 0; i < 4; i++) {
            fa[i] = (float)((int32_t)rnd()) / 65536.0f;
            fb[i] = (float)((int32_t)rnd()) / 1e6f;
        }
        if (round % 5 == 0) { fa[1] = 0.0f; fb[2] = -0.0f; fa[3] = 1e-40f; fb[0] = 3.4e38f; }
        float32x4_t f1 = vld1q_f32(fa), f2 = vld1q_f32(fb);
        M(vaddq_f32(f1, f2)); M(vsubq_f32(f1, f2)); M(vmulq_f32(f1, f2)); M(vmlaq_f32(f1, f2, f1)); M(vmlsq_f32(f2, f1, f2));
        M(vfmaq_f32(f1, f2, f1)); M(vfmsq_f32(f2, f1, f1));
        M(vabdq_f32(f1, f2)); M(vmaxq_f32(f1, f2)); M(vminq_f32(f1, f2)); M(vpadd_f32(vget_low_f32(f1), vget_high_f32(f2)));
        M(vpmax_f32(vget_low_f32(f1), vget_high_f32(f2))); M(vabsq_f32(f1)); M(vnegq_f32(f2));
        M(vceqq_f32(f1, f2)); M(vcgeq_f32(f1, f2)); M(vcgtq_f32(f1, f2)); M(vcageq_f32(f1, f2)); M(vcagtq_f32(f1, f2));
        M(vrecpeq_f32(f1)); M(vrecpsq_f32(f1, f2)); M(vrsqrteq_f32(vabsq_f32(f1))); M(vrsqrtsq_f32(f1, f2));
        M(vcvtq_s32_f32(f1)); M(vcvtq_u32_f32(f2)); M(vcvtq_f32_s32(a32)); M(vcvtq_f32_u32(ua32));
        M(vcvtq_n_s32_f32(f1, 8)); M(vcvtq_n_u32_f32(f2, 31)); M(vcvtq_n_f32_s32(a32, 16)); M(vcvtq_n_f32_u32(ua32, 1));
        M(vmulq_lane_f32(f1, vget_low_f32(f2), 1)); M(vmlaq_lane_f32(f1, f2, vget_high_f32(f1), 0)); M(vmulq_n_f32(f1, 0.25f));
        M(vcvt_f16_f32(f1)); M(vcvt_f32_f16(vcvt_f16_f32(f2)));
        /* loads and stores */
        uint8_t out[128] __attribute__((aligned(16)));
        memset(out, 0, sizeof out);
        int8x16x2_t l2 = vld2q_s8((int8_t *)buf + 1); vst2q_s8((int8_t *)out, l2); M(l2);
        int16x4x3_t l3 = vld3_s16((int16_t *)(buf + 2)); vst3_s16((int16_t *)(out + 32), l3); M(l3);
        int32x4x4_t l4 = vld4q_s32((int32_t *)(buf + 4)); vst4q_s32((int32_t *)(out + 64), l4); M(l4);
        uint8x8x3_t l3b = vld3_u8(buf + 3); vst3_u8(out + 5, l3b);
        int16x8_t ln = vld1q_lane_s16((int16_t *)(buf + 6), a16, 6); M(ln);
        int32x2x2_t lane2 = vld2_lane_s32((int32_t *)(buf + 8), ((int32x2x2_t){{vget_low_s32(a32), vget_high_s32(b32)}}), 1); M(lane2);
        uint16x4x4_t lane4 = vld4_lane_u16((uint16_t *)(buf + 10), ((uint16x4x4_t){{vget_low_u16(ua16), vget_high_u16(ua16), vget_low_u16(ub16), vget_high_u16(ub16)}}), 3); M(lane4);
        vst4_lane_u16((uint16_t *)(out + 100), lane4, 2);
        int8x8_t dupl = vld1_dup_s8((int8_t *)buf + 11); M(dupl);
        int32x4_t dupq = vld1q_dup_s32((int32_t *)(buf + 12)); M(dupq);
        int16x4x2_t dup2 = vld2_dup_s16((int16_t *)(buf + 14)); M(dup2);
        uint32x2x3_t dup3 = vld3_dup_u32((uint32_t *)(buf + 16)); M(dup3);
        uint8x8x4_t dup4 = vld4_dup_u8(buf + 18); M(dup4);
        vst1q_lane_s64((int64_t *)(out + 120), a64, 1);
        mixb(out, sizeof out);
    }
    printf("neon %08x\n", h);
    return 0;
}
