/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    uint32_t rv_jit_enter(struct M68KState *ctx, void *code, int check)

    Load the m68k state into the pinned host registers
    (include/riscv64/register_map.h), call the translation unit and store the
    state back. Units return with ret; they keep ra, sp and tp. Used for
    runs with trace, one unit (or one chain of units) at a time; runs
    without trace use the dispatcher in loop.S.

    An exit stub that is not chained yet returns to ra + 4 with a7 pointing
    at itself (translator.c, rv_emit_exit); the address is left in
    rv_jit_exit_site, 0 after a plain exit.

    With check != 0 the trampoline also verifies, after the unit, that every
    m68k register is still sign-extended from bit 31, the invariant all
    emitters rely on. It returns the mask of registers that are not (bit 0 =
    D0 ... bit 15 = A7), and 0 otherwise. A violation is a translator bug
    whose effect may only show many instructions later.
*/

#include "riscv64/jit.h"

    .section .text
    .balign 4
    .globl rv_jit_enter
rv_jit_enter:
    addi    sp, sp, -16*8
    sd      ra,   0*8(sp)
    sd      s0,   1*8(sp)
    sd      s1,   2*8(sp)
    sd      s2,   3*8(sp)
    sd      s3,   4*8(sp)
    sd      s4,   5*8(sp)
    sd      s5,   6*8(sp)
    sd      s6,   7*8(sp)
    sd      s7,   8*8(sp)
    sd      s8,   9*8(sp)
    sd      s9,  10*8(sp)
    sd      s10, 11*8(sp)
    sd      s11, 12*8(sp)
    sd      gp,  13*8(sp)
    sd      tp,  14*8(sp)
    sd      a2,  15*8(sp)

    mv      tp, a0
    /* D0-D7 */
    lw      s0,  RV_CTX_D0+0(tp)
    lw      s1,  RV_CTX_D0+4(tp)
    lw      s2,  RV_CTX_D0+8(tp)
    lw      s3,  RV_CTX_D0+12(tp)
    lw      s4,  RV_CTX_D0+16(tp)
    lw      s5,  RV_CTX_D0+20(tp)
    lw      s6,  RV_CTX_D0+24(tp)
    lw      s7,  RV_CTX_D0+28(tp)
    /* A0-A7 */
    lw      s8,  RV_CTX_A0+0(tp)
    lw      s9,  RV_CTX_A0+4(tp)
    lw      s10, RV_CTX_A0+8(tp)
    lw      s11, RV_CTX_A0+12(tp)
    lw      t3,  RV_CTX_A0+16(tp)
    lw      t4,  RV_CTX_A0+20(tp)
    lw      t5,  RV_CTX_A0+24(tp)
    lw      t6,  RV_CTX_A0+28(tp)
    lw      gp,  RV_CTX_PC(tp)
    lhu     t2,  RV_CTX_SR(tp)

    /* ra + 4 must be the instruction after li: no compressed forms here */
    .option push
    .option norvc
    jalr    ra, 0(a1)
    li      a7,  0               /* ra + 0: plain exit; ra + 4: exit stub */
    .option pop
    lla     t1,  rv_jit_exit_site
    sd      a7,  0(t1)

    /* checked mode: mask of m68k registers that are not canonical */
    ld      t0,  15*8(sp)
    beqz    t0,  2f
    li      t0,  0
    addiw   t1, s0, 0
    xor     t1, t1, s0
    snez    t1, t1
    or      t0, t0, t1
    addiw   t1, s1, 0
    xor     t1, t1, s1
    snez    t1, t1
    slli    t1, t1, 1
    or      t0, t0, t1
    addiw   t1, s2, 0
    xor     t1, t1, s2
    snez    t1, t1
    slli    t1, t1, 2
    or      t0, t0, t1
    addiw   t1, s3, 0
    xor     t1, t1, s3
    snez    t1, t1
    slli    t1, t1, 3
    or      t0, t0, t1
    addiw   t1, s4, 0
    xor     t1, t1, s4
    snez    t1, t1
    slli    t1, t1, 4
    or      t0, t0, t1
    addiw   t1, s5, 0
    xor     t1, t1, s5
    snez    t1, t1
    slli    t1, t1, 5
    or      t0, t0, t1
    addiw   t1, s6, 0
    xor     t1, t1, s6
    snez    t1, t1
    slli    t1, t1, 6
    or      t0, t0, t1
    addiw   t1, s7, 0
    xor     t1, t1, s7
    snez    t1, t1
    slli    t1, t1, 7
    or      t0, t0, t1
    addiw   t1, s8, 0
    xor     t1, t1, s8
    snez    t1, t1
    slli    t1, t1, 8
    or      t0, t0, t1
    addiw   t1, s9, 0
    xor     t1, t1, s9
    snez    t1, t1
    slli    t1, t1, 9
    or      t0, t0, t1
    addiw   t1, s10, 0
    xor     t1, t1, s10
    snez    t1, t1
    slli    t1, t1, 10
    or      t0, t0, t1
    addiw   t1, s11, 0
    xor     t1, t1, s11
    snez    t1, t1
    slli    t1, t1, 11
    or      t0, t0, t1
    addiw   t1, t3, 0
    xor     t1, t1, t3
    snez    t1, t1
    slli    t1, t1, 12
    or      t0, t0, t1
    addiw   t1, t4, 0
    xor     t1, t1, t4
    snez    t1, t1
    slli    t1, t1, 13
    or      t0, t0, t1
    addiw   t1, t5, 0
    xor     t1, t1, t5
    snez    t1, t1
    slli    t1, t1, 14
    or      t0, t0, t1
    addiw   t1, t6, 0
    xor     t1, t1, t6
    snez    t1, t1
    slli    t1, t1, 15
    or      t0, t0, t1
2:
    sd      t0,  15*8(sp)        /* 0 when not checked: a2 was 0 */

    sw      s0,  RV_CTX_D0+0(tp)
    sw      s1,  RV_CTX_D0+4(tp)
    sw      s2,  RV_CTX_D0+8(tp)
    sw      s3,  RV_CTX_D0+12(tp)
    sw      s4,  RV_CTX_D0+16(tp)
    sw      s5,  RV_CTX_D0+20(tp)
    sw      s6,  RV_CTX_D0+24(tp)
    sw      s7,  RV_CTX_D0+28(tp)
    sw      s8,  RV_CTX_A0+0(tp)
    sw      s9,  RV_CTX_A0+4(tp)
    sw      s10, RV_CTX_A0+8(tp)
    sw      s11, RV_CTX_A0+12(tp)
    sw      t3,  RV_CTX_A0+16(tp)
    sw      t4,  RV_CTX_A0+20(tp)
    sw      t5,  RV_CTX_A0+24(tp)
    sw      t6,  RV_CTX_A0+28(tp)
    sw      gp,  RV_CTX_PC(tp)
    sh      t2,  RV_CTX_SR(tp)

    ld      ra,   0*8(sp)
    ld      s0,   1*8(sp)
    ld      s1,   2*8(sp)
    ld      s2,   3*8(sp)
    ld      s3,   4*8(sp)
    ld      s4,   5*8(sp)
    ld      s5,   6*8(sp)
    ld      s6,   7*8(sp)
    ld      s7,   8*8(sp)
    ld      s8,   9*8(sp)
    ld      s9,  10*8(sp)
    ld      s10, 11*8(sp)
    ld      s11, 12*8(sp)
    ld      gp,  13*8(sp)
    ld      tp,  14*8(sp)
    ld      a0,  15*8(sp)
    addi    sp, sp, 16*8
    ret
