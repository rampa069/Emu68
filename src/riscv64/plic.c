/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    RISC-V PLIC for supervisor external interrupts (scause 9). Register
    layout of the RISC-V PLIC specification, as QEMU's hw/intc/sifive_plic.c
    and SpinalHDL's PLIC implement it:

      0x000000 + 4 * source            priority
      0x002000 + 0x80 * context        enable bits, one per source
      0x200000 + 0x1000 * context      priority threshold
      0x200004 + 0x1000 * context      claim (read) and complete (write)

    The context is found in the PLIC node's interrupts-extended, whose
    (phandle, cause) pairs list the contexts in order: the first one with
    cause 9, the supervisor external interrupt, is this hart's S-mode
    context (one hart only).

    A claimed source runs its handler, which must quieten the device, and
    is completed; the m68k side then sees an ARM interrupt, level 6, the
    same report as a timer tick and as an unclaimed IRQ on AArch64.
*/

#include <stdint.h>
#include "support.h"
#include "devicetree.h"
#include "riscv64/mmu_rv.h"
#include "riscv64/plic.h"

void M68kReportInterrupt(int irq);

struct rv_plic_stats rv_plic_stats;

#define MAX_HANDLERS 4

static volatile uint32_t *plic;
static int context = -1;
static struct { uint32_t source; void (*fn)(void); } handlers[MAX_HANDLERS];
static int nhandlers;

static uint64_t reg_address(of_node_t *node)
{
    of_property_t *p = dt_find_property(node, "reg");
    if (p == NULL || node->on_parent == NULL)
        return 0;
    int cells = dt_get_property_value_u32(node->on_parent, "#address-cells", 2, TRUE);
    const uint32_t *v = p->op_value;
    return cells == 2 ? ((uint64_t)BE32(v[0]) << 32) | BE32(v[1]) : BE32(v[0]);
}

static of_node_t *interrupt_parent(of_node_t *dev)
{
    for (of_node_t *n = dev; n; n = n->on_parent) {
        of_property_t *p = dt_find_property(n, "interrupt-parent");
        if (p && p->op_length >= 4)
            return dt_find_node_by_phandle(BE32(*(uint32_t *)p->op_value));
    }
    return NULL;
}

static int s_context(of_node_t *pl)
{
    of_property_t *p = dt_find_property(pl, "interrupts-extended");
    if (p == NULL)
        return -1;
    const uint32_t *v = p->op_value;
    for (uint32_t i = 0; i + 1 < p->op_length / 4; i += 2)
        if (BE32(v[i + 1]) == 9)
            return (int)(i / 2);
    return -1;
}

int rv_plic_attach(of_node_t *dev, void (*handler)(void))
{
    of_property_t *irq = dev ? dt_find_property(dev, "interrupts") : NULL;
    of_node_t *pl = dev ? interrupt_parent(dev) : NULL;
    if (irq == NULL || irq->op_length < 4 || pl == NULL || nhandlers == MAX_HANDLERS) {
        kprintf("[PLIC] %s: no interrupt or no interrupt parent\n", dev ? dev->on_name : "?");
        return -1;
    }
    uint32_t source = BE32(*(uint32_t *)irq->op_value);

    if (plic == NULL) {
        uint64_t base = reg_address(pl);
        context = s_context(pl);
        if (base == 0 || context < 0) {
            kprintf("[PLIC] %s: no registers or no S-mode context\n", pl->on_name);
            return -1;
        }
        plic = (volatile uint32_t *)RV_PHYS_TO_VIRT(base);
        plic[(0x200000 + 0x1000 * context) / 4] = 0;   /* threshold */
        kprintf("[PLIC] %s at %p, S-mode context %d\n", pl->on_name, (void *)base, context);
    }

    handlers[nhandlers].source = source;
    handlers[nhandlers].fn = handler;
    nhandlers++;
    plic[source] = 1;                                   /* priority */
    plic[(0x2000 + 0x80 * context) / 4 + source / 32] |= 1u << (source % 32);
    __asm__ volatile("csrs sie, %0" :: "r"(1ul << 9));          /* SEIE */
    __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 1));      /* SIE */
    kprintf("[PLIC] source %d for %s\n", (int)source, dev->on_name);
    return (int)source;
}

int rv_plic_active(void)
{
    return nhandlers != 0;
}

void rv_plic_interrupt(void)
{
    volatile uint32_t *claim = &plic[(0x200004 + 0x1000 * context) / 4];
    int any = 0;
    for (;;) {
        uint32_t id = *claim;
        if (id == 0)
            break;
        for (int i = 0; i < nhandlers; i++)
            if (handlers[i].source == id)
                handlers[i].fn();
        *claim = id;                                    /* complete */
        rv_plic_stats.claims++;
        any = 1;
    }
    if (any)
        M68kReportInterrupt(1);
    else
        rv_plic_stats.spurious++;
}
