# RV64 build of Emu68 (TARGET=riscv-virt), included from CMakeLists.txt.
#
# This is a separate, self-contained path so the AArch64 build stays as it is.
# Only the runtime is built so far; the m68k translator follows once the RV64
# emitters exist (see include/riscv64/).

enable_language(ASM)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_EXTENSIONS ON)

configure_file(include/version.h.in include/version.h @ONLY)

set(RV64_MARCH "rv64gc_zba_zbb_zbs_zifencei" CACHE STRING "Target ISA for the RV64 build")

set(RV64_FILES
    src/riscv64/entry.S
    src/riscv64/trap_entry.S
    src/riscv64/start.c
    src/riscv64/mmu.c
    src/riscv64/traps.c
    src/riscv64/timer.c
    src/riscv64/ipltest_emu.c
    src/riscv64/cache.c
    src/riscv64/insn_decode.c
    src/riscv64/mmio.c
    src/riscv64/pl011_emu.c
    src/riscv64/systimer_emu.c
    src/riscv64/jit/enter.S
    src/riscv64/jit/loop.S
    src/riscv64/jit/translator.c
    src/riscv64/jit/codegen.c
    src/riscv64/jit/ops.c
    src/riscv64/jit/shift.c
    src/riscv64/jit/muldiv.c
    src/riscv64/jit/negx.c
    src/riscv64/jit/exception.c
    src/riscv64/jit/sr.c
    src/riscv64/jit/flags.c
    src/riscv64/jit/bcd.c
    src/riscv64/jit/cas.c
    src/riscv64/jit/bitfield.c
    src/riscv64/jit/misc.c
    src/riscv64/jit/chk2.c
    src/riscv64/jit/cache040.c
    src/riscv64/jit/interrupt.c
    src/riscv-virt/start_virt.c
    src/riscv-virt/support_virt.c
    src/support.c
    src/tlsf.c
    src/devicetree.c
    src/HunkLoader.c
    src/findtoken.c
)

add_executable(Emu68.elf ${RV64_FILES})

target_include_directories(Emu68.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
)

# gp holds the m68k PC and tp the context pointer (include/riscv64/register_map.h),
# so the compiler must never use them and the linker must not relax to gp.
target_compile_options(Emu68.elf PRIVATE
    -march=${RV64_MARCH} -mabi=lp64d -mcmodel=medany -mno-relax
    -ffixed-x3 -ffixed-x4
    -ffreestanding -fno-builtin -fno-jump-tables -fno-pic -fno-pie
    -fno-stack-protector -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
    -fomit-frame-pointer -O2 -Wall -Wextra -Werror
)

set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/ldscript-rv64.lds)
target_link_options(Emu68.elf PRIVATE
    -march=${RV64_MARCH} -mabi=lp64d -nostdlib -nostartfiles -static
    -Wl,--no-relax -Wl,--build-id=none -Wl,-Map,${CMAKE_BINARY_DIR}/Emu68.map
    -T ${LINKER_SCRIPT}
)
set_target_properties(Emu68.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})
target_link_libraries(Emu68.elf PRIVATE gcc)

add_custom_command(TARGET Emu68.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${CMAKE_BINARY_DIR}/Emu68.elf ${CMAKE_BINARY_DIR}/Emu68.img
)
