/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Exception processing for the RV64 translator, emitted inline:

      1. the old SR is kept, S is set and T1/T0 cleared; coming from user
         mode, A7 is saved as USP and the interrupt stack pointer loaded
      2. a 68020 stack frame is pushed on the supervisor stack:
           format $0: SR, PC, 0000 | vector*4
           format $2: SR, PC, 2000 | vector*4, address of the instruction
      3. the new PC is read from VBR + 4 * vector

    Format $2 is used for zero divide, CHK, TRAPV and TRAPcc (MC68020 UM),
    format $0 for TRAP #n, ILLEGAL, Line-A and Line-F.

    RTE pops SR and PC and discards the rest of a format $0 or $2 frame;
    any other format stops the run (format error is not modelled yet).
    Returning to user mode saves A7 as ISP and loads USP.

    The master stack (M bit) and trace (T bits) are not modelled.
*/

#include <stddef.h>
#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "jit_internal.h"

#define CTX_USP 64
#define CTX_ISP 72
#define CTX_VBR 84
_Static_assert(offsetof(struct M68KState, USP) == CTX_USP, "USP");
_Static_assert(offsetof(struct M68KState, ISP) == CTX_ISP, "ISP");
_Static_assert(offsetof(struct M68KState, VBR) == CTX_VBR, "VBR");

#define SR_S    0x2000

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
    uint8_t sp = rv_areg(7);

    rv_emit(t, rv_mv(old, RVR_SR));

    /* from user mode: switch to the interrupt stack */
    uint32_t *b = t->ptr;
    rv_emit(t, rv_bexti(k, old, 13));
    rv_emit(t, rv_bnez(k, 0));
    rv_emit(t, rv_sw(sp, RVR_CTX, CTX_USP));
    rv_emit(t, rv_lw(sp, RVR_CTX, CTX_ISP));
    if (!t->overflow)
        b[1] = rv_patch_b(b[1], (int32_t)((t->ptr - (b + 1)) * 4));

    /* S = 1, T1 = T0 = 0 */
    rv_emit_li(t, k, SR_S);
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));
    rv_emit_li(t, k, 0x3fff);
    rv_emit(t, rv_and(RVR_SR, RVR_SR, k));

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

/* RTE (supervisor only: privilege checks come with the SR instructions) */
int rv_emit_rte(struct rv_tc *t)
{
    uint8_t sp = rv_areg(7);
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

    rv_emit(t, rv_mv(RVR_SR, sr));
    /* back to user mode: save ISP, load USP */
    uint32_t *b = t->ptr;
    rv_emit(t, rv_bexti(k, sr, 13));
    rv_emit(t, rv_bnez(k, 0));
    rv_emit(t, rv_sw(sp, RVR_CTX, CTX_ISP));
    rv_emit(t, rv_lw(sp, RVR_CTX, CTX_USP));
    if (!t->overflow)
        b[1] = rv_patch_b(b[1], (int32_t)((t->ptr - (b + 1)) * 4));

    rv_free(t, sr);
    rv_free(t, f);
    rv_free(t, k);
    t->ended = 1;
    return 1;
}

/*
    MOVE An,USP (0x4e60) and MOVE USP,An (0x4e68). The USP lives in the
    context while in supervisor mode. Privilege violations are checked once
    the SR instructions and user mode are translated; for now the
    instruction is assumed to run in supervisor mode.
*/
int rv_emit_move_usp(struct rv_tc *t, uint16_t op)
{
    uint8_t an = rv_areg(op & 7);
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
