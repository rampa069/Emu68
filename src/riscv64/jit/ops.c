/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    m68k instruction emitters of the RV64 translator, first subset:

      MOVE MOVEA MOVEQ LEA PEA CLR TST EXT EXTB SWAP NOP
      ORI ANDI SUBI ADDI EORI CMPI (to an effective address)
      ADD ADDA ADDQ SUB SUBA SUBQ CMP CMPA AND OR EOR
      MOVE from SR, MOVEM, LINK, UNLK
      Bcc BRA BSR DBcc Scc JMP JSR RTS
      ASL ASR LSL LSR ROL ROR ROXL ROXR (shift.c)
      BTST BCHG BCLR BSET
      MULU MULS DIVU DIVS, word and long forms (muldiv.c)
      NEG NEGX NOT ADDX SUBX (negx.c)
      TRAP TRAPV TRAPcc ILLEGAL RTE MOVE USP, Line-A, Line-F, zero divide (exception.c)

    Every other opcode returns 0, and the translator stops with a message.
    Condition codes are always computed in full; using Emu68's look-ahead
    (M68K_GetSRMask) to skip dead flags comes later.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

static const int size_bits[4] = { 1, 2, 4, 0 };   /* size field 00 01 10 11 */

#define EA_MODE(op)     (((op) >> 3) & 7)
#define EA_REG(op)      ((op) & 7)

/* ------------------------------------------------------------ helpers */

/* PC-relative target, held canonical in gp */
static void set_pc(struct rv_tc *t, uint32_t target)
{
    rv_emit_li(t, RVR_PC, (int32_t)target);
}

/* gp = cond ? target : fall; cond in a register */
static void select_pc(struct rv_tc *t, uint8_t cond, uint32_t target, uint32_t fall)
{
    uint32_t *b = t->ptr;
    rv_emit(t, rv_beqz(cond, 0));
    set_pc(t, target);
    uint32_t *j = t->ptr;
    rv_emit(t, rv_jal(0, 0));
    uint32_t *l_fall = t->ptr;
    set_pc(t, fall);
    if (!t->overflow) {
        *b = rv_patch_b(*b, (int32_t)((l_fall - b) * 4));
        *j = rv_patch_j(*j, (int32_t)((t->ptr - j) * 4));
    }
}

static void push_long(struct rv_tc *t, uint8_t val)
{
    uint8_t sp = rv_areg(7);
    rv_emit(t, rv_addiw(sp, sp, -4));
    rv_store68(t, 4, val, sp, 0);
}

/* Read <ea> at size into a register; caller frees it with release() */
static uint8_t read_ea(struct rv_tc *t, struct rv_ea *ea, int size, int *owned)
{
    return rv_ea_read(t, ea, size, owned);
}

static void release(struct rv_tc *t, uint8_t r, int owned)
{
    if (owned)
        rv_free(t, r);
}

/* ------------------------------------------------------------ data movement */

static int op_move(struct rv_tc *t, uint16_t op)
{
    static const int sz[4] = { 0, 1, 4, 2 };
    int size = sz[(op >> 12) & 3];
    int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
    struct rv_ea src, dst;
    int owned;

    if (dmode == 1 && size == 1)
        return 0;
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &src))
        return 0;
    uint8_t v = read_ea(t, &src, size, &owned);
    rv_ea_done(t, &src);

    /* MOVE An,-(An): the value stored is An before the pre-decrement (as on
       TG68K and Emu68 on AArch64), so copy it before decoding the destination */
    if (src.kind == EA_AREG && dmode == 4 && dreg == EA_REG(op)) {
        uint8_t c = rv_tmp(t);
        rv_emit(t, rv_mv(c, v));
        v = c;
        owned = 1;
    }

    if (!rv_ea_decode(t, dmode, dreg, size, &dst) || dst.kind == EA_IMM) {
        release(t, v, owned);
        return 0;
    }
    rv_ea_write(t, &dst, size, v);
    rv_ea_done(t, &dst);
    if (dmode != 1)                     /* MOVEA leaves the flags alone */
        rv_flags_nz(t, size, v);
    release(t, v, owned);
    return 1;
}

static int op_moveq(struct rv_tc *t, uint16_t op)
{
    if (op & 0x0100)
        return 0;
    uint8_t d = rv_dreg((op >> 9) & 7);
    rv_emit_li(t, d, (int8_t)(op & 0xff));
    rv_flags_nz(t, 4, d);
    return 1;
}

static int op_lea(struct rv_tc *t, uint16_t op)
{
    struct rv_ea ea;
    int mode = EA_MODE(op);

    if (mode < 2 || mode == 3 || mode == 4)
        return 0;
    if (!rv_ea_decode(t, mode, EA_REG(op), 0, &ea) || ea.kind != EA_MEM)
        return 0;
    uint8_t a = rv_ea_address(t, &ea);
    rv_ea_done(t, &ea);
    rv_emit(t, rv_mv(rv_areg((op >> 9) & 7), a));
    rv_free(t, a);
    return 1;
}

static int op_pea(struct rv_tc *t, uint16_t op)
{
    struct rv_ea ea;
    int mode = EA_MODE(op);

    if (mode < 2 || mode == 3 || mode == 4)
        return 0;
    if (!rv_ea_decode(t, mode, EA_REG(op), 0, &ea) || ea.kind != EA_MEM)
        return 0;
    uint8_t a = rv_ea_address(t, &ea);
    rv_ea_done(t, &ea);
    push_long(t, a);
    rv_free(t, a);
    return 1;
}

static int op_clr(struct rv_tc *t, uint16_t op)
{
    int size = size_bits[(op >> 6) & 3];
    struct rv_ea ea;

    if (!size || EA_MODE(op) == 1 || !rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea) ||
        ea.kind == EA_IMM)
        return 0;
    rv_ea_write(t, &ea, size, 0);
    rv_ea_done(t, &ea);
    if (!(t->live & (CCR_N | CCR_Z | CCR_V | CCR_C)))
        return 1;
    uint8_t z = rv_tmp(t);
    rv_emit(t, rv_addi(z, 0, CCR_Z));
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, z));
    rv_free(t, z);
    return 1;
}

static int op_tst(struct rv_tc *t, uint16_t op)
{
    int size = size_bits[(op >> 6) & 3];
    struct rv_ea ea;
    int owned;

    if (!size || !rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea))
        return 0;
    uint8_t v = read_ea(t, &ea, size, &owned);
    rv_ea_done(t, &ea);
    rv_flags_nz(t, size, v);
    release(t, v, owned);
    return 1;
}

static int op_ext(struct rv_tc *t, uint16_t op)
{
    uint8_t d = rv_dreg(EA_REG(op));

    switch ((op >> 6) & 7) {
    case 2: {                           /* EXT.W: byte to word */
        uint8_t w = rv_tmp(t);
        rv_emit(t, rv_sext_b(w, d));
        struct rv_ea ea = { .kind = EA_DREG, .reg = d };
        rv_ea_write(t, &ea, 2, w);
        rv_flags_nz(t, 2, w);
        rv_free(t, w);
        return 1;
    }
    case 3:                             /* EXT.L: word to long */
        rv_emit(t, rv_sext_h(d, d));
        rv_flags_nz(t, 4, d);
        return 1;
    case 7:                             /* EXTB.L: byte to long */
        rv_emit(t, rv_sext_b(d, d));
        rv_flags_nz(t, 4, d);
        return 1;
    default:
        return 0;
    }
}

static int op_swap(struct rv_tc *t, uint16_t op)
{
    uint8_t d = rv_dreg(EA_REG(op));
    uint8_t hi = rv_tmp(t);
    rv_emit(t, rv_slliw(hi, d, 16));
    rv_emit(t, rv_srliw(d, d, 16));
    rv_emit(t, rv_or(d, d, hi));
    rv_flags_nz(t, 4, d);
    rv_free(t, hi);
    return 1;
}

static int op_movem(struct rv_tc *t, uint16_t op)
{
    int to_regs = (op >> 10) & 1;
    int size = (op & 0x40) ? 4 : 2;
    int mode = EA_MODE(op), reg = EA_REG(op);
    uint16_t mask = rv_fetch16(t);
    int count = 0;

    for (int i = 0; i < 16; i++)
        count += (mask >> i) & 1;

    if (mode == 4 && !to_regs) {
        /* -(An): mask bit 0 is A7 ... bit 15 is D0; store downwards */
        uint8_t an = rv_areg(reg);
        int off = 0;
        for (int i = 0; i < 16; i++) {
            if (mask & (1u << i)) {
                int r = 15 - i;
                off -= size;
                rv_store68(t, size, r < 8 ? rv_dreg(r) : rv_areg(r - 8), an, off);
            }
        }
        rv_emit(t, rv_addiw(an, an, -size * count));
        return 1;
    }
    if (mode == 4 || (mode == 3 && !to_regs) || mode < 2)
        return 0;
    if (to_regs && mode == 7 && reg == 4)
        return 0;

    uint8_t base;
    int base_tmp = 0;
    if (mode == 2 || mode == 3) {
        base = rv_areg(reg);
    } else {
        struct rv_ea ea;
        if (!rv_ea_decode(t, mode, reg, 0, &ea) || ea.kind != EA_MEM)
            return 0;
        base = rv_ea_address(t, &ea);
        rv_ea_done(t, &ea);
        base_tmp = 1;
    }
    /* Keep a copy of the base so loading An from the list cannot disturb it */
    uint8_t a = rv_tmp(t);
    rv_emit(t, rv_mv(a, base));

    int off = 0;
    for (int r = 0; r < 16; r++) {
        if (!(mask & (1u << r)))
            continue;
        uint8_t hr = r < 8 ? rv_dreg(r) : rv_areg(r - 8);
        if (to_regs) {
            rv_load68(t, size, hr, a, off);
            if (size == 2)
                rv_emit(t, rv_sext_h(hr, hr));  /* words are sign-extended into the whole register */
        } else {
            rv_store68(t, size, hr, a, off);
        }
        off += size;
    }
    if (mode == 3)                              /* (An)+ */
        rv_emit(t, rv_addiw(rv_areg(reg), a, size * count));
    rv_free(t, a);
    if (base_tmp)
        rv_free(t, base);
    return 1;
}

static int op_link(struct rv_tc *t, uint16_t op)
{
    uint8_t an = rv_areg(EA_REG(op)), sp = rv_areg(7);
    int32_t d = (int16_t)rv_fetch16(t);

    if (EA_REG(op) == 7)
        return 0;
    push_long(t, an);
    rv_emit(t, rv_mv(an, sp));
    if (rv_imm12_ok(d)) {
        rv_emit(t, rv_addiw(sp, sp, d));
    } else {
        uint8_t k = rv_tmp(t);
        rv_emit_li(t, k, d);
        rv_emit(t, rv_addw(sp, sp, k));
        rv_free(t, k);
    }
    return 1;
}

static int op_unlk(struct rv_tc *t, uint16_t op)
{
    uint8_t an = rv_areg(EA_REG(op)), sp = rv_areg(7);

    if (EA_REG(op) == 7)
        return 0;
    rv_emit(t, rv_mv(sp, an));
    rv_load68(t, 4, an, sp, 0);
    rv_emit(t, rv_addiw(sp, sp, 4));
    return 1;
}

/* ------------------------------------------------------------ arithmetic and logic */

enum alu { ALU_ADD, ALU_SUB, ALU_CMP, ALU_AND, ALU_OR, ALU_EOR };

/* dst (in register d, at size) op src (in s); result written by the caller */
static uint8_t alu(struct rv_tc *t, enum alu kind, int size, uint8_t d, uint8_t s)
{
    uint8_t r = rv_tmp(t);
    switch (kind) {
    case ALU_ADD: rv_flags_add(t, size, d, s, r, 1); break;
    case ALU_SUB: rv_flags_sub(t, size, d, s, r, 1); break;
    case ALU_CMP: rv_flags_sub(t, size, d, s, r, 0); break;
    case ALU_AND: rv_emit(t, rv_and(r, d, s)); rv_flags_nz(t, size, r); break;
    case ALU_OR:  rv_emit(t, rv_or(r, d, s));  rv_flags_nz(t, size, r); break;
    case ALU_EOR: rv_emit(t, rv_xor(r, d, s)); rv_flags_nz(t, size, r); break;
    }
    return r;
}

/* <ea> op Dn -> Dn, or Dn op <ea> -> <ea> */
static int op_alu_dn(struct rv_tc *t, uint16_t op, enum alu kind)
{
    int size = size_bits[(op >> 6) & 3];
    int to_ea = (op >> 8) & 1;
    uint8_t dn = rv_dreg((op >> 9) & 7);
    struct rv_ea ea;
    int owned;

    if (!size)
        return 0;
    /* Dn op <ea> with a register <ea> is ADDX, SUBX, ABCD, SBCD or EXG, except
       for EOR, where Dn,Dn is a plain EOR */
    if (to_ea && (kind == ALU_CMP || EA_MODE(op) == 1 || (EA_MODE(op) == 0 && kind != ALU_EOR)))
        return 0;
    if (!to_ea && EA_MODE(op) == 1 && size == 1)
        return 0;
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea))
        return 0;
    if (to_ea && ea.kind == EA_IMM)
        return 0;

    uint8_t v = read_ea(t, &ea, size, &owned);
    uint8_t r;
    if (to_ea) {
        r = alu(t, kind, size, v, dn);
        rv_ea_write(t, &ea, size, r);
    } else {
        r = alu(t, kind, size, dn, v);
        if (kind != ALU_CMP) {
            struct rv_ea d = { .kind = EA_DREG, .reg = dn };
            rv_ea_write(t, &d, size, r);
        }
    }
    rv_free(t, r);
    release(t, v, owned);
    rv_ea_done(t, &ea);
    return 1;
}

/* ADDA SUBA CMPA: <ea> (word sign-extended) with An, 32 bits */
static int op_alu_an(struct rv_tc *t, uint16_t op, enum alu kind)
{
    int size = (op & 0x100) ? 4 : 2;
    uint8_t an = rv_areg((op >> 9) & 7);
    struct rv_ea ea;
    int owned;

    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea))
        return 0;
    uint8_t v = read_ea(t, &ea, size, &owned);
    uint8_t s = v;
    if (size == 2) {
        s = rv_tmp(t);
        rv_emit(t, rv_sext_h(s, v));
    }
    if (kind == ALU_CMP) {
        uint8_t r = rv_tmp(t);
        rv_flags_sub(t, 4, an, s, r, 0);
        rv_free(t, r);
    } else {
        rv_emit(t, kind == ALU_ADD ? rv_addw(an, an, s) : rv_subw(an, an, s));
    }
    if (s != v)
        rv_free(t, s);
    release(t, v, owned);
    rv_ea_done(t, &ea);
    return 1;
}

/* ORI ANDI SUBI ADDI EORI CMPI #imm,<ea> */
static int op_immediate(struct rv_tc *t, uint16_t op)
{
    static const int kinds[8] = { ALU_OR, ALU_AND, ALU_SUB, ALU_ADD, -1, ALU_EOR, ALU_CMP, -1 };
    int kind = kinds[(op >> 9) & 7];
    int size = size_bits[(op >> 6) & 3];
    struct rv_ea ea;
    int owned;

    if (kind < 0 || !size || (op & 0x0100) || EA_MODE(op) == 1)
        return 0;
    if (EA_MODE(op) == 7 && EA_REG(op) == 4)
        return rv_emit_sr_imm(t, op);   /* to CCR / SR (sr.c) */

    int64_t imm = size == 4 ? (int32_t)rv_fetch32(t)
                : size == 2 ? (int16_t)rv_fetch16(t) : (int8_t)(rv_fetch16(t) & 0xff);
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea) || ea.kind == EA_IMM)
        return 0;

    uint8_t s = rv_tmp(t);
    rv_emit_li(t, s, imm);
    uint8_t v = read_ea(t, &ea, size, &owned);
    uint8_t r = alu(t, (enum alu)kind, size, v, s);
    if (kind != ALU_CMP)
        rv_ea_write(t, &ea, size, r);
    rv_free(t, r);
    rv_free(t, s);
    release(t, v, owned);
    rv_ea_done(t, &ea);
    return 1;
}

/*
    BTST BCHG BCLR BSET, dynamic (bit number in Dn) and static (in an
    extension word that precedes the effective address extensions). On a
    data register the operation is on 32 bits with the bit number modulo 32,
    on memory it is on a byte, modulo 8. Only Z changes: it is set when the
    tested bit was 0.
*/
static int op_bit(struct rv_tc *t, uint16_t op)
{
    int kind = (op >> 6) & 3;               /* 0 BTST, 1 BCHG, 2 BCLR, 3 BSET */
    int mode = EA_MODE(op);
    int is_static = !(op & 0x0100);
    struct rv_ea ea;
    int owned;

    if (mode == 1)
        return 0;                           /* MOVEP */
    if (kind != 0 && (mode == 7 && EA_REG(op) >= 2))
        return 0;                           /* only BTST takes PC-relative and immediate */
    if (is_static && mode == 7 && EA_REG(op) == 4)
        return 0;                           /* BTST #n,#imm is not a valid encoding */

    int size = mode == 0 ? 4 : 1;
    uint8_t b = rv_tmp(t);
    if (is_static)
        rv_emit(t, rv_addi(b, 0, rv_fetch16(t) & (size == 4 ? 31 : 7)));
    else
        rv_emit(t, rv_andi(b, rv_dreg((op >> 9) & 7), size == 4 ? 31 : 7));

    if (!rv_ea_decode(t, mode, EA_REG(op), size, &ea)) {
        rv_free(t, b);
        return 0;
    }
    uint8_t v = rv_ea_read(t, &ea, size, &owned);

    /* Z = !bit */
    if (t->live & CCR_Z) {
        uint8_t z = rv_tmp(t);
        rv_emit(t, rv_bext(z, v, b));
        rv_emit(t, rv_seqz(z, z));
        rv_emit(t, rv_slli(z, z, 2));
        rv_emit(t, rv_andi(RVR_SR, RVR_SR, ~CCR_Z));
        rv_emit(t, rv_or(RVR_SR, RVR_SR, z));
        rv_free(t, z);
    }

    if (kind != 0) {
        uint8_t r = rv_tmp(t);
        rv_emit(t, kind == 1 ? rv_binv(r, v, b) : kind == 2 ? rv_bclr(r, v, b) : rv_bset(r, v, b));
        if (size == 4)
            rv_emit(t, rv_sext_w(r, r));    /* bit 31 may have changed */
        rv_ea_write(t, &ea, size, r);
        rv_free(t, r);
    }
    if (owned)
        rv_free(t, v);
    rv_ea_done(t, &ea);
    rv_free(t, b);
    return 1;
}

/* ADDQ SUBQ */
static int op_quick(struct rv_tc *t, uint16_t op)
{
    int size = size_bits[(op >> 6) & 3];
    int data = (op >> 9) & 7;
    int is_sub = (op >> 8) & 1;
    struct rv_ea ea;
    int owned;

    if (data == 0)
        data = 8;
    if (EA_MODE(op) == 1) {
        /* to An: whole register, no flags, any size */
        uint8_t an = rv_areg(EA_REG(op));
        rv_emit(t, rv_addiw(an, an, is_sub ? -data : data));
        return 1;
    }
    if (!rv_ea_decode(t, EA_MODE(op), EA_REG(op), size, &ea) || ea.kind == EA_IMM)
        return 0;
    uint8_t s = rv_tmp(t);
    rv_emit(t, rv_addi(s, 0, data));
    uint8_t v = read_ea(t, &ea, size, &owned);
    uint8_t r = alu(t, is_sub ? ALU_SUB : ALU_ADD, size, v, s);
    rv_ea_write(t, &ea, size, r);
    rv_free(t, r);
    rv_free(t, s);
    release(t, v, owned);
    rv_ea_done(t, &ea);
    return 1;
}

/* ------------------------------------------------------------ control flow */

static int op_bcc(struct rv_tc *t, uint16_t op)
{
    uint32_t base = t->pc + 2;
    int32_t disp = (int8_t)(op & 0xff);
    int cc = (op >> 8) & 15;

    if (disp == 0)
        disp = (int16_t)rv_fetch16(t);
    else if (disp == -1)
        disp = (int32_t)rv_fetch32(t);
    uint32_t target = base + (uint32_t)disp;
    uint32_t next = t->cursor;

    if (cc == 0) {                      /* BRA */
        set_pc(t, target);
    } else if (cc == 1) {               /* BSR */
        uint8_t r = rv_tmp(t);
        rv_emit_li(t, r, (int32_t)next);
        push_long(t, r);
        rv_free(t, r);
        set_pc(t, target);
    } else {
        uint8_t c = rv_tmp(t);
        rv_cond(t, cc, c);
        select_pc(t, c, target, next);
        rv_free(t, c);
    }
    t->ended = 1;
    return 1;
}

/* Scc <ea>: byte = condition ? 0xff : 0x00; flags unchanged */
static int op_scc(struct rv_tc *t, uint16_t op)
{
    struct rv_ea ea;
    int mode = EA_MODE(op);

    if (mode == 7 && EA_REG(op) >= 2)
        return rv_emit_trapcc(t, op);       /* TRAPcc */
    if (mode == 1)
        return 0;                           /* DBcc, decoded before */
    if (!rv_ea_decode(t, mode, EA_REG(op), 1, &ea))
        return 0;
    uint8_t c = rv_tmp(t);
    rv_cond(t, (op >> 8) & 15, c);
    rv_emit(t, rv_neg(c, c));               /* 1 -> 0xff..ff, 0 -> 0 */
    rv_ea_write(t, &ea, 1, c);
    rv_ea_done(t, &ea);
    rv_free(t, c);
    return 1;
}

static int op_dbcc(struct rv_tc *t, uint16_t op)
{
    uint8_t dn = rv_dreg(EA_REG(op));
    uint32_t target = t->pc + 2 + (uint32_t)(int32_t)(int16_t)rv_fetch16(t);
    uint32_t next = t->cursor;
    uint8_t c = rv_tmp(t), w = rv_tmp(t);

    /* condition true: fall through, without touching Dn */
    rv_cond(t, (op >> 8) & 15, c);
    uint32_t *b_true = t->ptr;
    rv_emit(t, rv_bnez(c, 0));

    /* Dn.W -= 1; loop unless it became -1 */
    rv_emit(t, rv_addi(w, dn, -1));
    rv_emit(t, rv_zext_h(w, w));
    struct rv_ea d = { .kind = EA_DREG, .reg = dn };
    rv_ea_write(t, &d, 2, w);
    rv_emit(t, rv_xori(w, w, -1));          /* 0xffff -> ...ffff0000 */
    rv_emit(t, rv_slli(w, w, 48));          /* zero exactly when the counter is -1 */
    uint32_t *b_expired = t->ptr;
    rv_emit(t, rv_beqz(w, 0));
    set_pc(t, target);
    uint32_t *j_end = t->ptr;
    rv_emit(t, rv_jal(0, 0));
    uint32_t *l_fall = t->ptr;
    set_pc(t, next);
    if (!t->overflow) {
        *b_true = rv_patch_b(*b_true, (int32_t)((l_fall - b_true) * 4));
        *b_expired = rv_patch_b(*b_expired, (int32_t)((l_fall - b_expired) * 4));
        *j_end = rv_patch_j(*j_end, (int32_t)((t->ptr - j_end) * 4));
    }
    rv_free(t, c);
    rv_free(t, w);
    t->ended = 1;
    return 1;
}

static int op_jmp_jsr(struct rv_tc *t, uint16_t op, int is_jsr)
{
    struct rv_ea ea;
    int mode = EA_MODE(op);

    if (mode < 2 || mode == 3 || mode == 4)
        return 0;
    if (!rv_ea_decode(t, mode, EA_REG(op), 0, &ea) || ea.kind != EA_MEM)
        return 0;
    uint8_t a = rv_ea_address(t, &ea);
    rv_ea_done(t, &ea);
    if (is_jsr) {
        uint8_t r = rv_tmp(t);
        rv_emit_li(t, r, (int32_t)t->cursor);
        push_long(t, r);
        rv_free(t, r);
    }
    rv_emit(t, rv_mv(RVR_PC, a));
    rv_free(t, a);
    t->ended = 1;
    return 1;
}

static int op_rts(struct rv_tc *t)
{
    uint8_t sp = rv_areg(7);
    rv_load68(t, 4, RVR_PC, sp, 0);
    rv_emit(t, rv_addiw(sp, sp, 4));
    t->ended = 1;
    return 1;
}

/* ------------------------------------------------------------ decoder */

static int line4(struct rv_tc *t, uint16_t op)
{
    if (op == 0x4e71)                           /* NOP */
        return 1;
    if (op == 0x4e75)
        return op_rts(t);
    if (op == 0x4e73)
        return rv_emit_rte(t);
    if ((op & 0xfff0) == 0x4e60)
        return rv_emit_move_usp(t, op);
    if (op == 0x4e76)
        return rv_emit_trapv(t);
    if (op == 0x4afc || (op & 0xfff0) == 0x4e40)
        return rv_emit_trap(t, op);             /* ILLEGAL, TRAP #n */
    if ((op & 0xfff8) == 0x4e50)
        return op_link(t, op);
    if ((op & 0xfff8) == 0x4e58)
        return op_unlk(t, op);
    if ((op & 0xffc0) == 0x4ec0)
        return op_jmp_jsr(t, op, 0);
    if ((op & 0xffc0) == 0x4e80)
        return op_jmp_jsr(t, op, 1);
    /* CHK before EXT: CHK.W <ea>,D4 (0x4980) also matches the EXT pattern */
    if ((op & 0xf140) == 0x4100)
        return rv_emit_chk(t, op);              /* CHK.L, CHK.W (exception.c) */
    /* EXT/EXTB before LEA: EXTB.L (0x49c0) also matches the LEA pattern */
    if ((op & 0xfeb8) == 0x4880 || (op & 0xfff8) == 0x49c0)
        return op_ext(t, op);
    if ((op & 0xf1c0) == 0x41c0)
        return op_lea(t, op);
    if ((op & 0xffc0) == 0x4800)
        return rv_emit_nbcd(t, op);             /* NBCD (bcd.c) */
    if ((op & 0xf9c0) == 0x40c0 || (op & 0xfffe) == 0x4e7a)
        return rv_emit_sr_op(t, op);            /* SR, CCR, MOVEC (sr.c) */
    if ((op & 0xfff8) == 0x4840)
        return op_swap(t, op);
    if ((op & 0xffc0) == 0x4840)
        return op_pea(t, op);
    if ((op & 0xff80) == 0x4c00)
        return rv_emit_muldiv_l(t, op);
    if ((op & 0xfb80) == 0x4880)
        return op_movem(t, op);
    if ((op & 0xff00) == 0x4200 && (op & 0xc0) != 0xc0)
        return op_clr(t, op);
    if (((op & 0xff00) == 0x4000 || (op & 0xff00) == 0x4400 || (op & 0xff00) == 0x4600) &&
        (op & 0xc0) != 0xc0)
        return rv_emit_neg_not(t, op);
    if ((op & 0xffc0) == 0x4ac0)
        return rv_emit_tas(t, op);              /* TAS (misc.c); ILLEGAL is taken above */
    if ((op & 0xff00) == 0x4a00 && (op & 0xc0) != 0xc0)
        return op_tst(t, op);
    return 0;
}

int rv_emit_insn(struct rv_tc *t, uint16_t op)
{
    switch (op >> 12) {
    case 0x0:
        if ((op & 0xf9c0) == 0x08c0 && (op & 0x0600))
            return rv_emit_cas(t, op);          /* CAS, CAS2 (cas.c) */
        if ((op & 0x0138) == 0x0108)
            return rv_emit_movep(t, op);        /* MOVEP (misc.c) */
        if ((op & 0x0100) || (op & 0xff00) == 0x0800)
            return op_bit(t, op);
        return op_immediate(t, op);
    case 0x1: case 0x2: case 0x3:
        return op_move(t, op);
    case 0x4:
        return line4(t, op);
    case 0x5:
        if ((op & 0xf0f8) == 0x50c8)
            return op_dbcc(t, op);
        if ((op & 0xc0) == 0xc0)
            return op_scc(t, op);               /* TRAPcc not yet */
        return op_quick(t, op);
    case 0x6:
        return op_bcc(t, op);
    case 0x7:
        return op_moveq(t, op);
    case 0x8:
        if ((op & 0xc0) == 0xc0)
            return rv_emit_div_w(t, op);
        if ((op & 0x01f0) == 0x0100)
            return rv_emit_abcd_sbcd(t, op);    /* SBCD (bcd.c) */
        if ((op & 0x01f0) == 0x0140 || (op & 0x01f0) == 0x0180)
            return rv_emit_pack_unpk(t, op);    /* PACK, UNPK (misc.c) */
        return op_alu_dn(t, op, ALU_OR);
    case 0x9:
        if ((op & 0xc0) == 0xc0)
            return op_alu_an(t, op, ALU_SUB);
        if ((op & 0x0130) == 0x0100)
            return rv_emit_addx_subx(t, op);
        return op_alu_dn(t, op, ALU_SUB);
    case 0xb:
        if ((op & 0xc0) == 0xc0)
            return op_alu_an(t, op, ALU_CMP);
        if (op & 0x0100) {
            if (EA_MODE(op) == 1)
                return rv_emit_cmpm(t, op);     /* CMPM (misc.c) */
            return op_alu_dn(t, op, ALU_EOR);   /* EOR Dn,<ea> */
        }
        return op_alu_dn(t, op, ALU_CMP);
    case 0xc:
        if ((op & 0xc0) == 0xc0)
            return rv_emit_mul_w(t, op);
        if ((op & 0x01f0) == 0x0100)
            return rv_emit_abcd_sbcd(t, op);    /* ABCD (bcd.c) */
        if ((op & 0x0130) == 0x0100)
            return rv_emit_exg(t, op);          /* EXG (misc.c) */
        return op_alu_dn(t, op, ALU_AND);
    case 0xd:
        if ((op & 0xc0) == 0xc0)
            return op_alu_an(t, op, ALU_ADD);
        if ((op & 0x0130) == 0x0100)
            return rv_emit_addx_subx(t, op);
        return op_alu_dn(t, op, ALU_ADD);
    case 0xe:
        return rv_emit_lineE(t, op);
    case 0xa:
        return rv_emit_trap(t, op);             /* Line-A */
    case 0xf:
        /* Line-F: the FPU (coprocessor 1) is not built, so its opcodes
           trap; MOVE16, CINV, CPUSH, PFLUSH, PTEST are not translated yet */
        if (((op >> 9) & 7) == 1)
            return rv_emit_trap(t, op);
        return 0;
    default:
        return 0;
    }
}
