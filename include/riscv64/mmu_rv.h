/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Sv39 address space of the RV64 host.

    0x0000000000000000 - 0x00000000ffffffff  m68k address space (4 GiB)
    0xffffffc000000000 + phys                linear map of physical memory,
                                             the kernel runs here
    0xffffffff80000000 - 0xffffffffffffffff  mirror of m68k 0x80000000 -
                                             0xffffffff, so that a 68k address
                                             held sign-extended (the result of
                                             addw) is still a valid pointer

    mmu_map() takes RV_MMU_* attributes in attr_low; attr_high is unused.
    Memory types come from the platform PMAs (Svpbmt is not assumed).
*/

#ifndef _EMU68_RISCV64_MMU_RV_H
#define _EMU68_RISCV64_MMU_RV_H

#include <stdint.h>

#define RV_LINEAR_BASE      0xffffffc000000000UL
#define RV_LINEAR_GIB       64

#define RV_PHYS_TO_VIRT(p)  ((uintptr_t)(p) + RV_LINEAR_BASE)
#define RV_VIRT_TO_PHYS(v)  ((uintptr_t)(v) - RV_LINEAR_BASE)

#define RV_MMU_READ         0x02
#define RV_MMU_WRITE        0x04
#define RV_MMU_EXEC         0x08
#define RV_MMU_RW           (RV_MMU_READ | RV_MMU_WRITE)
#define RV_MMU_RWX          (RV_MMU_READ | RV_MMU_WRITE | RV_MMU_EXEC)

/* Early tables, called with the MMU off. Returns the satp value. */
uint64_t mmu_early(void);

#endif /* _EMU68_RISCV64_MMU_RV_H */
