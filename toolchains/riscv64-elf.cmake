set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# Homebrew and most distributions name the bare-metal toolchain riscv64-elf-*;
# riscv-gnu-toolchain builds use riscv64-unknown-elf-*.
if(NOT DEFINED CROSS_COMPILE)
    set(CROSS_COMPILE riscv64-elf)
endif()

set(CMAKE_C_COMPILER ${CROSS_COMPILE}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}-g++)
set(CMAKE_ASM_COMPILER ${CROSS_COMPILE}-gcc)
set(CMAKE_AR ${CROSS_COMPILE}-ar)
set(CMAKE_RANLIB ${CROSS_COMPILE}-ranlib)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}-objcopy)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
