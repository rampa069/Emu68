/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Platform set-up for qemu-system-riscv64 -M virt.

    m68k programs written for the RasPi and the AArch64 virt target print
    through a PL011 at 0xf2201000 and read the BCM2835 system timer at
    0xf2003000. QEMU riscv virt has neither, so those pages are left unmapped
    and emulated devices (src/riscv64/pl011_emu.c, systimer_emu.c) answer
    the faults.
*/

#include <stdint.h>
#include "support.h"
#include "devicetree.h"
#include "riscv64/mmio.h"

#define M68K_PL011_ADDR     0xf2201000
#define M68K_SYSTIMER_ADDR  0xf2003000

void virt_console_putc(char c);

static struct mmio_device pl011, systimer;

void platform_init()
{
    pl011_emu_init(&pl011, M68K_PL011_ADDR, virt_console_putc);
    mmio_register(&pl011);

    uint64_t timebase = dt_get_property_value_u32(dt_find_node("/cpus"), "timebase-frequency", 10000000, FALSE);
    systimer_emu_init(&systimer, M68K_SYSTIMER_ADDR, timebase);
    mmio_register(&systimer);
}

void platform_post_init()
{
}
