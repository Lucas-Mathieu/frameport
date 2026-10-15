/* Prints "<assembly>\t<encoding>" for the JIT's A64 encoders; tests/test_a64.py checks them with llvm-mc.
 * Built for the host (plain cc): the encoders are pure functions. */
#include <inttypes.h>
#include <stdio.h>
#include "../../src/a64.h"

#define T(asmtext, word) printf("%s\t%08x\n", asmtext, (uint32_t)(word))
static char buf[256];
#define F(...) (snprintf(buf, sizeof buf, __VA_ARGS__), buf)

int main(void)
{
    static const char *sh[] = {"lsl", "lsr", "asr", "ror"};
    static const char *cc[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"};
    for (int sf = 0; sf < 2; sf++) {
        char r = sf ? 'x' : 'w';
        T(F("add %c1, %c2, #4095", r, r), a64_addsub_imm(sf, 0, 0, 1, 2, 4095, 0));
        T(F("subs %c3, %c4, #1, lsl #12", r, r), a64_addsub_imm(sf, 1, 1, 3, 4, 1, 1));
        T(F("cmp %c5, #17", r), a64_addsub_imm(sf, 1, 1, XZR, 5, 17, 0));
        T(F("add %c1, %s, #8", r, sf ? "sp" : "wsp"), a64_addsub_imm(sf, 0, 0, 1, SPREG, 8, 0));
        for (int s = 0; s < 3; s++) {
            T(F("add %c1, %c2, %c3, %s #5", r, r, r, sh[s]), a64_addsub_reg(sf, 0, 0, 1, 2, 3, s, 5));
            T(F("subs %c7, %c8, %c9, %s #31", r, r, r, sh[s]), a64_addsub_reg(sf, 1, 1, 7, 8, 9, s, 31));
        }
        T(F("cmn %c3, %c4", r, r), a64_addsub_reg(sf, 0, 1, XZR, 3, 4, 0, 0));
        T(F("adc %c1, %c2, %c3", r, r, r), a64_adc(sf, 0, 0, 1, 2, 3));
        T(F("sbcs %c1, %c2, %c3", r, r, r), a64_adc(sf, 1, 1, 1, 2, 3));
        static const char *lo[] = {"and", "orr", "eor", "ands"}, *ln[] = {"bic", "orn", "eon", "bics"};
        for (int o = 0; o < 4; o++)
            for (int s = 0; s < 4; s++) {
                T(F("%s %c1, %c2, %c3, %s #7", lo[o], r, r, r, sh[s]), a64_logic_reg(sf, o, 0, 1, 2, 3, s, 7));
                T(F("%s %c4, %c5, %c6, %s #1", ln[o], r, r, r, sh[s]), a64_logic_reg(sf, o, 1, 4, 5, 6, s, 1));
            }
        T(F("movz %c3, #0x1234, lsl #16", r), a64_movw(sf, 2, 3, 0x1234, 1));
        T(F("movk %c3, #0xffff", r), a64_movw(sf, 3, 3, 0xffff, 0));
        T(F("movn %c3, #0x10", r), a64_movw(sf, 0, 3, 0x10, 0));
        T(F("sbfx %c1, %c2, #3, #9", r, r), a64_bfm(sf, 0, 1, 2, 3, 11));
        T(F("ubfx %c1, %c2, #31, #1", r, r), a64_bfm(sf, 2, 1, 2, 31, 31));
        T(F("bfxil %c1, %c2, #4, #8", r, r), a64_bfm(sf, 1, 1, 2, 4, 11));
        T(F("extr %c1, %c2, %c3, #1", r, r, r), a64_extr(sf, 1, 2, 3, 1));
        static const char *d2[] = {"", "", "udiv", "sdiv", "", "", "", "", "lsl", "lsr", "asr", "ror"};
        for (int o = 2; o < 12; o++)
            if (d2[o][0]) T(F("%s %c1, %c2, %c3", d2[o], r, r, r), a64_dp2(sf, o, 1, 2, 3));
        T(F("rbit %c1, %c2", r, r), a64_dp1(sf, 0, 1, 2));
        T(F("rev16 %c1, %c2", r, r), a64_dp1(sf, 1, 1, 2));
        T(F("clz %c1, %c2", r, r), a64_dp1(sf, 4, 1, 2));
        T(F("madd %c1, %c2, %c3, %c4", r, r, r, r), a64_madd(sf, 0, 1, 2, 3, 4));
        T(F("msub %c1, %c2, %c3, %c4", r, r, r, r), a64_madd(sf, 1, 1, 2, 3, 4));
        T(F("csel %c1, %c2, %c3, ne", r, r, r), a64_csel(sf, 0, 1, 2, 3, A64_NE));
        T(F("csinc %c1, %c2, %c3, ge", r, r, r), a64_csel(sf, 1, 1, 2, 3, A64_GE));
        T(F("csinv %c1, %c2, %c3, lo", r, r, r), a64_csel(sf, 2, 1, 2, 3, A64_CC));
        T(F("csneg %c1, %c2, %c3, vs", r, r, r), a64_csel(sf, 3, 1, 2, 3, A64_VS));
        T(F("ccmp %c1, #3, #5, eq", r), a64_ccmp_imm(sf, 0, 1, 3, 5, A64_EQ));
    }
    T("rev w1, w2", a64_dp1(0, 2, 1, 2));
    T("rev x1, x2", a64_dp1(1, 3, 1, 2));
    T("smaddl x1, w2, w3, x4", a64_maddl(0, 0, 1, 2, 3, 4));
    T("umsubl x1, w2, w3, x4", a64_maddl(1, 1, 1, 2, 3, 4));
    T("smulh x1, x2, x3", a64_mulh(0, 1, 2, 3));
    T("umulh x1, x2, x3", a64_mulh(1, 1, 2, 3));
    T("add x1, x2, w3, uxtw", a64_addsub_ext(1, 0, 0, 1, 2, 3, 2, 0));
    T("mrs x17, nzcv", a64_mrs(17, SYS_NZCV));
    T("msr nzcv, x16", a64_msr(16, SYS_NZCV));
    T("mrs x1, fpcr", a64_mrs(1, SYS_FPCR));
    T("msr fpsr, x2", a64_msr(2, SYS_FPSR));
    T("mrs x3, cntvct_el0", a64_mrs(3, SYS_CNTVCT));
    T("mrs x3, cntfrq_el0", a64_mrs(3, SYS_CNTFRQ));
    static const char *ld[4][4] = {{"strb", "ldrb", "ldrsb", "ldrsb"}, {"strh", "ldrh", "ldrsh", "ldrsh"},
                                   {"str", "ldr", "ldrsw", ""}, {"str", "ldr", "", ""}};
    for (int size = 0; size < 4; size++)
        for (int opc = 0; opc < 4; opc++) {
            if (!ld[size][opc][0]) continue;
            char rt = (size == 3 || opc == 2) ? 'x' : 'w';
            T(F("%s %c1, [x2, #%d]", ld[size][opc], rt, 12 << size), a64_ldst_imm(size, opc, 1, 2, 12u << size));
            T(F("%s %c1, [x2]", ld[size][opc], rt), a64_ldst_imm(size, opc, 1, 2, 0));
            T(F("%s %c3, [x4, w5, uxtw]", ld[size][opc], rt), a64_ldst_reg(size, opc, 3, 4, 5, 2, 0));
            const char *u = ld[size][opc];
            char un[16];
            snprintf(un, sizeof un, "%.*sur%s", 2, u, u + 3);
            T(F("%s %c1, [x2, #-5]", un, rt), a64_ldst_unscaled(size, opc, 1, 2, -5));
        }
    T("ldr w1, [sp, #4]", a64_ldst_imm(2, 1, 1, SPREG, 4));
    T("stp w1, w2, [x16]", a64_ldstp(0, 0, 1, 2, 16, 0));
    T("ldp w3, w4, [x16, #8]", a64_ldstp(0, 1, 3, 4, 16, 8));
    T("stp x19, x20, [sp, #-16]", a64_ldstp(1, 0, 19, 20, SPREG, -16));
    T("ldp x1, x2, [x3, #504]", a64_ldstp(1, 1, 1, 2, 3, 504));
    T("ldr s16, [x19, #200]", a64_ldst_fp(2, 1, 16, 19, 200));
    T("str d17, [x19, #256]", a64_ldst_fp(3, 0, 17, 19, 256));
    T("ldr q18, [x19, #448]", a64_ldst_fp(4, 1, 18, 19, 448));
    T("str q31, [x2]", a64_ldst_fp(4, 0, 31, 2, 0));
    T("ldr d1, [x2, x3]", a64_ldst_fp_reg(3, 1, 1, 2, 3));
    T("str s1, [x2, #-4]", a64_ldst_fp_unscaled(2, 0, 1, 2, -4));
    T("ldr q5, [x2, #-16]", a64_ldst_fp_unscaled(4, 1, 5, 2, -16));
    T("ldar w1, [x2]", a64_ldar(2, 1, 2));
    T("stlrb w1, [x2]", a64_stlr(0, 1, 2));
    T("br x16", a64_br(16));
    T("blr x17", a64_blr(17));
    T("ret", a64_ret());
    T("dmb ish", A64_DMB_ISH);
    T("dsb ish", A64_DSB_ISH);
    T("isb", A64_ISB);
    T("nop", A64_NOP);
    static const char *fp2[] = {"fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"};
    static const char *fp1[] = {"fmov", "fabs", "fneg", "fsqrt", "", "", "", "", "frintn", "frintp", "frintm",
                                "frintz", "frinta", "", "frintx", "frinti"};
    static const char *fp3[] = {"fmadd", "fmsub", "fnmadd", "fnmsub"};
    for (int ty = 0; ty < 2; ty++) {
        char r = ty ? 'd' : 's';
        for (int o = 0; o < 9; o++) T(F("%s %c1, %c2, %c3", fp2[o], r, r, r), a64_fp2(ty, o, 1, 2, 3));
        for (int o = 0; o < 16; o++)
            if (fp1[o][0]) T(F("%s %c4, %c5", fp1[o], r, r), a64_fp1(ty, o, 4, 5));
        for (int o = 0; o < 4; o++) T(F("%s %c1, %c2, %c3, %c4", fp3[o], r, r, r, r), a64_fp3(ty, o, 1, 2, 3, 4));
        T(F("fcmp %c1, %c2", r, r), a64_fcmp(ty, 0, 0, 1, 2));
        T(F("fcmpe %c1, %c2", r, r), a64_fcmp(ty, 1, 0, 1, 2));
        T(F("fcmp %c1, #0.0", r), a64_fcmp(ty, 0, 1, 1, 0));
        T(F("fcmpe %c1, #0.0", r), a64_fcmp(ty, 1, 1, 1, 0));
        static const struct { int op; const char *n; } cv[] = {{0x02, "scvtf"}, {0x03, "ucvtf"}};
        for (int k = 0; k < 2; k++) T(F("%s %c1, w2", cv[k].n, r), a64_fcvt_int(0, ty, cv[k].op, 1, 2));
        static const struct { int op; const char *n; } ci[] = {{0x18, "fcvtzs"}, {0x19, "fcvtzu"}, {0x00, "fcvtns"},
            {0x01, "fcvtnu"}, {0x08, "fcvtps"}, {0x09, "fcvtpu"}, {0x10, "fcvtms"}, {0x11, "fcvtmu"}, {0x04, "fcvtas"},
            {0x05, "fcvtau"}};
        for (int k = 0; k < 10; k++) T(F("%s w1, %c2", ci[k].n, r), a64_fcvt_int(0, ty, ci[k].op, 1, 2));
        T(F("scvtf %c1, w2, #16", r), a64_fcvt_fix(0, ty, 0x02, 1, 2, 16));
        T(F("fcvtzu w1, %c2, #32", r), a64_fcvt_fix(0, ty, 0x19, 1, 2, 32));
        T(F("fcsel %c1, %c2, %c3, gt", r, r, r), a64_fcsel(ty, 1, 2, 3, A64_GT));
    }
    T("fcvt d1, s2", a64_fp1(0, 5, 1, 2));
    T("fcvt s1, d2", a64_fp1(1, 4, 1, 2));
    T("fcvt h1, s2", a64_fp1(0, 7, 1, 2));
    T("fmov w1, s2", a64_fcvt_int(0, 0, 0x06, 1, 2));
    T("fmov s1, w2", a64_fcvt_int(0, 0, 0x07, 1, 2));
    T("fmov x1, d2", a64_fcvt_int(1, 1, 0x06, 1, 2));
    T("fmov d1, x2", a64_fcvt_int(1, 1, 0x07, 1, 2));
    T("add v1.4s, v2.4s, v3.4s", a64_simd3(1, 0, 2, 0x10, 1, 2, 3));
    T("sub v1.8b, v2.8b, v3.8b", a64_simd3(0, 1, 0, 0x10, 1, 2, 3));
    T("mul v1.8h, v2.8h, v3.8h", a64_simd3(1, 0, 1, 0x13, 1, 2, 3));
    T("and v1.16b, v2.16b, v3.16b", a64_simd3(1, 0, 0, 0x03, 1, 2, 3));
    T("bsl v1.8b, v2.8b, v3.8b", a64_simd3(0, 1, 1, 0x03, 1, 2, 3));
    T("fadd v1.4s, v2.4s, v3.4s", a64_simd3(1, 0, 0, 0x1a, 1, 2, 3));
    T("fmul v1.2s, v2.2s, v3.2s", a64_simd3(0, 1, 0, 0x1b, 1, 2, 3));
    T("sqadd v1.2d, v2.2d, v3.2d", a64_simd3(1, 0, 3, 0x01, 1, 2, 3));
    T("cmeq v1.16b, v2.16b, v3.16b", a64_simd3(1, 1, 0, 0x11, 1, 2, 3));
    T("abs v1.4s, v2.4s", a64_simd2(1, 0, 2, 0x0b, 1, 2));
    T("cnt v1.8b, v2.8b", a64_simd2(0, 0, 0, 0x05, 1, 2));
    T("rev64 v1.16b, v2.16b", a64_simd2(1, 0, 0, 0x00, 1, 2));
    T("sshr v1.4s, v2.4s, #3", a64_simd_shift(1, 0, 64 - 3, 0x00, 1, 2));
    T("shl v1.8h, v2.8h, #5", a64_simd_shift(1, 0, 16 + 5, 0x0a, 1, 2));
    T("ushr v1.2d, v2.2d, #64", a64_simd_shift(1, 1, 128 - 64, 0x00, 1, 2));
    T("dup v1.4s, v2.s[3]", a64_dup_elem(1, 1, 2, 0x1c));
    T("dup v1.8b, w2", a64_dup_gen(0, 1, 2, 0x01));
    T("mov v1.s[2], w3", a64_ins_gen(1, 3, 0x14));
    T("mov v1.d[1], x3", a64_ins_gen(1, 3, 0x18));
    T("umov w1, v2.h[5]", a64_umov(0, 1, 2, 0x16));
    T("mov x1, v2.d[1]", a64_umov(1, 1, 2, 0x18));
    T("smov w1, v2.b[9]", a64_smov(1, 2, 0x13));
    T("mov v1.d[1], v2.d[0]", a64_ins_elem(1, 2, 0x18, 0));
    T("mov v1.s[0], v2.s[3]", a64_ins_elem(1, 2, 0x04, 12));
    T("movi v1.2d, #0xff00ff00ff00ff00", a64_movi(1, 1, 14, 1, 0xaa));
    T("ext v1.16b, v2.16b, v3.16b, #7", a64_ext(1, 1, 2, 3, 7));
    T("smull v1.4s, v2.4h, v3.4h", a64_simd3diff(0, 0, 1, 0x0c, 1, 2, 3));
    T("zip1 v1.8h, v2.8h, v3.8h", a64_perm(1, 1, 3, 1, 2, 3));
    T("uzp2 v1.4s, v2.4s, v3.4s", a64_perm(1, 2, 5, 1, 2, 3));
    T("stp q16, q17, [x19, #192]", a64_ldstp_q(0, 16, 17, 19, 192));
    T("ldp q30, q31, [x19, #416]", a64_ldstp_q(1, 30, 31, 19, 416));
    T("mov s0, v17.s[2]", a64_dup_scalar(0, 17, a64_imm5(2, 2)));
    T("mov d1, v18.d[1]", a64_dup_scalar(1, 18, a64_imm5(3, 1)));
    T("mov v17.s[1], v0.s[0]", a64_ins_elem(17, 0, a64_imm5(2, 1), a64_imm4(2, 0)));
    T("mov v20.d[1], v2.d[0]", a64_ins_elem(20, 2, a64_imm5(3, 1), a64_imm4(3, 0)));
    T("mov v20.h[5], v2.h[3]", a64_ins_elem(20, 2, a64_imm5(1, 5), a64_imm4(1, 3)));
    T("mov v21.b[15], w3", a64_ins_gen(21, 3, a64_imm5(0, 15)));
    T("mov v2.16b, v17.16b", a64_mov_v(1, 2, 17));
    T("mov v2.8b, v17.8b", a64_mov_v(0, 2, 17));
    T("ld1 {v17.s}[2], [x3]", a64_ld1_lane(1, 2, 2, 17, 3));
    T("ld1 {v17.s}[1], [x3]", a64_ld1_lane(1, 2, 1, 17, 3));
    T("st1 {v20.s}[3], [x16]", a64_ld1_lane(0, 2, 3, 20, 16));
    T("ld1 {v20.d}[1], [x16]", a64_ld1_lane(1, 3, 1, 20, 16));
    T("st1 {v20.d}[0], [x2]", a64_ld1_lane(0, 3, 0, 20, 2));
    T("ld1 {v5.h}[7], [x2]", a64_ld1_lane(1, 1, 7, 5, 2));
    T("st1 {v5.b}[13], [x2]", a64_ld1_lane(0, 0, 13, 5, 2));
    T("ldxr w17, [x16]", a64_ldxr(2, 0, 17, 16));
    T("ldaxrb w17, [x16]", a64_ldxr(0, 1, 17, 16));
    T("ldxrh w3, [x4]", a64_ldxr(1, 0, 3, 4));
    T("stxr w20, w5, [x16]", a64_stxr(2, 0, 20, 5, 16));
    T("stlxrh w20, w5, [x16]", a64_stxr(1, 1, 20, 5, 16));
    T("clrex", A64_CLREX);
    T("b #-8", a64_b(-2));
    T("bl #400", a64_bl(100));
    T("b.ne #-4", a64_bcond(A64_NE, -1));
    T("cbz w3, #16", a64_cbz(0, 0, 3, 4));
    T("cbnz x5, #-32", a64_cbz(1, 1, 5, -8));
    T("tbnz w1, #29, #8", a64_tbz(1, 1, 29, 2));
    T("tbz x1, #40, #-4", a64_tbz(0, 1, 40, -1));
    T("adr x16, #12", a64_adr(16, 12));
    for (int k = 0; k < 14; k++) T(F("b.%s #20", cc[k]), a64_bcond(k, 5));
    /* logical immediates */
    static const uint64_t li[] = {0xff, 0xff00, 0x80000000u, 0x7fffffffu, 0x0f0f0f0fu, 0xaaaaaaaau, 0xfffffffeu,
                                  0x3fc00u, 0x1, 0xffff0000u, 0x00ff00ffu, 0x80000001u};
    for (unsigned k = 0; k < sizeof li / sizeof *li; k++) {
        int e = a64_bitmask(li[k], 0);
        if (e < 0) { printf("# bitmask %#llx not encodable?\n", (unsigned long long)li[k]); continue; }
        T(F("and w1, w2, #0x%llx", (unsigned long long)li[k]), a64_logic_imm(0, 0, 1, 2, (uint32_t)e));
    }
    static const uint64_t lx[] = {0xffull, 0x00ff00ff00ff00ffull, 0x5555555555555555ull, 0xfffffffffffffffeull,
                                  0x0000ffff00000000ull};
    for (unsigned k = 0; k < sizeof lx / sizeof *lx; k++) {
        int e = a64_bitmask(lx[k], 1);
        T(F("orr x1, x2, #0x%llx", (unsigned long long)lx[k]), a64_logic_imm(1, 1, 1, 2, (uint32_t)e));
    }
    printf("# notencodable %d %d %d\n", a64_bitmask(0x12345678, 0), a64_bitmask(0, 0), a64_bitmask(0xffffffffu, 0));
    return 0;
}
