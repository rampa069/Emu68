/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Platform set-up for qemu-system-riscv64 -M virt.

    As on the AArch64 virt target, the console UART is also mapped into the
    m68k address space at 0xf2201000. That address is above 0x80000000, so
    it is reached through the sign-extension mirror, which makes it a useful
    end-to-end check of the m68k window.
*/

#include <stdint.h>
#include "support.h"
#include "mmu.h"
#include "riscv64/mmu_rv.h"

extern uintptr_t virt_uart_phys;

#define M68K_UART_ADDR 0xf2201000

void platform_init()
{
    if (virt_uart_phys)
        mmu_map(virt_uart_phys & ~0xfffUL, M68K_UART_ADDR, 0x1000, RV_MMU_RW, 0);
}

void platform_post_init()
{
}
