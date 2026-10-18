/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Host cache maintenance for RV64. The names keep the arm_ prefix of the
    shared interface in include/support.h.

    The JIT writes code through the data side and runs it on the same hart,
    so making new code visible to instruction fetch is a fence.i. Data cache
    maintenance by address needs Zicbom, which is not assumed: QEMU and the
    planned softcore are coherent for normal memory, so the data-side calls
    are ordering fences only.
*/

#include <stdint.h>
#include "support.h"

void arm_flush_cache(uintptr_t addr, uint32_t length)
{
    (void)addr;
    (void)length;
    __asm__ volatile("fence rw, rw" ::: "memory");
}

void arm_flush_dcache_for_jit(uintptr_t addr, uint32_t length)
{
    (void)addr;
    (void)length;
    __asm__ volatile("fence rw, rw" ::: "memory");
}

void arm_flush_icache_for_jit(uintptr_t addr, uint32_t length)
{
    (void)addr;
    (void)length;
    __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");
}

void arm_icache_invalidate(uintptr_t addr, uint32_t length)
{
    (void)addr;
    (void)length;
    __asm__ volatile("fence.i" ::: "memory");
}

void arm_dcache_invalidate(uintptr_t addr, uint32_t length)
{
    (void)addr;
    (void)length;
    __asm__ volatile("fence rw, rw" ::: "memory");
}

void clear_entire_dcache()
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}
