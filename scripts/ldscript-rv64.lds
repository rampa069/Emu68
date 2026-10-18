/*
    RV64 kernel image. OpenSBI enters at the physical address 0x80200000;
    the image is linked at its linear-map address (see
    include/riscv64/mmu_rv.h) and runs position independently until the MMU
    is on.
*/
OUTPUT_ARCH(riscv)
ENTRY(_start)

PHYS_BASE = 0x80200000;
VIRT_BASE = 0xffffffc080200000;

SECTIONS
{
    . = VIRT_BASE;
    __image_start = .;

    .text : AT(PHYS_BASE) {
        *(.startup)
        *(.text .text.*)
    }
    .rodata : ALIGN(4096) {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(32);
        __init_start = .;
        *(.init_array .init_array.* .init)
        QUAD(0)
        . = ALIGN(32);
        __boards_start = .;
        *(.boards.z2) *(.boards.z3) *(.boards)
        QUAD(0)
    }
    .data : ALIGN(4096) { *(.data .data.* .sdata .sdata.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        __bss_end = .;
    }
    .mmu (NOLOAD) : ALIGN(4096) { *(.mmu) }
    . = ALIGN(16);
    . += 64K;
    __stack_top = .;
    . = ALIGN(4096);
    __image_end = .;

    /DISCARD/ : { *(.comment) *(.note.GNU-stack) *(.eh_frame*) *(.riscv.attributes) }
}
