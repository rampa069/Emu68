/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Platform set-up for qemu-system-riscv64 -M virt.

    m68k programs written for the RasPi and the AArch64 virt target print
    through a PL011 at 0xf2201000. QEMU riscv virt has an NS16550 instead, so
    that page is left unmapped and an emulated PL011 (src/riscv64/pl011_emu.c)
    answers the faults and forwards characters to the console.
*/

#include <stdint.h>
#include "support.h"
#include "riscv64/mmio.h"

#define M68K_PL011_ADDR 0xf2201000

void virt_console_putc(char c);

static struct mmio_device pl011;

void platform_init()
{
    pl011_emu_init(&pl011, M68K_PL011_ADDR, virt_console_putc);
    mmio_register(&pl011);
}

void platform_post_init()
{
}
