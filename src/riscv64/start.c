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

    The RV64 translator does not exist yet, so the boot stops where
    M68K_StartEmu() would enter MainLoop and reports the m68k entry state.
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

#define HEAP_SIZE       (16UL << 20)
#define SZ_2M           (1UL << 21)
#define M68K_RAM_LIMIT  0xf2000000UL    /* keep the peripheral window free */

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

    /*
        Write through the m68k view of the UART at 0xf2201000, using the
        address in the sign-extended form translated code will hold.
    */
    {
        volatile uint8_t *m68k_uart = (volatile uint8_t *)(intptr_t)(int32_t)0xf2201000;
        static const char msg[] = "[BOOT] m68k window check: this line went through 0xf2201000\n";
        for (const char *c = msg; *c; c++) {
            if (*c == '\n')
                m68k_uart[0] = '\r';
            m68k_uart[0] = (uint8_t)*c;
        }
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
    int code_ok = BE32(code[0]) == 0x3e9 &&
        bytes_equal((void *)(uintptr_t)pc, &code[2], 16);

    const uint8_t *op = (const uint8_t *)(uintptr_t)pc;
    kprintf("[BOOT] Entry %08x: %02x%02x %02x%02x %02x%02x %02x%02x, %s the HUNK_CODE data\n",
        pc, op[0], op[1], op[2], op[3], op[4], op[5], op[6], op[7],
        code_ok ? "matches" : "DOES NOT match");

    uint32_t isp = ((uint32_t)(uintptr_t)hunks - 4096) & 0xfffff000;
    kprintf("[JIT] m68k entry state: PC=%08x ISP=%08x SR=2700\n", pc, isp);
    kprintf("[JIT] The RV64 translator is not available yet, stopping before MainLoop\n");

    sbi_shutdown(!code_ok);
}
