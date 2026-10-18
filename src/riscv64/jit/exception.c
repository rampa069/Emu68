/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Exception processing for the RV64 translator, emitted inline:

      1. the old SR is kept, S is set and T1/T0 cleared; coming from user
         mode, A7 is saved as USP and the supervisor stack pointer (ISP, or
         MSP when M is set) loaded
      2. a 68020 stack frame is pushed on the supervisor stack:
           format $0: SR, PC, 0000 | vector*4
           format $2: SR, PC, 2000 | vector*4, address of the instruction
      3. the new PC is read from VBR + 4 * vector

    Format $2 is used for zero divide, CHK, TRAPV and TRAPcc (MC68020 UM),
    format $0 for TRAP #n, ILLEGAL, Line-A and Line-F.

    RTE pops SR and PC and discards the rest of a format $0 or $2 frame;
    any other format stops the run (format error is not modelled yet).
    The stack pointer follows the new S and M bits.

    Privileged instructions run rv_emit_priv_check first, which takes the
    privilege violation exception (vector 8, format $0, PC of the
    instruction) in user mode.

    Trace (T1/T0) is kept in SR but not modelled.
*/

#include <stddef.h>
#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "jit_internal.h"

#define CTX_USP 64
#define CTX_MSP 68
#define CTX_ISP 72
#define CTX_VBR 84
_Static_assert(offsetof(struct M68KState, USP) == CTX_USP, "USP");
_Static_assert(offsetof(struct M68KState, MSP) == CTX_MSP, "MSP");
_Static_assert(offsetof(struct M68KState, ISP) == CTX_ISP, "ISP");
_Static_assert(offsetof(struct M68KState, VBR) == CTX_VBR, "VBR");

#define SR_S    0x2000
#define SR_MASK 0xf71f      /* T1 T0 S M - I2 I1 I0 - - - X N Z V C (68020) */

/*
    rd = RVR_CTX + (offset of the stack pointer slot for SR value sr) - CTX_USP:
    USP when S = 0, ISP when S = 1 and M = 0, MSP when S = 1 and M = 1,
    that is CTX_USP + 8 * S - 4 * (S & M).
*/
static void sp_slot(struct rv_tc *t, uint8_t rd, uint8_t sr, uint8_t k)
{
    rv_emit(t, rv_bexti(rd, sr, 13));
    rv_emit(t, rv_bexti(k, sr, 12));
    rv_emit(t, rv_and(k, k, rd));
    rv_emit(t, rv_sh3add(rd, rd, RVR_CTX));
    rv_emit(t, rv_slli(k, k, 2));
    rv_emit(t, rv_sub(rd, rd, k));
}

/*
    Switch A7 from the stack of old_sr to the stack of new_sr. Always stores
    and reloads, so no branch is needed when the stack does not change.
*/
void rv_emit_sp_switch(struct rv_tc *t, uint8_t old_sr, uint8_t new_sr)
{
    uint8_t sp = rv_areg(7);
    uint8_t a = rv_tmp(t), k = rv_tmp(t);

    sp_slot(t, a, old_sr, k);
    rv_emit(t, rv_sw(sp, a, CTX_USP));
    sp_slot(t, a, new_sr, k);
    rv_emit(t, rv_lw(sp, a, CTX_USP));
    rv_free(t, a);
    rv_free(t, k);
}

/* Store A7 to (store = 1) or load it from (store = 0) the slot of the current SR. */
void rv_emit_sp_sync(struct rv_tc *t, int store)
{
    uint8_t sp = rv_areg(7);
    uint8_t a = rv_tmp(t), k = rv_tmp(t);

    sp_slot(t, a, RVR_SR, k);
    rv_emit(t, store ? rv_sw(sp, a, CTX_USP) : rv_lw(sp, a, CTX_USP));
    rv_free(t, a);
    rv_free(t, k);
}

/*
    Write val to SR (masked to the 68020 bits), switch stacks if S or M
    changed and end the unit at the next instruction, as Emu68 does, so that
    a pending interrupt can be taken once interrupts exist.
*/
void rv_emit_write_sr(struct rv_tc *t, uint8_t val)
{
    uint8_t old = rv_tmp(t), k = rv_tmp(t);

    rv_emit(t, rv_mv(old, RVR_SR));
    rv_emit_li(t, k, SR_MASK);
    rv_emit(t, rv_and(RVR_SR, val, k));
    rv_free(t, k);
    rv_emit_sp_switch(t, old, RVR_SR);
    rv_free(t, old);
    rv_emit_li(t, RVR_PC, (int32_t)t->cursor);
    t->ended = 1;
}

/* In user mode take the privilege violation exception and leave the unit. */
void rv_emit_priv_check(struct rv_tc *t)
{
    uint8_t k = rv_tmp(t);
    uint32_t *b = t->ptr;

    rv_emit(t, rv_bexti(k, RVR_SR, 13));
    rv_emit(t, rv_bnez(k, 0));
    rv_free(t, k);
    rv_emit_exception(t, 8, 0, t->pc, t->pc);
    rv_emit(t, rv_ret());
    if (!t->overflow)
        b[1] = rv_patch_b(b[1], (int32_t)((t->ptr - (b + 1)) * 4));
}

static void push(struct rv_tc *t, int size, uint8_t val)
{
    uint8_t sp = rv_areg(7);
    rv_emit(t, rv_addiw(sp, sp, -size));
    rv_store68(t, size, val, sp, 0);
}

/*
    Take exception `vector` with a frame of format 0 or 2. frame_pc is the
    PC to store in the frame (the next instruction for traps and zero divide,
    this instruction for ILLEGAL and Line-A/F), insn the address of the
    instruction (format 2). Sets gp to the handler; the caller ends the unit.
*/
void rv_emit_exception(struct rv_tc *t, int vector, int format, uint32_t frame_pc, uint32_t insn)
{
    uint8_t old = rv_tmp(t), k = rv_tmp(t);

    rv_emit(t, rv_mv(old, RVR_SR));

    /* S = 1, T1 = T0 = 0; from user mode switch to ISP or MSP */
    rv_emit_li(t, k, SR_S);
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));
    rv_emit_li(t, k, 0x3fff);
    rv_emit(t, rv_and(RVR_SR, RVR_SR, k));
    rv_emit_sp_switch(t, old, RVR_SR);

    if (format == 2) {
        rv_emit_li(t, k, (int32_t)insn);
        push(t, 4, k);
    }
    rv_emit_li(t, k, (format << 12) | (vector * 4));
    push(t, 2, k);
    rv_emit_li(t, k, (int32_t)frame_pc);
    push(t, 4, k);
    push(t, 2, old);

    /* PC = (VBR + 4 * vector) */
    rv_emit(t, rv_lw(k, RVR_CTX, CTX_VBR));
    rv_load68(t, 4, RVR_PC, k, vector * 4);
    rv_free(t, old);
    rv_free(t, k);
}

/* RTE (privileged) */
int rv_emit_rte(struct rv_tc *t)
{
    uint8_t sp = rv_areg(7);

    rv_emit_priv_check(t);
    uint8_t sr = rv_tmp(t), f = rv_tmp(t), k = rv_tmp(t);

    rv_load68(t, 2, sr, sp, 0);
    rv_load68(t, 4, RVR_PC, sp, 2);
    rv_load68(t, 2, f, sp, 6);
    rv_emit(t, rv_srli(f, f, 12));

    /* format 0: 8 bytes, format 2: 12 bytes, anything else stops the run */
    uint32_t *is0 = t->ptr;
    rv_emit(t, rv_beqz(f, 0));
    rv_emit(t, rv_addi(k, 0, 2));
    uint32_t *is2 = t->ptr;
    rv_emit(t, rv_beq(f, k, 0));
    /* unsupported format: report it as vector 14 and stop here */
    rv_emit_li(t, k, (int64_t)(intptr_t)&rv_jit_exception);
    rv_emit(t, rv_addi(f, 0, 14));
    rv_emit(t, rv_sw(f, k, 0));
    rv_emit_li(t, RVR_PC, (int32_t)t->pc);
    rv_emit(t, rv_ret());

    uint32_t *l2 = t->ptr;
    rv_emit(t, rv_addiw(sp, sp, 4));        /* format 2: instruction address */
    uint32_t *l0 = t->ptr;
    rv_emit(t, rv_addiw(sp, sp, 8));
    if (!t->overflow) {
        *is0 = rv_patch_b(*is0, (int32_t)((l0 - is0) * 4));
        *is2 = rv_patch_b(*is2, (int32_t)((l2 - is2) * 4));
    }

    /* new SR, then follow S and M to the right stack */
    rv_emit(t, rv_mv(f, RVR_SR));
    rv_emit_li(t, k, SR_MASK);
    rv_emit(t, rv_and(RVR_SR, sr, k));
    rv_emit_sp_switch(t, f, RVR_SR);

    rv_free(t, sr);
    rv_free(t, f);
    rv_free(t, k);
    t->ended = 1;
    return 1;
}

/*
    MOVE An,USP (0x4e60) and MOVE USP,An (0x4e68), privileged. The USP lives
    in the context while in supervisor mode.
*/
int rv_emit_move_usp(struct rv_tc *t, uint16_t op)
{
    uint8_t an = rv_areg(op & 7);
    rv_emit_priv_check(t);
    if (op & 0x08)
        rv_emit(t, rv_lw(an, RVR_CTX, CTX_USP));
    else
        rv_emit(t, rv_sw(an, RVR_CTX, CTX_USP));
    return 1;
}

/* TRAP #n, TRAPV, ILLEGAL, Line-A, Line-F (no FPU) */
int rv_emit_trap(struct rv_tc *t, uint16_t op)
{
    if ((op & 0xfff0) == 0x4e40) {          /* TRAP #n: format 0, PC = next */
        rv_emit_exception(t, 32 + (op & 15), 0, t->cursor, t->pc);
    } else if (op == 0x4afc) {              /* ILLEGAL: PC = this instruction */
        rv_emit_exception(t, 4, 0, t->pc, t->pc);
    } else if ((op & 0xf000) == 0xa000) {   /* Line-A */
        rv_emit_exception(t, 10, 0, t->pc, t->pc);
    } else if ((op & 0xf000) == 0xf000) {   /* Line-F: FPU and other coprocessors */
        rv_emit_exception(t, 11, 0, t->pc, t->pc);
    } else {
        return 0;
    }
    t->ended = 1;
    return 1;
}

/* Conditional traps: TRAPV and TRAPcc (format 2, PC = next instruction) */
static void trap_if(struct rv_tc *t, uint8_t cond)
{
    uint32_t *b = t->ptr;
    rv_emit(t, rv_beqz(cond, 0));
    rv_emit_exception(t, 7, 2, t->cursor, t->pc);
    rv_emit(t, rv_ret());
    if (!t->overflow)
        *b = rv_patch_b(*b, (int32_t)((t->ptr - b) * 4));
}

int rv_emit_trapv(struct rv_tc *t)
{
    uint8_t c = rv_tmp(t);
    rv_cond(t, 9, c);                       /* VS */
    trap_if(t, c);
    rv_free(t, c);
    return 1;
}

/* TRAPcc 0101 cccc 1111 1ooo: o = 010 word, 011 long, 100 no operand */
int rv_emit_trapcc(struct rv_tc *t, uint16_t op)
{
    int o = op & 7;
    if (o == 2)
        rv_fetch16(t);
    else if (o == 3)
        rv_fetch32(t);
    else if (o != 4)
        return 0;
    uint8_t c = rv_tmp(t);
    rv_cond(t, (op >> 8) & 15, c);
    trap_if(t, c);
    rv_free(t, c);
    return 1;
}
