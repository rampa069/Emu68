/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Supervisor trap entry. Saves every general register in a frame on the
    current stack and calls trap_handler(frame). The handler may change the
    saved registers (MMIO emulation writes the destination register) and the
    saved sepc. Translated code runs in S-mode on the same stack, so no
    stack switch is needed yet.
*/

    .section .text
    .balign 4
    .globl trap_entry
trap_entry:
    addi    sp, sp, -34*8
    .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\n, \n*8(sp)
    .endr
    addi    t0, sp, 34*8
    sd      t0, 2*8(sp)
    sd      zero, 0(sp)
    csrr    t0, sepc
    sd      t0, 32*8(sp)
    csrr    t0, sstatus
    sd      t0, 33*8(sp)

    mv      a0, sp
    call    trap_handler

    ld      t0, 32*8(sp)
    csrw    sepc, t0
    .irp n, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\n, \n*8(sp)
    .endr
    addi    sp, sp, 34*8
    sret
