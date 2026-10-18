/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Internal interface of the RV64 translator: translation context, code
    emission helpers, m68k memory access, effective addresses and condition
    codes. See include/riscv64/jit.h for the overall design.

    Conventions
    - Every 32-bit m68k value in a host register is sign-extended from bit 31
      ("canonical"). Word and byte values produced by rv_load68 and
      rv_ea_read are zero-extended; only their low 16 or 8 bits matter.
    - m68k memory is big-endian and the host is little-endian, so 16- and
      32-bit accesses byte-swap with rev8 (docs/porting-map.md, 2.1).
    - Flag sequences follow sw/flag-recipes in the testbed repository, where
      they are checked against a C model of the 68k rules.
*/

#ifndef _EMU68_RISCV64_JIT_INTERNAL_H
#define _EMU68_RISCV64_JIT_INTERNAL_H

#include <stdint.h>
#include "riscv64/rv64.h"
#include "riscv64/register_map.h"

#define CCR_C   0x01
#define CCR_V   0x02
#define CCR_Z   0x04
#define CCR_N   0x08
#define CCR_X   0x10

struct rv_tc {
    uint32_t *start;        /* first word of the unit */
    uint32_t *ptr;          /* next word to emit */
    uint32_t *end;          /* end of the code buffer */
    uint32_t pc;            /* m68k address of the instruction being translated */
    uint32_t cursor;        /* m68k address of the next word to fetch */
    uint32_t temps;         /* free temporaries, bit per host register */
    int ended;              /* the instruction ended the unit and set gp */
    int overflow;           /* the code buffer ran out */
    uint8_t live;           /* XNZVC flags that may still be read after this
                               instruction; it may skip writing the others */
};

/* Emission */
void rv_emit(struct rv_tc *t, uint32_t insn);
void rv_emit_li(struct rv_tc *t, uint8_t rd, int64_t value);
uint8_t rv_tmp(struct rv_tc *t);
void rv_free(struct rv_tc *t, uint8_t reg);

/* Instruction stream (big-endian m68k memory) */
uint16_t rv_fetch16(struct rv_tc *t);
uint32_t rv_fetch32(struct rv_tc *t);

/* m68k register to host register */
uint8_t rv_dreg(int n);
uint8_t rv_areg(int n);

/*
    m68k memory access at base + off (base holds a canonical 68k address).
    Loads: size 1 and 2 zero-extend, size 4 gives a canonical value.
*/
void rv_load68(struct rv_tc *t, int size, uint8_t rd, uint8_t base, int32_t off);
void rv_store68(struct rv_tc *t, int size, uint8_t val, uint8_t base, int32_t off);

/* Effective addresses */
enum rv_ea_kind { EA_DREG, EA_AREG, EA_MEM, EA_IMM };

struct rv_ea {
    enum rv_ea_kind kind;
    uint8_t reg;            /* EA_DREG, EA_AREG: host register */
    uint8_t base;           /* EA_MEM: host register holding the address */
    uint8_t base_is_tmp;    /* base must be freed by rv_ea_done */
    int32_t off;            /* EA_MEM: displacement added at access time */
    int64_t imm;            /* EA_IMM: the value */
    int8_t postinc_areg;    /* (An)+: register to advance in rv_ea_done, or -1 */
    int8_t postinc_step;
};

/*
    Decode mode/reg for an access of size bytes (0 for control addressing:
    LEA, JMP, JSR). Fetches extension words and performs pre-decrement.
    Returns 0 if the mode is not supported.
*/
int rv_ea_decode(struct rv_tc *t, int mode, int reg, int size, struct rv_ea *ea);
/* Value of the operand in a host register; *owned tells whether to free it. */
uint8_t rv_ea_read(struct rv_tc *t, struct rv_ea *ea, int size, int *owned);
/* Store val (low size bits significant, .L canonical) to the operand. */
void rv_ea_write(struct rv_tc *t, struct rv_ea *ea, int size, uint8_t val);
/* Address of a memory operand in a new temporary (for LEA, PEA, JMP). */
uint8_t rv_ea_address(struct rv_tc *t, struct rv_ea *ea);
/* Apply post-increment and free temporaries. */
void rv_ea_done(struct rv_tc *t, struct rv_ea *ea);

/* Condition codes, kept in RVR_SR */
void rv_flags_nz(struct rv_tc *t, int size, uint8_t result);                /* N Z, V = C = 0 */
void rv_flags_add(struct rv_tc *t, int size, uint8_t d, uint8_t s, uint8_t r_out, int set_x);
void rv_flags_sub(struct rv_tc *t, int size, uint8_t d, uint8_t s, uint8_t r_out, int set_x);
void rv_cond(struct rv_tc *t, int cc, uint8_t rd);                          /* rd = condition true */

/*
    Flag liveness (flags.c). rv_sr_usage gives needs << 8 | sets for an
    opcode; rv_flags_liveness fills live[i] for the instructions of a unit.
    Emitters may skip any flag not in t->live, or compute it anyway.
*/
uint32_t rv_sr_usage(uint16_t opcode);
void rv_flags_liveness(const uint16_t *opcodes, uint32_t count, uint8_t *live);

/* Instruction emitters (ops.c, shift.c); return 0 if the opcode is not supported */
int rv_emit_insn(struct rv_tc *t, uint16_t opcode);
int rv_emit_lineE(struct rv_tc *t, uint16_t opcode);
int rv_emit_mul_w(struct rv_tc *t, uint16_t opcode);
int rv_emit_div_w(struct rv_tc *t, uint16_t opcode);
int rv_emit_muldiv_l(struct rv_tc *t, uint16_t opcode);
int rv_emit_neg_not(struct rv_tc *t, uint16_t opcode);
int rv_emit_addx_subx(struct rv_tc *t, uint16_t opcode);
int rv_emit_trap(struct rv_tc *t, uint16_t opcode);
int rv_emit_trapv(struct rv_tc *t);
int rv_emit_trapcc(struct rv_tc *t, uint16_t opcode);
int rv_emit_rte(struct rv_tc *t);
int rv_emit_move_usp(struct rv_tc *t, uint16_t opcode);
int rv_emit_sr_op(struct rv_tc *t, uint16_t opcode);
int rv_emit_abcd_sbcd(struct rv_tc *t, uint16_t opcode);
int rv_emit_nbcd(struct rv_tc *t, uint16_t opcode);
int rv_emit_chk(struct rv_tc *t, uint16_t opcode);
int rv_emit_cas(struct rv_tc *t, uint16_t opcode);
int rv_emit_bitfield(struct rv_tc *t, uint16_t opcode);
int rv_emit_sr_imm(struct rv_tc *t, uint16_t opcode);
/* SR and stack pointers (exception.c) */
void rv_emit_write_sr(struct rv_tc *t, uint8_t val);   /* ends the unit */
void rv_emit_priv_check(struct rv_tc *t);
void rv_emit_sp_switch(struct rv_tc *t, uint8_t old_sr, uint8_t new_sr);
void rv_emit_sp_sync(struct rv_tc *t, int store);
/* Take an exception with a format 0 or 2 frame; sets gp, caller ends the unit */
void rv_emit_exception(struct rv_tc *t, int vector, int format, uint32_t frame_pc, uint32_t insn);

/*
    Set by translated code to an exception vector number when an exception
    should be taken; exceptions are not translated yet, so the dispatcher
    stops the run and reports it.
*/
extern volatile uint32_t rv_jit_exception;

#endif /* _EMU68_RISCV64_JIT_INTERNAL_H */
