/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Sv39 page tables for the RV64 host, see include/riscv64/mmu_rv.h for the
    layout. The m68k window uses four static level-1 tables (one per GiB) and
    2 MiB pages, split into level-2 tables of 4 KiB pages on demand.

    mmu_early() runs with the MMU off at the physical load address, so it
    must not use absolute addresses; the file is built with -fno-jump-tables
    and -mcmodel=medany keeps every reference PC relative.
*/

#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "mmu.h"
#include "riscv64/mmu_rv.h"

#define ENTRIES     512
#define PTE_V       (1UL << 0)
#define PTE_RWX     (7UL << 1)
#define PTE_G       (1UL << 5)
#define PTE_A       (1UL << 6)
#define PTE_D       (1UL << 7)
#define PTE_PPN(p)  (((uint64_t)(p) >> 12) << 10)
#define PTE_ADDR(e) (((e) >> 10) << 12)
#define PTE_IS_LEAF(e) (((e) & PTE_RWX) != 0)

#define ROOT_LINEAR 256     /* 0xffffffc000000000 */
#define ROOT_MIRROR 508     /* 0xffffffff00000000 */

#define SZ_2M       (1UL << 21)
#define SZ_4K       (1UL << 12)

static uint64_t mmu_root[ENTRIES] __attribute__((aligned(4096), section(".mmu")));
static uint64_t mmu_68k_l1[4][ENTRIES] __attribute__((aligned(4096), section(".mmu")));

static inline uint64_t leaf(uint64_t phys, uint64_t flags)
{
    return PTE_PPN(phys) | (flags & PTE_RWX) | PTE_V | PTE_A | PTE_D;
}

static inline void sfence_all(void)
{
    __asm__ volatile("sfence.vma" ::: "memory");
}

uint64_t mmu_early(void)
{
    for (int i = 0; i < ENTRIES; i++)
        mmu_root[i] = 0;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < ENTRIES; i++)
            mmu_68k_l1[t][i] = 0;

    /* Linear map of physical memory in 1 GiB pages. */
    for (uint64_t g = 0; g < RV_LINEAR_GIB; g++)
        mmu_root[ROOT_LINEAR + g] = leaf(g << 30, PTE_RWX) | PTE_G;

    /* Temporary identity map of the gigabyte holding the kernel. */
    uint64_t here = (uint64_t)(uintptr_t)&mmu_root[0];
    mmu_root[here >> 30] = leaf(here & ~((1UL << 30) - 1), PTE_RWX);

    return (8UL << 60) | (here >> 12);
}

/* Drop the identity map and attach the m68k window and its mirror. */
void mmu_init()
{
    uint64_t root_phys = RV_VIRT_TO_PHYS(&mmu_root[0]);
    mmu_root[root_phys >> 30] = 0;

    for (int t = 0; t < 4; t++)
        mmu_root[t] = PTE_PPN(RV_VIRT_TO_PHYS(&mmu_68k_l1[t][0])) | PTE_V;
    /*
        Mirror the whole window at -4 GiB..0. A sign-extended 68k address plus
        a sign-extended displacement, added in 64 bits, always lands in
        [-4 GiB, 4 GiB) and so on a mapping of the right 68k page.
    */
    for (int t = 0; t < 4; t++)
        mmu_root[ROOT_MIRROR + t] = mmu_root[t];
    sfence_all();

    kprintf("[MMU] Sv39 root at %p, m68k window 0-4G mirrored at 0xffffffff00000000\n",
        (void *)root_phys);
}

/* Return the level-2 table for a 2 MiB slot, splitting a leaf if needed. */
static uint64_t *l2_for(uint32_t virt)
{
    uint64_t *slot = &mmu_68k_l1[virt >> 30][(virt >> 21) & (ENTRIES - 1)];
    uint64_t e = *slot;

    if ((e & PTE_V) && !PTE_IS_LEAF(e))
        return (uint64_t *)RV_PHYS_TO_VIRT(PTE_ADDR(e));

    uint64_t *l2 = tlsf_malloc_aligned(tlsf, SZ_4K, SZ_4K);
    if (l2 == NULL)
        return NULL;
    for (int i = 0; i < ENTRIES; i++)
        l2[i] = (e & PTE_V) ? leaf(PTE_ADDR(e) + i * SZ_4K, e) : 0;
    *slot = PTE_PPN(RV_VIRT_TO_PHYS(l2)) | PTE_V;
    return l2;
}

void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high)
{
    (void)attr_high;

    if (virt + length > 0x100000000UL || ((phys | virt | length) & (SZ_4K - 1))) {
        kprintf("[MMU] mmu_map(%p, %p, %p): only page aligned m68k window mappings are supported\n",
            (void *)phys, (void *)virt, (void *)length);
        return;
    }

    while (length) {
        if (((phys | virt) & (SZ_2M - 1)) == 0 && length >= SZ_2M) {
            mmu_68k_l1[virt >> 30][(virt >> 21) & (ENTRIES - 1)] = leaf(phys, attr_low);
            phys += SZ_2M;
            virt += SZ_2M;
            length -= SZ_2M;
        } else {
            uint64_t *l2 = l2_for((uint32_t)virt);
            if (l2 == NULL) {
                kprintf("[MMU] out of memory for page tables\n");
                return;
            }
            l2[(virt >> 12) & (ENTRIES - 1)] = leaf(phys, attr_low);
            phys += SZ_4K;
            virt += SZ_4K;
            length -= SZ_4K;
        }
    }
    sfence_all();
}

uintptr_t mmu_virt2phys(uintptr_t addr)
{
    if (addr >= RV_LINEAR_BASE && addr < RV_LINEAR_BASE + ((uint64_t)RV_LINEAR_GIB << 30))
        return RV_VIRT_TO_PHYS(addr);

    uint32_t a = (uint32_t)addr;    /* m68k window or its mirror */
    uint64_t e = mmu_68k_l1[a >> 30][(a >> 21) & (ENTRIES - 1)];
    if (!(e & PTE_V))
        return (uintptr_t)-1;
    if (PTE_IS_LEAF(e))
        return PTE_ADDR(e) + (a & (SZ_2M - 1));
    uint64_t *l2 = (uint64_t *)RV_PHYS_TO_VIRT(PTE_ADDR(e));
    e = l2[(a >> 12) & (ENTRIES - 1)];
    if (!(e & PTE_V))
        return (uintptr_t)-1;
    return PTE_ADDR(e) + (a & (SZ_4K - 1));
}
