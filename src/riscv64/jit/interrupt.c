/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    m68k interrupt entry, called by the dispatcher between translation
    units when ctx->INT64 is not zero, with the m68k state in the context.
    It follows the interrupt block of MainLoop (src/ExecutionLoop.c):

      level  7 for INTF.ARM_err (cleared here), else 6 for INTF.ARM, 2 for
             INTF.PPC, raised to INTF.IPL (the external IPL lines) if higher
      taken  when level is 7 or above the SR interrupt mask
      frame  format $0 with the autovector offset 0x60 + 4 * level, the PC
             of the next instruction and the SR before the interrupt, on
             the ISP (or the MSP when M is set) when coming from user mode
      SR     S set, T1/T0 cleared, mask set to level; M is left alone, as
             in Emu68 (a 68020 would also push a throwaway frame and clear M)

    The sources keep their flags set until the device is acknowledged, as
    level-triggered interrupt lines do; only ARM_err is cleared here.

    Level 7 from the IPL lines is edge-triggered, as the M68000 User's
    Manual describes: a rise to 7 (reported by the source through
    rv_jit_ipl_changed) is taken once even under mask 7, and a held 7 is
    only taken again through the normal comparison, when the mask is below
    7. Emu68 on AArch64 takes a held 7 again after every unit, which nests
    the handler; this follows the hardware (and TG68K.C once fixed).
*/

#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "riscv64/jit.h"
#include "riscv64/timer.h"

static inline volatile uint8_t *m68k_ptr(uint32_t a)
{
    return (volatile uint8_t *)(intptr_t)(int32_t)a;
}

static void write16(uint32_t a, uint16_t v)
{
    m68k_ptr(a)[0] = (uint8_t)(v >> 8);
    m68k_ptr(a + 1)[0] = (uint8_t)v;
}

static void write32(uint32_t a, uint32_t v)
{
    write16(a, (uint16_t)(v >> 16));
    write16(a + 2, (uint16_t)v);
}

static uint32_t read32(uint32_t a)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = (v << 8) | m68k_ptr(a + i)[0];
    return v;
}

struct rv_irq_stats rv_irq_stats;
static int nmi_pending;         /* a rise of INTF.IPL to 7 not yet taken */

void rv_jit_ipl_changed(uint8_t old_level, uint8_t new_level)
{
    if (new_level == 7 && old_level != 7)
        nmi_pending = 1;
}
struct M68KState *rv_m68k_ctx;

/*
    As in src/ExecutionLoop.c: irq 1 is an ARM-side interrupt (level 6 for
    the m68k), irq 2 an ARM error (level 7). Called from interrupt context.
*/
void M68kReportInterrupt(int irq)
{
    struct M68KState *ctx = rv_m68k_ctx;
    if (!ctx)
        return;
    if (irq == 1)
        ctx->INTF.ARM = 1;
    else if (irq == 2)
        ctx->INTF.ARM_err = 1;
}

int rv_jit_interrupt(struct M68KState *ctx)
{
    int level = 0, ipl7 = 0;

    if (ctx->INTF.ARM_err) {
        level = 7;
        ctx->INTF.ARM_err = 0;
    } else {
        if (ctx->INTF.ARM)
            level = 6;
        else if (ctx->INTF.PPC)
            level = 2;
        if (ctx->INTF.IPL > level)
            level = ctx->INTF.IPL & 7;
        ipl7 = level == 7;
    }

    uint16_t sr = ctx->SR;
    int mask = (sr >> 8) & 7;
    if (level == 0)
        return 0;
    if (ipl7) {
        if (mask == 7 && !nmi_pending)
            return 0;                   /* a held 7 under mask 7 */
        nmi_pending = 0;
    } else if (level != 7 && level <= mask) {
        return 0;
    }

    if (!(sr & SR_S)) {
        ctx->USP.u32 = ctx->A[7].u32;
        ctx->A[7].u32 = (sr & SR_M) ? ctx->MSP.u32 : ctx->ISP.u32;
    }
    uint32_t vector = 0x60 + 4 * level;
    uint32_t sp = ctx->A[7].u32 - 8;
    write16(sp, sr);
    write32(sp + 2, ctx->PC);
    write16(sp + 6, (uint16_t)vector);              /* format 0 */
    ctx->A[7].u32 = sp;

    ctx->SR = (uint16_t)(((sr | SR_S) & ~(SR_T0 | SR_T1) & ~SR_IPL) | (level << 8));
    ctx->PC = read32(ctx->VBR + vector);
    rv_irq_stats.taken[level]++;
    return level;
}

/*
    After STOP: wait until an interrupt that the new SR does not mask has
    been taken. Unlike Emu68 on AArch64, which goes on after STOP as soon as
    any interrupt is flagged, a masked request keeps the CPU stopped, as on
    a real 68k. The check and the wfi run with SIE clear, so a tick between
    them still ends the wfi (it waits for pending enabled interrupts, not for
    the handler); the handler runs once SIE is set again.
*/
int rv_jit_stop_wait(struct M68KState *ctx)
{
    rv_irq_stats.stops++;
    for (;;) {
        __asm__ volatile("csrc sstatus, %0" :: "r"(1ul << 1) : "memory");
        if (ctx->INT64 && rv_jit_interrupt(ctx))
            break;
        if (!rv_timer_running()) {
            kprintf("[JIT] STOP at %08x with no interrupt pending and no interrupt source\n", ctx->PC);
            return -1;
        }
        __asm__ volatile("wfi" ::: "memory");
        __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 1) : "memory");
    }
    if (rv_timer_running())
        __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 1) : "memory");
    return 1;
}
