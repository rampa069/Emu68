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
*/

#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "riscv64/jit.h"

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

int rv_jit_interrupt(struct M68KState *ctx)
{
    int level = 0;

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
    }

    uint16_t sr = ctx->SR;
    if (level == 0 || (level != 7 && level <= ((sr >> 8) & 7)))
        return 0;

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
