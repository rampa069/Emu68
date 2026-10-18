/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    S-mode entry from OpenSBI (fw_dynamic): a0 = hart id, a1 = physical
    address of the device tree. Runs at the physical load address with the
    MMU off, builds the early Sv39 tables, then continues in the linear map
    and calls boot(hartid, dtb).
*/

#define RV_LINEAR_BASE 0xffffffc000000000

    .section .startup, "ax"
    .globl _start
_start:
    mv      s0, a0
    mv      s1, a1
    csrw    sie, zero
    csrci   sstatus, 2

    la      sp, __stack_top
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    call    mmu_early
    sfence.vma
    csrw    satp, a0
    sfence.vma

    li      t1, RV_LINEAR_BASE
    la      t0, 3f
    add     t0, t0, t1
    jr      t0
3:
    li      t1, RV_LINEAR_BASE
    add     sp, sp, t1
    la      t0, trap_entry
    csrw    stvec, t0

    mv      a0, s0
    add     a1, s1, t1
    call    boot
4:  wfi
    j       4b
