/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    BCM2835 System Timer model for m68k programs written for the RasPi,
    which read the free-running 1 MHz counter (for example Dhrystone at
    0xf2003004). Register layout as QEMU's hw/timer/bcm2835_systmr.c:

      0x00 CS   match flags, writing 1 clears
      0x04 CLO  counter, low 32 bits
      0x08 CHI  counter, high 32 bits
      0x0c-0x18 C0-C3 compare registers

    The counter comes from the RISC-V time CSR scaled to 1 MHz. Compare
    matches and their interrupts are not modelled: the flags stay clear.

    Register values are little-endian at the bus, like the real peripheral;
    m68k code reads them through LE32() (see include/riscv64/mmio.h).
*/

#include <stdint.h>
#include "support.h"
#include "riscv64/mmio.h"

struct systimer {
    uint64_t timebase;          /* Hz of the time CSR */
    uint32_t cs;
    uint32_t compare[4];
};

static uint64_t counter_1mhz(const struct systimer *s)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    /* t * 1e6 / timebase without overflowing 64 bits */
    return (t / s->timebase) * 1000000 + (t % s->timebase) * 1000000 / s->timebase;
}

static uint32_t read_reg(struct systimer *s, uint32_t index)
{
    switch (index) {
    case 0: return s->cs;
    case 1: return (uint32_t)counter_1mhz(s);
    case 2: return (uint32_t)(counter_1mhz(s) >> 32);
    case 3: case 4: case 5: case 6: return s->compare[index - 3];
    default: return 0;
    }
}

static uint64_t systimer_read(struct mmio_device *dev, uint32_t offset, int size)
{
    uint32_t v = read_reg(dev->priv, offset >> 2) >> (8 * (offset & 3));
    return size >= 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void systimer_write(struct mmio_device *dev, uint32_t offset, int size, uint64_t value)
{
    struct systimer *s = dev->priv;
    uint32_t index = offset >> 2;
    (void)size;

    if (index == 0)
        s->cs &= ~(uint32_t)value;              /* acknowledge */
    else if (index >= 3 && index <= 6)
        s->compare[index - 3] = (uint32_t)value;
}

void systimer_emu_init(struct mmio_device *dev, uint32_t base, uint64_t timebase)
{
    static struct systimer state;

    state.timebase = timebase ? timebase : 10000000;
    state.cs = 0;
    for (int i = 0; i < 4; i++)
        state.compare[i] = 0;

    dev->name = "BCM2835 system timer (emulated)";
    dev->base = base;
    dev->size = 0x1000;
    dev->read = systimer_read;
    dev->write = systimer_write;
    dev->priv = &state;
    dev->reads = dev->writes = 0;
}
