/* A32 (ARM state) decoder, following the ARMv7-A/R ARM (DDI 0406C) chapter A5 plus the ARMv8 AArch32 additions
 * that user code meets (LDA/STL, CRC32, VSEL & co. live in vfp_neon_decode.c). */
#include <string.h>
#include "insn.h"

static void shift_imm(insn_t *i, uint32_t w)
{
    unsigned type = BITS(w, 6, 5), amt = BITS(w, 11, 7);
    i->rm = BITS(w, 3, 0);
    i->form = F_REG;
    i->shtype = type;
    i->shimm = amt;
    if (type == SH_LSR || type == SH_ASR) {
        if (amt == 0) i->shimm = 32;
    } else if (type == SH_ROR && amt == 0) {
        i->shtype = SH_RRX;
        i->shimm = 1;
    }
}

/* The condition is kept: a conditional UNDEFINED instruction that fails its condition is skipped
 * (IMPLEMENTATION DEFINED; qemu and many cores do this). The unconditional space has cond = AL already. */
static int undef(insn_t *i)
{
    i->op = OP_UNDEF;
    return 4;
}

static int dp(uint32_t w, insn_t *i, int form)
{
    unsigned opc = BITS(w, 24, 21);
    i->op = OP_DP;
    i->sub = opc;
    i->s = BIT(w, 20);
    i->rn = BITS(w, 19, 16);
    i->rd = BITS(w, 15, 12);
    if (form == F_IMM) {
        unsigned rot = BITS(w, 11, 8) * 2;
        i->form = F_IMM;
        i->imm = ror32(BITS(w, 7, 0), rot);
        i->immcarry = rot != 0;
    } else if (form == F_REG) {
        shift_imm(i, w);
    } else {
        i->form = F_RSR;
        i->rm = BITS(w, 3, 0);
        i->rs = BITS(w, 11, 8);
        i->shtype = BITS(w, 6, 5);
        if (i->rd == 15 || i->rn == 15 || i->rm == 15 || i->rs == 15) i->unpred = 1;
    }
    if (opc >= A_TST && opc <= A_CMN) {
        /* S must be 1 (else it is a misc/halfword-multiply encoding, handled by the caller) */
        if (i->rd != 0) i->unpred = 1; /* Rd should be zero */
        i->rd = 0;
    } else if (i->rd == 15) {
        i->writes_pc = 1;
        if (i->s) i->unpred = 1; /* SUBS PC, LR and friends: exception return, not in user mode */
    }
    if (opc == A_MOV || opc == A_MVN) {
        if (i->rn != 0) i->unpred = 1; /* Rn should be zero */
        i->rn = 0;
    }
    return 4;
}

/* Miscellaneous instructions (op 10xx0, bit 7 = 0) */
static int misc(uint32_t w, insn_t *i)
{
    unsigned op = BITS(w, 22, 21), op2 = BITS(w, 6, 4);
    switch (op2) {
    case 0:
        if (BIT(w, 9)) { /* banked register MRS/MSR: UNPREDICTABLE in user mode */
            i->unpred = 1;
            return undef(i);
        }
        if (!(op & 1)) { /* MRS */
            if (BIT(w, 22)) return undef(i); /* SPSR */
            i->op = OP_MRS;
            i->rd = BITS(w, 15, 12);
            if (i->rd == 15 || BITS(w, 19, 16) != 15 || BITS(w, 11, 0) != 0) i->unpred = 1;
            return 4;
        }
        if (op == 1) { /* MSR APSR, Rn */
            i->op = OP_MSR;
            i->imm = BITS(w, 19, 18) << 2;
            i->rn = BITS(w, 3, 0);
            i->form = F_REG;
            if (i->rn == 15 || i->imm == 0 || BITS(w, 15, 12) != 15 || BITS(w, 11, 4) != 0) i->unpred = 1;
            if (BITS(w, 17, 16)) i->imm |= 0x100; /* system fields: ignored in user mode */
            return 4;
        }
        return undef(i); /* MSR SPSR */
    case 1:
        if (op == 1) {
            i->op = OP_BX;
            i->rm = BITS(w, 3, 0);
            i->writes_pc = 1;
            if (BITS(w, 19, 8) != 0xfff) i->unpred = 1;
            return 4;
        }
        if (op == 3) {
            i->op = OP_CLZ;
            i->rd = BITS(w, 15, 12);
            i->rm = BITS(w, 3, 0);
            if (i->rd == 15 || i->rm == 15 || BITS(w, 19, 16) != 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
            return 4;
        }
        return undef(i);
    case 2:
        if (op == 1) { /* BXJ: no Jazelle, behaves as BX */
            i->op = OP_BX;
            i->rm = BITS(w, 3, 0);
            i->writes_pc = 1;
            if (BITS(w, 19, 8) != 0xfff) i->unpred = 1;
            return 4;
        }
        return undef(i);
    case 3:
        if (op == 1) {
            i->op = OP_BLX_REG;
            i->rm = BITS(w, 3, 0);
            if (i->rm == 15 || BITS(w, 19, 8) != 0xfff) i->unpred = 1;
            i->writes_pc = 1;
            return 4;
        }
        return undef(i);
    case 4: /* CRC32 (ARMv8) */
        i->op = OP_CRC32;
        i->size = 1u << op;
        if (BIT(w, 9)) i->flags |= MF_CRC32C;
        i->rn = BITS(w, 19, 16);
        i->rd = BITS(w, 15, 12);
        i->rm = BITS(w, 3, 0);
        if (op == 3 || i->cond != C_AL || BITS(w, 11, 10) || BIT(w, 8)) i->unpred = 1;
        if (op == 3) return undef(i);
        return 4;
    case 5:
        i->op = OP_QADDSUB;
        i->sub = op;
        i->rn = BITS(w, 19, 16);
        i->rd = BITS(w, 15, 12);
        i->rm = BITS(w, 3, 0);
        if (i->rd == 15 || i->rn == 15 || i->rm == 15 || BITS(w, 11, 8)) i->unpred = 1;
        return 4;
    case 7:
        if (op == 1) {
            if (i->cond != C_AL) i->unpred = 1;
            i->op = OP_BKPT;
            i->imm = BITS(w, 19, 8) << 4 | BITS(w, 3, 0);
            return 4;
        }
        return undef(i); /* SMC, HVC */
    default:
        return undef(i);
    }
}

static int halfword_mul(uint32_t w, insn_t *i)
{
    unsigned op1 = BITS(w, 22, 21);
    i->op = OP_MULXY;
    i->rd = BITS(w, 19, 16);
    i->ra = BITS(w, 15, 12);
    i->rm = BITS(w, 11, 8);
    i->rn = BITS(w, 3, 0);
    if (BIT(w, 5)) i->flags |= MF_NHI;
    if (BIT(w, 6)) i->flags |= MF_MHI;
    switch (op1) {
    case 0: i->sub = MX_SMLA; break;
    case 1:
        /* SMLAWy (bit 5 = 0) / SMULWy (bit 5 = 1); y is bit 6 */
        i->sub = BIT(w, 5) ? MX_SMULW : MX_SMLAW;
        i->flags &= ~MF_NHI;
        break;
    case 2: i->sub = MX_SMLAL; break; /* ra = RdLo, rd = RdHi */
    case 3: i->sub = MX_SMUL; break;
    }
    if (i->rd == 15 || i->rn == 15 || i->rm == 15 || (i->sub != MX_SMUL && i->sub != MX_SMULW && i->ra == 15))
        i->unpred = 1;
    if (i->sub == MX_SMLAL && i->ra == i->rd) i->unpred = 1;
    if ((i->sub == MX_SMUL || i->sub == MX_SMULW) && i->ra != 0) i->unpred = 1; /* Ra should be zero */
    return 4;
}

static int multiply(uint32_t w, insn_t *i)
{
    static const signed char ops[8] = {M_MUL, M_MLA, M_UMAAL, M_MLS, M_UMULL, M_UMLAL, M_SMULL, M_SMLAL};
    unsigned op = BITS(w, 23, 21);
    i->op = OP_MUL;
    i->sub = ops[op];
    i->s = BIT(w, 20);
    i->rd = BITS(w, 19, 16);  /* Rd or RdHi */
    i->ra = BITS(w, 15, 12);  /* Ra or RdLo */
    i->rm = BITS(w, 11, 8);
    i->rn = BITS(w, 3, 0);
    if ((op == 2 || op == 3) && i->s) return undef(i);  /* 0101 / 0111 unallocated */
    if (i->rd == 15 || i->rn == 15 || i->rm == 15) i->unpred = 1;
    if (op == 0 && i->ra != 0) i->unpred = 1; /* MUL: Ra should be zero */
    if (op >= 4 || op == 2) {
        /* long multiplies: rd = RdHi, ra = RdLo */
        if (i->ra == 15 || i->ra == i->rd) i->unpred = 1;
    } else if ((op == 1 || op == 3) && i->ra == 15) {
        i->unpred = 1;
    }
    return 4;
}

static int sync_prim(uint32_t w, insn_t *i)
{
    unsigned op = BITS(w, 23, 20);
    i->rn = BITS(w, 19, 16);
    if (!(op & 8)) {
        /* SWP / SWPB */
        if (op & 3) return undef(i);
        i->op = OP_SWP;
        i->size = BIT(w, 22) ? 1 : 4;
        i->rd = BITS(w, 15, 12);
        i->rm = BITS(w, 3, 0);
        if (i->rd == 15 || i->rn == 15 || i->rm == 15 || i->rn == i->rd || i->rn == i->rm || BITS(w, 11, 8))
            i->unpred = 1;
        return 4;
    }
    static const uint8_t sizes[4] = {4, 8, 1, 2};
    unsigned l = op & 1, sz = sizes[BITS(op, 2, 1)];
    unsigned ex = BITS(w, 9, 8); /* 11 exclusive (v7 and v8), 10 acquire/release exclusive, 00 acquire/release */
    i->size = sz;
    if (ex == 1) return undef(i);
    if (ex != 3) i->flags |= MF_ACQREL;
    if (ex == 0) {
        /* LDA/STL (no exclusive), only words, bytes and halfwords */
        if (sz == 8) return undef(i);
        if (l) {
            i->op = OP_LDA;
            i->rd = BITS(w, 15, 12);
            if (BITS(w, 3, 0) != 15) i->unpred = 1;
        } else {
            i->op = OP_STL;
            i->rm = BITS(w, 3, 0);
            if (BITS(w, 15, 12) != 15) i->unpred = 1;
        }
        return 4;
    }
    if (l) {
        i->op = OP_LDREX;
        i->rd = BITS(w, 15, 12);
        if (BITS(w, 3, 0) != 15) i->unpred = 1;
        if (sz == 8) {
            i->ra = i->rd + 1;
            if ((i->rd & 1) || i->rd == 14) i->unpred = 1;
        }
        if (i->rd == 15 || i->rn == 15) i->unpred = 1;
    } else {
        i->op = OP_STREX;
        i->rd = BITS(w, 15, 12);
        i->rm = BITS(w, 3, 0);
        if (sz == 8) {
            i->ra = i->rm + 1;
            if ((i->rm & 1) || i->rm == 14) i->unpred = 1;
        }
        if (i->rd == 15 || i->rm == 15 || i->rn == 15 || i->rd == i->rn || i->rd == i->rm ||
            (sz == 8 && i->rd == i->ra))
            i->unpred = 1;
    }
    return 4;
}

/* LDRH/STRH/LDRD/STRD/LDRSB/LDRSH */
static int extra_ldst(uint32_t w, insn_t *i)
{
    unsigned op2 = BITS(w, 6, 5), l = BIT(w, 20);
    i->op = OP_LDST;
    i->p = BIT(w, 24);
    i->u = BIT(w, 23);
    i->w = !i->p || BIT(w, 21);
    if (!i->p && BIT(w, 21)) i->flags |= MF_USER;
    i->rn = BITS(w, 19, 16);
    i->rd = BITS(w, 15, 12);
    if (BIT(w, 22)) {
        i->form = F_IMM;
        i->imm = BITS(w, 11, 8) << 4 | BITS(w, 3, 0);
    } else {
        i->form = F_REG;
        i->rm = BITS(w, 3, 0);
        i->shtype = SH_LSL;
        i->shimm = 0;
        if (BITS(w, 11, 8)) i->unpred = 1;
    }
    if (op2 == 1) {
        i->size = 2;
        if (l) i->flags |= MF_LOAD;
    } else if (!l) {
        /* LDRD (op2 = 10) / STRD (op2 = 11) */
        i->size = 8;
        if (op2 == 2) i->flags |= MF_LOAD;
        if (i->flags & MF_USER) return undef(i);
        if (i->rd & 1) i->unpred = 1;
        i->ra = i->rd + 1;
        if (i->ra == 15) i->unpred = 1;
        if (i->form == F_REG && (i->rm == i->rd || i->rm == i->ra) && (i->flags & MF_LOAD)) i->unpred = 1;
        if (i->w && (i->rn == i->rd || i->rn == i->ra)) i->unpred = 1;
    } else {
        i->size = op2 == 2 ? 1 : 2;
        i->sign = 1;
        i->flags |= MF_LOAD;
    }
    if (i->rd == 15) i->unpred = 1;
    if (i->form == F_REG && i->rm == 15) i->unpred = 1;
    if (i->w && (i->rn == 15 || i->rn == i->rd)) i->unpred = 1;
    if (i->w && i->form == F_REG && i->rm == i->rn) i->unpred = 1;
    return 4;
}

static int dp_misc(uint32_t w, insn_t *i)
{
    unsigned op = BITS(w, 24, 20);
    if (BIT(w, 25)) {
        if (op == 0x10 || op == 0x14) {
            i->op = op == 0x10 ? OP_MOVW : OP_MOVT;
            i->rd = BITS(w, 15, 12);
            i->imm = BITS(w, 19, 16) << 12 | BITS(w, 11, 0);
            if (i->rd == 15) i->unpred = 1;
            return 4;
        }
        if ((op & 0x1b) == 0x12) {
            /* MSR immediate and hints */
            unsigned mask = BITS(w, 19, 16);
            if (op == 0x12 && mask == 0) {
                i->op = OP_NOP; /* NOP, YIELD, WFE, WFI, SEV, CSDB, DBG */
                if (BITS(w, 15, 8) != 0xf0) i->unpred = 1;
                return 4;
            }
            if (op & 4) {
                i->unpred = 1; /* MSR SPSR: UNPREDICTABLE in user mode */
                return undef(i);
            }
            i->op = OP_MSR;
            i->imm = BITS(w, 19, 18) << 2;
            if (mask & 3) i->imm |= 0x100;
            if (BITS(w, 15, 12) != 15 || !(mask & 0xc)) i->unpred = 1;
            i->form = F_IMM;
            i->reglist = ror32(BITS(w, 7, 0), BITS(w, 11, 8) * 2);
            return 4;
        }
        if ((op & 0x19) == 0x10) return undef(i); /* TST..CMN without S: unallocated (covered above) */
        return dp(w, i, F_IMM);
    }
    if (!BIT(w, 4)) {
        if ((op & 0x19) == 0x10) return BIT(w, 7) ? halfword_mul(w, i) : misc(w, i);
        return dp(w, i, F_REG);
    }
    if (!BIT(w, 7)) {
        if ((op & 0x19) == 0x10) return misc(w, i);
        return dp(w, i, F_RSR);
    }
    if (BITS(w, 6, 5) == 0) return BIT(w, 24) ? sync_prim(w, i) : multiply(w, i);
    return extra_ldst(w, i);
}

static int ldst_word(uint32_t w, insn_t *i)
{
    i->op = OP_LDST;
    i->p = BIT(w, 24);
    i->u = BIT(w, 23);
    i->w = !i->p || BIT(w, 21);
    if (!i->p && BIT(w, 21)) i->flags |= MF_USER;
    i->size = BIT(w, 22) ? 1 : 4;
    if (BIT(w, 20)) i->flags |= MF_LOAD;
    i->rn = BITS(w, 19, 16);
    i->rd = BITS(w, 15, 12);
    if (BIT(w, 25)) {
        shift_imm(i, w);
        if (i->rm == 15) i->unpred = 1;
        if (i->w && i->rm == i->rn) i->unpred = 1;
    } else {
        i->form = F_IMM;
        i->imm = BITS(w, 11, 0);
    }
    if ((i->flags & MF_LOAD) && i->rd == 15) {
        i->writes_pc = 1;
        if (i->size == 1) i->unpred = 1;
    }
    if (!(i->flags & MF_LOAD) && i->size == 1 && i->rd == 15) i->unpred = 1;
    if (i->w && (i->rn == 15 || i->rn == i->rd)) i->unpred = 1;
    return 4;
}

static int media(uint32_t w, insn_t *i)
{
    unsigned op1 = BITS(w, 24, 20), op2 = BITS(w, 7, 5);
    unsigned rn = BITS(w, 19, 16), rd = BITS(w, 15, 12), rm = BITS(w, 3, 0), ra = BITS(w, 15, 12);
    i->rn = rn;
    i->rd = rd;
    i->rm = rm;
    if ((op1 & 0x18) == 0x00) {
        /* parallel addition and subtraction */
        static const signed char prefix[8] = {-1, PP_S, PP_Q, PP_SH, -1, PP_U, PP_UQ, PP_UH};
        static const signed char pop[8] = {PO_ADD16, PO_ASX, PO_SAX, PO_SUB16, PO_ADD8, -1, -1, PO_SUB8};
        if (prefix[op1 & 7] < 0 || pop[op2] < 0) return undef(i);
        i->op = OP_PARALLEL;
        i->sub = prefix[op1 & 7] << 4 | pop[op2];
        if (rd == 15 || rn == 15 || rm == 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
        return 4;
    }
    if ((op1 & 0x18) == 0x08) {
        unsigned o = op1 & 7;
        if (o == 0 && !(op2 & 1)) { /* PKH */
            i->op = OP_PKH;
            i->sub = BIT(w, 6);
            i->shimm = BITS(w, 11, 7);
            if (i->sub && i->shimm == 0) i->shimm = 32;
            if (rd == 15 || rn == 15 || rm == 15) i->unpred = 1;
            return 4;
        }
        if ((o & 2) && !(op2 & 1)) { /* SSAT / USAT */
            i->op = OP_SAT;
            i->sign = !(o & 4);
            i->imm = BITS(w, 20, 16) + (i->sign ? 1 : 0);
            i->rn = rm;
            i->shtype = BIT(w, 6) ? SH_ASR : SH_LSL;
            i->shimm = BITS(w, 11, 7);
            if (i->shtype == SH_ASR && i->shimm == 0) i->shimm = 32;
            if (rd == 15 || rm == 15) i->unpred = 1;
            return 4;
        }
        if ((o == 2 || o == 6) && op2 == 1) { /* SSAT16 / USAT16 */
            i->op = OP_SAT16;
            i->sign = o == 2;
            i->imm = BITS(w, 19, 16) + (i->sign ? 1 : 0);
            i->rn = rm;
            if (rd == 15 || rm == 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
            return 4;
        }
        if (op2 == 3) { /* extend (and add) */
            static const signed char ext[8] = {E_SXTB16, -1, E_SXTB, E_SXTH, E_UXTB16, -1, E_UXTB, E_UXTH};
            if (ext[o] < 0) return undef(i);
            i->op = OP_EXTEND;
            i->sub = ext[o];
            i->shimm = BITS(w, 11, 10) * 8;
            if (rd == 15 || rm == 15 || BITS(w, 9, 8)) i->unpred = 1;
            return 4;
        }
        if (o == 0 && op2 == 5) {
            i->op = OP_SEL;
            if (rd == 15 || rn == 15 || rm == 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
            return 4;
        }
        if (op2 == 1 && (o == 3 || o == 7)) {
            i->op = o == 3 ? OP_REV : OP_RBIT;
            if (rd == 15 || rm == 15 || rn != 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
            return 4;
        }
        if (op2 == 5 && (o == 3 || o == 7)) {
            i->op = o == 3 ? OP_REV16 : OP_REVSH;
            if (rd == 15 || rm == 15 || rn != 15 || BITS(w, 11, 8) != 15) i->unpred = 1;
            return 4;
        }
        return undef(i);
    }
    if ((op1 & 0x18) == 0x10) {
        /* signed multiplies, divide; rd is bits 19:16 here, ra bits 15:12, rm bits 11:8, rn bits 3:0 */
        unsigned o = op1 & 7;
        i->rd = rn;
        i->ra = ra;
        i->rm = BITS(w, 11, 8);
        i->rn = rm;
        if (BIT(w, 5)) i->flags |= MF_SWAP;
        if (i->rd == 15 || i->rn == 15 || i->rm == 15) i->unpred = 1;
        if (o == 0 && op2 < 4) {
            i->op = OP_DUALMUL;
            i->sub = op2 & 2 ? DM_SMLSD : DM_SMLAD;
            return 4;
        }
        if ((o == 1 || o == 3) && op2 == 0) {
            i->op = OP_DIV;
            i->sign = o == 1;
            if (ra != 15) i->unpred = 1;
            return 4;
        }
        if (o == 4 && op2 < 4) {
            i->op = OP_DUALMUL;
            i->sub = op2 & 2 ? DM_SMLSLD : DM_SMLALD;
            if (i->ra == 15 || i->ra == i->rd) i->unpred = 1; /* ra = RdLo, rd = RdHi */
            return 4;
        }
        if (o == 5 && (op2 < 2 || op2 >= 6)) {
            i->op = OP_SMMUL;
            i->sub = op2 >= 6 ? 2 : (ra == 15 ? 0 : 1);
            if (op2 >= 6 && ra == 15) i->unpred = 1;
            i->flags &= ~MF_SWAP;
            if (BIT(w, 5)) i->flags |= MF_ROUND;
            return 4;
        }
        return undef(i);
    }
    /* op1 = 11xxx */
    if (op1 == 0x18 && op2 == 0) {
        i->op = OP_USAD8;
        i->rd = rn;
        i->ra = ra;
        i->rm = BITS(w, 11, 8);
        i->rn = rm;
        if (i->rd == 15 || i->rn == 15 || i->rm == 15) i->unpred = 1;
        return 4;
    }
    if ((op1 & 0x1e) == 0x1a && (op2 & 3) == 2) {
        i->op = OP_BFX;
        i->sign = 1;
        goto bfx;
    }
    if ((op1 & 0x1e) == 0x1e && (op2 & 3) == 2) {
        i->op = OP_BFX;
        i->sign = 0;
    bfx:
        i->rn = rm;
        i->imm = BITS(w, 11, 7);
        i->shimm = BITS(w, 20, 16) + 1;
        if (rd == 15 || rm == 15 || i->imm + i->shimm > 32) i->unpred = 1;
        return 4;
    }
    if ((op1 & 0x1e) == 0x1c && (op2 & 3) == 0) {
        i->op = OP_BFI;
        i->rn = rm; /* 15 = BFC */
        i->imm = BITS(w, 11, 7);
        i->shimm = BITS(w, 20, 16);
        if (rd == 15 || i->shimm < i->imm) i->unpred = 1;
        return 4;
    }
    if (op1 == 0x1f && op2 == 7) { /* UDF */
        i->op = OP_UNDEF;
        i->cond = C_AL;
        if (BITS(w, 31, 28) != C_AL) i->unpred = 1;
        return 4;
    }
    return undef(i);
}

static int ldm_stm(uint32_t w, insn_t *i)
{
    /* ^ (user registers / exception return) is UNPREDICTABLE in user mode; like qemu and real cores, run it as a
     * plain LDM/STM */
    if (BIT(w, 22)) i->unpred = 1;
    i->op = BIT(w, 20) ? OP_LDM : OP_STM;
    i->p = BIT(w, 24);
    i->u = BIT(w, 23);
    i->w = BIT(w, 21);
    i->rn = BITS(w, 19, 16);
    i->reglist = BITS(w, 15, 0);
    if (i->rn == 15 || i->reglist == 0) i->unpred = 1;
    if (i->op == OP_LDM) {
        if (i->reglist & 0x8000) i->writes_pc = 1;
        if (i->w && (i->reglist & (1u << i->rn))) i->unpred = 1;
    } else {
        /* STM with writeback and the base in the list (not lowest) stores an UNKNOWN value; PC in the list is
         * IMPLEMENTATION DEFINED (PC + 8 here) */
        if (i->w && (i->reglist & (1u << i->rn)) && (i->reglist & ((1u << i->rn) - 1))) i->unpred = 1;
        if (i->reglist & 0x8000) i->unpred = 1;
    }
    /* ARMv7: fewer than 2 registers is fine for ARM encodings except an empty list */
    return 4;
}

static int coproc(uint32_t w, insn_t *i)
{
    unsigned cp = BITS(w, 11, 8);
    if (BITS(w, 27, 24) == 15) {
        i->op = OP_SVC;
        i->imm = BITS(w, 23, 0);
        i->writes_pc = 1;
        return 4;
    }
    if ((cp & 0xe) == 0xa) {
        vfp_neon_decode(w, i);
        return 4;
    }
    if (cp == 15 && BITS(w, 27, 24) == 14 && BIT(w, 4)) {
        /* MRC/MCR p15 */
        unsigned opc1 = BITS(w, 23, 21), crn = BITS(w, 19, 16), crm = BITS(w, 3, 0), opc2 = BITS(w, 7, 5);
        i->rd = BITS(w, 15, 12);
        i->op = BIT(w, 20) ? OP_MRC : OP_MCR;
        if (opc1 == 0 && crn == 13 && crm == 0 && (opc2 == 2 || opc2 == 3)) {
            i->imm = opc2 == 3 ? CP15_TPIDRURO : CP15_TPIDRURW;
            if (i->op == OP_MCR && opc2 == 3) return undef(i);
        } else if (opc1 == 0 && crn == 14 && crm == 0 && opc2 == 0 && i->op == OP_MRC) {
            i->imm = CP15_CNTFRQ;
        } else if (opc1 == 0 && crn == 7 && ((crm == 10 && (opc2 == 4 || opc2 == 5)) || (crm == 5 && opc2 == 4))) {
            /* CP15DSB/CP15DMB/CP15ISB (deprecated barriers, still allowed at EL0) */
            if (i->op != OP_MCR) return undef(i);
            i->op = OP_BARRIER;
            return 4;
        } else {
            return undef(i);
        }
        if (i->rd == 15) {
            if (i->op == OP_MCR) i->unpred = 1;
            else return undef(i); /* APSR_nzcv form: not for these registers */
        }
        return 4;
    }
    if (cp == 15 && BITS(w, 27, 21) == 0x62 && BIT(w, 20)) {
        /* MRRC p15: generic timer counters */
        unsigned opc1 = BITS(w, 7, 4), crm = BITS(w, 3, 0);
        if (crm != 14 || opc1 > 1) return undef(i);
        i->op = OP_MRRC;
        i->rd = BITS(w, 15, 12);
        i->ra = BITS(w, 19, 16);
        i->imm = opc1 ? CP15_CNTVCT : CP15_CNTPCT;
        if (i->rd == 15 || i->ra == 15 || i->rd == i->ra) i->unpred = 1;
        return 4;
    }
    return undef(i);
}

static int uncond(uint32_t w, insn_t *i)
{
    i->cond = C_AL;
    if ((w & 0x0e000000) == 0x02000000) { /* Advanced SIMD data processing */
        vfp_neon_decode(w, i);
        return 4;
    }
    if ((w & 0x0f100000) == 0x04000000) { /* Advanced SIMD element or structure load/store */
        vfp_neon_decode(w, i);
        return 4;
    }
    if ((w & 0x0e000000) == 0x0a000000) {
        i->op = OP_BLX_IMM;
        int32_t off = (int32_t)(w << 8) >> 6;
        i->imm = i->pc + 8 + off + (BIT(w, 24) << 1);
        i->writes_pc = 1;
        return 4;
    }
    if ((w & 0xfffffdff) == 0xf1010000) {
        i->op = OP_SETEND;
        i->imm = BIT(w, 9);
        return 4;
    }
    if (w == 0xf57ff01f) {
        i->op = OP_CLREX;
        return 4;
    }
    if ((w & 0xffffff00) == 0xf57ff000 && BITS(w, 7, 4) >= 4 && BITS(w, 7, 4) <= 6) {
        i->op = OP_BARRIER;
        return 4;
    }
    if ((w & 0x0f700000) == 0x04500000 || (w & 0x0f300000) == 0x05100000 || (w & 0x0f300010) == 0x07100000 ||
        (w & 0x0f700010) == 0x06500000) {
        i->op = OP_NOP; /* PLI / PLD / PLDW */
        return 4;
    }
    if ((w & 0x0e000000) == 0x0c000000 || (w & 0x0f000000) == 0x0e000000) {
        /* unconditional coprocessor space: VSEL, VMAXNM, VRINT{A,N,P,M}, VCVT{A,N,P,M} use cp 10/11 */
        if ((BITS(w, 11, 8) & 0xe) == 0xa && (w & 0x0f000010) == 0x0e000000) {
            vfp_neon_decode(w, i);
            return 4;
        }
    }
    return undef(i);
}

int a32_decode(uint32_t w, uint32_t pc, insn_t *i)
{
    memset(i, 0, sizeof *i);
    i->pc = pc;
    i->pcval = pc + 8;
    i->raw = w;
    i->len = 4;
    i->cond = w >> 28;
    if (i->cond == C_NV) return uncond(w, i);
    switch (BITS(w, 27, 25)) {
    case 0:
    case 1:
        return dp_misc(w, i);
    case 2:
        return ldst_word(w, i);
    case 3:
        return BIT(w, 4) ? media(w, i) : ldst_word(w, i);
    case 4:
        return ldm_stm(w, i);
    case 5: {
        int32_t off = (int32_t)(w << 8) >> 6;
        i->op = BIT(w, 24) ? OP_BL : OP_B;
        i->imm = pc + 8 + off;
        i->writes_pc = 1;
        return 4;
    }
    default:
        return coproc(w, i);
    }
}
