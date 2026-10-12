/* JIT internals shared by jit.c (blocks, cache, integer instructions) and jit_vfp.c (VFP/NEON). */
#ifndef A32_JIT_H
#define A32_JIT_H

#include <stddef.h>
#include "a64.h"
#include "cpu.h"
#include "insn.h"

/* host registers */
#define R_CPU 19
#define T0 16
#define T1 17
#define T2 15
#define T3 20 /* T3..T6 survive helper calls */
#define T4 21
#define T5 22
#define T6 23

#define OFF_PC OFF_R(15)
#define OFF_THUMB offsetof(a32_cpu, thumb)
#define OFF_ITSTATE offsetof(a32_cpu, itstate)
#define OFF_EXIT offsetof(a32_cpu, exit_reason)
#define OFF_SVC offsetof(a32_cpu, svc_imm)
#define OFF_EXIT_STUB offsetof(a32_cpu, exit_stub)
#define OFF_HELPERS offsetof(a32_cpu, helper_tab)
#define OFF_LOOKUP offsetof(a32_cpu, lookup_tab)
#define OFF_INTERRUPT offsetof(a32_cpu, interrupt)
#define OFF_LAST_EXIT offsetof(a32_cpu, last_exit)
#define OFF_Q offsetof(a32_cpu, q)
#define OFF_GE offsetof(a32_cpu, ge)
#define OFF_FPSCR_NZCV offsetof(a32_cpu, fpscr_nzcv)
#define OFF_FPCR offsetof(a32_cpu, fpcr)
#define OFF_FPSR offsetof(a32_cpu, fpsr)
#define OFF_TPIDRURO offsetof(a32_cpu, tpidruro)
#define OFF_TPIDRURW offsetof(a32_cpu, tpidrurw)
#define OFF_EXCL_VALID offsetof(a32_cpu, excl_valid)

/* helper table indices (cpu->helper_tab) */
enum { HELP_EXEC, HELP_COUNT };

struct jit_block;

/* A chainable exit: the "b" at insn starts at the stub; once the target is translated it is patched to jump there. */
typedef struct jit_site {
    uint32_t *insn;
    uint32_t *stub;
    struct jit_block *owner;
    struct jit_block *target;      /* chained target, or NULL */
    struct jit_site *next_in;      /* list of sites chained into the same target */
} jit_site;

typedef struct jit_block {
    uint64_t key;                  /* must stay first: inline lookup reads key and code with one ldp */
    uint32_t *code;
    uint32_t *end;
    uint32_t guest_lo, guest_hi;   /* guest bytes covered */
    struct jit_block *hnext;
    jit_site *in;                  /* sites chained into this block */
    jit_site *sites;               /* this block's own chainable exits */
    int nsites;
    uint32_t *map_host;            /* per guest instruction: first host instruction */
    uint32_t *map_pc;              /* guest pc */
    uint8_t *map_it;               /* ITSTATE before the instruction */
    uint64_t *map_vc;              /* scalar VFP cache state before the instruction (jit_vfp_state) */
    int nmap;
    uint8_t thumb, valid;
    struct jit_block *pnext[2];    /* lists of the blocks on the guest page(s) of guest_lo and guest_hi - 1 */
} jit_block;

#define MAX_STUBS 64
typedef struct {
    int kind;                      /* STUB_DIRECT, STUB_INDIRECT, STUB_REASON, STUB_ASIS, STUB_HELPER_EXIT */
    uint32_t *site;                /* instruction(s) that branch to the stub (patched once the stub exists) */
    uint32_t pc;
    uint8_t thumb, it;
    uint32_t reason, svc;
} jit_stub;
enum { STUB_DIRECT, STUB_INDIRECT, STUB_REASON, STUB_ASIS };
enum { FPCR_GUEST, FPCR_STD, FPCR_UNKNOWN };

typedef struct {
    cbuf cb;
    jit_block *b;
    uint32_t b_pc, b_it;  /* the block's start (pc, ITSTATE) */
    uint32_t pc;          /* current guest instruction */
    int thumb;
    uint32_t it;          /* ITSTATE before the current instruction */
    const insn_t *i;
    const insn_t *next;   /* the following instruction if it is in this block (peepholes), else NULL */
    int skip_vmrs;        /* the previous VCMP already put its flags into NZCV */
    jit_stub stubs[MAX_STUBS];
    int nstubs;
    int ended;            /* the block ended (unconditional exit emitted) */
    int side_exit;        /* the instruction emitted a conditional exit and falls through (CBZ) */
    int fpcr_std;         /* host FPCR: FPCR_GUEST, FPCR_STD (Advanced SIMD standard value) or FPCR_UNKNOWN */
    /* scalar VFP cache in v8-v15 (jit_vfp.c): view 0 = empty, 1 = S register, 2 = D register */
    struct { uint8_t view, reg, dirty; uint32_t used; } vc[8];
    uint32_t vc_tick;
} jit_ctx;

/* merges dirty cached VFP values into the pinned Q registers and empties the cache (before exits, helpers and
 * control-flow splits) */
void jit_vfp_flush(jit_ctx *x);
void jit_vfp_emit_merges(jit_ctx *x); /* same code, translation-time state unchanged (side exits) */
/* encodes the cache state for the fault map (slot k: byte k = dirty << 7 | view << 5 | reg) */
uint64_t jit_vfp_state(const jit_ctx *x);

/* emit helpers (jit.c) */
void jx_mov32(cbuf *b, int rd, uint32_t v);
void jx_mov64(cbuf *b, int rd, uint64_t v);
int jx_src(jit_ctx *x, unsigned guest_reg, int tmp);   /* host register holding guest_reg (pc materialized in tmp) */
void jx_spill(cbuf *b);                                 /* guest r0-r14 + NZCV -> cpu */
void jx_reload(cbuf *b);
void jx_call_helper(jit_ctx *x, const insn_t *i);       /* full interpreter fallback for one instruction */
uint32_t *jx_placeholder(cbuf *b);
void jx_patch_bcond(uint32_t *site, int cond, uint32_t *target);
void jx_patch_b(uint32_t *site, uint32_t *target);
void jx_patch_cbz(uint32_t *site, uint32_t *target);
/* exits */
void jx_exit_direct(jit_ctx *x, uint32_t pc, int thumb, uint32_t it);
void jx_exit_indirect_t0(jit_ctx *x);                   /* target (bit 0 = Thumb) in w16 */
void jx_exit_reason(jit_ctx *x, uint32_t pc, int thumb, uint32_t it, uint32_t reason, uint32_t svc);
/* host FPCR = guest VFP control bits / Advanced SIMD standard value (emits nothing if already so) */
void jx_fpcr_guest(jit_ctx *x);
void jx_fpcr_std(jit_ctx *x);
extern int jit_exact_addr;

/* jit_vfp.c: returns 1 if it emitted native code for the VFP/NEON instruction */
int jit_vfp(jit_ctx *x, const insn_t *i);

extern int jit_stats;
#endif
