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

    With the serial_irq boot option the console's receive interrupt is
    enabled through the PLIC: received bytes reach the m68k side through
    the emulated PL011, and each interrupt is reported as an ARM interrupt
    (level 6). Without it the console never interrupts and the PL011
    receives nothing, as before.
*/

#include <stdint.h>
#include "support.h"
#include "devicetree.h"
#include "riscv64/mmio.h"
#include "riscv64/plic.h"

#define M68K_PL011_ADDR     0xf2201000
#define M68K_SYSTIMER_ADDR  0xf2003000
#define M68K_IPLTEST_ADDR   0xf2fff000      /* test fixture, see ipltest_emu.c */

void virt_console_putc(char c);
int virt_console_getc(void);
int virt_console_rx_ready(void);
void virt_console_rx_drain(void);
void virt_console_rx_enable(void);
extern of_node_t *virt_uart_node;

static struct mmio_device pl011, systimer, ipltest;

void platform_init()
{
    of_property_t *args = dt_find_property(dt_find_node("/chosen"), "bootargs");
    int serial_irq = args && find_token(args->op_value, "serial_irq");
    if (serial_irq && rv_plic_attach(virt_uart_node, virt_console_rx_drain) >= 0)
        virt_console_rx_enable();
    else
        serial_irq = 0;
    pl011_emu_init(&pl011, M68K_PL011_ADDR, virt_console_putc,
                   serial_irq ? virt_console_getc : NULL, serial_irq ? virt_console_rx_ready : NULL);
    mmio_register(&pl011);

    uint64_t timebase = dt_get_property_value_u32(dt_find_node("/cpus"), "timebase-frequency", 10000000, FALSE);
    systimer_emu_init(&systimer, M68K_SYSTIMER_ADDR, timebase);
    mmio_register(&systimer);
    ipltest_emu_init(&ipltest, M68K_IPLTEST_ADDR);
    mmio_register(&ipltest);
}

void platform_post_init()
{
}
