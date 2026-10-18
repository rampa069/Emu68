/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Emulated devices in the m68k address space.

    A device region is left unmapped in the m68k window, so every access to
    it raises a load or store page fault. The trap handler decodes the
    faulting instruction and calls the device.

    Byte order: the JIT byte-swaps every 68k access (rev8 before a store,
    after a load), so the bytes on the bus are in m68k order, exactly as on a
    real 68k bus. The value passed to and returned by a device is the
    little-endian reading of the bytes at the accessed address, which is the
    raw register value of the faulting RISC-V instruction. A little-endian
    peripheral such as the PL011 therefore needs no swapping; a big-endian
    one (Amiga custom chips) swaps by access size.
*/

#ifndef _EMU68_RISCV64_MMIO_H
#define _EMU68_RISCV64_MMIO_H

#include <stdint.h>
#include "riscv64/traps.h"

struct mmio_device {
    const char *name;
    uint32_t base;      /* m68k address, page aligned */
    uint32_t size;      /* bytes, page multiple */
    uint64_t (*read)(struct mmio_device *dev, uint32_t offset, int size);
    void (*write)(struct mmio_device *dev, uint32_t offset, int size, uint64_t value);
    void *priv;
    uint64_t reads, writes;
};

/* Register a device; its range must not be mapped in the m68k window. */
int mmio_register(struct mmio_device *dev);

/*
    Called by the trap handler for load and store page faults. Returns 1 if
    the access was emulated (frame updated, instruction skipped), 0 if no
    device claims the address.
*/
int mmio_handle_fault(struct trap_frame *f, uint64_t scause, uint64_t stval);

/* Device covering an m68k address, or NULL. */
struct mmio_device *mmio_find_device(uint32_t addr);

/* PL011 model forwarding transmitted characters to putc. */
void pl011_emu_init(struct mmio_device *dev, uint32_t base, void (*putc)(char));

#endif /* _EMU68_RISCV64_MMIO_H */
