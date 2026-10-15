/* A64 instruction encoders used by the JIT. Register numbers are 0..31 (31 = WZR/XZR or SP depending on the
 * instruction); sf selects the 64-bit form. Checked against llvm-mc by tests/test_a64.py. */
#ifndef A32_A64_H
#define A32_A64_H

#include <stdint.h>

typedef struct {
    uint32_t *start, *p, *end;
    int overflow;
} cbuf;

static inline void emit(cbuf *b, uint32_t w)
{
    if (b->p < b->end) *b->p++ = w;
    else b->overflow = 1;
}

enum { A64_EQ, A64_NE, A64_CS, A64_CC, A64_MI, A64_PL, A64_VS, A64_VC, A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE,
       A64_AL };
enum { SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR };
#define XZR 31
#define SPREG 31

/* ---------------------------------------------------------------- integer */

static inline uint32_t a64_addsub_imm(int sf, int sub, int s, int rd, int rn, uint32_t imm12, int sh12)
{
    return 0x11000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | (uint32_t)sh12 << 22 |
           (imm12 & 0xfff) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_addsub_reg(int sf, int sub, int s, int rd, int rn, int rm, int shift, int amt)
{
    return 0x0b000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | (uint32_t)shift << 22 |
           (uint32_t)rm << 16 | (uint32_t)(amt & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* extended register: option 0..7 = UXTB UXTH UXTW UXTX SXTB SXTH SXTW SXTX */
static inline uint32_t a64_addsub_ext(int sf, int sub, int s, int rd, int rn, int rm, int option, int amt)
{
    return 0x0b200000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | (uint32_t)rm << 16 |
           (uint32_t)option << 13 | (uint32_t)amt << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_adc(int sf, int sub, int s, int rd, int rn, int rm)
{
    return 0x1a000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 | (uint32_t)s << 29 | (uint32_t)rm << 16 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; n: invert rm (BIC, ORN, EON, BICS) */
static inline uint32_t a64_logic_reg(int sf, int opc, int n, int rd, int rn, int rm, int shift, int amt)
{
    return 0x0a000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | (uint32_t)shift << 22 | (uint32_t)n << 21 |
           (uint32_t)rm << 16 | (uint32_t)(amt & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_logic_imm(int sf, int opc, int rd, int rn, uint32_t nimms /* N:immr:imms */)
{
    return 0x12000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | nimms << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* opc: 0 MOVN, 2 MOVZ, 3 MOVK */
static inline uint32_t a64_movw(int sf, int opc, int rd, uint32_t imm16, int hw)
{
    return 0x12800000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | (uint32_t)hw << 21 | (imm16 & 0xffff) << 5 |
           (uint32_t)rd;
}
/* opc: 0 SBFM, 1 BFM, 2 UBFM */
static inline uint32_t a64_bfm(int sf, int opc, int rd, int rn, int immr, int imms)
{
    return 0x13000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 | (uint32_t)sf << 22 | (uint32_t)immr << 16 |
           (uint32_t)imms << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_extr(int sf, int rd, int rn, int rm, int lsb)
{
    return 0x13800000u | (uint32_t)sf << 31 | (uint32_t)sf << 22 | (uint32_t)rm << 16 | (uint32_t)lsb << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* 2-source: 2 UDIV, 3 SDIV, 8 LSLV, 9 LSRV, 10 ASRV, 11 RORV */
static inline uint32_t a64_dp2(int sf, int opcode, int rd, int rn, int rm)
{
    return 0x1ac00000u | (uint32_t)sf << 31 | (uint32_t)rm << 16 | (uint32_t)opcode << 10 | (uint32_t)rn << 5 |
           (uint32_t)rd;
}
/* 1-source: 0 RBIT, 1 REV16, 2 REV(32) / REV32(64), 3 REV(64), 4 CLZ, 5 CLS */
static inline uint32_t a64_dp1(int sf, int opcode, int rd, int rn)
{
    return 0x5ac00000u | (uint32_t)sf << 31 | (uint32_t)opcode << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_madd(int sf, int sub, int rd, int rn, int rm, int ra)
{
    return 0x1b000000u | (uint32_t)sf << 31 | (uint32_t)rm << 16 | (uint32_t)sub << 15 | (uint32_t)ra << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* SMADDL/SMSUBL/UMADDL/UMSUBL: xd = xa +/- wn * wm */
static inline uint32_t a64_maddl(int u, int sub, int rd, int rn, int rm, int ra)
{
    return 0x9b200000u | (uint32_t)u << 23 | (uint32_t)rm << 16 | (uint32_t)sub << 15 | (uint32_t)ra << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_mulh(int u, int rd, int rn, int rm)
{
    return 0x9b407c00u | (uint32_t)u << 23 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* op: 0 CSEL, 1 CSINC, 2 CSINV, 3 CSNEG */
static inline uint32_t a64_csel(int sf, int op, int rd, int rn, int rm, int cond)
{
    return 0x1a800000u | (uint32_t)sf << 31 | (uint32_t)(op >> 1) << 30 | (uint32_t)rm << 16 | (uint32_t)cond << 12 |
           (uint32_t)(op & 1) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_ccmp_imm(int sf, int neg, int rn, int imm5, int nzcv, int cond)
{
    return 0x7a400800u | (uint32_t)sf << 31 | (uint32_t)(!neg) << 30 | (uint32_t)imm5 << 16 | (uint32_t)cond << 12 |
           (uint32_t)rn << 5 | (uint32_t)nzcv;
}

#define SYS_NZCV 0x5a10u /* op0 3, op1 3, CRn 4, CRm 2, op2 0 */
#define SYS_FPCR 0x5a20u
#define SYS_FPSR 0x5a21u
#define SYS_CNTVCT 0x5f02u
#define SYS_CNTFRQ 0x5f00u
static inline uint32_t a64_mrs(int rt, uint32_t sysreg) { return 0xd5300000u | sysreg << 5 | (uint32_t)rt; }
static inline uint32_t a64_msr(int rt, uint32_t sysreg) { return 0xd5100000u | sysreg << 5 | (uint32_t)rt; }

/* loads/stores, unsigned scaled immediate. size: 0 B, 1 H, 2 W, 3 X; opc: 0 store, 1 load, 2 load signed 64,
 * 3 load signed 32 */
static inline uint32_t a64_ldst_imm(int size, int opc, int rt, int rn, uint32_t byte_off)
{
    return 0x39000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 | ((byte_off >> size) & 0xfff) << 10 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}
/* unscaled signed 9-bit offset (LDUR/STUR) */
static inline uint32_t a64_ldst_unscaled(int size, int opc, int rt, int rn, int off)
{
    return 0x38000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 | ((uint32_t)off & 0x1ff) << 12 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}
/* register offset: option 2 UXTW, 3 LSL, 6 SXTW, 7 SXTX; s: scale by access size */
static inline uint32_t a64_ldst_reg(int size, int opc, int rt, int rn, int rm, int option, int s)
{
    return 0x38200800u | (uint32_t)size << 30 | (uint32_t)opc << 22 | (uint32_t)rm << 16 | (uint32_t)option << 13 |
           (uint32_t)s << 12 | (uint32_t)rn << 5 | (uint32_t)rt;
}
/* LDP/STP signed offset; sf 0: W registers (offset multiple of 4), 1: X */
static inline uint32_t a64_ldstp(int sf, int load, int rt, int rt2, int rn, int off)
{
    int sc = sf ? 3 : 2;
    return 0x29000000u | (uint32_t)sf << 31 | (uint32_t)load << 22 | ((uint32_t)(off >> sc) & 0x7f) << 15 |
           (uint32_t)rt2 << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}
/* FP/SIMD register load/store, unsigned scaled offset. size: 2 S, 3 D, 4 Q */
static inline uint32_t a64_ldst_fp(int size, int load, int rt, int rn, uint32_t byte_off)
{
    uint32_t sz = size == 4 ? 0 : (uint32_t)size, opc = (size == 4 ? 2u : 0u) | (uint32_t)load;
    return 0x3d000000u | sz << 30 | opc << 22 | ((byte_off >> size) & 0xfff) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}
/* FP/SIMD register offset (Xn + Xm, unscaled) */
static inline uint32_t a64_ldst_fp_reg(int size, int load, int rt, int rn, int rm)
{
    uint32_t sz = size == 4 ? 0 : (uint32_t)size, opc = (size == 4 ? 2u : 0u) | (uint32_t)load;
    return 0x3c206800u | sz << 30 | opc << 22 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t a64_ldst_fp_unscaled(int size, int load, int rt, int rn, int off)
{
    uint32_t sz = size == 4 ? 0 : (uint32_t)size, opc = (size == 4 ? 2u : 0u) | (uint32_t)load;
    return 0x3c000000u | sz << 30 | opc << 22 | ((uint32_t)off & 0x1ff) << 12 | (uint32_t)rn << 5 | (uint32_t)rt;
}

static inline uint32_t a64_b(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x3ffffff); }
static inline uint32_t a64_bl(int32_t words) { return 0x94000000u | ((uint32_t)words & 0x3ffffff); }
static inline uint32_t a64_bcond(int cond, int32_t words) { return 0x54000000u | ((uint32_t)words & 0x7ffff) << 5 | (uint32_t)cond; }
static inline uint32_t a64_cbz(int sf, int nz, int rt, int32_t words)
{
    return 0x34000000u | (uint32_t)sf << 31 | (uint32_t)nz << 24 | ((uint32_t)words & 0x7ffff) << 5 | (uint32_t)rt;
}
static inline uint32_t a64_tbz(int nz, int rt, int bit, int32_t words)
{
    return 0x36000000u | (uint32_t)(bit >> 5) << 31 | (uint32_t)nz << 24 | (uint32_t)(bit & 31) << 19 |
           ((uint32_t)words & 0x3fff) << 5 | (uint32_t)rt;
}
static inline uint32_t a64_br(int rn) { return 0xd61f0000u | (uint32_t)rn << 5; }
static inline uint32_t a64_blr(int rn) { return 0xd63f0000u | (uint32_t)rn << 5; }
static inline uint32_t a64_ret(void) { return 0xd65f03c0u; }
static inline uint32_t a64_adr(int rd, int32_t off)
{
    return 0x10000000u | ((uint32_t)off & 3) << 29 | (((uint32_t)off >> 2) & 0x7ffff) << 5 | (uint32_t)rd;
}
#define A64_NOP 0xd503201fu
#define A64_DMB_ISH 0xd5033bbfu
#define A64_DSB_ISH 0xd5033b9fu
#define A64_ISB 0xd5033fdfu

/* exclusives (size 0..3): LDXR/LDAXR Rt, [Xn]; STXR/STLXR Ws, Rt, [Xn] */
static inline uint32_t a64_ldxr(int size, int acq, int rt, int rn)
{
    return 0x085f7c00u | (uint32_t)size << 30 | (uint32_t)acq << 15 | (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t a64_stxr(int size, int rel, int rs, int rt, int rn)
{
    return 0x08007c00u | (uint32_t)size << 30 | (uint32_t)rs << 16 | (uint32_t)rel << 15 | (uint32_t)rn << 5 | (uint32_t)rt;
}
#define A64_CLREX 0xd5033f5fu
/* load-acquire / store-release (size 0..3) */
static inline uint32_t a64_ldar(int size, int rt, int rn) { return 0x08dffc00u | (uint32_t)size << 30 | (uint32_t)rn << 5 | (uint32_t)rt; }
static inline uint32_t a64_stlr(int size, int rt, int rn) { return 0x089ffc00u | (uint32_t)size << 30 | (uint32_t)rn << 5 | (uint32_t)rt; }

/* ---------------------------------------------------------------- scalar floating point (type 0 = S, 1 = D) */

/* opcode: 0 FMUL, 1 FDIV, 2 FADD, 3 FSUB, 4 FMAX, 5 FMIN, 6 FMAXNM, 7 FMINNM, 8 FNMUL */
static inline uint32_t a64_fp2(int type, int opcode, int rd, int rn, int rm)
{
    return 0x1e200800u | (uint32_t)type << 22 | (uint32_t)rm << 16 | (uint32_t)opcode << 12 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* opcode: 0 FMOV, 1 FABS, 2 FNEG, 3 FSQRT, 4 FCVT->S, 5 FCVT->D, 7 FCVT->H, 8 FRINTN, 9 FRINTP, 10 FRINTM,
 * 11 FRINTZ, 12 FRINTA, 14 FRINTX, 15 FRINTI */
static inline uint32_t a64_fp1(int type, int opcode, int rd, int rn)
{
    return 0x1e204000u | (uint32_t)type << 22 | (uint32_t)opcode << 15 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* o1:o0: 0 FMADD, 1 FMSUB, 2 FNMADD, 3 FNMSUB */
static inline uint32_t a64_fp3(int type, int op, int rd, int rn, int rm, int ra)
{
    return 0x1f000000u | (uint32_t)type << 22 | (uint32_t)(op >> 1) << 21 | (uint32_t)rm << 16 | (uint32_t)(op & 1) << 15 |
           (uint32_t)ra << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_fcmp(int type, int e, int zero, int rn, int rm)
{
    return 0x1e202000u | (uint32_t)type << 22 | (uint32_t)(zero ? 0 : rm) << 16 | (uint32_t)rn << 5 |
           (uint32_t)e << 4 | (uint32_t)zero << 3;
}
/* int <-> fp. rmode:opcode as in the ARM ARM: SCVTF 0x02, UCVTF 0x03, FCVTZS 0x18, FCVTZU 0x19, FCVTNS 0x00,
 * FCVTNU 0x01, FCVTPS 0x08, FCVTPU 0x09, FCVTMS 0x10, FCVTMU 0x11, FCVTAS 0x04, FCVTAU 0x05, FMOV W<-S 0x06,
 * FMOV S<-W 0x07 */
static inline uint32_t a64_fcvt_int(int sf, int type, int rmode_op, int rd, int rn)
{
    return 0x1e200000u | (uint32_t)sf << 31 | (uint32_t)type << 22 | (uint32_t)rmode_op << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* fixed point: SCVTF 0x02, UCVTF 0x03, FCVTZS 0x18, FCVTZU 0x19; fbits 1..32 (sf = 0) */
static inline uint32_t a64_fcvt_fix(int sf, int type, int rmode_op, int rd, int rn, int fbits)
{
    return 0x1e000000u | (uint32_t)sf << 31 | (uint32_t)type << 22 | (uint32_t)rmode_op << 16 |
           (uint32_t)(64 - fbits) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t a64_fcsel(int type, int rd, int rn, int rm, int cond)
{
    return 0x1e200c00u | (uint32_t)type << 22 | (uint32_t)rm << 16 | (uint32_t)cond << 12 | (uint32_t)rn << 5 | (uint32_t)rd;
}

/* ---------------------------------------------------------------- Advanced SIMD (subset) */

/* three same: opcode 5 bits, size 2 bits */
static inline uint32_t a64_simd3(int q, int u, int size, int opcode, int rd, int rn, int rm)
{
    return 0x0e200400u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)size << 22 | (uint32_t)rm << 16 |
           (uint32_t)opcode << 11 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* two-register misc: opcode 5 bits */
static inline uint32_t a64_simd2(int q, int u, int size, int opcode, int rd, int rn)
{
    return 0x0e200800u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)size << 22 | (uint32_t)opcode << 12 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* shift by immediate: immh:immb encodes element size and amount; opcode 5 bits */
static inline uint32_t a64_simd_shift(int q, int u, int immhb, int opcode, int rd, int rn)
{
    return 0x0f000400u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)immhb << 16 | (uint32_t)opcode << 11 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* DUP (element): imm5 encodes size and index */
static inline uint32_t a64_dup_elem(int q, int rd, int rn, int imm5)
{
    return 0x0e000400u | (uint32_t)q << 30 | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* DUP (general) */
static inline uint32_t a64_dup_gen(int q, int rd, int rn, int imm5)
{
    return 0x0e000c00u | (uint32_t)q << 30 | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* INS (general): Vd.T[idx] = Rn */
static inline uint32_t a64_ins_gen(int rd, int rn, int imm5) { return 0x4e001c00u | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd; }
/* UMOV / SMOV (to W) */
static inline uint32_t a64_umov(int sf, int rd, int rn, int imm5) { return 0x0e003c00u | (uint32_t)sf << 30 | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd; }
static inline uint32_t a64_smov(int rd, int rn, int imm5) { return 0x0e002c00u | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd; }
/* INS (element): Vd.T[i1] = Vn.T[i2] */
static inline uint32_t a64_ins_elem(int rd, int rn, int imm5, int imm4)
{
    return 0x6e000400u | (uint32_t)imm5 << 16 | (uint32_t)imm4 << 11 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* MOVI/MVNI/ORR/BIC (vector, immediate) */
static inline uint32_t a64_movi(int q, int op, int cmode, int rd, uint32_t imm8)
{
    return 0x0f000400u | (uint32_t)q << 30 | (uint32_t)op << 29 | (imm8 >> 5) << 16 | (uint32_t)cmode << 12 |
           (imm8 & 31) << 5 | (uint32_t)rd;
}
static inline uint32_t a64_ext(int q, int rd, int rn, int rm, int idx)
{
    return 0x2e000000u | (uint32_t)q << 30 | (uint32_t)rm << 16 | (uint32_t)idx << 11 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* three different (long/wide/narrow): opcode 4 bits */
static inline uint32_t a64_simd3diff(int q, int u, int size, int opcode, int rd, int rn, int rm)
{
    return 0x0e200000u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)size << 22 | (uint32_t)rm << 16 |
           (uint32_t)opcode << 12 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* by element: opcode 4 bits, L/M/H index bits supplied */
static inline uint32_t a64_simd_elem(int q, int u, int size, int opcode, int rd, int rn, int rm, int l, int m, int h)
{
    return 0x0f000000u | (uint32_t)q << 30 | (uint32_t)u << 29 | (uint32_t)size << 22 | (uint32_t)l << 21 |
           (uint32_t)m << 20 | (uint32_t)(rm & 15) << 16 | (uint32_t)opcode << 12 | (uint32_t)h << 11 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* permute: opcode 1 UZP1, 2 TRN1, 3 ZIP1, 5 UZP2, 6 TRN2, 7 ZIP2 */
static inline uint32_t a64_perm(int q, int size, int opcode, int rd, int rn, int rm)
{
    return 0x0e000800u | (uint32_t)q << 30 | (uint32_t)size << 22 | (uint32_t)rm << 16 | (uint32_t)opcode << 12 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
/* vector register copy (ORR Vd, Vn, Vn) */
static inline uint32_t a64_mov_v(int q, int rd, int rn)
{
    return 0x0ea01c00u | (uint32_t)q << 30 | (uint32_t)rn << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* LDP/STP of Q registers, signed offset (multiple of 16) */
static inline uint32_t a64_ldstp_q(int load, int rt, int rt2, int rn, int off)
{
    return 0xad000000u | (uint32_t)load << 22 | ((uint32_t)(off >> 4) & 0x7f) << 15 | (uint32_t)rt2 << 10 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}
/* DUP (element, scalar): Sd/Dd/... = Vn.T[i] ("mov s0, v1.s[2]") */
static inline uint32_t a64_dup_scalar(int rd, int rn, int imm5)
{
    return 0x5e000400u | (uint32_t)imm5 << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
/* LD1/ST1 (single structure, one lane) {Vt.T}[idx], [Xn]; es = log2 element bytes */
static inline uint32_t a64_ld1_lane(int load, int es, int idx, int rt, int rn)
{
    uint32_t opcode, q, s, size;
    switch (es) {
    case 0: opcode = 0; q = idx >> 3; s = (idx >> 2) & 1; size = idx & 3; break;
    case 1: opcode = 2; q = idx >> 2; s = (idx >> 1) & 1; size = (idx & 1) << 1; break;
    case 2: opcode = 4; q = idx >> 1; s = idx & 1; size = 0; break;
    default: opcode = 4; q = idx & 1; s = 0; size = 1; break;
    }
    return 0x0d000000u | q << 30 | (uint32_t)load << 22 | opcode << 13 | s << 12 | size << 10 | (uint32_t)rn << 5 |
           (uint32_t)rt;
}
/* lane encodings: imm5 for element size log2 es and index i; imm4 for INS (element) source index */
static inline int a64_imm5(int es, int idx) { return ((idx << 1) | 1) << es; }
static inline int a64_imm4(int es, int idx) { return idx << es; }

/* Encodes a 32- or 64-bit logical immediate (N:immr:imms), or returns -1. */
static inline int a64_bitmask(uint64_t v, int sf)
{
    if (!sf) v = (v & 0xffffffffull) | (v << 32);
    if (v == 0 || v == ~0ull) return -1;
    int size = 64;
    for (; size > 2; size >>= 1) {
        uint64_t mask = (1ull << (size / 2)) - 1;
        if ((v & mask) != ((v >> (size / 2)) & mask)) break;
    }
    uint64_t emask = size == 64 ? ~0ull : (1ull << size) - 1, e = v & emask;
    /* rotate e right until it is of the form 0..01..1 */
    int rot;
    for (rot = 0; rot < size; rot++) {
        uint64_t r = ((e >> rot) | (e << (size - rot))) & emask;
        if (rot == 0) r = e;
        if ((r & (r + 1)) == 0 && (r & 1)) {
            int ones = __builtin_popcountll(r);
            int immr = (size - rot) % size, imms = ((~(size - 1) << 1) & 0x3f) | (ones - 1);
            int n = size == 64;
            if (!sf && n) return -1;
            return n << 12 | immr << 6 | (imms & 0x3f);
        }
    }
    return -1;
}

#endif
