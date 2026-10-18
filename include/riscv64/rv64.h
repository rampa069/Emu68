/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    RV64 instruction encoders for the RISC-V backend, the counterpart of
    include/A64.h.

    Every encoder returns one 32-bit instruction word, ready to be stored in
    the code buffer. RISC-V fetches instructions little-endian whatever the
    data endianness, so RV_INSN() byte-swaps on a big-endian-data host and is
    the identity otherwise.

    Conventions
    - Registers are numeric indices 0-31 (see include/riscv64/register_map.h
      for the names used by the JIT).
    - Branch and jump offsets are in bytes, relative to the branch itself,
      as in the RISC-V assembler. B-type offsets must be even and within
      +-4 KiB, J-type offsets even and within +-1 MiB; use rv_b_in_range()
      and rv_j_in_range() before emitting, and rv_patch_b()/rv_patch_j() to
      fix up an emitted branch.
    - Immediates are passed as signed values and truncated to their field.
      Callers check the range with rv_imm12_ok() and friends.

    Covered: RV64I, M, A (word and doubleword), Zicsr, Zifencei, Zba, Zbb,
    Zbs, Zicond, Zihintpause, the privileged instructions the runtime needs,
    and the usual assembler pseudo-instructions. Compressed forms are not
    emitted yet.

    All encodings are checked against the GNU assembler by
    tests/riscv64/encoders (8,780 random instances of every encoder, plus
    rv_li() over 200,024 values and the fixup helpers). The big-endian path
    of RV_INSN() compiles but has not been run on a big-endian-data host.
*/

#ifndef _EMU68_RISCV64_RV64_H
#define _EMU68_RISCV64_RV64_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RV_CONSTEXPR static inline __attribute__((always_inline, const))

/* Instruction words are always stored little-endian. */
RV_CONSTEXPR uint32_t RV_INSN(uint32_t insn)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(insn);
#else
    return insn;
#endif
}

/* ------------------------------------------------------------ opcodes */

#define RV_OP_LOAD      0x03
#define RV_OP_MISC_MEM  0x0f
#define RV_OP_IMM       0x13
#define RV_OP_AUIPC     0x17
#define RV_OP_IMM_32    0x1b
#define RV_OP_STORE     0x23
#define RV_OP_AMO       0x2f
#define RV_OP_OP        0x33
#define RV_OP_LUI       0x37
#define RV_OP_OP_32     0x3b
#define RV_OP_BRANCH    0x63
#define RV_OP_JALR      0x67
#define RV_OP_JAL       0x6f
#define RV_OP_SYSTEM    0x73

/* ------------------------------------------------------------ ranges */

RV_CONSTEXPR int rv_imm12_ok(int64_t v) { return v >= -2048 && v <= 2047; }
RV_CONSTEXPR int rv_b_in_range(int64_t off) { return !(off & 1) && off >= -4096 && off <= 4094; }
RV_CONSTEXPR int rv_j_in_range(int64_t off) { return !(off & 1) && off >= -1048576 && off <= 1048574; }

/* ------------------------------------------------------------ formats */

RV_CONSTEXPR uint32_t rv_r(uint32_t f7, uint8_t rs2, uint8_t rs1, uint32_t f3, uint8_t rd, uint32_t op)
{
    return RV_INSN((f7 << 25) | ((rs2 & 31u) << 20) | ((rs1 & 31u) << 15) |
                   (f3 << 12) | ((rd & 31u) << 7) | op);
}

RV_CONSTEXPR uint32_t rv_i(int32_t imm, uint8_t rs1, uint32_t f3, uint8_t rd, uint32_t op)
{
    return RV_INSN(((uint32_t)(imm & 0xfff) << 20) | ((rs1 & 31u) << 15) |
                   (f3 << 12) | ((rd & 31u) << 7) | op);
}

RV_CONSTEXPR uint32_t rv_s(int32_t imm, uint8_t rs2, uint8_t rs1, uint32_t f3, uint32_t op)
{
    uint32_t u = (uint32_t)imm;
    return RV_INSN((((u >> 5) & 0x7f) << 25) | ((rs2 & 31u) << 20) |
                   ((rs1 & 31u) << 15) | (f3 << 12) | ((u & 0x1f) << 7) | op);
}

RV_CONSTEXPR uint32_t rv_b(int32_t off, uint8_t rs2, uint8_t rs1, uint32_t f3)
{
    uint32_t u = (uint32_t)off;
    return RV_INSN((((u >> 12) & 1) << 31) | (((u >> 5) & 0x3f) << 25) |
                   ((rs2 & 31u) << 20) | ((rs1 & 31u) << 15) | (f3 << 12) |
                   (((u >> 1) & 0xf) << 8) | (((u >> 11) & 1) << 7) |
                   RV_OP_BRANCH);
}

RV_CONSTEXPR uint32_t rv_u(uint32_t imm20, uint8_t rd, uint32_t op)
{
    return RV_INSN(((imm20 & 0xfffff) << 12) | ((rd & 31u) << 7) | op);
}

RV_CONSTEXPR uint32_t rv_j(int32_t off, uint8_t rd)
{
    uint32_t u = (uint32_t)off;
    return RV_INSN((((u >> 20) & 1) << 31) | (((u >> 1) & 0x3ff) << 21) |
                   (((u >> 11) & 1) << 20) | (((u >> 12) & 0xff) << 12) |
                   ((rd & 31u) << 7) | RV_OP_JAL);
}

/* Shift-immediate with a 6-bit (RV64) or 5-bit (W form) shift amount. */
RV_CONSTEXPR uint32_t rv_shift(uint32_t f6, uint8_t shamt, uint8_t rs1, uint32_t f3, uint8_t rd, uint32_t op)
{
    return RV_INSN((f6 << 26) | ((shamt & 63u) << 20) | ((rs1 & 31u) << 15) |
                   (f3 << 12) | ((rd & 31u) << 7) | op);
}

RV_CONSTEXPR uint32_t rv_shiftw(uint32_t f7, uint8_t shamt, uint8_t rs1, uint32_t f3, uint8_t rd)
{
    return RV_INSN((f7 << 25) | ((shamt & 31u) << 20) | ((rs1 & 31u) << 15) |
                   (f3 << 12) | ((rd & 31u) << 7) | RV_OP_IMM_32);
}

/* ------------------------------------------------------------ RV64I */

RV_CONSTEXPR uint32_t rv_lui(uint8_t rd, uint32_t imm20) { return rv_u(imm20, rd, RV_OP_LUI); }
RV_CONSTEXPR uint32_t rv_auipc(uint8_t rd, uint32_t imm20) { return rv_u(imm20, rd, RV_OP_AUIPC); }
RV_CONSTEXPR uint32_t rv_jal(uint8_t rd, int32_t off) { return rv_j(off, rd); }
RV_CONSTEXPR uint32_t rv_jalr(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 0, rd, RV_OP_JALR); }

RV_CONSTEXPR uint32_t rv_beq(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 0); }
RV_CONSTEXPR uint32_t rv_bne(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 1); }
RV_CONSTEXPR uint32_t rv_blt(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 4); }
RV_CONSTEXPR uint32_t rv_bge(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 5); }
RV_CONSTEXPR uint32_t rv_bltu(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 6); }
RV_CONSTEXPR uint32_t rv_bgeu(uint8_t rs1, uint8_t rs2, int32_t off) { return rv_b(off, rs2, rs1, 7); }

RV_CONSTEXPR uint32_t rv_lb(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 0, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_lh(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 1, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_lw(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 2, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_ld(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 3, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_lbu(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 4, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_lhu(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 5, rd, RV_OP_LOAD); }
RV_CONSTEXPR uint32_t rv_lwu(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 6, rd, RV_OP_LOAD); }

RV_CONSTEXPR uint32_t rv_sb(uint8_t rs2, uint8_t rs1, int32_t imm) { return rv_s(imm, rs2, rs1, 0, RV_OP_STORE); }
RV_CONSTEXPR uint32_t rv_sh(uint8_t rs2, uint8_t rs1, int32_t imm) { return rv_s(imm, rs2, rs1, 1, RV_OP_STORE); }
RV_CONSTEXPR uint32_t rv_sw(uint8_t rs2, uint8_t rs1, int32_t imm) { return rv_s(imm, rs2, rs1, 2, RV_OP_STORE); }
RV_CONSTEXPR uint32_t rv_sd(uint8_t rs2, uint8_t rs1, int32_t imm) { return rv_s(imm, rs2, rs1, 3, RV_OP_STORE); }

RV_CONSTEXPR uint32_t rv_addi(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 0, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_slti(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 2, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_sltiu(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 3, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_xori(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 4, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_ori(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 6, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_andi(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 7, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_slli(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shift(0x00, sh, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_srli(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shift(0x00, sh, rs1, 5, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_srai(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shift(0x10, sh, rs1, 5, rd, RV_OP_IMM); }

RV_CONSTEXPR uint32_t rv_add(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 0, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 0, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sll(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_slt(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 2, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sltu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 3, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_xor(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 4, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_srl(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sra(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_or(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 6, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_and(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 7, rd, RV_OP_OP); }

RV_CONSTEXPR uint32_t rv_addiw(uint8_t rd, uint8_t rs1, int32_t imm) { return rv_i(imm, rs1, 0, rd, RV_OP_IMM_32); }
RV_CONSTEXPR uint32_t rv_slliw(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shiftw(0x00, sh, rs1, 1, rd); }
RV_CONSTEXPR uint32_t rv_srliw(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shiftw(0x00, sh, rs1, 5, rd); }
RV_CONSTEXPR uint32_t rv_sraiw(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shiftw(0x20, sh, rs1, 5, rd); }
RV_CONSTEXPR uint32_t rv_addw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 0, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_subw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 0, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_sllw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 1, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_srlw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x00, rs2, rs1, 5, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_sraw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 5, rd, RV_OP_OP_32); }

/* fence pred, succ: bit 3 = I, 2 = O, 1 = R, 0 = W */
#define RV_FENCE_R  2
#define RV_FENCE_W  1
#define RV_FENCE_I  8
#define RV_FENCE_O  4
#define RV_FENCE_RW (RV_FENCE_R | RV_FENCE_W)
#define RV_FENCE_IORW 15
RV_CONSTEXPR uint32_t rv_fence(uint8_t pred, uint8_t succ) { return rv_i(((pred & 15) << 4) | (succ & 15), 0, 0, 0, RV_OP_MISC_MEM); }
RV_CONSTEXPR uint32_t rv_fence_i(void) { return rv_i(0, 0, 1, 0, RV_OP_MISC_MEM); }
RV_CONSTEXPR uint32_t rv_pause(void) { return rv_fence(RV_FENCE_W, 0); }

RV_CONSTEXPR uint32_t rv_ecall(void) { return RV_INSN(0x00000073); }
RV_CONSTEXPR uint32_t rv_ebreak(void) { return RV_INSN(0x00100073); }
RV_CONSTEXPR uint32_t rv_sret(void) { return RV_INSN(0x10200073); }
RV_CONSTEXPR uint32_t rv_wfi(void) { return RV_INSN(0x10500073); }
RV_CONSTEXPR uint32_t rv_sfence_vma(uint8_t rs1, uint8_t rs2) { return rv_r(0x09, rs2, rs1, 0, 0, RV_OP_SYSTEM); }

/* ------------------------------------------------------------ Zicsr */

RV_CONSTEXPR uint32_t rv_csrrw(uint8_t rd, uint16_t csr, uint8_t rs1) { return rv_i(csr, rs1, 1, rd, RV_OP_SYSTEM); }
RV_CONSTEXPR uint32_t rv_csrrs(uint8_t rd, uint16_t csr, uint8_t rs1) { return rv_i(csr, rs1, 2, rd, RV_OP_SYSTEM); }
RV_CONSTEXPR uint32_t rv_csrrc(uint8_t rd, uint16_t csr, uint8_t rs1) { return rv_i(csr, rs1, 3, rd, RV_OP_SYSTEM); }
RV_CONSTEXPR uint32_t rv_csrrwi(uint8_t rd, uint16_t csr, uint8_t uimm) { return rv_i(csr, uimm, 5, rd, RV_OP_SYSTEM); }
RV_CONSTEXPR uint32_t rv_csrrsi(uint8_t rd, uint16_t csr, uint8_t uimm) { return rv_i(csr, uimm, 6, rd, RV_OP_SYSTEM); }
RV_CONSTEXPR uint32_t rv_csrrci(uint8_t rd, uint16_t csr, uint8_t uimm) { return rv_i(csr, uimm, 7, rd, RV_OP_SYSTEM); }

#define RV_CSR_SSTATUS  0x100
#define RV_CSR_SIE      0x104
#define RV_CSR_STVEC    0x105
#define RV_CSR_SSCRATCH 0x140
#define RV_CSR_SEPC     0x141
#define RV_CSR_SCAUSE   0x142
#define RV_CSR_STVAL    0x143
#define RV_CSR_SIP      0x144
#define RV_CSR_SATP     0x180
#define RV_CSR_CYCLE    0xc00
#define RV_CSR_TIME     0xc01
#define RV_CSR_INSTRET  0xc02

/* ------------------------------------------------------------ M */

RV_CONSTEXPR uint32_t rv_mul(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 0, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_mulh(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_mulhsu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 2, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_mulhu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 3, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_div(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 4, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_divu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_rem(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 6, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_remu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 7, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_mulw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 0, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_divw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 4, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_divuw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 5, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_remw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 6, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_remuw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x01, rs2, rs1, 7, rd, RV_OP_OP_32); }

/* ------------------------------------------------------------ A */

/* f3 = 2 for .w, 3 for .d; aq and rl are the ordering bits. */
RV_CONSTEXPR uint32_t rv_amo(uint32_t f5, int aq, int rl, uint8_t rs2, uint8_t rs1, uint32_t f3, uint8_t rd)
{
    return rv_r((f5 << 2) | ((aq & 1) << 1) | (rl & 1), rs2, rs1, f3, rd, RV_OP_AMO);
}
RV_CONSTEXPR uint32_t rv_lr_w(uint8_t rd, uint8_t rs1, int aq, int rl) { return rv_amo(0x02, aq, rl, 0, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_lr_d(uint8_t rd, uint8_t rs1, int aq, int rl) { return rv_amo(0x02, aq, rl, 0, rs1, 3, rd); }
RV_CONSTEXPR uint32_t rv_sc_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x03, aq, rl, rs2, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_sc_d(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x03, aq, rl, rs2, rs1, 3, rd); }
RV_CONSTEXPR uint32_t rv_amoswap_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x01, aq, rl, rs2, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_amoadd_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x00, aq, rl, rs2, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_amoand_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x0c, aq, rl, rs2, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_amoor_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x08, aq, rl, rs2, rs1, 2, rd); }
RV_CONSTEXPR uint32_t rv_amoxor_w(uint8_t rd, uint8_t rs2, uint8_t rs1, int aq, int rl) { return rv_amo(0x04, aq, rl, rs2, rs1, 2, rd); }

/* ------------------------------------------------------------ Zba */

RV_CONSTEXPR uint32_t rv_add_uw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x04, rs2, rs1, 0, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_sh1add(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 2, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sh2add(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 4, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sh3add(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 6, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_sh1add_uw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 2, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_sh2add_uw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 4, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_sh3add_uw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x10, rs2, rs1, 6, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_slli_uw(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shift(0x02, sh, rs1, 1, rd, RV_OP_IMM_32); }

/* ------------------------------------------------------------ Zbb */

RV_CONSTEXPR uint32_t rv_andn(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 7, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_orn(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 6, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_xnor(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x20, rs2, rs1, 4, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_clz(uint8_t rd, uint8_t rs1) { return rv_i(0x600, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_ctz(uint8_t rd, uint8_t rs1) { return rv_i(0x601, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_cpop(uint8_t rd, uint8_t rs1) { return rv_i(0x602, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_sext_b(uint8_t rd, uint8_t rs1) { return rv_i(0x604, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_sext_h(uint8_t rd, uint8_t rs1) { return rv_i(0x605, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_clzw(uint8_t rd, uint8_t rs1) { return rv_i(0x600, rs1, 1, rd, RV_OP_IMM_32); }
RV_CONSTEXPR uint32_t rv_ctzw(uint8_t rd, uint8_t rs1) { return rv_i(0x601, rs1, 1, rd, RV_OP_IMM_32); }
RV_CONSTEXPR uint32_t rv_cpopw(uint8_t rd, uint8_t rs1) { return rv_i(0x602, rs1, 1, rd, RV_OP_IMM_32); }
RV_CONSTEXPR uint32_t rv_zext_h(uint8_t rd, uint8_t rs1) { return rv_r(0x04, 0, rs1, 4, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_max(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x05, rs2, rs1, 6, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_maxu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x05, rs2, rs1, 7, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_min(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x05, rs2, rs1, 4, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_minu(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x05, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_rol(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x30, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_ror(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x30, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_rolw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x30, rs2, rs1, 1, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_rorw(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x30, rs2, rs1, 5, rd, RV_OP_OP_32); }
RV_CONSTEXPR uint32_t rv_rori(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shift(0x18, sh, rs1, 5, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_roriw(uint8_t rd, uint8_t rs1, uint8_t sh) { return rv_shiftw(0x30, sh, rs1, 5, rd); }
RV_CONSTEXPR uint32_t rv_orc_b(uint8_t rd, uint8_t rs1) { return rv_i(0x287, rs1, 5, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_rev8(uint8_t rd, uint8_t rs1) { return rv_i(0x6b8, rs1, 5, rd, RV_OP_IMM); }

/* ------------------------------------------------------------ Zbs */

RV_CONSTEXPR uint32_t rv_bclr(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x24, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_bext(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x24, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_binv(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x34, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_bset(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x14, rs2, rs1, 1, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_bclri(uint8_t rd, uint8_t rs1, uint8_t bit) { return rv_shift(0x12, bit, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_bexti(uint8_t rd, uint8_t rs1, uint8_t bit) { return rv_shift(0x12, bit, rs1, 5, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_binvi(uint8_t rd, uint8_t rs1, uint8_t bit) { return rv_shift(0x1a, bit, rs1, 1, rd, RV_OP_IMM); }
RV_CONSTEXPR uint32_t rv_bseti(uint8_t rd, uint8_t rs1, uint8_t bit) { return rv_shift(0x0a, bit, rs1, 1, rd, RV_OP_IMM); }

/* ------------------------------------------------------------ Zicond */

RV_CONSTEXPR uint32_t rv_czero_eqz(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x07, rs2, rs1, 5, rd, RV_OP_OP); }
RV_CONSTEXPR uint32_t rv_czero_nez(uint8_t rd, uint8_t rs1, uint8_t rs2) { return rv_r(0x07, rs2, rs1, 7, rd, RV_OP_OP); }

/* ------------------------------------------------------------ pseudo-instructions */

RV_CONSTEXPR uint32_t rv_nop(void) { return rv_addi(0, 0, 0); }
RV_CONSTEXPR uint32_t rv_mv(uint8_t rd, uint8_t rs) { return rv_addi(rd, rs, 0); }
RV_CONSTEXPR uint32_t rv_not(uint8_t rd, uint8_t rs) { return rv_xori(rd, rs, -1); }
RV_CONSTEXPR uint32_t rv_neg(uint8_t rd, uint8_t rs) { return rv_sub(rd, 0, rs); }
RV_CONSTEXPR uint32_t rv_negw(uint8_t rd, uint8_t rs) { return rv_subw(rd, 0, rs); }
RV_CONSTEXPR uint32_t rv_sext_w(uint8_t rd, uint8_t rs) { return rv_addiw(rd, rs, 0); }
RV_CONSTEXPR uint32_t rv_zext_w(uint8_t rd, uint8_t rs) { return rv_add_uw(rd, rs, 0); }
RV_CONSTEXPR uint32_t rv_zext_b(uint8_t rd, uint8_t rs) { return rv_andi(rd, rs, 0xff); }
RV_CONSTEXPR uint32_t rv_seqz(uint8_t rd, uint8_t rs) { return rv_sltiu(rd, rs, 1); }
RV_CONSTEXPR uint32_t rv_snez(uint8_t rd, uint8_t rs) { return rv_sltu(rd, 0, rs); }
RV_CONSTEXPR uint32_t rv_sltz(uint8_t rd, uint8_t rs) { return rv_slt(rd, rs, 0); }
RV_CONSTEXPR uint32_t rv_sgtz(uint8_t rd, uint8_t rs) { return rv_slt(rd, 0, rs); }
RV_CONSTEXPR uint32_t rv_beqz(uint8_t rs, int32_t off) { return rv_beq(rs, 0, off); }
RV_CONSTEXPR uint32_t rv_bnez(uint8_t rs, int32_t off) { return rv_bne(rs, 0, off); }
RV_CONSTEXPR uint32_t rv_bltz(uint8_t rs, int32_t off) { return rv_blt(rs, 0, off); }
RV_CONSTEXPR uint32_t rv_bgez(uint8_t rs, int32_t off) { return rv_bge(rs, 0, off); }
RV_CONSTEXPR uint32_t rv_j_(int32_t off) { return rv_jal(0, off); }
RV_CONSTEXPR uint32_t rv_jr(uint8_t rs) { return rv_jalr(0, rs, 0); }
RV_CONSTEXPR uint32_t rv_ret(void) { return rv_jalr(0, 1, 0); }

/* ------------------------------------------------------------ fixups */

/* Replace the offset of an emitted B-type or J-type instruction. */
#define RV_B_KEEP_MASK 0x01fff07fu  /* rs2, rs1, funct3 and opcode */

static inline uint32_t rv_patch_b(uint32_t insn, int32_t off)
{
    uint32_t keep = RV_INSN(insn) & RV_B_KEEP_MASK;
    uint32_t offset_bits = RV_INSN(rv_b(off, 0, 0, 0)) & ~RV_B_KEEP_MASK;
    return RV_INSN(keep | offset_bits);
}

static inline uint32_t rv_patch_j(uint32_t insn, int32_t off)
{
    uint32_t rd = (RV_INSN(insn) >> 7) & 31;
    return rv_j(off, (uint8_t)rd);
}

/*
    Load a 64-bit constant into rd. Writes at most 8 instructions to buf and
    returns the number written. The sequence is lui/addi(w) for values that
    fit in 32 bits, otherwise the upper part is built recursively and shifted
    into place, in the style of the assembler's li expansion.
*/
static inline int rv_li(uint32_t *buf, uint8_t rd, int64_t value)
{
    if (value >= -2048 && value <= 2047) {
        buf[0] = rv_addi(rd, 0, (int32_t)value);
        return 1;
    }
    if (value >= INT32_MIN && value <= INT32_MAX) {
        int32_t lo = (int32_t)((uint32_t)value << 20) >> 20;   /* sign-extended low 12 */
        uint32_t hi = (uint32_t)(((int64_t)value - lo) >> 12) & 0xfffff;
        int n = 0;
        buf[n++] = rv_lui(rd, hi);
        if (lo)
            buf[n++] = rv_addiw(rd, rd, lo);
        return n;
    }

    int32_t lo = (int32_t)((int64_t)((uint64_t)value << 52) >> 52);  /* sign-extended low 12 */
    int64_t hi = (int64_t)((uint64_t)value - (uint64_t)(int64_t)lo) >> 12;
    int shift = 12 + __builtin_ctzll((uint64_t)hi);
    hi >>= shift - 12;

    int n = rv_li(buf, rd, hi);
    buf[n++] = rv_slli(rd, rd, (uint8_t)shift);
    if (lo)
        buf[n++] = rv_addi(rd, rd, lo);
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* _EMU68_RISCV64_RV64_H */
