/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Fault-driven MMIO emulation for the m68k window, the RV64 counterpart
    of SYSPageFaultReadHandler and SYSPageFaultWriteHandler in
    src/aarch64/vectors.c. See include/riscv64/mmio.h for the byte order.
*/

#include <stdint.h>
#include "support.h"
#include "riscv64/insn_decode.h"
#include "riscv64/mmio.h"

#define CAUSE_LOAD_PAGE_FAULT   13
#define CAUSE_STORE_PAGE_FAULT  15
#define MAX_DEVICES             16

static struct mmio_device *devices[MAX_DEVICES];
static int device_count;

int mmio_register(struct mmio_device *dev)
{
    if (device_count == MAX_DEVICES)
        return -1;
    devices[device_count++] = dev;
    kprintf("[MMIO] %s at %08x-%08x\n", dev->name, dev->base, dev->base + dev->size - 1);
    return 0;
}

static struct mmio_device *find(uint32_t addr, int size)
{
    for (int i = 0; i < device_count; i++) {
        struct mmio_device *d = devices[i];
        if (addr >= d->base && (uint64_t)addr + size <= (uint64_t)d->base + d->size)
            return d;
    }
    return NULL;
}

struct mmio_device *mmio_find_device(uint32_t addr)
{
    return find(addr, 1);
}

int mmio_handle_fault(struct trap_frame *f, uint64_t scause, uint64_t stval)
{
    /* The m68k window is 0-4 GiB, plus its sign-extended mirror. */
    if (stval > 0xffffffffUL && stval < 0xffffffff80000000UL)
        return 0;
    uint32_t addr = (uint32_t)stval;

    const uint16_t *pc = (const uint16_t *)f->sepc;
    struct rv_mem_insn d;
    if (!rv_decode_mem(pc[0], (pc[0] & 3) == 3 ? pc[1] : 0, &d))
        return 0;
    if (d.is_store != (scause == CAUSE_STORE_PAGE_FAULT))
        return 0;

    struct mmio_device *dev = find(addr, d.size);
    if (dev == NULL)
        return 0;

    uint32_t offset = addr - dev->base;
    if (d.is_store) {
        uint64_t v = f->x[d.reg];
        if (d.size < 8)
            v &= (1UL << (8 * d.size)) - 1;
        dev->write(dev, offset, d.size, v);
        dev->writes++;
    } else {
        uint64_t v = dev->read(dev, offset, d.size);
        if (d.size < 8) {
            int shift = 64 - 8 * d.size;
            v = d.is_signed ? (uint64_t)((int64_t)(v << shift) >> shift)
                            : (v << shift) >> shift;
        }
        if (d.reg != 0)
            f->x[d.reg] = v;
        dev->reads++;
    }
    f->sepc += d.length;
    return 1;
}
