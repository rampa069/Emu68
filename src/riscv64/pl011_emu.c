/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Minimal ARM PL011 model for m68k programs written for the RasPi and the
    AArch64 virt target, which print by polling UARTFR and writing UARTDR.
    Register indices, flag bits, reset values and the identification bytes
    follow QEMU's hw/char/pl011.c. Transmission is immediate and nothing is
    ever received, so TXFE and RXFE always read as set.
*/

#include <stdint.h>
#include "support.h"
#include "riscv64/mmio.h"

#define FR_RXFE 0x10
#define FR_TXFF 0x20
#define FR_TXFE 0x80

static const uint8_t pl011_id[8] = { 0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };

struct pl011 {
    void (*putc)(char);
    uint32_t reg[19];   /* indexed by offset >> 2, up to UARTDMACR */
};

static uint32_t read_reg(struct pl011 *s, uint32_t index)
{
    if (index >= 0x3f8 && index <= 0x3ff)
        return pl011_id[index - 0x3f8];
    switch (index) {
    case 0:  return 0;                      /* UARTDR: no receive data */
    case 6:  return FR_TXFE | FR_RXFE;      /* UARTFR */
    case 16: return s->reg[15] & s->reg[14];   /* UARTMIS = RIS & IMSC */
    default: return index < 19 ? s->reg[index] : 0;
    }
}

static uint64_t pl011_read(struct mmio_device *dev, uint32_t offset, int size)
{
    struct pl011 *s = dev->priv;
    uint32_t v = read_reg(s, offset >> 2) >> (8 * (offset & 3));
    return size >= 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void pl011_write(struct mmio_device *dev, uint32_t offset, int size, uint64_t value)
{
    struct pl011 *s = dev->priv;
    uint32_t index = offset >> 2;
    (void)size;

    if (index == 0) {                       /* UARTDR */
        if ((offset & 3) == 0)
            s->putc((char)(value & 0xff));
        return;
    }
    if (index == 17) {                      /* UARTICR clears RIS bits */
        s->reg[15] &= ~(uint32_t)value;
        return;
    }
    if (index < 19 && index != 6 && index != 15 && index != 16)
        s->reg[index] = (uint32_t)value;
}

void pl011_emu_init(struct mmio_device *dev, uint32_t base, void (*putc)(char))
{
    static struct pl011 state;

    state.putc = putc;
    for (int i = 0; i < 19; i++)
        state.reg[i] = 0;
    state.reg[12] = 0x300;                  /* UARTCR: TXE | RXE, as QEMU */
    state.reg[13] = 0x12;                   /* UARTIFLS */

    dev->name = "PL011 (emulated)";
    dev->base = base;
    dev->size = 0x1000;
    dev->read = pl011_read;
    dev->write = pl011_write;
    dev->priv = &state;
    dev->reads = dev->writes = 0;
}
