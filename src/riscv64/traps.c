/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Supervisor trap handler, the RV64 counterpart of src/aarch64/vectors.c.

    For now every trap is reported and the hart stops. The next steps are
    emulating m68k accesses to unmapped addresses (load/store page faults
    with the m68k address in stval, decoded from the instruction at sepc as
    SYSPageFaultReadHandler/WriteHandler do on AArch64) and the timer and
    external interrupts.
*/

#include <stdint.h>
#include "support.h"
#include "cpu_relax.h"
#include "riscv64/traps.h"

static const char *cause_name(uint64_t scause)
{
    if (scause >> 63) {
        switch (scause & 0xff) {
            case 1: return "supervisor software interrupt";
            case 5: return "supervisor timer interrupt";
            case 9: return "supervisor external interrupt";
            default: return "interrupt";
        }
    }
    switch (scause) {
        case 0: return "instruction address misaligned";
        case 1: return "instruction access fault";
        case 2: return "illegal instruction";
        case 3: return "breakpoint";
        case 4: return "load address misaligned";
        case 5: return "load access fault";
        case 6: return "store address misaligned";
        case 7: return "store access fault";
        case 8: return "environment call from U-mode";
        case 9: return "environment call from S-mode";
        case 12: return "instruction page fault";
        case 13: return "load page fault";
        case 15: return "store page fault";
        default: return "unknown";
    }
}

void trap_handler(struct trap_frame *f)
{
    uint64_t scause, stval;
    __asm__ volatile("csrr %0, scause" : "=r"(scause));
    __asm__ volatile("csrr %0, stval" : "=r"(stval));

    kprintf("[TRAP] %s (scause %lx) at %p, stval %p\n",
        cause_name(scause), scause, (void *)f->sepc, (void *)stval);
    for (int i = 0; i < 32; i += 4)
        kprintf("[TRAP]   x%02d %016lx  x%02d %016lx  x%02d %016lx  x%02d %016lx\n",
            i, f->x[i], i + 1, f->x[i + 1], i + 2, f->x[i + 2], i + 3, f->x[i + 3]);

    while (1)
        cpu_halt();
}
