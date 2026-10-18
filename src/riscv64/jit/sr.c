/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    SR and CCR instructions and MOVEC for the RV64 translator.

      MOVE from SR    0x40c0  privileged (68010 and later)
      MOVE from CCR   0x42c0
      MOVE to CCR     0x44c0
      MOVE to SR      0x46c0  privileged
      ORI/ANDI/EORI to CCR  0x003c 0x023c 0x0a3c
      ORI/ANDI/EORI to SR   0x007c 0x027c 0x0a7c  privileged
      MOVEC           0x4e7a 0x4e7b  privileged

    A write to SR keeps the 68020 bits (0xf71f), switches A7 when S or M
    changes and ends the unit (rv_emit_write_sr in exception.c). CCR writes
    only touch the low five bits.

    MOVEC knows SFC, DFC, CACR, USP, VBR, MSP and ISP, and the 68040 MMU
    registers TC, ITT0, ITT1, DTT0, DTT1, MMUSR, URP and SRP, stored with the
    masks Emu68 uses on AArch64 (src/M68k_LINE4.c, EMIT_MOVEC): CACR keeps
    0x80008000, TC only P (0x4000, so the MMU can never be enabled), the
    transparent translation registers 0xffffe364, URP and SRP drop bits 8-0,
    MMUSR is stored as written. Nothing acts on the MMU registers yet. Any
    other control register, CAAR included (68020/68030 only, not on the
    68040 that Emu68 presents), takes the illegal instruction exception.
*/

#include <stddef.h>
#include <stdint.h>
#include "support.h"
#include "M68k.h"
#include "jit_internal.h"

#define CTX_USP     64
#define CTX_MSP     68
#define CTX_ISP     72
#define CTX_VBR     84
#define CTX_CACR    88
#define CTX_SFC     ((int)offsetof(struct M68KState, SFC))
#define CTX_DFC     ((int)offsetof(struct M68KState, DFC))
_Static_assert(offsetof(struct M68KState, CACR) == CTX_CACR, "CACR");
_Static_assert(offsetof(struct M68KState, DTT1) < 2048, "context offsets fit in 12 bits");

/* 68040 MMU registers: offset, write mask, size of the field */
struct mmu_reg { uint16_t code; uint16_t off; uint32_t mask; uint8_t size; };
static const struct mmu_reg mmu_regs[] = {
    { 0x003, offsetof(struct M68KState, TCR),   0x00004000, 2 },
    { 0x004, offsetof(struct M68KState, ITT0),  0xffffe364, 4 },
    { 0x005, offsetof(struct M68KState, ITT1),  0xffffe364, 4 },
    { 0x006, offsetof(struct M68KState, DTT0),  0xffffe364, 4 },
    { 0x007, offsetof(struct M68KState, DTT1),  0xffffe364, 4 },
    { 0x805, offsetof(struct M68KState, MMUSR), 0xffffffff, 4 },
    { 0x806, offsetof(struct M68KState, URP),   0xfffffe00, 4 },
    { 0x807, offsetof(struct M68KState, SRP),   0xfffffe00, 4 },
};

#define EA_MODE(op)     (((op) >> 3) & 7)
#define EA_REG(op)      ((op) & 7)

/* MOVE <ea>,SR / MOVE <ea>,CCR */
static int move_to(struct rv_tc *t, uint16_t op, int to_sr)
{
    struct rv_ea ea;
    int owned;

    if (EA_MODE(op) == 1)
        return 0;
    if (to_sr)
        rv_emit_priv_check(t);
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), 2, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, 2, &owned);
    if (to_sr) {
        rv_emit_write_sr(t, v);
    } else {
        uint8_t k = rv_tmp(t);
        rv_emit(t, rv_andi(k, v, 0x1f));
        rv_emit(t, rv_andi(RVR_SR, RVR_SR, -32));
        rv_emit(t, rv_or(RVR_SR, RVR_SR, k));
        rv_free(t, k);
    }
    if (owned)
        rv_free(t, v);
    rv_ea_done(t, &ea);
    return 1;
}

/* MOVE SR,<ea> / MOVE CCR,<ea> */
static int move_from(struct rv_tc *t, uint16_t op, int from_sr)
{
    struct rv_ea ea;

    if (EA_MODE(op) == 1 || (EA_MODE(op) == 7 && EA_REG(op) > 1))
        return 0;
    if (from_sr)
        rv_emit_priv_check(t);
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), 2, &ea))
        return 0;
    if (from_sr) {
        rv_ea_write(t, &ea, 2, RVR_SR);
    } else {
        uint8_t k = rv_tmp(t);
        rv_emit(t, rv_andi(k, RVR_SR, 0x1f));
        rv_ea_write(t, &ea, 2, k);
        rv_free(t, k);
    }
    rv_ea_done(t, &ea);
    return 1;
}

static int movec(struct rv_tc *t, uint16_t op)
{
    uint16_t ext = rv_fetch16(t);
    int to_cr = op & 1;
    uint8_t r = (ext & 0x8000) ? rv_areg((ext >> 12) & 7) : rv_dreg((ext >> 12) & 7);
    uint8_t k = 0;

    rv_emit_priv_check(t);
    switch (ext & 0xfff) {
    case 0x000:                                 /* SFC */
    case 0x001:                                 /* DFC */
    {
        int off = (ext & 1) ? CTX_DFC : CTX_SFC;
        if (to_cr) {
            k = rv_tmp(t);
            rv_emit(t, rv_andi(k, r, 7));
            rv_emit(t, rv_sb(k, RVR_CTX, off));
            rv_free(t, k);
        } else {
            rv_emit(t, rv_lbu(r, RVR_CTX, off));
        }
        break;
    }
    case 0x002:                                 /* CACR */
        if (to_cr) {
            k = rv_tmp(t);
            rv_emit_li(t, k, (int32_t)0x80008000);
            rv_emit(t, rv_and(k, k, r));
            rv_emit(t, rv_sw(k, RVR_CTX, CTX_CACR));
            rv_free(t, k);
        } else {
            rv_emit(t, rv_lw(r, RVR_CTX, CTX_CACR));
        }
        break;
    case 0x800:                                 /* USP */
    case 0x801:                                 /* VBR */
    {
        int off = (ext & 1) ? CTX_VBR : CTX_USP;
        rv_emit(t, to_cr ? rv_sw(r, RVR_CTX, off) : rv_lw(r, RVR_CTX, off));
        break;
    }
    case 0x803:                                 /* MSP */
    case 0x804:                                 /* ISP */
    {
        /* A7 is one of them in supervisor mode: go through the context */
        int off = (ext & 7) == 3 ? CTX_MSP : CTX_ISP;
        rv_emit_sp_sync(t, 1);
        if (to_cr) {
            rv_emit(t, rv_sw(r, RVR_CTX, off));
            rv_emit_sp_sync(t, 0);
        } else {
            rv_emit(t, rv_lw(r, RVR_CTX, off));
        }
        break;
    }
    default:
        for (unsigned i = 0; i < sizeof(mmu_regs) / sizeof(mmu_regs[0]); i++) {
            const struct mmu_reg *m = &mmu_regs[i];
            if (m->code != (ext & 0xfff))
                continue;
            if (to_cr) {
                k = rv_tmp(t);
                rv_emit_li(t, k, (int32_t)m->mask);
                rv_emit(t, rv_and(k, k, r));
                rv_emit(t, m->size == 2 ? rv_sh(k, RVR_CTX, m->off) : rv_sw(k, RVR_CTX, m->off));
                rv_free(t, k);
            } else {
                rv_emit(t, m->size == 2 ? rv_lhu(r, RVR_CTX, m->off) : rv_lw(r, RVR_CTX, m->off));
            }
            return 1;
        }
        rv_emit_exception(t, 4, 0, t->pc, t->pc);
        t->ended = 1;
        break;
    }
    return 1;
}

/* Line 4 opcodes: MOVE from/to SR and CCR, MOVEC; 0 if not one of them */
int rv_emit_sr_op(struct rv_tc *t, uint16_t op)
{
    switch (op & 0xffc0) {
    case 0x40c0: return move_from(t, op, 1);
    case 0x42c0: return move_from(t, op, 0);
    case 0x44c0: return move_to(t, op, 0);
    case 0x46c0: return move_to(t, op, 1);
    }
    if ((op & 0xfffe) == 0x4e7a)
        return movec(t, op);
    return 0;
}

/* ORI, ANDI, EORI #imm to CCR (0x003c...) and to SR (0x007c..., privileged) */
int rv_emit_sr_imm(struct rv_tc *t, uint16_t op)
{
    int kind = (op >> 9) & 7;                   /* 0 ORI, 1 ANDI, 5 EORI */
    int to_sr = (op & 0xffc0) == 0x0040 || (op & 0xffc0) == 0x0240 || (op & 0xffc0) == 0x0a40;

    if ((kind != 0 && kind != 1 && kind != 5) || (op & 0x3f) != 0x3c || ((op >> 6) & 3) > 1)
        return 0;
    uint16_t imm = rv_fetch16(t);

    if (!to_sr) {
        int v = imm & 0x1f;
        if (kind == 0)
            rv_emit(t, rv_ori(RVR_SR, RVR_SR, v));
        else if (kind == 1)
            rv_emit(t, rv_andi(RVR_SR, RVR_SR, -32 | v));
        else
            rv_emit(t, rv_xori(RVR_SR, RVR_SR, v));
        return 1;
    }

    rv_emit_priv_check(t);
    uint8_t k = rv_tmp(t);
    rv_emit_li(t, k, imm);
    if (kind == 0)
        rv_emit(t, rv_or(k, RVR_SR, k));
    else if (kind == 1)
        rv_emit(t, rv_and(k, RVR_SR, k));
    else
        rv_emit(t, rv_xor(k, RVR_SR, k));
    rv_emit_write_sr(t, k);
    rv_free(t, k);
    return 1;
}
