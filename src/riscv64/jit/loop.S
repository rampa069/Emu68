/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    int rv_jit_loop(struct M68KState *ctx, int skip_int)

    Dispatcher for runs without trace. The m68k state is loaded into the
    pinned registers once and stays there while units run one after the
    other; it is only stored back when the loop hands over to C, which it
    does for everything that is not the fast path:

      RV_LOOP_PC0      gp = 0, the program has returned
      RV_LOOP_STUB     gp in 0x400..0x5ff, an exception without a handler
      RV_LOOP_INT      ctx->INT64 != 0 before a unit: C calls
                       rv_jit_interrupt (skip_int = 1 skips this test once,
                       after C found the interrupt masked)
      RV_LOOP_MISS     no entry for gp in rv_jit_fast: C translates
      RV_LOOP_EXC      rv_jit_exception set by the unit
      RV_LOOP_STOP     rv_jit_stopped set by the unit (STOP)
      RV_LOOP_CHAIN    the unit left through an exit stub that is not
                       chained yet; rv_jit_exit_site points at it

    Units return to ra (a plain exit, gp holds the next PC) or to ra + 4
    (an exit stub asking to be chained, a7 = the stub). See translator.c,
    rv_emit_exit, for the stub.
*/

#include "riscv64/jit.h"

    .section .text
    .balign 4
    .globl rv_jit_loop
rv_jit_loop:
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
    sd      a1,  15*8(sp)

    mv      tp, a0
    lw      s0,  RV_CTX_D0+0(tp)
    lw      s1,  RV_CTX_D0+4(tp)
    lw      s2,  RV_CTX_D0+8(tp)
    lw      s3,  RV_CTX_D0+12(tp)
    lw      s4,  RV_CTX_D0+16(tp)
    lw      s5,  RV_CTX_D0+20(tp)
    lw      s6,  RV_CTX_D0+24(tp)
    lw      s7,  RV_CTX_D0+28(tp)
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

    ld      t0,  15*8(sp)
    bnez    t0,  .Lskip_int

.Lloop:
    beqz    gp,  .Lpc0
    addi    t0,  gp, -RV_STUB_BASE
    sltiu   t0,  t0, RV_STUB_END - RV_STUB_BASE
    bnez    t0,  .Lstub
    /* pending interrupt: let C decide (contract with rv_jit_interrupt) */
    ld      t0,  RV_CTX_INT64(tp)
    bnez    t0,  .Lint
.Lfast:
    /* rv_jit_fast[(pc >> 1) & (RV_FAST_SIZE - 1)] = { pc, code } */
    slli    t0,  gp, 64 - 1 - RV_FAST_BITS
    srli    t0,  t0, 64 - RV_FAST_BITS
    slli    t0,  t0, 4
    lla     t1,  rv_jit_fast
    add     t1,  t1, t0
    lw      a0,  0(t1)
    bne     a0,  gp, .Lmiss
    ld      a1,  8(t1)
    /* ra + 4 must be the instruction after li: no compressed forms here */
    .option push
    .option norvc
    jalr    ra,  0(a1)
    li      a7,  0              /* ra + 0: plain exit */
    .option pop
                                /* ra + 4: exit stub, a7 = the stub */
    lla     t0,  rv_jit_exception
    lw      t0,  0(t0)
    bnez    t0,  .Lexc
    lla     t0,  rv_jit_stopped
    lw      t0,  0(t0)
    bnez    t0,  .Lstop
    bnez    a7,  .Lchain
    j       .Lloop

.Lskip_int:
    beqz    gp,  .Lpc0
    addi    t0,  gp, -RV_STUB_BASE
    sltiu   t0,  t0, RV_STUB_END - RV_STUB_BASE
    bnez    t0,  .Lstub
    j       .Lfast

.Lpc0:
    li      a0,  RV_LOOP_PC0
    j       .Lout
.Lstub:
    li      a0,  RV_LOOP_STUB
    j       .Lout
.Lint:
    li      a0,  RV_LOOP_INT
    j       .Lout
.Lmiss:
    li      a0,  RV_LOOP_MISS
    j       .Lout
.Lexc:
    li      a0,  RV_LOOP_EXC
    j       .Lout
.Lstop:
    li      a0,  RV_LOOP_STOP
    j       .Lout
.Lchain:
    li      a0,  RV_LOOP_CHAIN
    lla     t0,  rv_jit_exit_site
    sd      a7,  0(t0)

.Lout:
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
