/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    RV64 boot, the counterpart of src/aarch64/start.c.

    Entered from src/riscv64/entry.S in S-mode, running from the linear map,
    with the hart id and the device tree. It sets up the heap, parses the
    device tree, opens the console, attaches the m68k window, maps m68k RAM,
    and loads the HUNK executable passed as initrd, exactly as the AArch64
    build does without PiStorm.

    The m68k program then runs on the RV64 translator (src/riscv64/jit)
    until it returns to PC 0, and the final m68k context is printed in the
    same format as on AArch64.
*/

#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "devicetree.h"
#include "mmu.h"
#include "HunkLoader.h"
#include "cpu_relax.h"
#include "version.h"
#include "riscv64/mmu_rv.h"
#include "riscv64/mmio.h"
#include "riscv64/jit.h"
#include "riscv64/timer.h"

#define HEAP_SIZE       (16UL << 20)
#define SZ_2M           (1UL << 21)
#define M68K_RAM_LIMIT  0xf2000000UL    /* keep the peripheral window free */
#define JIT_CODE_SIZE   (4UL << 20)

extern char __image_start[], __image_end[];

void *firmware_file = NULL;
uint32_t firmware_size = 0;
struct MemoryBlock *sys_memory;

static const char bootstrapName[] = "Emu68 runtime for RV64";

static void sbi_shutdown(int failed)
{
    register long a0 __asm__("a0") = 0;             /* shutdown */
    register long a1 __asm__("a1") = failed ? 1 : 0; /* system failure */
    register long a6 __asm__("a6") = 0;
    register long a7 __asm__("a7") = 0x53525354;    /* SRST */
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    while (1)
        cpu_halt();
}

/* One or two cell address property, as written by the firmware or QEMU. */
static uint64_t dt_read_address(of_property_t *p)
{
    const uint32_t *cell = (const uint32_t *)p->op_value;
    if (p->op_length >= 8)
        return ((uint64_t)BE32(cell[0]) << 32) | BE32(cell[1]);
    return BE32(cell[0]);
}

static int bytes_equal(const void *a, const void *b, uintptr_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static uintptr_t align_up(uintptr_t v, uintptr_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/*
    m68k accesses exactly as the JIT emits them: the value is byte-swapped
    between the m68k view and the bus, and the address is held sign-extended.
*/
static uint32_t m68k_read32(intptr_t a)
{
    uint64_t v;
    __asm__ volatile("lw %0, 0(%1)\n\trev8 %0, %0\n\tsrai %0, %0, 32"
                     : "=&r"(v) : "r"(a) : "memory");
    return (uint32_t)v;
}

static void m68k_write32(intptr_t a, uint32_t v68)
{
    uint64_t t;
    __asm__ volatile("rev8 %0, %1\n\tsrli %0, %0, 32\n\tsw %0, 0(%2)"
                     : "=&r"(t) : "r"((uint64_t)v68), "r"(a) : "memory");
}

/* Compressed accesses need x8-x15, so pin the operands to a0 and a1. */
static __attribute__((noinline)) uint64_t m68k_c_lw(intptr_t addr)
{
    register intptr_t a0 __asm__("a0") = addr;
    register uint64_t a1 __asm__("a1");
    __asm__ volatile(".option push\n\t.option rvc\n\tc.lw a1, 0(a0)\n\t.option pop"
                     : "=r"(a1) : "r"(a0) : "memory");
    return a1;
}

static __attribute__((noinline)) void m68k_c_sw(intptr_t addr, uint64_t value)
{
    register intptr_t a0 __asm__("a0") = addr;
    register uint64_t a1 __asm__("a1") = value;
    __asm__ volatile(".option push\n\t.option rvc\n\tc.sw a1, 0(a0)\n\t.option pop"
                     :: "r"(a0), "r"(a1) : "memory");
}

static intptr_t m68k_addr(uint32_t a)
{
    return (intptr_t)(int32_t)a;     /* sign-extended, as in a JIT register */
}

/*
    Exercise the emulated PL011 at 0xf2201000 the way the example programs
    do (poll UARTFR, write LE32(c) to UARTDR), plus byte, signed and
    compressed accesses. Returns 1 when every check passes.
*/
static int mmio_selftest(void)
{
    const uint32_t base = 0xf2201000;
    int ok = 1;

    static const char msg[] = "[MMIO] this line was written by m68k-style code to an emulated PL011\n";
    for (const char *c = msg; *c; c++) {
        while (__builtin_bswap32(m68k_read32(m68k_addr(base + 0x18))) & 0x20)
            ;   /* the program reads FR through its LE32() helper */
        m68k_write32(m68k_addr(base), __builtin_bswap32((uint32_t)(uint8_t)*c));
    }

    /* Byte loads of the identification registers, zero and sign extended. */
    uint64_t id0, pcell3;
    __asm__ volatile("lbu %0, 0(%1)" : "=r"(id0) : "r"((uintptr_t)base + 0xfe0) : "memory");
    __asm__ volatile("lb %0, 0(%1)" : "=r"(pcell3) : "r"(m68k_addr(base + 0xffc)) : "memory");
    kprintf("[MMIO] UARTPeriphID0 %02lx, UARTPCellID3 as lb %016lx\n", id0, pcell3);
    ok &= id0 == 0x11;
    ok &= pcell3 == 0xffffffffffffffb1UL;

    /* Compressed forms: c.lw of UARTFR and c.sw to UARTDR. */
    uint64_t fr = m68k_c_lw(m68k_addr(base + 0x18));
    kprintf("[MMIO] UARTFR via c.lw %lx\n", fr);
    ok &= fr == 0x90;

    struct mmio_device *dev = mmio_find_device(base);
    uint64_t writes = dev ? dev->writes : 0;
    static const char tail[] = "[MMIO] and this one through c.sw\n";
    for (const char *c = tail; *c; c++)
        m68k_c_sw(m68k_addr(base), (uint8_t)*c);
    ok &= dev && dev->writes - writes == sizeof(tail) - 1;

    kprintf("[MMIO] self-test %s\n", ok ? "passed" : "FAILED");
    return ok;
}

void boot(uint64_t hartid, void *dtb)
{
    uintptr_t heap = align_up((uintptr_t)__image_end, 4096);
    tlsf = tlsf_init_with_memory((void *)heap, HEAP_SIZE);

    dt_parse(dtb);
    setup_serial();

    kprintf("[BOOT] Booting %s\n", bootstrapName);
    kprintf("[BOOT] %s\n", &VERSION_STRING[6]);
    kprintf("[BOOT] Hart %ld, device tree at %p, image %p-%p\n",
        hartid, dtb, __image_start, __image_end);

    of_property_t *p = dt_find_property(dt_find_node("/chosen"), "bootargs");
    if (p)
        kprintf("[BOOT] Boot args: %s\n", (const char *)p->op_value);

    p = dt_find_property(dt_find_node("/cpus/cpu@0"), "riscv,isa");
    if (p)
        kprintf("[BOOT] ISA: %s\n", (const char *)p->op_value);

    /* Physical memory: QEMU virt has a single block. */
    of_node_t *mem = dt_find_node("/memory");
    p = dt_find_property(mem, "reg");
    if (p == NULL || p->op_length < 16) {
        kprintf("[BOOT] No usable /memory node\n");
        sbi_shutdown(1);
    }
    const uint32_t *r = p->op_value;
    uint64_t ram_base = ((uint64_t)BE32(r[0]) << 32) | BE32(r[1]);
    uint64_t ram_size = ((uint64_t)BE32(r[2]) << 32) | BE32(r[3]);
    kprintf("[BOOT] RAM %p-%p (%ld MiB)\n", (void *)ram_base,
        (void *)(ram_base + ram_size - 1), (long)(ram_size >> 20));

    sys_memory = tlsf_malloc(tlsf, 2 * sizeof(struct MemoryBlock));
    sys_memory[0].mb_Base = ram_base;
    sys_memory[0].mb_Size = ram_size;
    sys_memory[1].mb_Base = 0;
    sys_memory[1].mb_Size = 0;

    /* Copy the initrd before m68k RAM is handed out, it may live there. */
    void *initrd = NULL;
    uintptr_t initrd_size = 0;
    of_node_t *chosen = dt_find_node("/chosen");
    of_property_t *ps = dt_find_property(chosen, "linux,initrd-start");
    of_property_t *pe = dt_find_property(chosen, "linux,initrd-end");
    if (ps && pe) {
        uint64_t start = dt_read_address(ps), end = dt_read_address(pe);
        initrd_size = end - start;
        initrd = tlsf_malloc(tlsf, (initrd_size + 3) & ~3);
        if (initrd == NULL) {
            kprintf("[BOOT] initrd of %ld bytes does not fit in the heap\n", (long)initrd_size);
            sbi_shutdown(1);
        }
        memcpy(initrd, (void *)RV_PHYS_TO_VIRT(start), initrd_size);
        kprintf("[BOOT] initrd %p-%p (%ld bytes) copied to %p\n",
            (void *)start, (void *)(end - 1), (long)initrd_size, initrd);
    }

    mmu_init();

    /* m68k RAM: everything above the kernel and heap, mapped at m68k 0. */
    uint64_t m68k_phys = align_up(RV_VIRT_TO_PHYS(heap + HEAP_SIZE), SZ_2M);
    uint64_t m68k_size = (ram_base + ram_size - m68k_phys) & ~(SZ_2M - 1);
    if (m68k_size > M68K_RAM_LIMIT)
        m68k_size = M68K_RAM_LIMIT;
    mmu_map(m68k_phys, 0, m68k_size, RV_MMU_RWX, 0);
    kprintf("[BOOT] m68k RAM 0x00000000-%p at physical %p (%ld MiB)\n",
        (void *)(m68k_size - 1), (void *)m68k_phys, (long)(m68k_size >> 20));

    platform_init();

    of_property_t *args = dt_find_property(dt_find_node("/chosen"), "bootargs");
    if (args && find_token(args->op_value, "mmio_selftest")) {
        if (!mmio_selftest())
            sbi_shutdown(1);
    }

    if (initrd == NULL) {
        kprintf("[BOOT] No initrd, nothing to run\n");
        sbi_shutdown(0);
    }

    uint32_t magic = BE32(*(uint32_t *)initrd);
    if (magic != 0x3f3) {
        kprintf("[BOOT] initrd is not a HUNK executable (magic %08x)\n", magic);
        sbi_shutdown(1);
    }

    uintptr_t top_of_ram = m68k_size;
    int sz = GetHunkFileSize(initrd);
    top_of_ram -= sz;
    top_of_ram &= ~0x1fffff;
    top_of_ram -= 8;

    kprintf("[BOOT] Loading HUNK executable (%d bytes in memory) at %p\n", sz, (void *)top_of_ram);
    void *hunks = LoadHunkFile(initrd, (void *)top_of_ram, NULL);
    uint32_t pc = (uint32_t)((intptr_t)hunks + 4);

    /*
        Check the first loaded hunk against the file. Without resident
        library names the header is 0x3f3, 0, table size, first, last and
        one size word per hunk, followed by the first hunk.
    */
    const uint32_t *w = initrd;
    uint32_t nhunks = BE32(w[4]) - BE32(w[3]) + 1;
    const uint32_t *code = w + 5 + nhunks;
    uint32_t code_bytes = 4 * (BE32(code[1]) & 0x3fffffff);
    int code_ok = BE32(code[0]) == 0x3e9 &&
        bytes_equal((void *)(uintptr_t)pc, &code[2], code_bytes < 16 ? code_bytes : 16);

    const uint8_t *op = (const uint8_t *)(uintptr_t)pc;
    kprintf("[BOOT] Entry %08x: %02x%02x %02x%02x %02x%02x %02x%02x, %s the HUNK_CODE data\n",
        pc, op[0], op[1], op[2], op[3], op[4], op[5], op[6], op[7],
        code_ok ? "matches" : "DOES NOT match");

    if (!code_ok)
        sbi_shutdown(1);
    if (args && find_token(args->op_value, "norun")) {
        kprintf("[JIT] norun: stopping before the translator\n");
        sbi_shutdown(0);
    }

    /*
        Vector table at 68k address 0 (VBR = 0): every vector the program
        does not set points at 0x400 + 2 * vector, which the dispatcher
        reports as an unhandled exception (the TG68K testbench does the same).
    */
    for (int v = 2; v < 256; v++) {
        volatile uint8_t *e = (volatile uint8_t *)(uintptr_t)(4 * v);
        uint32_t stub = 0x400 + 2 * v;
        e[0] = (uint8_t)(stub >> 24);
        e[1] = (uint8_t)(stub >> 16);
        e[2] = (uint8_t)(stub >> 8);
        e[3] = (uint8_t)stub;
    }

    /* m68k start state, as M68K_StartEmu sets it up without PiStorm */
    uint32_t isp = (((uint32_t)(uintptr_t)hunks - 4096) & 0xfffff000) - 4;
    *(volatile uint32_t *)(intptr_t)(int32_t)isp = 0;      /* return address 0 */

    struct M68KState *ctx = tlsf_malloc(tlsf, sizeof(*ctx));
    for (unsigned i = 0; i < sizeof(*ctx); i++)
        ((uint8_t *)ctx)[i] = 0;
    ctx->PC = pc;
    ctx->ISP.u32 = isp;
    ctx->A[7].u32 = isp;
    ctx->SR = 0x2700;

    struct rv_jit_config cfg = { .icnt = 256, .trace = 0, .flag_liveness = 1, .chain = 1 };
    if (args) {
        const char *cmdline = args->op_value;
        cfg.trace = find_token(cmdline, "trace") != NULL;
        cfg.flag_liveness = find_token(cmdline, "noliveness") == NULL;
        /* chaining is on by default, and with trace only when asked for */
        cfg.chain = find_token(cmdline, "nochain") == NULL &&
                    (!cfg.trace || find_token(cmdline, "chain") != NULL);
        const char *icnt = find_token(cmdline, "ICNT=");
        if (icnt) {
            int v = 0;
            for (const char *c = icnt + 5; *c >= '0' && *c <= '9'; c++)
                v = v * 10 + (*c - '0');
            if (v >= 1 && v <= 256)
                cfg.icnt = v;
        }
    }
    /*
        timer=<us>: periodic supervisor timer, each tick reported to the m68k
        as an ARM interrupt (level 6, acknowledged through MOVEC JITCTRL2)
    */
    uint32_t timer_us = 0;
    uint64_t timebase = dt_get_property_value_u32(dt_find_node("/cpus"), "timebase-frequency", 10000000, FALSE);
    if (args) {
        const char *tm = find_token(args->op_value, "timer=");
        if (tm)
            for (const char *c = tm + 6; *c >= '0' && *c <= '9'; c++)
                timer_us = timer_us * 10 + (uint32_t)(*c - '0');
    }
    rv_m68k_ctx = ctx;

    void *jit_code = tlsf_malloc_aligned(tlsf, JIT_CODE_SIZE, 4096);
    rv_jit_init(jit_code, JIT_CODE_SIZE, &cfg);

    kprintf("[JIT] start pc=%08x isp=%08x icnt=%d%s%s\n", pc, isp, cfg.icnt, cfg.trace ? " trace" : "",
            cfg.flag_liveness ? "" : " noliveness");
    uint64_t t0, t1, i0, i1;
    if (timer_us) {
        kprintf("[JIT] timer every %d us\n", (int)timer_us);
        rv_timer_start(timebase, timer_us);
    }
    __asm__ volatile("rdtime %0\n\trdinstret %1" : "=r"(t0), "=r"(i0));
    int status = rv_jit_run(ctx);
    __asm__ volatile("rdtime %0\n\trdinstret %1" : "=r"(t1), "=r"(i1));
    __asm__ volatile("csrc sie, %0" :: "r"(1ul << 5));     /* timer off for the report */

    if (status == 0)
        kprintf("[JIT] Back from translated code.\n");
    else
        kprintf("[JIT] Stopped at %08x.\n", ctx->PC);
    if (status == 2)
        status = 0;     /* an unhandled exception is a normal end of the program */
    kprintf("[JIT] M68K Context:\n");
    for (int i = 0; i < 8; i += 4)
        kprintf("[JIT]     D%d = 0x%08x    D%d = 0x%08x    D%d = 0x%08x    D%d = 0x%08x\n",
            i, ctx->D[i].u32, i + 1, ctx->D[i + 1].u32, i + 2, ctx->D[i + 2].u32, i + 3, ctx->D[i + 3].u32);
    for (int i = 0; i < 8; i += 4)
        kprintf("[JIT]     A%d = 0x%08x    A%d = 0x%08x    A%d = 0x%08x    A%d = 0x%08x\n",
            i, ctx->A[i].u32, i + 1, ctx->A[i + 1].u32, i + 2, ctx->A[i + 2].u32, i + 3, ctx->A[i + 3].u32);
    kprintf("[JIT]     PC = 0x%08x    SR = 0x%04x\n", ctx->PC, ctx->SR);
    kprintf("[JIT] %ld units, %ld m68k instructions translated into %ld RV64 instructions, %ld units executed\n",
        (long)rv_jit_stats.units, (long)rv_jit_stats.m68k_insns, (long)rv_jit_stats.rv_insns,
        (long)rv_jit_stats.executed);
    kprintf("[JIT] %ld returns from the dispatcher to C, %ld exits chained\n",
        (long)rv_jit_stats.dispatches, (long)rv_jit_stats.chains);
    if (rv_jit_stats.m68k_insns)
        kprintf("[JIT] Mean RV64 instructions per m68k instruction: %ld.%02ld\n",
            (long)(rv_jit_stats.rv_insns / rv_jit_stats.m68k_insns),
            (long)(rv_jit_stats.rv_insns * 100 / rv_jit_stats.m68k_insns % 100));
    kprintf("[JIT] Time in m68k code: %ld us\n", (long)((t1 - t0) * 1000000 / timebase));
    if (timer_us || rv_irq_stats.stops) {
        kprintf("[JIT] Timer ticks: %ld, STOPs: %ld, interrupts taken:", (long)rv_timer_ticks,
            (long)rv_irq_stats.stops);
        for (int l = 1; l < 8; l++)
            if (rv_irq_stats.taken[l])
                kprintf(" level %d x %ld", l, (long)rv_irq_stats.taken[l]);
        kprintf("\n");
    }
    /* instret: the exact count under QEMU only with -icount */
    kprintf("[JIT] RV64 instructions retired during the run: %ld\n", (long)(i1 - i0));

    sbi_shutdown(status != 0);
}
