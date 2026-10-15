/* Reference interpreter: executes one decoded instruction on the CPU state. It defines the semantics; the JIT
 * calls it for instructions it does not translate itself. */
#ifndef A32_INTERP_H
#define A32_INTERP_H

#include <string.h>
#include "cpu.h"
#include "insn.h"

/* Executes i (condition included), sets r[15] to the next instruction and advances ITSTATE.
 * Returns an enum exit_reason (EXIT_NONE to continue). */
int a32_exec(a32_cpu *c, const insn_t *i);
int a32_exec_vfp(a32_cpu *c, const insn_t *i);
int a32_exec_neon(a32_cpu *c, const insn_t *i);

/* Decodes the instruction at r[15] in the current state (ARM/Thumb, ITSTATE). */
int a32_decode_at(const a32_cpu *c, uint32_t pc, insn_t *i);

static inline int cond_pass(uint32_t nzcv, unsigned cond)
{
    unsigned n = nzcv >> 31, z = (nzcv >> 30) & 1, cy = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    int r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: return 1;
    }
    return (cond & 1) ? !r : r;
}

static inline uint32_t itstate_advance(uint32_t it)
{
    return (it & 7) == 0 ? 0 : ((it & 0xe0) | ((it << 1) & 0x1f));
}

/* Guest memory is identity mapped into the low 4 GiB of the host address space. */
static inline void *g2h(uint32_t a) { return (void *)(uintptr_t)a; }
static inline uint8_t ld8(uint32_t a) { return *(volatile uint8_t *)g2h(a); }
static inline uint16_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g2h(a), 2); return v; }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, g2h(a), 4); return v; }
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, g2h(a), 8); return v; }
static inline void st8(uint32_t a, uint8_t v) { *(volatile uint8_t *)g2h(a) = v; }
static inline void st16(uint32_t a, uint16_t v) { memcpy(g2h(a), &v, 2); }
static inline void st32(uint32_t a, uint32_t v) { memcpy(g2h(a), &v, 4); }
static inline void st64(uint32_t a, uint64_t v) { memcpy(g2h(a), &v, 8); }

#endif
