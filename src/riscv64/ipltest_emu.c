/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    IPL test register, a test fixture with no counterpart on real hardware.
    A write sets the external interrupt level (ctx->INTF.IPL, what PiStorm
    reads from the Amiga's IPL lines) to bits 2-0 of the least significant
    byte of the m68k value, and it stays there until written again. Reads
    return the current level.

    The TG68K.C testbench (tools/tg68k/tb_tg68k.vhd in the testbed) drives
    the core's IPL inputs from a register at the same address, so programs
    can raise interrupts at a known point on both and be compared.
*/

#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "riscv64/mmio.h"
#include "riscv64/jit.h"

static uint64_t ipl_read(struct mmio_device *dev, uint32_t offset, int size)
{
    (void)dev; (void)offset; (void)size;
    return rv_m68k_ctx ? rv_m68k_ctx->INTF.IPL : 0;
}

static void ipl_write(struct mmio_device *dev, uint32_t offset, int size, uint64_t value)
{
    (void)dev; (void)offset;
    /* the value arrives in host (little-endian) order: the m68k low byte is the last one */
    uint8_t low = (uint8_t)(value >> (8 * (size - 1)));
    if (rv_m68k_ctx) {
        rv_jit_ipl_changed(rv_m68k_ctx->INTF.IPL, low & 7);
        rv_m68k_ctx->INTF.IPL = low & 7;
    }
}

void ipltest_emu_init(struct mmio_device *dev, uint32_t base)
{
    dev->name = "IPL test register (emulated)";
    dev->base = base;
    dev->size = 0x1000;
    dev->read = ipl_read;
    dev->write = ipl_write;
    dev->priv = NULL;
    dev->reads = dev->writes = 0;
}
