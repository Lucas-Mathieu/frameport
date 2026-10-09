/* Decoded instruction: the common form of A32 and T32 instructions. Both decoders produce it, the interpreter
 * executes it and the JIT translates it, so ARM and Thumb share all semantics.
 *
 * VFP and Advanced SIMD (NEON) instructions are first rewritten to their A32 encoding (T32 differs only in the
 * prefix bits), then decoded once by vfp_neon_decode.c. */
#ifndef A32_INSN_H
#define A32_INSN_H

#include <stdint.h>

#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define BIT(x, n) (((x) >> (n)) & 1u)

enum cond { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL, C_NV };

enum alu { A_AND, A_EOR, A_SUB, A_RSB, A_ADD, A_ADC, A_SBC, A_RSC, A_TST, A_TEQ, A_CMP, A_CMN, A_ORR, A_MOV, A_BIC,
           A_MVN, A_ORN };

enum shift { SH_LSL, SH_LSR, SH_ASR, SH_ROR, SH_RRX };

enum form {
    F_IMM,  /* immediate operand (imm, immcarry) */
    F_REG,  /* register rm shifted by an immediate (shtype, shimm; LSR/ASR #32 decoded as 32, ROR #0 as RRX) */
    F_RSR,  /* register rm shifted by register rs */
};

enum op {
    OP_UNDEF, OP_NOP,
    OP_DP,        /* data processing: sub = enum alu, s, rd, rn, form + operand */
    OP_MOVW, OP_MOVT,
    OP_MUL,       /* sub = enum mul; rd = Rd / RdLo, ra = Ra / RdHi */
    OP_MULXY,     /* halfword multiplies: sub = enum mulxy, flags MF_NHI/MF_MHI */
    OP_DUALMUL,   /* SMUAD/SMUSD/SMLAD/SMLSD/SMLALD/SMLSLD: sub = enum dualmul, flags MF_SWAP */
    OP_SMMUL,     /* SMMUL/SMMLA/SMMLS: sub 0 = mul, 1 = mla, 2 = mls; flags MF_ROUND */
    OP_USAD8,     /* ra == 15: USAD8, else USADA8 */
    OP_DIV,       /* sign: SDIV/UDIV */
    OP_CLZ, OP_REV, OP_REV16, OP_REVSH, OP_RBIT,
    OP_EXTEND,    /* sub = enum ext, shimm = rotation, rn == 15: no add */
    OP_PKH,       /* sub 0 = PKHBT (LSL), 1 = PKHTB (ASR); shimm */
    OP_BFX,       /* sign; imm = lsb, shimm = width */
    OP_BFI,       /* rn == 15: BFC; imm = lsb, shimm = msb */
    OP_SAT,       /* sign: SSAT/USAT; imm = saturate_to, shtype/shimm = LSL/ASR amount */
    OP_SAT16,     /* sign; imm */
    OP_QADDSUB,   /* sub 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB */
    OP_PARALLEL,  /* sub = prefix (enum par_prefix) << 4 | op (enum par_op) */
    OP_SEL,
    OP_LDST,      /* single load/store: flags MF_LOAD, size 1/2/4/8 (LDRD/STRD: ra = Rt2), sign, p/u/w, form */
    OP_LDM,       /* p, u, w, reglist */
    OP_STM,
    OP_LDREX,     /* size 1/2/4/8 (ra = Rt2); flags MF_ACQREL */
    OP_STREX,     /* rd = status, rm = Rt, ra = Rt2 */
    OP_LDA, OP_STL, /* load-acquire / store-release (ARMv8): size, rd = Rt (LDA) / rm = Rt (STL) */
    OP_SWP,       /* size 1/4; rd = Rt, rm = Rt2, rn = address */
    OP_B,         /* imm = target address */
    OP_BL,        /* imm = target, stays in the current instruction set */
    OP_BLX_IMM,   /* imm = target, switches instruction set */
    OP_BX,        /* rm */
    OP_BLX_REG,   /* rm */
    OP_CBZ,       /* rn, imm = target, flags MF_NONZERO */
    OP_TB,        /* TBB/TBH: rn, rm, size 1/2 */
    OP_IT,        /* imm = firstcond:mask */
    OP_SVC,       /* imm */
    OP_BKPT,
    OP_MRS,       /* rd */
    OP_MSR,       /* imm = mask (bit 3 nzcvq, bit 2 g), form F_IMM (imm in reglist field) or rn */
    OP_MRC,       /* rd = Rt, imm = cp15 register id (see CP15_*) */
    OP_MCR,
    OP_MRRC,      /* rd, ra; imm = id */
    OP_BARRIER,
    OP_CLREX,
    OP_SETEND,    /* imm = E bit */
    OP_CRC32,     /* size 1/2/4, flags MF_CRC32C */
    OP_VFP,       /* sub = enum vop; see vfp_neon_decode.c for fields */
    OP_NEON,      /* sub = enum nop */
    OP_COUNT
};

/* insn.flags */
#define MF_LOAD    0x01
#define MF_NHI     0x02  /* SMULxy: use top half of Rn */
#define MF_MHI     0x04  /* SMULxy/SMLAWy: use top half of Rm */
#define MF_SWAP    0x08  /* dual multiply: swap Rm halves (X suffix) */
#define MF_ROUND   0x10  /* SMMUL R suffix */
#define MF_NONZERO 0x20  /* CBNZ */
#define MF_ACQREL  0x40  /* acquire/release variants */
#define MF_CRC32C  0x80
#define MF_SETFLAGS_IT 0x100 /* (T16) set flags only outside an IT block: resolved by the decoder */
#define MF_USER    0x200 /* LDRT/STRT etc. (user mode access, same as normal in user mode) */

enum mul { M_MUL, M_MLA, M_MLS, M_UMULL, M_UMLAL, M_SMULL, M_SMLAL, M_UMAAL };
enum mulxy { MX_SMLA, MX_SMLAW, MX_SMULW, MX_SMLAL, MX_SMUL };
enum dualmul { DM_SMLAD, DM_SMLSD, DM_SMLALD, DM_SMLSLD };  /* SMUAD/SMUSD = SMLAD/SMLSD with ra == 15 */
enum ext { E_SXTB16, E_SXTB, E_SXTH, E_UXTB16, E_UXTB, E_UXTH };
enum par_prefix { PP_S, PP_Q, PP_SH, PP_U, PP_UQ, PP_UH };
enum par_op { PO_ADD16, PO_ASX, PO_SAX, PO_SUB16, PO_ADD8, PO_SUB8 };

enum cp15 { CP15_TPIDRURW, CP15_TPIDRURO, CP15_CNTFRQ, CP15_CNTVCT, CP15_CNTPCT };

/* VFP operations (insn.sub when op == OP_VFP). dp = double precision. */
enum vop {
    V_MLA, V_MLS, V_NMLA, V_NMLS, V_MUL, V_NMUL, V_ADD, V_SUB, V_DIV, V_FNMA, V_FNMS, V_FMA, V_FMS,
    V_MOVI, V_MOV, V_ABS, V_NEG, V_SQRT,
    V_CVT_HALF,   /* VCVTB/VCVTT: flags VF_TOP (T), VF_TOHALF (to half precision); dp = double <-> half */
    V_CMP,        /* flags VF_E (VCMPE), VF_ZERO (compare with 0) */
    V_RINT,       /* imm = rounding: 0..3 = RN RP RM RZ, 4 = FPSCR (VRINTR), 5 = exact FPSCR (VRINTX), 6 = ties away (VRINTA) */
    V_CVT_DS,     /* dp: double -> single, else single -> double */
    V_CVT_I2F,    /* VF_SIGNED */
    V_CVT_F2I,    /* VF_SIGNED; imm = rounding as V_RINT (3 = RZ for VCVT, 4 = FPSCR for VCVTR, 6/0/1/2 for VCVTA/N/P/M) */
    V_CVT_FIX,    /* VF_SIGNED, VF_TOFIX, imm = fraction bits, size = 16/32 */
    V_SEL,        /* imm = cond (EQ, VS, GE, GT) */
    V_MAXNM, V_MINNM,
    V_MOV_CS,     /* core <-> S: rd = Rt, vn = S index, VF_TOCORE */
    V_MOV_CSS,    /* two core <-> two consecutive S: rd = Rt, ra = Rt2, vm = S index */
    V_MOV_CD,     /* two core <-> D: rd = Rt, ra = Rt2, vm = D index */
    V_MOV_SCALAR, /* core <-> scalar: rd = Rt, vd/vn = D index, imm = lane, size = 1/2/4 bytes, VF_SIGNED, VF_TOCORE */
    V_DUP,        /* core -> all lanes: rd = Rt, vd = D index, q, size = 1/2/4 */
    V_MSR,        /* VMSR FPSCR (imm = system register id), rd = Rt */
    V_MRS,        /* VMRS, rd = Rt (15 = APSR_nzcv) */
    V_LDST,       /* VLDR/VSTR: flags VF_LOAD, rn, imm (signed offset), vd = S or D index */
    V_LDSTM,      /* VLDM/VSTM: VF_LOAD, rn, p/u/w, vd, nregs (count of S or D registers) */
    V_COUNT
};
#define VF_E       0x01
#define VF_ZERO    0x02
#define VF_SIGNED  0x04
#define VF_TOFIX   0x08
#define VF_TOCORE  0x10
#define VF_LOAD    0x20
#define VF_TOP     0x40
#define VF_TOHALF  0x80

/* Advanced SIMD operations (insn.sub when op == OP_NEON). Field use is documented per group in
 * vfp_neon_decode.c; registers vd/vn/vm are D register numbers (a Q register n is D 2n). */
enum nop {
    /* 3 registers, same length (size = log2 element bytes, VF_SIGNED unless U) */
    N_VHADD, N_VQADD, N_VRHADD, N_VAND, N_VBIC, N_VORR, N_VORN, N_VEOR, N_VBSL, N_VBIT, N_VBIF, N_VHSUB, N_VQSUB,
    N_VCGT, N_VCGE, N_VSHL, N_VQSHL, N_VRSHL, N_VQRSHL, N_VMAX, N_VMIN, N_VABD, N_VABA, N_VADD, N_VSUB, N_VTST,
    N_VCEQ, N_VMLA, N_VMLS, N_VMUL, N_VMULP, N_VPMAX, N_VPMIN, N_VQDMULH, N_VQRDMULH, N_VPADD,
    N_FADD, N_FSUB, N_FPADD, N_FABD, N_FMUL, N_FMLA, N_FMLS, N_FCEQ, N_FCGE, N_FCGT, N_FACGE, N_FACGT, N_FMAX,
    N_FMIN, N_FPMAX, N_FPMIN, N_FRECPS, N_FRSQRTS, N_FMA, N_FMS, N_FMAXNM, N_FMINNM,
    /* 3 registers, different lengths (long/wide/narrow) */
    N_VADDL, N_VADDW, N_VSUBL, N_VSUBW, N_VADDHN, N_VRADDHN, N_VABAL, N_VSUBHN, N_VRSUBHN, N_VABDL, N_VMLAL,
    N_VMLSL, N_VQDMLAL, N_VQDMLSL, N_VMULL, N_VQDMULL, N_VMULLP,
    /* 2 registers and a scalar (imm = scalar lane, vm = D register of the scalar) */
    N_VMLA_S, N_VMLS_S, N_VMLAL_S, N_VMLSL_S, N_VQDMLAL_S, N_VQDMLSL_S, N_VMUL_S, N_VMULL_S, N_VQDMULL_S,
    N_VQDMULH_S, N_VQRDMULH_S, N_FMLA_S, N_FMLS_S, N_FMUL_S,
    /* 2 registers and a shift amount (imm) */
    N_VSHR, N_VSRA, N_VRSHR, N_VRSRA, N_VSRI, N_VSHL_I, N_VSLI, N_VQSHLU_I, N_VQSHL_I, N_VSHRN, N_VRSHRN,
    N_VQSHRUN, N_VQRSHRUN, N_VQSHRN, N_VQRSHRN, N_VSHLL, N_VMOVL, N_VCVT_FIXF /* float<->fixed, VF_TOFIX */,
    /* 1 register and a modified immediate (imm64) */
    N_VMOV_I, N_VORR_I, N_VMVN_I, N_VBIC_I,
    /* 2 registers, miscellaneous */
    N_VREV64, N_VREV32, N_VREV16, N_VPADDL, N_VCLS, N_VCLZ, N_VCNT, N_VMVN, N_VPADAL, N_VQABS, N_VQNEG,
    N_VCGT0, N_VCGE0, N_VCEQ0, N_VCLE0, N_VCLT0, N_VABS, N_VNEG, N_FCGT0, N_FCGE0, N_FCEQ0, N_FCLE0, N_FCLT0,
    N_FABS, N_FNEG, N_VSWP, N_VTRN, N_VUZP, N_VZIP, N_VMOVN, N_VQMOVUN, N_VQMOVN, N_VSHLL_MAX, N_VCVT_HS
    /* VF_TOHALF: f32 -> f16 narrow, else widen */, N_VRECPE, N_VRSQRTE, N_FRECPE, N_FRSQRTE,
    N_VCVT_FI /* float <-> int: VF_SIGNED, VF_TOFIX = to integer */, N_FRINT /* imm = rounding */,
    N_VCVT_RND /* VCVTA/N/P/M: imm = rounding, VF_SIGNED */,
    N_AESE, N_AESD, N_AESMC, N_AESIMC, N_SHA1H, N_SHA1SU1, N_SHA256SU0,
    /* other */
    N_VEXT,      /* imm = byte offset */
    N_VTBL,      /* VF_E = VTBX, nregs = list length, vn = first table register */
    N_VDUP_S,    /* imm = lane, size = log2 element */
    /* 3-same crypto (ARMv8) */
    N_SHA1C, N_SHA1P, N_SHA1M, N_SHA1SU0, N_SHA256H, N_SHA256H2, N_SHA256SU1,
    /* element/structure load/store: VF_LOAD; rn, rm (15 = no writeback, 13 = writeback by size, else by Rm);
       nregs = number of registers, imm = structure type (1-4), size = log2 element, imm2 = lane or -1 for all
       lanes (VLDn to all lanes uses flags NF_ALL), inc = register spacing */
    N_VLDST_MULTI, N_VLDST_LANE, N_VLD_ALL,
    N_COUNT
};
#define NF_ALL 0x100

typedef struct insn {
    uint32_t pc;        /* address of the instruction */
    uint32_t pcval;     /* value read from r15 by this instruction (pc + 8, pc + 4, or Align(pc + 4, 4)) */
    uint32_t raw;       /* A32: the word; T32: hw1 << 16 | hw2, or hw1 for 16-bit; VFP/NEON: A32 form */
    uint16_t op;        /* enum op */
    uint16_t sub;
    uint16_t flags;     /* MF_* or VF_* */
    uint8_t cond;       /* enum cond; Thumb instructions in an IT block get the block's condition */
    uint8_t len;        /* 2 or 4 */
    uint8_t thumb;
    uint8_t s;          /* sets flags */
    uint8_t rd, rn, rm, rs, ra;
    uint8_t shtype, shimm, form, immcarry;
    uint8_t p, u, w, size, sign;
    uint8_t unpred;     /* encoding is UNPREDICTABLE / CONSTRAINED UNPREDICTABLE (executed best effort) */
    uint8_t writes_pc;  /* instruction can write r15 (ends a block) */
    /* VFP / NEON */
    uint8_t vd, vn, vm, q, dp, nregs, inc, esize;
    uint32_t imm;
    uint32_t reglist;
    int32_t imm2;
    uint64_t imm64;
} insn_t;

int a32_decode(uint32_t w, uint32_t pc, insn_t *i);
/* in_it: inside an IT block (Thumb 16-bit flag setting depends on it), itcond: condition from ITSTATE */
int t32_decode(const uint16_t *code, uint32_t pc, int in_it, int itcond, insn_t *i);
int t32_insn_len(uint16_t hw1);
void vfp_neon_decode(uint32_t a32, insn_t *i);  /* fills op/sub/fields for a VFP or Advanced SIMD A32 word */
const char *op_name(const insn_t *i);

static inline uint32_t ror32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

#endif
