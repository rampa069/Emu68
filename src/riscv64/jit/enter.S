/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    void rv_jit_enter(struct M68KState *ctx, void *code)

    Load the m68k state into the pinned host registers
    (include/riscv64/register_map.h), call the translation unit and store the
    state back. Units return with ret; they keep ra, sp and tp.
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

    jalr    ra, 0(a1)

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
    addi    sp, sp, 16*8
    ret
