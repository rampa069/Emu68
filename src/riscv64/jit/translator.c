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
_Static_assert(offsetof(struct M68KState, INT64) == RV_CTX_INT64, "RV_CTX_INT64");

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
volatile uint32_t rv_jit_stopped;       /* set by translated STOP */
uint32_t *rv_jit_exit_site;             /* exit stub asking to be chained */

/*
    Direct-mapped table the assembly dispatcher looks units up in, indexed by
    (pc >> 1) & (RV_FAST_SIZE - 1); filled by C on a miss.
*/
#define RV_FAST_SIZE (1u << RV_FAST_BITS)
struct rv_fast { int32_t pc; uint32_t pad; uint32_t *code; };
struct rv_fast rv_jit_fast[RV_FAST_SIZE];
_Static_assert(sizeof(struct rv_fast) == 16, "rv_fast");

static struct rv_unit *cache[HASH_SIZE];
static int chain_enabled;
static uint32_t *code_base, *code_ptr, *code_end;
static struct rv_jit_config config = { .icnt = 256, .trace = 0, .flag_liveness = 1 };

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
    chain_enabled = config.chain;
    for (unsigned i = 0; i < RV_FAST_SIZE; i++)
        rv_jit_fast[i] = (struct rv_fast){ 0 };
    rv_jit_stats = (struct rv_jit_stats){ 0 };
}

/*
    Exit to a known m68k address. gp gets the target, then an exit stub:

        ld    t0, INT64(tp)     interrupt pending: plain exit, so that the
        bnez  t0, 1f            dispatcher takes it
        auipc a7, 0             not chained yet: back to the dispatcher at
        jalr  x0, 4(ra)         ra + 4 with a7 = this stub
     1: ret

    Chaining rewrites the auipc/jalr pair into auipc t0 / jalr x0 that jump
    straight into the target unit, with the m68k state still in the pinned
    registers and ra still the dispatcher's. A stub that must never be
    chained (target 0, a vector stub) gets ret instead of its jalr. Without
    chaining only li and ret are emitted.
*/
void rv_emit_exit(struct rv_tc *t, uint32_t target)
{
    rv_emit_li(t, RVR_PC, (int32_t)target);
    if (!chain_enabled)
        return;                         /* the unit ends with ret */
    rv_emit(t, rv_ld(5, RVR_CTX, RV_CTX_INT64));
    rv_emit(t, rv_bnez(5, 16));
    rv_emit(t, rv_auipc(17, 0));
    rv_emit(t, rv_jalr(0, 1, 4));
    rv_emit(t, rv_ret());
}

/*
    Translation runs twice. The first pass, with every flag live, finds the
    instructions of the unit; flags.c then works out which flags each of
    them may leave stale, and the second pass emits the code that is kept.
    A unit holds at most MAX_UNIT_INSNS instructions, whatever ICNT says.
*/
#define MAX_UNIT_INSNS 256

static uint16_t unit_opcodes[MAX_UNIT_INSNS];
static uint8_t unit_live[MAX_UNIT_INSNS];

/*
    Emit up to limit instructions from pc into a fresh context. live gives
    the flags each one must keep, or is NULL for all of them. Returns the
    number of instructions, or -1 if the first one is not supported.
*/
static int emit_insns(struct rv_tc *t, uint32_t pc, uint32_t limit, const uint8_t *live)
{
    uint32_t count = 0;

    *t = (struct rv_tc){
        .start = code_ptr, .ptr = code_ptr, .end = code_end,
        .cursor = pc, .temps = RV_TEMP_MASK,
    };
    while (!t->ended && count < limit) {
        uint32_t *mark = t->ptr;
        t->pc = t->cursor;
        uint16_t opcode = rv_fetch16(t);
        unit_opcodes[count] = opcode;
        t->live = live ? live[count] : 0x1f;

        if (!rv_emit_insn(t, opcode)) {
            if (count == 0) {
                kprintf("[JIT] opcode %04x at %08x is not supported by the RV64 translator yet\n",
                    opcode, t->pc);
                return -1;
            }
            t->ptr = mark;              /* end the unit before it */
            t->cursor = t->pc;
            t->temps = RV_TEMP_MASK;
            break;
        }
        if (t->temps != RV_TEMP_MASK) {
            kprintf("[JIT] temporary register leak after %04x at %08x\n", opcode, t->pc);
            t->temps = RV_TEMP_MASK;
        }
        count++;
    }
    return (int)count;
}

static struct rv_unit *translate(uint32_t pc)
{
    if ((uintptr_t)(code_end - code_ptr) < MAX_UNIT_WORDS) {
        kprintf("[JIT] code buffer full\n");
        return NULL;
    }

    struct rv_tc t;
    uint32_t limit = (uint32_t)config.icnt < MAX_UNIT_INSNS ? (uint32_t)config.icnt : MAX_UNIT_INSNS;
    int n = emit_insns(&t, pc, limit, NULL);
    if (n < 0)
        return NULL;
    uint32_t count = (uint32_t)n;

    if (config.flag_liveness) {
        uint32_t end_pc = t.cursor;
        rv_flags_liveness(unit_opcodes, count, unit_live);
        if (emit_insns(&t, pc, count, unit_live) != n || t.cursor != end_pc) {
            kprintf("[JIT] unit at %08x: second pass differs from the first\n", pc);
            return NULL;
        }
    }

    if (!t.ended)
        rv_emit_exit(&t, t.cursor);
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

static void fast_fill(struct rv_unit *u)
{
    struct rv_fast *f = &rv_jit_fast[(u->pc >> 1) & (RV_FAST_SIZE - 1)];
    f->pc = (int32_t)u->pc;
    f->code = u->code;
}

/* The unit for pc, translated if needed, or NULL (a message was printed) */
static struct rv_unit *get_unit(uint32_t pc)
{
    struct rv_unit *u = lookup(pc);
    if (u == NULL)
        u = translate(pc);
    return u;
}

/*
    Drop every unit. Chains only live inside the code buffer, so starting it
    again from its base also drops every chain; the fast table and the hash
    table are cleared so that no stale code address survives.
*/
void rv_jit_flush(void)
{
    for (unsigned i = 0; i < HASH_SIZE; i++) {
        for (struct rv_unit *u = cache[i], *n; u; u = n) {
            n = u->next;
            tlsf_free(tlsf, u);
        }
        cache[i] = NULL;
    }
    for (unsigned i = 0; i < RV_FAST_SIZE; i++)
        rv_jit_fast[i] = (struct rv_fast){ 0 };
    code_ptr = code_base;
    __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");
}

/*
    Chain the exit stub at site (its auipc) to the unit for pc. Returns 0, or
    -1 if pc cannot be translated.
*/
static int chain(uint32_t *site, uint32_t pc)
{
    if (pc == 0 || (pc >= RV_STUB_BASE && pc < RV_STUB_END)) {
        site[1] = rv_ret();             /* never chained: plain exit */
    } else {
        struct rv_unit *u = get_unit(pc);
        if (u == NULL)
            return -1;
        fast_fill(u);
        int64_t off = (int64_t)(intptr_t)u->code - (int64_t)(intptr_t)site;
        int32_t hi = (int32_t)((off + 0x800) >> 12);
        int32_t lo = (int32_t)(off - ((int64_t)hi << 12));
        site[0] = rv_auipc(5, (uint32_t)hi);
        site[1] = rv_jalr(0, 5, lo);
        rv_jit_stats.chains++;
    }
    __asm__ volatile("fence rw, rw\n\tfence.i" ::: "memory");
    return 0;
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

/*
    Vectors without a handler point at 0x400 + 2 * vector (set up by the boot
    code, as the TG68K testbench does). Reaching one ends the run.
*/
#define STUB_BASE   0x400
#define STUB_END    0x600

static int unhandled(uint32_t pc)
{
    int vector = (int)(pc - STUB_BASE) / 2;
    if (config.trace)
        kprintf("H exception %d\n", vector);
    kprintf("[JIT] exception %d taken with no handler installed\n", vector);
    return 2;
}

static int unsupported_exception(struct M68KState *ctx)
{
    kprintf("[JIT] exception vector %d at %08x: exceptions are not supported by the RV64 translator yet\n",
        (int)rv_jit_exception, ctx->PC);
    rv_jit_exception = 0;
    return -1;
}

/* After STOP: 0 to go on at ctx->PC, -1 if STOP can never end */
static int stopped(struct M68KState *ctx)
{
    rv_jit_stopped = 0;
    return rv_jit_stop_wait(ctx) < 0 ? -1 : 0;
}

/*
    With trace: the C loop, one unit at a time through rv_jit_enter, so that
    every unit gets its trace record and the canonical register check. Units
    are only chained when the "chain" boot option asks for it; records are
    then printed only where control comes back here.
*/
static int run_traced(struct M68KState *ctx)
{
    for (;;) {
        if (ctx->PC == 0) {
            kprintf("H pc0\n");
            return 0;
        }
        if (ctx->PC >= STUB_BASE && ctx->PC < STUB_END)
            return unhandled(ctx->PC);
        /* interrupts are taken between units, where SR is exact */
        if (ctx->INT64 && rv_jit_interrupt(ctx))
            kprintf("# interrupt\n");
        trace_record(ctx);

        struct rv_unit *u = get_unit(ctx->PC);
        if (u == NULL)
            return -1;
        uint32_t bad = rv_jit_enter(ctx, u->code, 1);
        rv_jit_stats.executed++;
        if (rv_jit_exception)
            return unsupported_exception(ctx);
        if (bad) {
            /* the translated code broke the canonical register invariant */
            kprintf("[JIT] non-canonical register mask %04x after the unit at %08x\n", bad, u->pc);
            return -1;
        }
        if (rv_jit_stopped && stopped(ctx) < 0)
            return -1;
        if (rv_jit_exit_site && chain(rv_jit_exit_site, ctx->PC) < 0)
            return -1;
    }
}

int rv_jit_run(struct M68KState *ctx)
{
    if (config.trace)
        return run_traced(ctx);

    int skip_int = 0;
    for (;;) {
        int why = rv_jit_loop(ctx, skip_int);
        /* a miss returns before any unit ran: keep the skip for the next entry */
        if (why != RV_LOOP_MISS)
            skip_int = 0;
        rv_jit_stats.dispatches++;
        switch (why) {
        case RV_LOOP_PC0:
            return 0;
        case RV_LOOP_STUB:
            return unhandled(ctx->PC);
        case RV_LOOP_INT:
            /*
                Run at least one unit before looking again: when the request
                is masked, so as not to spin, and when it was taken, so that
                the handler gets to run before a level 7 that is still held
                (it ignores the mask) is taken again, as MainLoop does.
            */
            rv_jit_interrupt(ctx);
            skip_int = 1;
            break;
        case RV_LOOP_MISS: {
            struct rv_unit *u = get_unit(ctx->PC);
            if (u == NULL)
                return -1;
            fast_fill(u);
            break;
        }
        case RV_LOOP_EXC:
            return unsupported_exception(ctx);
        case RV_LOOP_STOP:
            if (stopped(ctx) < 0)
                return -1;
            break;
        case RV_LOOP_CHAIN:
            if (chain(rv_jit_exit_site, ctx->PC) < 0)
                return -1;
            break;
        default:
            return -1;
        }
    }
}
