/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Code generation helpers of the RV64 translator: emission, m68k memory
    access, effective addresses and condition codes. See jit_internal.h for
    the conventions.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

/* ------------------------------------------------------------ emission */

void rv_emit(struct rv_tc *t, uint32_t insn)
{
    if (t->ptr >= t->end) {
        t->overflow = 1;
        return;
    }
    *t->ptr++ = insn;
}

void rv_emit_li(struct rv_tc *t, uint8_t rd, int64_t value)
{
    uint32_t buf[8];
    int n = rv_li(buf, rd, value);
    for (int i = 0; i < n; i++)
        rv_emit(t, buf[i]);
}

uint8_t rv_tmp(struct rv_tc *t)
{
    for (int r = 5; r < 32; r++) {
        if (t->temps & (1u << r)) {
            t->temps &= ~(1u << r);
            return (uint8_t)r;
        }
    }
    kprintf("[JIT] out of temporary registers at %08x\n", t->pc);
    return 5;
}

void rv_free(struct rv_tc *t, uint8_t reg)
{
    if (RV_TEMP_MASK & (1u << reg))
        t->temps |= 1u << reg;
}

/* ------------------------------------------------------------ instruction stream */

uint16_t rv_fetch16(struct rv_tc *t)
{
    const uint8_t *p = (const uint8_t *)(intptr_t)(int32_t)t->cursor;
    t->cursor += 2;
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t rv_fetch32(struct rv_tc *t)
{
    uint32_t hi = rv_fetch16(t);
    return (hi << 16) | rv_fetch16(t);
}

uint8_t rv_dreg(int n)
{
    static const uint8_t map[8] = {
        RVR_D0, RVR_D1, RVR_D2, RVR_D3, RVR_D4, RVR_D5, RVR_D6, RVR_D7
    };
    return map[n & 7];
}

uint8_t rv_areg(int n)
{
    return (uint8_t)(RVR_A0 + (n & 7));
}

/* ------------------------------------------------------------ memory */

/* Address base + off as (register, small offset) usable by a load/store. */
static uint8_t addr_for(struct rv_tc *t, uint8_t base, int32_t *off, int *tmp_used)
{
    *tmp_used = 0;
    if (rv_imm12_ok(*off))
        return base;
    uint8_t a = rv_tmp(t);
    rv_emit_li(t, a, *off);
    rv_emit(t, rv_add(a, base, a));
    *off = 0;
    *tmp_used = 1;
    return a;
}

void rv_load68(struct rv_tc *t, int size, uint8_t rd, uint8_t base, int32_t off)
{
    int tmp_used;
    uint8_t a = addr_for(t, base, &off, &tmp_used);

    switch (size) {
    case 1:
        rv_emit(t, rv_lbu(rd, a, off));
        break;
    case 2:
        rv_emit(t, rv_lhu(rd, a, off));
        rv_emit(t, rv_rev8(rd, rd));
        rv_emit(t, rv_srli(rd, rd, 48));
        break;
    default:
        rv_emit(t, rv_lw(rd, a, off));
        rv_emit(t, rv_rev8(rd, rd));
        rv_emit(t, rv_srai(rd, rd, 32));
        break;
    }
    if (tmp_used)
        rv_free(t, a);
}

void rv_store68(struct rv_tc *t, int size, uint8_t val, uint8_t base, int32_t off)
{
    int tmp_used;
    uint8_t a = addr_for(t, base, &off, &tmp_used);

    if (size == 1) {
        rv_emit(t, rv_sb(val, a, off));
    } else {
        uint8_t s = rv_tmp(t);
        rv_emit(t, rv_rev8(s, val));
        rv_emit(t, rv_srli(s, s, size == 2 ? 48 : 32));
        rv_emit(t, size == 2 ? rv_sh(s, a, off) : rv_sw(s, a, off));
        rv_free(t, s);
    }
    if (tmp_used)
        rv_free(t, a);
}

/* ------------------------------------------------------------ effective addresses */

/* Index register of a brief extension word, scaled, in a new temporary. */
static uint8_t brief_index(struct rv_tc *t, uint16_t ext)
{
    int n = (ext >> 12) & 7;
    uint8_t x = (ext & 0x8000) ? rv_areg(n) : rv_dreg(n);
    uint8_t i = rv_tmp(t);

    if (ext & 0x0800)
        rv_emit(t, rv_mv(i, x));
    else
        rv_emit(t, rv_sext_h(i, x));
    int scale = (ext >> 9) & 3;
    if (scale)
        rv_emit(t, rv_slli(i, i, (uint8_t)scale));
    return i;
}

/*
    68020 full extension word (bit 8 set): base An or PC (or suppressed),
    base displacement, index (or suppressed), optional memory indirection,
    pre- or post-indexed, and an outer displacement. The address is computed
    into a temporary and kept as a canonical 32-bit value, since the 68k adds
    addresses modulo 2^32 and the sum may otherwise leave the mirrored window.
    base_reg is the host register of An, or 0 with pc_base for PC-relative.
    Returns the temporary, or 0 for a reserved encoding.
*/
static uint8_t full_extension(struct rv_tc *t, uint16_t ext, uint8_t base_reg, uint32_t pc_base)
{
    int bs = (ext >> 7) & 1, is = (ext >> 6) & 1;
    int bd_size = (ext >> 4) & 3, iis = ext & 7;

    if (bd_size == 0 || (ext & 0x08) || (is && iis > 3) || (!is && iis == 4))
        return 0;

    int32_t bd = 0;
    if (bd_size == 2)
        bd = (int16_t)rv_fetch16(t);
    else if (bd_size == 3)
        bd = (int32_t)rv_fetch32(t);

    int od_size = iis & 3;          /* 0 no indirection (iis 0), 1 null, 2 word, 3 long */
    int32_t od = 0;
    if (iis != 0) {
        if (od_size == 2)
            od = (int16_t)rv_fetch16(t);
        else if (od_size == 3)
            od = (int32_t)rv_fetch32(t);
    }
    int indirect = iis != 0;
    int post = indirect && !is && (iis & 4);

    /* a = base + bd */
    uint8_t a = rv_tmp(t);
    if (bs) {
        rv_emit_li(t, a, bd);
    } else if (base_reg) {
        if (rv_imm12_ok(bd)) {
            rv_emit(t, rv_addiw(a, base_reg, bd));
        } else {
            rv_emit_li(t, a, bd);
            rv_emit(t, rv_addw(a, base_reg, a));
        }
    } else {
        rv_emit_li(t, a, (int32_t)(pc_base + (uint32_t)bd));
    }

    uint8_t i = 0;
    if (!is)
        i = brief_index(t, ext);

    if (i && !post) {               /* index before any indirection */
        rv_emit(t, rv_addw(a, a, i));
    }
    if (indirect) {
        rv_load68(t, 4, a, a, 0);   /* fetch the pointer */
        if (i && post)
            rv_emit(t, rv_addw(a, a, i));
        if (od) {
            uint8_t k = rv_tmp(t);
            rv_emit_li(t, k, od);
            rv_emit(t, rv_addw(a, a, k));
            rv_free(t, k);
        }
    }
    if (i)
        rv_free(t, i);
    return a;
}

int rv_ea_decode(struct rv_tc *t, int mode, int reg, int size, struct rv_ea *ea)
{
    ea->base_is_tmp = 0;
    ea->off = 0;
    ea->postinc_areg = -1;
    ea->postinc_step = 0;

    switch (mode) {
    case 0:
        ea->kind = EA_DREG;
        ea->reg = rv_dreg(reg);
        return 1;
    case 1:
        ea->kind = EA_AREG;
        ea->reg = rv_areg(reg);
        return 1;
    case 2:
        ea->kind = EA_MEM;
        ea->base = rv_areg(reg);
        return 1;
    case 3:
        ea->kind = EA_MEM;
        ea->base = rv_areg(reg);
        ea->postinc_areg = (int8_t)reg;
        ea->postinc_step = (int8_t)((size == 1 && reg == 7) ? 2 : size);
        return size != 0;
    case 4: {
        int step = (size == 1 && reg == 7) ? 2 : size;
        if (size == 0)
            return 0;
        ea->kind = EA_MEM;
        ea->base = rv_areg(reg);
        rv_emit(t, rv_addiw(ea->base, ea->base, -step));
        return 1;
    }
    case 5:
        ea->kind = EA_MEM;
        ea->base = rv_areg(reg);
        ea->off = (int16_t)rv_fetch16(t);
        return 1;
    case 6: {
        uint16_t ext = rv_fetch16(t);
        if (ext & 0x0100) {
            uint8_t a = full_extension(t, ext, rv_areg(reg), 0);
            if (!a)
                return 0;
            ea->kind = EA_MEM;
            ea->base = a;
            ea->base_is_tmp = 1;
            return 1;
        }
        uint8_t i = brief_index(t, ext);
        rv_emit(t, rv_addw(i, rv_areg(reg), i));   /* 32-bit wrap, canonical */
        ea->kind = EA_MEM;
        ea->base = i;
        ea->base_is_tmp = 1;
        ea->off = (int8_t)(ext & 0xff);
        return 1;
    }
    case 7:
        break;
    default:
        return 0;
    }

    switch (reg) {
    case 0:                         /* abs.W */
    case 1: {                       /* abs.L */
        int32_t a = reg == 0 ? (int16_t)rv_fetch16(t) : (int32_t)rv_fetch32(t);
        ea->kind = EA_MEM;
        ea->base = rv_tmp(t);
        ea->base_is_tmp = 1;
        rv_emit_li(t, ea->base, a);
        return 1;
    }
    case 2: {                       /* d16(PC) */
        uint32_t ext_pc = t->cursor;
        int32_t d = (int16_t)rv_fetch16(t);
        ea->kind = EA_MEM;
        ea->base = rv_tmp(t);
        ea->base_is_tmp = 1;
        rv_emit_li(t, ea->base, (int32_t)(ext_pc + (uint32_t)d));
        return 1;
    }
    case 3: {                       /* d8(PC,Xn) */
        uint32_t ext_pc = t->cursor;
        uint16_t ext = rv_fetch16(t);
        if (ext & 0x0100) {
            uint8_t a = full_extension(t, ext, 0, ext_pc);
            if (!a)
                return 0;
            ea->kind = EA_MEM;
            ea->base = a;
            ea->base_is_tmp = 1;
            return 1;
        }
        uint8_t i = brief_index(t, ext);
        uint8_t b = rv_tmp(t);
        rv_emit_li(t, b, (int32_t)ext_pc);
        rv_emit(t, rv_addw(i, b, i));               /* 32-bit wrap, canonical */
        rv_free(t, b);
        ea->kind = EA_MEM;
        ea->base = i;
        ea->base_is_tmp = 1;
        ea->off = (int8_t)(ext & 0xff);
        return 1;
    }
    case 4:                         /* #imm */
        if (size == 0)
            return 0;
        ea->kind = EA_IMM;
        if (size == 4)
            ea->imm = (int32_t)rv_fetch32(t);
        else if (size == 2)
            ea->imm = (int16_t)rv_fetch16(t);
        else
            ea->imm = (int8_t)(rv_fetch16(t) & 0xff);
        return 1;
    default:
        return 0;
    }
}

uint8_t rv_ea_read(struct rv_tc *t, struct rv_ea *ea, int size, int *owned)
{
    uint8_t r;

    switch (ea->kind) {
    case EA_DREG:
    case EA_AREG:
        *owned = 0;
        return ea->reg;
    case EA_IMM:
        r = rv_tmp(t);
        rv_emit_li(t, r, ea->imm);
        *owned = 1;
        return r;
    default:
        r = rv_tmp(t);
        rv_load68(t, size, r, ea->base, ea->off);
        *owned = 1;
        return r;
    }
}

void rv_ea_write(struct rv_tc *t, struct rv_ea *ea, int size, uint8_t val)
{
    if (ea->kind == EA_MEM) {
        rv_store68(t, size, val, ea->base, ea->off);
        return;
    }
    uint8_t d = ea->reg;
    if (ea->kind == EA_AREG) {
        /* Address registers are always written as a whole, words sign-extended */
        rv_emit(t, size == 2 ? rv_sext_h(d, val) : rv_mv(d, val));
        return;
    }
    if (size == 4) {
        if (d != val)
            rv_emit(t, rv_mv(d, val));
    } else if (size == 2) {
        uint8_t lo = rv_tmp(t);
        rv_emit(t, rv_zext_h(lo, val));
        rv_emit(t, rv_srai(d, d, 16));
        rv_emit(t, rv_slli(d, d, 16));
        rv_emit(t, rv_or(d, d, lo));
        rv_free(t, lo);
    } else {
        uint8_t lo = rv_tmp(t);
        rv_emit(t, rv_andi(lo, val, 0xff));
        rv_emit(t, rv_andi(d, d, -256));
        rv_emit(t, rv_or(d, d, lo));
        rv_free(t, lo);
    }
}

uint8_t rv_ea_address(struct rv_tc *t, struct rv_ea *ea)
{
    uint8_t a = rv_tmp(t);
    /* addiw keeps the 32-bit wrap-around and the canonical form */
    if (rv_imm12_ok(ea->off)) {
        rv_emit(t, rv_addiw(a, ea->base, ea->off));
    } else {
        rv_emit_li(t, a, ea->off);
        rv_emit(t, rv_addw(a, ea->base, a));
    }
    return a;
}

void rv_ea_done(struct rv_tc *t, struct rv_ea *ea)
{
    if (ea->postinc_areg >= 0) {
        uint8_t an = rv_areg(ea->postinc_areg);
        rv_emit(t, rv_addiw(an, an, ea->postinc_step));
    }
    if (ea->base_is_tmp)
        rv_free(t, ea->base);
    ea->base_is_tmp = 0;
    ea->postinc_areg = -1;
}

/* ------------------------------------------------------------ condition codes */

/* SR = (SR & keep) | bits */
static void merge_ccr(struct rv_tc *t, uint8_t bits, int keep_x)
{
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, keep_x ? -16 : -32));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, bits));
}

/* v = N<<3 | Z<<2 | V<<1 | C, accumulated in v (as in sw/flag-recipes) */
static void pack_nzvc(struct rv_tc *t, uint8_t n, uint8_t z, uint8_t v, uint8_t c)
{
    rv_emit(t, rv_sh1add(v, v, c));
    rv_emit(t, rv_sh2add(v, z, v));
    rv_emit(t, rv_sh3add(v, n, v));
}

void rv_flags_nz(struct rv_tc *t, int size, uint8_t result)
{
    uint8_t n = rv_tmp(t), z = rv_tmp(t);
    uint8_t src = result;

    if (size != 4) {
        rv_emit(t, rv_slli(z, result, (uint8_t)(64 - 8 * size)));
        src = z;
    }
    rv_emit(t, rv_sltz(n, src));
    rv_emit(t, rv_seqz(z, src));
    rv_emit(t, rv_sh1add(n, n, z));         /* N<<1 | Z */
    rv_emit(t, rv_slli(n, n, 2));
    merge_ccr(t, n, 1);
    rv_free(t, n);
    rv_free(t, z);
}

static void flags_addsub(struct rv_tc *t, int size, uint8_t d, uint8_t s, uint8_t r_out,
                         int set_x, int is_sub)
{
    uint8_t r = rv_tmp(t), v = rv_tmp(t), c = rv_tmp(t), n = rv_tmp(t);

    if (size == 4) {
        /* d and s are canonical: sltu gives the 32-bit carry or borrow, and
           the exact 64-bit sum differs from the 32-bit one on overflow */
        rv_emit(t, is_sub ? rv_subw(r, d, s) : rv_addw(r, d, s));
        rv_emit(t, is_sub ? rv_sub(v, d, s) : rv_add(v, d, s));
        rv_emit(t, rv_xor(v, v, r));
        rv_emit(t, rv_snez(v, v));
        rv_emit(t, is_sub ? rv_sltu(c, d, s) : rv_sltu(c, r, d));
        rv_emit(t, rv_sltz(n, r));
        if (r_out != r)
            rv_emit(t, rv_mv(r_out, r));
        rv_emit(t, rv_seqz(r, r));          /* r becomes Z */
    } else {
        /* byte and word: operands at the top of the register */
        uint8_t sh = (uint8_t)(64 - 8 * size);
        uint8_t ds = rv_tmp(t), ss = rv_tmp(t);
        rv_emit(t, rv_slli(ds, d, sh));
        rv_emit(t, rv_slli(ss, s, sh));
        rv_emit(t, is_sub ? rv_sub(r, ds, ss) : rv_add(r, ds, ss));
        rv_emit(t, is_sub ? rv_sltu(c, ds, ss) : rv_sltu(c, r, ds));
        if (is_sub) {
            rv_emit(t, rv_xor(v, ds, ss));      /* V = ((d^s) & (d^r)) < 0 */
            rv_emit(t, rv_xor(ss, ds, r));
        } else {
            rv_emit(t, rv_xor(v, ds, r));       /* V = ((d^r) & (s^r)) < 0 */
            rv_emit(t, rv_xor(ss, ss, r));
        }
        rv_emit(t, rv_and(v, v, ss));
        rv_emit(t, rv_sltz(v, v));
        rv_emit(t, rv_sltz(n, r));
        rv_emit(t, rv_srli(r_out, r, sh));  /* result, zero-extended */
        rv_emit(t, rv_seqz(r, r));          /* r becomes Z */
        rv_free(t, ds);
        rv_free(t, ss);
    }

    /* r holds Z now */
    pack_nzvc(t, n, r, v, c);
    if (set_x) {
        rv_emit(t, rv_slli(c, c, 4));
        rv_emit(t, rv_or(v, v, c));
    }
    merge_ccr(t, v, !set_x);
    rv_free(t, r);
    rv_free(t, v);
    rv_free(t, c);
    rv_free(t, n);
}

/*
    r_out receives d + s (or d - s) at the operation size: zero-extended for
    bytes and words, canonical for longs. For bytes and words it must not be
    the destination register itself; merge it with rv_ea_write.
*/
void rv_flags_add(struct rv_tc *t, int size, uint8_t d, uint8_t s, uint8_t r_out, int set_x)
{
    flags_addsub(t, size, d, s, r_out, set_x, 0);
}

void rv_flags_sub(struct rv_tc *t, int size, uint8_t d, uint8_t s, uint8_t r_out, int set_x)
{
    flags_addsub(t, size, d, s, r_out, set_x, 1);
}

void rv_cond(struct rv_tc *t, int cc, uint8_t rd)
{
    uint8_t a, b;

    switch (cc & 15) {
    case 0:  rv_emit(t, rv_addi(rd, 0, 1)); break;                  /* T  */
    case 1:  rv_emit(t, rv_addi(rd, 0, 0)); break;                  /* F  */
    case 2:  rv_emit(t, rv_andi(rd, RVR_SR, CCR_C | CCR_Z));        /* HI */
             rv_emit(t, rv_seqz(rd, rd)); break;
    case 3:  rv_emit(t, rv_andi(rd, RVR_SR, CCR_C | CCR_Z));        /* LS */
             rv_emit(t, rv_snez(rd, rd)); break;
    case 4:  rv_emit(t, rv_bexti(rd, RVR_SR, 0));                   /* CC */
             rv_emit(t, rv_xori(rd, rd, 1)); break;
    case 5:  rv_emit(t, rv_bexti(rd, RVR_SR, 0)); break;            /* CS */
    case 6:  rv_emit(t, rv_bexti(rd, RVR_SR, 2));                   /* NE */
             rv_emit(t, rv_xori(rd, rd, 1)); break;
    case 7:  rv_emit(t, rv_bexti(rd, RVR_SR, 2)); break;            /* EQ */
    case 8:  rv_emit(t, rv_bexti(rd, RVR_SR, 1));                   /* VC */
             rv_emit(t, rv_xori(rd, rd, 1)); break;
    case 9:  rv_emit(t, rv_bexti(rd, RVR_SR, 1)); break;            /* VS */
    case 10: rv_emit(t, rv_bexti(rd, RVR_SR, 3));                   /* PL */
             rv_emit(t, rv_xori(rd, rd, 1)); break;
    case 11: rv_emit(t, rv_bexti(rd, RVR_SR, 3)); break;            /* MI */
    default:                                                        /* GE LT GT LE */
        a = rv_tmp(t);
        b = rv_tmp(t);
        rv_emit(t, rv_bexti(a, RVR_SR, 3));
        rv_emit(t, rv_bexti(b, RVR_SR, 1));
        rv_emit(t, rv_xor(rd, a, b));       /* N != V */
        if ((cc & 15) >= 14) {
            rv_emit(t, rv_bexti(a, RVR_SR, 2));
            rv_emit(t, rv_or(rd, rd, a));   /* Z || N != V */
        }
        if ((cc & 15) == 12 || (cc & 15) == 14)
            rv_emit(t, rv_seqz(rd, rd));    /* GE, GT: negate */
        rv_free(t, a);
        rv_free(t, b);
        break;
    }
}
