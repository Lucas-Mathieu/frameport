/* T32 (Thumb/Thumb-2) decoder, ARMv7-A ARM chapter A6 plus ARMv8 AArch32 additions. Produces the same insn_t as
 * the A32 decoder; coprocessor/VFP/NEON encodings are rewritten to A32 form and decoded there. */
#include <string.h>
#include "insn.h"

int t32_insn_len(uint16_t hw1)
{
    unsigned top = hw1 >> 11;
    return (top == 0x1d || top == 0x1e || top == 0x1f) ? 4 : 2;
}

static int undef(insn_t *i)
{
    i->op = OP_UNDEF;
    return i->len;
}

static void dp_set(insn_t *i, unsigned alu, unsigned rd, unsigned rn, int s)
{
    i->op = OP_DP;
    i->sub = alu;
    i->rd = rd;
    i->rn = rn;
    i->s = s;
}

static void reg_shift(insn_t *i, unsigned rm, unsigned type, unsigned amt)
{
    i->form = F_REG;
    i->rm = rm;
    i->shtype = type;
    i->shimm = amt;
    if (type == SH_LSR || type == SH_ASR) {
        if (amt == 0) i->shimm = 32;
    } else if (type == SH_ROR && amt == 0) {
        i->shtype = SH_RRX;
        i->shimm = 1;
    }
}

/* ---------------------------------------------------------------- 16-bit */

static int t16(uint16_t h, insn_t *i, int in_it)
{
    unsigned rd = h & 7, rn = (h >> 3) & 7, rm = (h >> 6) & 7;
    int setflags = !in_it;
    switch (h >> 10) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: {
        unsigned opc = BITS(h, 13, 11);
        if (opc < 3) { /* LSL/LSR/ASR imm (LSL #0 = MOVS Rd, Rm) */
            dp_set(i, A_MOV, rd, 0, setflags);
            reg_shift(i, rn, opc, BITS(h, 10, 6));
            return 2;
        }
        /* 00011: ADD/SUB register / 3-bit immediate */
        unsigned sub = BIT(h, 9);
        dp_set(i, sub ? A_SUB : A_ADD, rd, rn, setflags);
        if (BIT(h, 10)) {
            i->form = F_IMM;
            i->imm = rm;
        } else {
            reg_shift(i, rm, SH_LSL, 0);
        }
        return 2;
    }
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
        static const uint8_t alu[4] = {A_MOV, A_CMP, A_ADD, A_SUB};
        unsigned opc = BITS(h, 12, 11), r = BITS(h, 10, 8);
        dp_set(i, alu[opc], opc == 1 ? 0 : r, opc == 0 ? 0 : r, opc == 1 ? 1 : setflags);
        i->form = F_IMM;
        i->imm = h & 0xff;
        return 2;
    }
    case 0x10: { /* data processing */
        unsigned opc = BITS(h, 9, 6);
        switch (opc) {
        case 2: case 3: case 4: case 7: { /* LSL/LSR/ASR/ROR by register */
            static const uint8_t t[8] = {0, 0, SH_LSL, SH_LSR, SH_ASR, 0, 0, SH_ROR};
            dp_set(i, A_MOV, rd, 0, setflags);
            i->form = F_RSR;
            i->rm = rd;
            i->rs = rn;
            i->shtype = t[opc];
            return 2;
        }
        case 9: /* RSB Rd, Rn, #0 */
            dp_set(i, A_RSB, rd, rn, setflags);
            i->form = F_IMM;
            i->imm = 0;
            return 2;
        case 13: /* MUL Rdm, Rn, Rdm */
            i->op = OP_MUL;
            i->sub = M_MUL;
            i->s = setflags;
            i->rd = rd;
            i->rn = rn;
            i->rm = rd;
            return 2;
        default: {
            static const uint8_t alu[16] = {A_AND, A_EOR, 0, 0, 0, A_ADC, A_SBC, 0, A_TST, 0, A_CMP, A_CMN, A_ORR, 0,
                                            A_BIC, A_MVN};
            unsigned a = alu[opc];
            int test = a == A_TST || a == A_CMP || a == A_CMN;
            dp_set(i, a, test ? 0 : rd, a == A_MVN ? 0 : rd, test ? 1 : setflags);
            reg_shift(i, rn, SH_LSL, 0);
            return 2;
        }
        }
    }
    case 0x11: { /* special data processing and branch/exchange */
        unsigned opc = BITS(h, 9, 8);
        unsigned d = BIT(h, 7) << 3 | rd, m = BITS(h, 6, 3);
        if (opc == 0) { /* ADD Rdn, Rm (high registers) */
            dp_set(i, A_ADD, d, d, 0);
            reg_shift(i, m, SH_LSL, 0);
            if (d == 15) {
                i->writes_pc = 1;
                if (m == 15) i->unpred = 1;
            }
            return 2;
        }
        if (opc == 1) { /* CMP high */
            dp_set(i, A_CMP, 0, d, 1);
            reg_shift(i, m, SH_LSL, 0);
            if ((d < 8 && m < 8) || d == 15 || m == 15) i->unpred = 1;
            return 2;
        }
        if (opc == 2) { /* MOV high */
            dp_set(i, A_MOV, d, 0, 0);
            reg_shift(i, m, SH_LSL, 0);
            if (d == 15) i->writes_pc = 1;
            return 2;
        }
        i->op = BIT(h, 7) ? OP_BLX_REG : OP_BX;
        i->rm = m;
        i->writes_pc = 1;
        if (h & 7) i->unpred = 1;
        if (i->op == OP_BLX_REG && m == 15) i->unpred = 1;
        return 2;
    }
    case 0x12: case 0x13: /* LDR literal */
        i->op = OP_LDST;
        i->flags = MF_LOAD;
        i->size = 4;
        i->rd = BITS(h, 10, 8);
        i->rn = 15;
        i->form = F_IMM;
        i->imm = (h & 0xff) << 2;
        i->p = 1;
        i->u = 1;
        i->pcval = (i->pc + 4) & ~3u;
        return 2;
    case 0x14: case 0x15: case 0x16: case 0x17: { /* load/store register offset */
        static const uint8_t size[8] = {4, 2, 1, 1, 4, 2, 1, 2};
        unsigned opb = BITS(h, 11, 9);
        i->op = OP_LDST;
        i->size = size[opb];
        if (opb >= 3) i->flags = MF_LOAD;
        i->sign = opb == 3 || opb == 7;
        i->rd = rd;
        i->rn = rn;
        reg_shift(i, rm, SH_LSL, 0);
        i->p = 1;
        i->u = 1;
        return 2;
    }
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
    case 0x20: case 0x21: case 0x22: case 0x23: { /* load/store immediate */
        unsigned opa = BITS(h, 15, 11), imm5 = BITS(h, 10, 6);
        i->op = OP_LDST;
        i->rd = rd;
        i->rn = rn;
        i->form = F_IMM;
        i->p = 1;
        i->u = 1;
        if (BIT(h, 11)) i->flags = MF_LOAD;
        if (opa == 0x0c || opa == 0x0d) {
            i->size = 4;
            i->imm = imm5 << 2;
        } else if (opa == 0x0e || opa == 0x0f) {
            i->size = 1;
            i->imm = imm5;
        } else {
            i->size = 2;
            i->imm = imm5 << 1;
        }
        return 2;
    }
    case 0x24: case 0x25: case 0x26: case 0x27: /* LDR/STR SP-relative */
        i->op = OP_LDST;
        if (BIT(h, 11)) i->flags = MF_LOAD;
        i->size = 4;
        i->rd = BITS(h, 10, 8);
        i->rn = 13;
        i->form = F_IMM;
        i->imm = (h & 0xff) << 2;
        i->p = 1;
        i->u = 1;
        return 2;
    case 0x28: case 0x29: /* ADR */
        dp_set(i, A_ADD, BITS(h, 10, 8), 15, 0);
        i->form = F_IMM;
        i->imm = (h & 0xff) << 2;
        i->pcval = (i->pc + 4) & ~3u;
        return 2;
    case 0x2a: case 0x2b: /* ADD Rd, SP, #imm */
        dp_set(i, A_ADD, BITS(h, 10, 8), 13, 0);
        i->form = F_IMM;
        i->imm = (h & 0xff) << 2;
        return 2;
    case 0x2c: case 0x2d: case 0x2e: case 0x2f: { /* miscellaneous 16-bit */
        unsigned opc = BITS(h, 11, 5);
        if ((opc & 0x7c) == 0x00) { /* ADD SP, SP, #imm7 */
            dp_set(i, A_ADD, 13, 13, 0);
            i->form = F_IMM;
            i->imm = (h & 0x7f) << 2;
            return 2;
        }
        if ((opc & 0x7c) == 0x04) {
            dp_set(i, A_SUB, 13, 13, 0);
            i->form = F_IMM;
            i->imm = (h & 0x7f) << 2;
            return 2;
        }
        if ((opc & 0x28) == 0x08) { /* CBZ/CBNZ: 1011 o0i1 */
            i->op = OP_CBZ;
            i->rn = rd;
            i->imm = i->pc + 4 + ((BIT(h, 9) << 6) | (BITS(h, 7, 3) << 1));
            if (BIT(h, 11)) i->flags = MF_NONZERO;
            i->writes_pc = 1;
            if (in_it) i->unpred = 1;
            return 2;
        }
        if ((opc & 0x78) == 0x10) { /* SXTH SXTB UXTH UXTB */
            static const uint8_t e[4] = {E_SXTH, E_SXTB, E_UXTH, E_UXTB};
            i->op = OP_EXTEND;
            i->sub = e[BITS(h, 7, 6)];
            i->rd = rd;
            i->rm = rn;
            i->rn = 15;
            return 2;
        }
        if ((opc & 0x70) == 0x20) { /* PUSH */
            i->op = OP_STM;
            i->rn = 13;
            i->p = 1;
            i->u = 0;
            i->w = 1;
            i->reglist = (h & 0xff) | (BIT(h, 8) << 14);
            if (!i->reglist) i->unpred = 1;
            return 2;
        }
        if ((opc & 0x70) == 0x60) { /* POP */
            i->op = OP_LDM;
            i->rn = 13;
            i->p = 0;
            i->u = 1;
            i->w = 1;
            i->reglist = (h & 0xff) | (BIT(h, 8) << 15);
            if (BIT(h, 8)) i->writes_pc = 1;
            if (!i->reglist) i->unpred = 1;
            return 2;
        }
        if (opc == 0x32) { /* SETEND */
            i->op = OP_SETEND;
            i->imm = BIT(h, 3);
            if (in_it) i->unpred = 1;
            return 2;
        }
        if (opc == 0x33) return undef(i); /* CPS: privileged (NOP in user mode on real cores; UNDEF is safer) */
        if ((opc & 0x7e) == 0x50 || (opc & 0x7e) == 0x52 || (opc & 0x7e) == 0x56) {
            unsigned k = BITS(h, 7, 6);
            i->op = k == 0 ? OP_REV : k == 1 ? OP_REV16 : OP_REVSH;
            i->rd = rd;
            i->rm = rn;
            return 2;
        }
        if ((opc & 0x78) == 0x70) {
            i->op = OP_BKPT;
            i->imm = h & 0xff;
            i->cond = C_AL;
            return 2;
        }
        if ((opc & 0x78) == 0x78) {
            if (h & 0xf) { /* IT */
                i->op = OP_IT;
                i->imm = h & 0xff;
                unsigned first = BITS(h, 7, 4), mask = h & 0xf;
                if (first == 15 || (first == 14 && __builtin_popcount(mask) != 1) || in_it) i->unpred = 1;
                i->cond = C_AL;
                return 2;
            }
            i->op = OP_NOP; /* NOP, YIELD, WFE, WFI, SEV */
            return 2;
        }
        return undef(i);
    }
    case 0x30: case 0x31: /* STM */
    case 0x32: case 0x33: { /* LDM */
        unsigned n = BITS(h, 10, 8), list = h & 0xff;
        i->op = BIT(h, 11) ? OP_LDM : OP_STM;
        i->rn = n;
        i->p = 0;
        i->u = 1;
        i->reglist = list;
        i->w = i->op == OP_STM || !(list & (1u << n));
        if (!list) i->unpred = 1;
        if (i->op == OP_STM && (list & (1u << n)) && (list & ((1u << n) - 1))) i->unpred = 1;
        return 2;
    }
    case 0x34: case 0x35: case 0x36: case 0x37: { /* conditional branch, UDF, SVC */
        unsigned c = BITS(h, 11, 8);
        if (c == 14) {
            i->op = OP_UNDEF;
            i->cond = C_AL;
            return 2;
        }
        if (c == 15) {
            i->op = OP_SVC;
            i->imm = h & 0xff;
            i->writes_pc = 1;
            return 2;
        }
        i->op = OP_B;
        i->cond = c;
        i->imm = i->pc + 4 + ((int32_t)(int8_t)(h & 0xff) << 1);
        i->writes_pc = 1;
        if (in_it) i->unpred = 1;
        return 2;
    }
    case 0x38: case 0x39: /* B */
        i->op = OP_B;
        i->imm = i->pc + 4 + (((int32_t)(h << 21)) >> 20);
        i->writes_pc = 1;
        return 2;
    }
    return undef(i);
}

/* ---------------------------------------------------------------- 32-bit */

static uint32_t thumb_expand_imm(uint32_t imm12, int *carry, int *unpred)
{
    *carry = 0;
    if (BITS(imm12, 11, 10) == 0) {
        uint32_t b = imm12 & 0xff;
        switch (BITS(imm12, 9, 8)) {
        case 0: return b;
        case 1: if (!b) *unpred = 1; return b << 16 | b;
        case 2: if (!b) *unpred = 1; return b << 24 | b << 8;
        default: if (!b) *unpred = 1; return b * 0x01010101u;
        }
    }
    *carry = 1;
    return ror32(0x80 | (imm12 & 0x7f), BITS(imm12, 11, 7));
}

/* opcode field of DP (shifted register) and DP (modified immediate) -> alu op; -1 = undefined */
static int t32_alu(unsigned op, unsigned rn, unsigned rd, int s)
{
    switch (op) {
    case 0: return (rd == 15 && s) ? A_TST : A_AND;
    case 1: return A_BIC;
    case 2: return rn == 15 ? A_MOV : A_ORR;
    case 3: return rn == 15 ? A_MVN : A_ORN;
    case 4: return (rd == 15 && s) ? A_TEQ : A_EOR;
    case 8: return (rd == 15 && s) ? A_CMN : A_ADD;
    case 10: return A_ADC;
    case 11: return A_SBC;
    case 13: return (rd == 15 && s) ? A_CMP : A_SUB;
    case 14: return A_RSB;
    default: return -1;
    }
}

static void t32_dp_checks(insn_t *i)
{
    unsigned a = i->sub;
    int test = a == A_TST || a == A_TEQ || a == A_CMP || a == A_CMN;
    if (test) {
        i->rd = 0;
    } else if (i->rd == 15) {
        i->unpred = 1;
        i->writes_pc = 1;
    } else if (i->rd == 13 && !((a == A_ADD || a == A_SUB) && i->rn == 13) && !(a == A_MOV && i->form == F_REG)) {
        i->unpred = 1;
    }
    if (i->form != F_IMM && (i->rm == 15 || (i->rm == 13 && !(a == A_MOV && i->rd != 13)))) {
        if (!(i->rm == 13 && (a == A_ADD || a == A_SUB || a == A_CMP) )) i->unpred = 1;
    }
    if (i->rn == 15 && a != A_MOV && a != A_MVN) i->unpred = 1;
    if (a == A_MOV || a == A_MVN) i->rn = 0;
}

static int ldst_multiple(uint32_t t, insn_t *i)
{
    unsigned op = BITS(t, 24, 23), l = BIT(t, 20);
    if (op == 0 || op == 3) return undef(i); /* SRS / RFE */
    i->op = l ? OP_LDM : OP_STM;
    i->rn = BITS(t, 19, 16);
    i->w = BIT(t, 21);
    i->p = op == 2;
    i->u = op == 1;
    i->reglist = t & 0xffff;
    if (i->rn == 15 || __builtin_popcount(i->reglist) < 2) i->unpred = 1;
    if (i->reglist & (1u << 13)) i->unpred = 1;
    if (l) {
        if ((i->reglist & 0xc000) == 0xc000) i->unpred = 1;
        if (i->reglist & 0x8000) i->writes_pc = 1;
        if (i->w && (i->reglist & (1u << i->rn))) i->unpred = 1;
    } else {
        if (i->reglist & 0x8000) i->unpred = 1;
        if (i->w && (i->reglist & (1u << i->rn))) i->unpred = 1;
    }
    return 4;
}

static int ldst_dual_excl(uint32_t t, insn_t *i)
{
    unsigned op1 = BITS(t, 24, 23), op2 = BITS(t, 21, 20), op3 = BITS(t, 7, 4);
    unsigned rn = BITS(t, 19, 16), rt = BITS(t, 15, 12), rt2 = BITS(t, 11, 8), imm8 = t & 0xff;
    i->rn = rn;
    if ((op1 & 2) || (op2 & 2)) {
        /* LDRD / STRD (immediate, literal) */
        i->op = OP_LDST;
        i->size = 8;
        i->p = BIT(t, 24);
        i->u = BIT(t, 23);
        i->w = BIT(t, 21);
        if (BIT(t, 20)) i->flags = MF_LOAD;
        i->rd = rt;
        i->ra = rt2;
        i->form = F_IMM;
        i->imm = imm8 << 2;
        if (rn == 15) {
            if (!(i->flags & MF_LOAD) || i->w) i->unpred = 1;
            i->pcval = (i->pc + 4) & ~3u;
        }
        if (i->w && (rn == rt || rn == rt2)) i->unpred = 1;
        if (rt == 13 || rt == 15 || rt2 == 13 || rt2 == 15) i->unpred = 1;
        if ((i->flags & MF_LOAD) && rt == rt2) i->unpred = 1;
        return 4;
    }
    if (op1 == 0) {
        if (op2 == 0) { /* STREX */
            i->op = OP_STREX;
            i->size = 4;
            i->rd = rt2;
            i->rm = rt;
            i->imm = imm8 << 2;
            if (rt2 == 13 || rt2 == 15 || rt == 13 || rt == 15 || rn == 15 || rt2 == rn || rt2 == rt) i->unpred = 1;
        } else { /* LDREX */
            i->op = OP_LDREX;
            i->size = 4;
            i->rd = rt;
            i->imm = imm8 << 2;
            if (rt == 13 || rt == 15 || rn == 15 || rt2 != 15) i->unpred = 1;
        }
        return 4;
    }
    /* op1 == 01 */
    if (op2 == 1 && op3 < 2) { /* TBB/TBH */
        i->op = OP_TB;
        i->rm = t & 0xf;
        i->size = op3 ? 2 : 1;
        i->writes_pc = 1;
        if (rn == 13 || i->rm == 13 || i->rm == 15) i->unpred = 1;
        return 4;
    }
    static const uint8_t sizes[4] = {1, 2, 4, 8};
    unsigned sz = sizes[op3 & 3];
    if (op3 < 4 || op3 == 6 || op3 == 11) return undef(i);
    i->size = sz;
    if (op3 >= 8) i->flags |= MF_ACQREL;
    int excl = (op3 & 4) != 0;
    if (op2 == 1) {
        if (!excl) {
            i->op = OP_LDA;
            i->rd = rt;
            return 4;
        }
        i->op = OP_LDREX;
        i->rd = rt;
        if (sz == 8) i->ra = rt2;
        if (rt == 13 || rt == 15 || rn == 15 || (sz == 8 ? (rt2 == 13 || rt2 == 15 || rt == rt2) : 0)) i->unpred = 1;
        if (sz != 8 && (t & 0xf) != 15) i->unpred = 1;
    } else {
        unsigned rd = t & 0xf;
        if (!excl) {
            i->op = OP_STL;
            i->rm = rt;
            return 4;
        }
        i->op = OP_STREX;
        i->rd = rd;
        i->rm = rt;
        if (sz == 8) i->ra = rt2;
        if (rd == 13 || rd == 15 || rt == 13 || rt == 15 || rn == 15 || rd == rn || rd == rt ||
            (sz == 8 && (rt2 == 13 || rt2 == 15 || rd == rt2)))
            i->unpred = 1;
    }
    return 4;
}

static int dp_shifted(uint32_t t, insn_t *i)
{
    unsigned op = BITS(t, 24, 21), s = BIT(t, 20), rn = BITS(t, 19, 16), rd = BITS(t, 11, 8);
    unsigned amt = BITS(t, 14, 12) << 2 | BITS(t, 7, 6), type = BITS(t, 5, 4), rm = t & 0xf;
    if (op == 6) { /* PKHBT / PKHTB */
        if (s || BIT(t, 4)) return undef(i);
        i->op = OP_PKH;
        i->sub = BIT(t, 5);
        i->rd = rd;
        i->rn = rn;
        i->rm = rm;
        i->shimm = amt;
        if (i->sub && amt == 0) i->shimm = 32;
        if (rd == 13 || rd == 15 || rn == 13 || rn == 15 || rm == 13 || rm == 15) i->unpred = 1;
        return 4;
    }
    int a = t32_alu(op, rn, rd, s);
    if (a < 0) return undef(i);
    dp_set(i, a, rd, rn, s);
    reg_shift(i, rm, type, amt);
    t32_dp_checks(i);
    return 4;
}

static int dp_modimm(uint32_t t, insn_t *i)
{
    unsigned op = BITS(t, 24, 21), s = BIT(t, 20), rn = BITS(t, 19, 16), rd = BITS(t, 11, 8);
    int a = t32_alu(op, rn, rd, s);
    if (a < 0) return undef(i);
    int carry, unpred = 0;
    uint32_t imm12 = BIT(t, 26) << 11 | BITS(t, 14, 12) << 8 | (t & 0xff);
    dp_set(i, a, rd, rn, s);
    i->form = F_IMM;
    i->imm = thumb_expand_imm(imm12, &carry, &unpred);
    i->immcarry = carry;
    i->unpred = unpred;
    t32_dp_checks(i);
    return 4;
}

static int dp_plainimm(uint32_t t, insn_t *i)
{
    unsigned op = BITS(t, 24, 20), rn = BITS(t, 19, 16), rd = BITS(t, 11, 8);
    unsigned imm12 = BIT(t, 26) << 11 | BITS(t, 14, 12) << 8 | (t & 0xff);
    unsigned imm5 = BITS(t, 14, 12) << 2 | BITS(t, 7, 6);
    i->rd = rd;
    i->rn = rn;
    if (rd == 13 || rd == 15) i->unpred = 1;
    switch (op) {
    case 0x00: case 0x0a: /* ADDW / SUBW / ADR */
        dp_set(i, op ? A_SUB : A_ADD, rd, rn, 0);
        i->form = F_IMM;
        i->imm = imm12;
        if (rn == 15) i->pcval = (i->pc + 4) & ~3u;
        if (rd == 13 && rn == 13) i->unpred = 0;
        return 4;
    case 0x04: case 0x0c:
        i->op = op == 4 ? OP_MOVW : OP_MOVT;
        i->imm = BITS(t, 19, 16) << 12 | imm12;
        return 4;
    case 0x10: case 0x12: case 0x18: case 0x1a:
        i->sign = op < 0x18;
        if ((op & 2) && imm5 == 0) { /* SSAT16 / USAT16 */
            i->op = OP_SAT16;
            i->imm = (t & 0xf) + i->sign;
            if (BITS(t, 5, 4)) return undef(i);
        } else {
            i->op = OP_SAT;
            i->imm = (t & 0x1f) + i->sign;
            i->shtype = (op & 2) ? SH_ASR : SH_LSL;
            i->shimm = imm5;
            if (BIT(t, 5)) return undef(i);
        }
        if (rn == 13 || rn == 15) i->unpred = 1;
        return 4;
    case 0x14: case 0x1c:
        i->op = OP_BFX;
        i->sign = op == 0x14;
        i->imm = imm5;
        i->shimm = (t & 0x1f) + 1;
        if (rn == 13 || rn == 15 || imm5 + i->shimm > 32) i->unpred = 1;
        return 4;
    case 0x16:
        i->op = OP_BFI;
        i->imm = imm5;
        i->shimm = t & 0x1f;
        if (rn == 13 || i->shimm < imm5) i->unpred = 1;
        return 4;
    default:
        return undef(i);
    }
}

static int branch_misc(uint32_t t, insn_t *i, int in_it, int last_in_it)
{
    unsigned op1 = BITS(t, 14, 12), op = BITS(t, 26, 20);
    unsigned s = BIT(t, 26), j1 = BIT(t, 13), j2 = BIT(t, 11);
    if (op1 & 1) { /* B (T4) / BL */
        unsigned i1 = !(j1 ^ s), i2 = !(j2 ^ s);
        int32_t off = (int32_t)((s << 24 | i1 << 23 | i2 << 22 | BITS(t, 25, 16) << 12 | BITS(t, 10, 0) << 1) << 7) >> 7;
        i->op = (op1 & 4) ? OP_BL : OP_B;
        i->imm = i->pc + 4 + off;
        i->writes_pc = 1;
        if (in_it && !last_in_it) i->unpred = 1;
        return 4;
    }
    if (op1 & 4) { /* BLX (immediate) */
        unsigned i1 = !(j1 ^ s), i2 = !(j2 ^ s);
        int32_t off = (int32_t)((s << 24 | i1 << 23 | i2 << 22 | BITS(t, 25, 16) << 12 | BITS(t, 10, 1) << 2) << 7) >> 7;
        i->op = OP_BLX_IMM;
        i->imm = ((i->pc + 4) & ~3u) + off;
        i->writes_pc = 1;
        if (t & 1) i->unpred = 1;
        if (in_it && !last_in_it) i->unpred = 1;
        return 4;
    }
    /* op1 = 0x0 */
    if ((op & 0x38) != 0x38) { /* B<c> (T3) */
        int32_t off = (int32_t)((s << 20 | j2 << 19 | j1 << 18 | BITS(t, 21, 16) << 12 | BITS(t, 10, 0) << 1) << 11) >> 11;
        i->op = OP_B;
        i->cond = BITS(t, 25, 22);
        i->imm = i->pc + 4 + off;
        i->writes_pc = 1;
        if (in_it) i->unpred = 1;
        return 4;
    }
    if (op1 == 2 && op == 0x7f) { /* UDF.W */
        i->op = OP_UNDEF;
        return 4;
    }
    if (op1 != 0) return undef(i);
    switch (op) {
    case 0x38: case 0x39: /* MSR */
        if (BIT(t, 20) || BIT(t, 5)) { /* SPSR / banked register: UNPREDICTABLE in user mode */
            i->unpred = 1;
            return undef(i);
        }
        i->op = OP_MSR;
        i->rn = BITS(t, 19, 16);
        i->form = F_REG;
        i->imm = BITS(t, 11, 10) << 2;
        if (BITS(t, 9, 8)) i->imm |= 0x100;
        if (i->rn == 13 || i->rn == 15 || !(i->imm & 0xc)) i->unpred = 1;
        return 4;
    case 0x3a: /* CPS / hints */
        if (BITS(t, 10, 8)) return undef(i);
        i->op = OP_NOP;
        return 4;
    case 0x3b: { /* misc control */
        unsigned o = BITS(t, 7, 4);
        if (o == 2) { i->op = OP_CLREX; return 4; }
        if (o >= 4 && o <= 6) { i->op = OP_BARRIER; return 4; }
        return undef(i);
    }
    case 0x3c: /* BXJ */
        i->op = OP_BX;
        i->rm = BITS(t, 19, 16);
        i->writes_pc = 1;
        if (i->rm == 13 || i->rm == 15) i->unpred = 1;
        return 4;
    case 0x3e: case 0x3f: /* MRS */
        if (BIT(t, 20) || BIT(t, 5)) {
            i->unpred = 1;
            return undef(i);
        }
        i->op = OP_MRS;
        i->rd = BITS(t, 11, 8);
        if (i->rd == 13 || i->rd == 15 || BITS(t, 7, 0) || BITS(t, 19, 16) != 15) i->unpred = 1;
        return 4;
    default:
        return undef(i);
    }
}

static int ldst_single(uint32_t t, insn_t *i)
{
    unsigned size = BITS(t, 22, 21), l = BIT(t, 20), sgn = BIT(t, 24);
    unsigned rn = BITS(t, 19, 16), rt = BITS(t, 15, 12);
    if (size == 3) return undef(i);
    if (sgn && (!l || size == 2)) return undef(i); /* no signed stores, no LDRSW in A32/T32 */
    i->op = OP_LDST;
    i->size = 1u << size;
    i->sign = sgn;
    if (l) i->flags = MF_LOAD;
    i->rn = rn;
    i->rd = rt;
    if (rn == 15) {
        if (!l) return undef(i);
        i->form = F_IMM;
        i->p = 1;
        i->u = BIT(t, 23);
        i->imm = t & 0xfff;
        i->pcval = (i->pc + 4) & ~3u;
    } else if (BIT(t, 23)) {
        i->form = F_IMM;
        i->p = 1;
        i->u = 1;
        i->imm = t & 0xfff;
    } else if (BIT(t, 11)) {
        i->form = F_IMM;
        i->p = BIT(t, 10);
        i->u = BIT(t, 9);
        i->w = BIT(t, 8);
        i->imm = t & 0xff;
        if (!i->p && !i->w) return undef(i);
        if (i->p && i->u && !i->w) i->flags |= MF_USER; /* LDRT etc. */
        if (!i->p) i->w = 1;
    } else if (BITS(t, 10, 6) == 0) {
        reg_shift(i, t & 0xf, SH_LSL, BITS(t, 5, 4));
        i->p = 1;
        i->u = 1;
        if (i->rm == 13 || i->rm == 15) i->unpred = 1;
    } else {
        return undef(i);
    }
    if (l && rt == 15) {
        if (size < 2) { /* PLD / PLI / unallocated hints */
            if (i->w || (i->flags & MF_USER)) i->unpred = 1; /* writeback and "unprivileged" forms are not hints */
            i->op = OP_NOP;
            return 4;
        }
        i->writes_pc = 1;
    }
    if (!l && rt == 15) i->unpred = 1;
    if (l && rt == 13 && size < 2) i->unpred = 1;
    if (!l && rt == 13 && size < 2) i->unpred = 1;
    if (i->w && (rn == rt)) i->unpred = 1;
    return 4;
}

static int dp_register(uint32_t t, insn_t *i)
{
    unsigned op1 = BITS(t, 23, 20), op2 = BITS(t, 7, 4);
    unsigned rn = BITS(t, 19, 16), rd = BITS(t, 11, 8), rm = t & 0xf;
    if (BITS(t, 15, 12) != 15) return undef(i);
    int bad = rd == 13 || rd == 15 || rm == 13 || rm == 15;
    if (op2 == 0 && op1 < 8) { /* shift by register */
        dp_set(i, A_MOV, rd, 0, BIT(t, 20));
        i->form = F_RSR;
        i->rm = rn;
        i->rs = rm;
        i->shtype = BITS(t, 22, 21);
        if (bad || rn == 13 || rn == 15) i->unpred = 1;
        return 4;
    }
    if (op1 < 6 && (op2 & 8)) { /* extend (and add) */
        static const uint8_t e[6] = {E_SXTH, E_UXTH, E_SXTB16, E_UXTB16, E_SXTB, E_UXTB};
        i->op = OP_EXTEND;
        i->sub = e[op1];
        i->rd = rd;
        i->rn = rn;
        i->rm = rm;
        i->shimm = BITS(t, 5, 4) * 8;
        if (bad || rn == 13 || BIT(t, 6)) i->unpred = 1;
        return 4;
    }
    if ((op1 & 8) && op2 < 8) { /* parallel add/sub */
        static const signed char po[8] = {PO_ADD8, PO_ADD16, PO_ASX, -1, PO_SUB8, PO_SUB16, PO_SAX, -1};
        unsigned pre = BITS(t, 5, 4);
        if (po[op1 & 7] < 0 || pre == 3) return undef(i);
        i->op = OP_PARALLEL;
        i->sub = ((op2 & 4 ? PP_U : PP_S) + pre) << 4 | po[op1 & 7];
        i->rd = rd;
        i->rn = rn;
        i->rm = rm;
        if (bad || rn == 13 || rn == 15) i->unpred = 1;
        return 4;
    }
    if ((op1 & 0xc) == 8 && (op2 & 0xc) == 8) { /* miscellaneous operations */
        unsigned a = BITS(t, 21, 20), b = BITS(t, 5, 4);
        i->rd = rd;
        i->rn = rn;
        i->rm = rm;
        if (a == 0) {
            static const uint8_t q[4] = {0, 2, 1, 3};
            i->op = OP_QADDSUB;
            i->sub = q[b];
        } else if (a == 1) {
            static const uint8_t r[4] = {OP_REV, OP_REV16, OP_RBIT, OP_REVSH};
            i->op = r[b];
            if (rn != rm) i->unpred = 1;
        } else if (a == 2 && b == 0) {
            i->op = OP_SEL;
        } else if (a == 3 && b == 0) {
            i->op = OP_CLZ;
            if (rn != rm) i->unpred = 1;
        } else {
            return undef(i);
        }
        if (bad || rn == 13 || rn == 15) i->unpred = 1;
        return 4;
    }
    if ((op1 & 0xe) == 0xc && (op2 & 0xc) == 8) { /* CRC32 / CRC32C */
        if (BITS(t, 5, 4) == 3) return undef(i);
        i->op = OP_CRC32;
        i->size = 1u << BITS(t, 5, 4);
        if (BIT(t, 20)) i->flags |= MF_CRC32C;
        i->rd = rd;
        i->rn = rn;
        i->rm = rm;
        if (bad || rn == 13 || rn == 15) i->unpred = 1;
        return 4;
    }
    return undef(i);
}

static int multiply(uint32_t t, insn_t *i)
{
    unsigned op1 = BITS(t, 22, 20), op2 = BITS(t, 5, 4);
    unsigned rn = BITS(t, 19, 16), ra = BITS(t, 15, 12), rd = BITS(t, 11, 8), rm = t & 0xf;
    if (BITS(t, 7, 6)) return undef(i);
    i->rd = rd;
    i->rn = rn;
    i->rm = rm;
    i->ra = ra;
    int bad = rd == 13 || rd == 15 || rn == 13 || rn == 15 || rm == 13 || rm == 15 || ra == 13;
    switch (op1) {
    case 0:
        if (op2 == 0) {
            i->op = OP_MUL;
            i->sub = ra == 15 ? M_MUL : M_MLA;
        } else if (op2 == 1) {
            i->op = OP_MUL;
            i->sub = M_MLS;
            if (ra == 15) i->unpred = 1;
        } else {
            return undef(i);
        }
        break;
    case 1:
        i->op = OP_MULXY;
        i->sub = ra == 15 ? MX_SMUL : MX_SMLA;
        if (BIT(t, 5)) i->flags |= MF_NHI;
        if (BIT(t, 4)) i->flags |= MF_MHI;
        break;
    case 2: case 4:
        if (op2 & 2) return undef(i);
        i->op = OP_DUALMUL;
        i->sub = op1 == 2 ? DM_SMLAD : DM_SMLSD;
        if (op2 & 1) i->flags |= MF_SWAP;
        break;
    case 3:
        if (op2 & 2) return undef(i);
        i->op = OP_MULXY;
        i->sub = ra == 15 ? MX_SMULW : MX_SMLAW;
        if (op2 & 1) i->flags |= MF_MHI;
        break;
    case 5: case 6:
        if (op2 & 2) return undef(i);
        i->op = OP_SMMUL;
        i->sub = op1 == 6 ? 2 : (ra == 15 ? 0 : 1);
        if (op1 == 6 && ra == 15) i->unpred = 1;
        if (op2 & 1) i->flags |= MF_ROUND;
        break;
    case 7:
        if (op2) return undef(i);
        i->op = OP_USAD8;
        break;
    }
    if (bad) i->unpred = 1;
    return 4;
}

static int long_multiply(uint32_t t, insn_t *i)
{
    unsigned op1 = BITS(t, 22, 20), op2 = BITS(t, 7, 4);
    unsigned rn = BITS(t, 19, 16), rdlo = BITS(t, 15, 12), rdhi = BITS(t, 11, 8), rm = t & 0xf;
    i->rn = rn;
    i->rm = rm;
    i->rd = rdhi;
    i->ra = rdlo;
    int bad = rdlo == 13 || rdlo == 15 || rdhi == 13 || rdhi == 15 || rn == 13 || rn == 15 || rm == 13 || rm == 15 ||
              rdhi == rdlo;
    if ((op1 == 1 || op1 == 3) && op2 == 15) {
        i->op = OP_DIV;
        i->sign = op1 == 1;
        i->rd = rdhi;
        i->ra = 0;
        if (rdlo != 15 || rdhi == 13 || rdhi == 15 || rn == 13 || rn == 15 || rm == 13 || rm == 15) i->unpred = 1;
        return 4;
    }
    i->op = OP_MUL;
    if (op1 == 0 && op2 == 0) i->sub = M_SMULL;
    else if (op1 == 2 && op2 == 0) i->sub = M_UMULL;
    else if (op1 == 4 && op2 == 0) i->sub = M_SMLAL;
    else if (op1 == 6 && op2 == 0) i->sub = M_UMLAL;
    else if (op1 == 6 && op2 == 6) i->sub = M_UMAAL;
    else if (op1 == 4 && (op2 & 0xc) == 8) {
        i->op = OP_MULXY;
        i->sub = MX_SMLAL;
        if (BIT(t, 5)) i->flags |= MF_NHI;
        if (BIT(t, 4)) i->flags |= MF_MHI;
    } else if ((op1 == 4 || op1 == 5) && (op2 & 0xe) == 0xc) {
        i->op = OP_DUALMUL;
        i->sub = op1 == 4 ? DM_SMLALD : DM_SMLSLD;
        if (op2 & 1) i->flags |= MF_SWAP;
    } else {
        return undef(i);
    }
    if (bad) i->unpred = 1;
    return 4;
}

/* Coprocessor space: VFP/NEON (cp10/11) and cp15 go through the A32 decoder. */
static int coproc(uint32_t t, insn_t *i)
{
    unsigned hw1 = t >> 16;
    if ((hw1 & 0xef00) == 0xef00) { /* Advanced SIMD data processing: 111U 1111 -> 1111 001U */
        uint32_t a = 0xf2000000u | (BIT(t, 28) << 24) | (t & 0x00ffffffu);
        vfp_neon_decode(a, i);
        return 4;
    }
    unsigned cp = BITS(t, 11, 8);
    if (BIT(t, 28)) {
        /* 1111 11xx: v8 VSEL/VMAXNM/VRINT/VCVT (cp 10/11), MCR2 & co. otherwise */
        if ((cp & 0xe) == 0xa && (t & 0x0f000010) == 0x0e000000) {
            vfp_neon_decode(0xf0000000u | (t & 0x0fffffffu), i);
            return 4;
        }
        return undef(i);
    }
    insn_t a;
    a32_decode(0xe0000000u | (t & 0x0fffffffu), i->pc, &a);
    if (a.op == OP_SVC || a.op == OP_UNDEF) return undef(i);
    uint32_t pc = i->pc, raw = i->raw, pcval = (i->pc + 4) & ~3u;
    uint8_t cond = i->cond;
    *i = a;
    i->pc = pc;
    i->raw = raw;
    i->cond = cond;
    i->len = 4;
    i->thumb = 1;
    i->pcval = pcval; /* VLDR literal uses Align(PC, 4) */
    if ((i->op == OP_MRC || i->op == OP_MCR || i->op == OP_MRRC) && (i->rd == 13 || i->ra == 13)) i->unpred = 1;
    return 4;
}

static int t32(uint32_t t, insn_t *i, int in_it, int last_in_it)
{
    unsigned op1 = BITS(t, 28, 27), op2 = BITS(t, 26, 20), op = BIT(t, 15);
    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) return ldst_multiple(t, i);
        if ((op2 & 0x64) == 0x04) return ldst_dual_excl(t, i);
        if ((op2 & 0x60) == 0x20) return dp_shifted(t, i);
        return coproc(t, i);
    }
    if (op1 == 2) {
        if (op) return branch_misc(t, i, in_it, last_in_it);
        return (op2 & 0x20) ? dp_plainimm(t, i) : dp_modimm(t, i);
    }
    /* op1 == 3 */
    if ((op2 & 0x71) == 0x10) { /* 001xxx0: Advanced SIMD element/structure load/store */
        vfp_neon_decode(0xf4000000u | (t & 0x00ffffffu), i);
        return 4;
    }
    if ((op2 & 0x60) == 0x00) {
        if ((op2 & 0x67) == 0x07) return undef(i);
        return ldst_single(t, i);
    }
    if ((op2 & 0x70) == 0x20) return dp_register(t, i);
    if ((op2 & 0x78) == 0x30) return multiply(t, i);
    if ((op2 & 0x78) == 0x38) return long_multiply(t, i);
    return coproc(t, i);
}

int t32_decode(const uint16_t *code, uint32_t pc, int in_it, int itcond, insn_t *i)
{
    memset(i, 0, sizeof *i);
    i->pc = pc;
    i->pcval = pc + 4;
    i->thumb = 1;
    i->cond = C_AL;
    uint16_t hw1 = code[0];
    /* last_in_it: the caller passes itcond | 0x100 for the last instruction of an IT block */
    int last = (itcond & 0x100) != 0;
    itcond &= 0xff;
    int len;
    if (t32_insn_len(hw1) == 2) {
        i->len = 2;
        i->raw = hw1;
        len = t16(hw1, i, in_it);
        if (in_it && i->writes_pc && !last && i->op != OP_CBZ && i->op != OP_B) i->unpred = 1;
    } else {
        i->len = 4;
        i->raw = (uint32_t)hw1 << 16 | code[1];
        len = t32(i->raw, i, in_it, last);
        if (in_it && i->writes_pc && !last) i->unpred = 1;
    }
    if (in_it) {
        if (i->op == OP_IT || i->op == OP_CBZ || (i->op == OP_B && i->cond != C_AL) || i->op == OP_SETEND)
            i->unpred = 1;
        else if (i->op != OP_BKPT)
            i->cond = itcond;
    }
    return len;
}
