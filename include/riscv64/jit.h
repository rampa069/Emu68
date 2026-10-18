/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    RV64 translator core.

    A translation unit covers the m68k instructions from its start PC up to
    and including the first control-flow instruction, or a configurable
    number of instructions (ICNT). Units do not chain: every unit returns to
    the dispatcher (rv_jit_run), which looks up or translates the next one,
    as Emu68's MainLoop does.

    Inside translated code the m68k registers live in the host registers of
    include/riscv64/register_map.h, gp holds the PC and t2 the SR. All 32-bit
    m68k values are kept sign-extended. rv_jit_enter loads that state from
    struct M68KState before a unit and stores it back afterwards.
*/

#ifndef _EMU68_RISCV64_JIT_H
#define _EMU68_RISCV64_JIT_H

/* struct M68KState offsets used by the trampoline (checked in translator.c) */
#define RV_CTX_D0   0
#define RV_CTX_A0   32
#define RV_CTX_PC   76
#define RV_CTX_SR   80

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "M68k.h"

struct rv_jit_config {
    int icnt;           /* maximum m68k instructions per unit, >= 1 */
    int trace;          /* print an "I" trace record before every unit */
    int flag_liveness;  /* compute only the flags a later instruction may read */
};

/* Code buffer for translated code; must be writable and executable. */
void rv_jit_init(void *code, uintptr_t size, const struct rv_jit_config *cfg);

/*
    Run m68k code from ctx->PC until it returns to PC 0. Returns 0 then, or
    -1 if an instruction cannot be translated (a message has been printed and
    ctx->PC points at it).
*/
int rv_jit_run(struct M68KState *ctx);

/* Statistics of the last run */
struct rv_jit_stats {
    uint64_t units;             /* translation units created */
    uint64_t m68k_insns;        /* m68k instructions translated */
    uint64_t rv_insns;          /* RISC-V instructions emitted */
    uint64_t executed;          /* units executed */
};
extern struct rv_jit_stats rv_jit_stats;

/*
    m68k interrupts (src/riscv64/jit/interrupt.c). The dispatcher calls
    rv_jit_interrupt between units whenever ctx->INT64 is not zero, with the
    m68k state stored in the context; it returns the level taken, or 0.
*/
struct M68KState;
int rv_jit_interrupt(struct M68KState *ctx);
struct rv_irq_stats {
    uint64_t taken[8];          /* m68k interrupts taken, per level */
    uint64_t stops;             /* STOP instructions executed */
};
/* After STOP: 1 once an unmasked interrupt was taken, -1 if none can come */
int rv_jit_stop_wait(struct M68KState *ctx);
extern struct rv_irq_stats rv_irq_stats;
extern struct M68KState *rv_m68k_ctx;   /* for M68kReportInterrupt */
void M68kReportInterrupt(int irq);

/*
    Assembly trampoline (src/riscv64/jit/enter.S). With check set it returns
    the mask of m68k registers left not sign-extended by the unit.
*/
uint32_t rv_jit_enter(struct M68KState *ctx, void *code, int check);

#endif /* __ASSEMBLER__ */

#endif /* _EMU68_RISCV64_JIT_H */
