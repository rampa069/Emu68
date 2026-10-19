#ifndef _EMU68_RISCV64_PLIC_H
#define _EMU68_RISCV64_PLIC_H

#include <stdint.h>
#include "devicetree.h"

/*
    Supervisor external interrupts through a RISC-V PLIC (src/riscv64/plic.c).
    rv_plic_attach finds the PLIC that serves dev (its interrupt-parent),
    enables dev's first interrupt for this hart's S-mode context and calls
    handler for it from the trap handler; it returns the source number or
    -1. Every claimed source is reported to the m68k side as an ARM
    interrupt (M68kReportInterrupt(1), level 6).
*/
int rv_plic_attach(of_node_t *dev, void (*handler)(void));
void rv_plic_interrupt(void);
int rv_plic_active(void);

struct rv_plic_stats {
    uint64_t claims;            /* sources claimed */
    uint64_t spurious;          /* external interrupts with nothing to claim */
};
extern struct rv_plic_stats rv_plic_stats;

#endif
