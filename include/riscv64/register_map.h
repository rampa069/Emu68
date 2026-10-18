/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Host register map of the RV64 backend.

    The m68k integer registers and the emulator's fixed state are pinned to
    host registers for the whole time translated code runs, in the same way
    the AArch64 backend pins x13-x29 (see include/M68k.h). Values are numeric
    register indices (x0-x31) for the encoders.

    Rules
    - 68k data and address registers are kept sign-extended from bit 31,
      which is what RV64 W-form arithmetic produces. Sign extension preserves
      unsigned order between 32-bit values, so sltu gives the 32-bit carry
      and borrow directly, and the Sv39 layout mirrors 68k 0x80000000 to
      0xffffffff in the top 2 GiB so a sign-extended address is a valid
      pointer (docs/porting-map.md in the testbed repository, section 2.2).
    - D0-D7 and A0-A3 use the callee-saved s registers, so C code called from
      translated code preserves them without help.
    - A4-A7 use t3-t6, which are caller-saved: they must be saved around
      calls into C, as REG_PROTECT does on AArch64.
    - s0 and s1 (D0, D1) and a0-a5 (temporaries) are in the RVC register
      window x8-x15, so the encoders can use compressed forms for them.
    - t2 caches the 68k SR inside translated code.
    - gp holds the 68k PC. The whole image is built with -mno-relax, so the
      linker never uses gp for relaxation.
    - tp holds the pointer to struct M68KState, which also makes the context
      reachable from the trap handler.

    Building C code that runs with these registers live needs
    -ffixed-x3 -ffixed-x4 (gp, tp) for every file, and in addition
    -ffixed-x28 ... -ffixed-x31 (t3-t6) for code that may run with the 68k
    registers loaded and is not wrapped by a register save frame. GCC accepts
    ABI names as well, Clang only accepts the xN form.
*/

#ifndef _EMU68_RISCV64_REGISTER_MAP_H
#define _EMU68_RISCV64_REGISTER_MAP_H

/* Fixed ABI registers */
#define RV_ZERO     0
#define RV_RA       1
#define RV_SP       2
#define RV_GP       3
#define RV_TP       4

/* 68k data registers: s0-s1 (x8-x9), s2-s7 (x18-x23) */
#define RVR_D0      8
#define RVR_D1      9
#define RVR_D2      18
#define RVR_D3      19
#define RVR_D4      20
#define RVR_D5      21
#define RVR_D6      22
#define RVR_D7      23

/* 68k address registers: s8-s11 (x24-x27), t3-t6 (x28-x31) */
#define RVR_A0      24
#define RVR_A1      25
#define RVR_A2      26
#define RVR_A3      27
#define RVR_A4      28
#define RVR_A5      29
#define RVR_A6      30
#define RVR_A7      31

/* Emulator state */
#define RVR_PC      RV_GP       /* 68k program counter */
#define RVR_CTX     RV_TP       /* struct M68KState * */

/*
    t2 (x7) caches the m68k SR (with the CCR in bits 4..0) while translated
    code runs; it is loaded and stored around each translation unit.
*/
#define RVR_SR      7

/*
    Temporaries for the register allocator: t0-t1 (x5-x6) and a0-a7
    (x10-x17). a0-a7 double as argument registers for calls into C, so the
    allocator hands them out last.
*/
#define RV_TEMP_MASK    ((1u << 5) | (1u << 6) | (0xffu << 10))
#define RV_TEMP_COUNT   10

/* 68k registers that live in caller-saved host registers. */
#define RV_M68K_CALLER_SAVED_MASK \
    ((1u << RVR_A4) | (1u << RVR_A5) | (1u << RVR_A6) | (1u << RVR_A7))

/* Host registers holding 68k state, for the save frame around C calls. */
#define RV_M68K_REG_MASK \
    ((1u << RVR_D0) | (1u << RVR_D1) | (0x3fu << RVR_D2) | \
     (0xffu << RVR_A0))

static inline __attribute__((always_inline)) int RV_MapM68kDataRegister(int dn)
{
    static const unsigned char map[8] = {
        RVR_D0, RVR_D1, RVR_D2, RVR_D3, RVR_D4, RVR_D5, RVR_D6, RVR_D7
    };
    return map[dn & 7];
}

static inline __attribute__((always_inline)) int RV_MapM68kAddressRegister(int an)
{
    return RVR_A0 + (an & 7);
}

/*
    Condition codes. The AArch64 backend swaps C and V (SR_Calt, SR_Valt in
    include/M68k.h) to suit mrs/msr NZCV. RISC-V has no flags register, so
    the RV64 backend keeps the native 68k layout in bits 4..0 of SR.
*/
#define RV_CCR_C    0x01
#define RV_CCR_V    0x02
#define RV_CCR_Z    0x04
#define RV_CCR_N    0x08
#define RV_CCR_X    0x10
#define RV_CCR_MASK 0x1f

#endif /* _EMU68_RISCV64_REGISTER_MAP_H */
