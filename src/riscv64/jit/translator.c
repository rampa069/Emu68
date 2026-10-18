/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    RV64 translator core: translation unit cache and the dispatch loop. See
    include/riscv64/jit.h.
*/

#include <stddef.h>
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "riscv64/jit.h"
#include "jit_internal.h"

_Static_assert(offsetof(struct M68KState, D) == RV_CTX_D0, "RV_CTX_D0");
_Static_assert(offsetof(struct M68KState, A) == RV_CTX_A0, "RV_CTX_A0");
_Static_assert(offsetof(struct M68KState, PC) == RV_CTX_PC, "RV_CTX_PC");
_Static_assert(offsetof(struct M68KState, SR) == RV_CTX_SR, "RV_CTX_SR");

#define HASH_BITS   12
#define HASH_SIZE   (1u << HASH_BITS)
#define MAX_UNIT_WORDS 4096     /* RISC-V instructions one unit may need at most */

struct rv_unit {
    struct rv_unit *next;
    uint32_t pc;
    uint32_t m68k_insns;
    uint32_t *code;
};

struct rv_jit_stats rv_jit_stats;
volatile uint32_t rv_jit_exception;

static struct rv_unit *cache[HASH_SIZE];
static uint32_t *code_base, *code_ptr, *code_end;
static struct rv_jit_config config = { .icnt = 256, .trace = 0 };

static uint32_t hash(uint32_t pc)
{
    return ((pc >> 1) ^ (pc >> (1 + HASH_BITS))) & (HASH_SIZE - 1);
}

void rv_jit_init(void *code, uintptr_t size, const struct rv_jit_config *cfg)
{
    code_base = code_ptr = code;
    code_end = (uint32_t *)((uintptr_t)code + size);
    for (unsigned i = 0; i < HASH_SIZE; i++)
        cache[i] = NULL;
    if (cfg)
        config = *cfg;
    if (config.icnt < 1)
        config.icnt = 1;
    rv_jit_stats = (struct rv_jit_stats){ 0 };
}

static struct rv_unit *translate(uint32_t pc)
{
    if ((uintptr_t)(code_end - code_ptr) < MAX_UNIT_WORDS) {
        kprintf("[JIT] code buffer full\n");
        return NULL;
    }

    struct rv_tc t = {
        .start = code_ptr, .ptr = code_ptr, .end = code_end,
        .cursor = pc, .temps = RV_TEMP_MASK,
    };
    uint32_t count = 0;

    while (!t.ended && count < (uint32_t)config.icnt) {
        uint32_t *mark = t.ptr;
        t.pc = t.cursor;
        uint16_t opcode = rv_fetch16(&t);

        if (!rv_emit_insn(&t, opcode)) {
            if (count == 0) {
                kprintf("[JIT] opcode %04x at %08x is not supported by the RV64 translator yet\n",
                    opcode, t.pc);
                return NULL;
            }
            t.ptr = mark;               /* end the unit before it */
            t.cursor = t.pc;
            t.temps = RV_TEMP_MASK;
            break;
        }
        if (t.temps != RV_TEMP_MASK) {
            kprintf("[JIT] temporary register leak after %04x at %08x\n", opcode, t.pc);
            t.temps = RV_TEMP_MASK;
        }
        count++;
    }

    if (!t.ended)
        rv_emit_li(&t, RVR_PC, (int32_t)t.cursor);
    rv_emit(&t, rv_ret());
    if (t.overflow) {
        kprintf("[JIT] unit at %08x overflowed the code buffer\n", pc);
        return NULL;
    }

    /* The unit is written through the data side and executed on this hart */
    __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");

    struct rv_unit *u = tlsf_malloc(tlsf, sizeof(*u));
    if (u == NULL)
        return NULL;
    u->pc = pc;
    u->m68k_insns = count;
    u->code = t.start;
    code_ptr = t.ptr;

    uint32_t h = hash(pc);
    u->next = cache[h];
    cache[h] = u;

    rv_jit_stats.units++;
    rv_jit_stats.m68k_insns += count;
    rv_jit_stats.rv_insns += (uint64_t)(t.ptr - t.start);
    return u;
}

static struct rv_unit *lookup(uint32_t pc)
{
    for (struct rv_unit *u = cache[hash(pc)]; u; u = u->next)
        if (u->pc == pc)
            return u;
    return NULL;
}

/* One line of docs/trace-format.md (testbed repository) */
static void trace_record(const struct M68KState *ctx)
{
    kprintf("I %08x %04x", ctx->PC, ctx->SR);
    for (int i = 0; i < 8; i++)
        kprintf(" %08x", ctx->D[i].u32);
    for (int i = 0; i < 8; i++)
        kprintf(" %08x", ctx->A[i].u32);
    kprintf("\n");
}

int rv_jit_run(struct M68KState *ctx)
{
    for (;;) {
        if (ctx->PC == 0) {
            if (config.trace)
                kprintf("H pc0\n");
            return 0;
        }
        if (config.trace)
            trace_record(ctx);

        struct rv_unit *u = lookup(ctx->PC);
        if (u == NULL && (u = translate(ctx->PC)) == NULL)
            return -1;
        uint32_t bad = rv_jit_enter(ctx, u->code, config.trace);
        rv_jit_stats.executed++;
        if (rv_jit_exception) {
            kprintf("[JIT] exception vector %d at %08x: exceptions are not supported by the RV64 translator yet\n",
                (int)rv_jit_exception, ctx->PC);
            rv_jit_exception = 0;
            return -1;
        }
        if (bad) {
            /* the translated code broke the canonical register invariant */
            kprintf("[JIT] non-canonical register mask %04x after the unit at %08x\n", bad, u->pc);
            return -1;
        }
    }
}
