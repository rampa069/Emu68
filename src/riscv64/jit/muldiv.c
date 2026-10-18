/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Multiplication and division for the RV64 translator:

      MULU.W MULS.W   16 x 16 -> 32
      DIVU.W DIVS.W   32 / 16 -> 16 remainder : 16 quotient
      MULU.L MULS.L   32 x 32 -> 32 (V on overflow) or -> 64 in Dh:Dl
      DIVU.L DIVS.L   32 / 32 -> 32 quotient, optional 32 remainder in Dr,
                      or 64 (Dr:Dq) / 32

    Flags follow the M68000 PRM: N and Z from the result, V on overflow,
    C = 0, X unchanged. On a division overflow the destination is left
    unchanged and N and Z, which the PRM leaves undefined, are set to N = 1,
    Z = 0 in every form, as TG68K.C does (and as real 68000 hardware is
    reported to do; not verified here). Emu68 on AArch64 derives them from
    the quotient instead (word forms: its low 16 bits; long forms: Dq after
    the instruction).

    The 32-bit DIVS.L overflow of -2^31 / -1 sets V, as the PRM requires.
    Emu68 on AArch64 does not detect it (sdiv does not trap).

    A zero divisor raises the zero divide exception (vector 5) with a format
    $2 frame: PC of the next instruction and the address of the division.
    N, Z and V are undefined then (C = 0); they are set to N = 0, Z = 1,
    V = 0 with X kept, which is what TG68K.C does in every form and for any
    previous CCR (measured over 106 cases). Emu68 on AArch64 leaves the CCR
    unchanged.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

#define VECTOR_ZERO_DIVIDE 5

/* if (divisor == 0) take the zero divide exception and leave the unit */
static void zero_divide_check(struct rv_tc *t, uint8_t divisor)
{
    uint32_t *b = t->ptr;
    rv_emit(t, rv_bnez(divisor, 0));
    uint8_t z = rv_tmp(t);
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));   /* CCR = X | Z */
    rv_emit(t, rv_addi(z, 0, 4));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, z));
    rv_free(t, z);
    rv_emit_exception(t, VECTOR_ZERO_DIVIDE, 2, t->cursor, t->pc);
    rv_emit(t, rv_ret());
    if (!t->overflow)
        *b = rv_patch_b(*b, (int32_t)((t->ptr - b) * 4));
}

/* SR = (SR & ~0xf) | N<<3 | Z<<2 | V<<1 */
static void set_nzv(struct rv_tc *t, uint8_t n, uint8_t z, uint8_t v)
{
    rv_emit(t, rv_slli(v, v, 1));
    rv_emit(t, rv_sh2add(v, z, v));
    rv_emit(t, rv_sh3add(v, n, v));
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, v));
}

/* MULU.W / MULS.W <ea>,Dn */
int rv_emit_mul_w(struct rv_tc *t, uint16_t op)
{
    int is_signed = (op >> 8) & 1;
    uint8_t dn = rv_dreg((op >> 9) & 7);
    struct rv_ea ea;
    int owned;

    if (((op >> 3) & 7) == 1 || !rv_ea_decode(t, (op >> 3) & 7, op & 7, 2, &ea))
        return 0;
    uint8_t s = rv_ea_read(t, &ea, 2, &owned);
    uint8_t a = rv_tmp(t), b = rv_tmp(t);
    rv_emit(t, is_signed ? rv_sext_h(a, dn) : rv_zext_h(a, dn));
    rv_emit(t, is_signed ? rv_sext_h(b, s) : rv_zext_h(b, s));
    rv_emit(t, rv_mulw(dn, a, b));          /* exact in 32 bits, canonical */
    rv_free(t, a);
    rv_free(t, b);
    if (owned)
        rv_free(t, s);
    rv_ea_done(t, &ea);
    rv_flags_nz(t, 4, dn);                  /* V = C = 0 */
    return 1;
}

/* DIVU.W / DIVS.W <ea>,Dn */
int rv_emit_div_w(struct rv_tc *t, uint16_t op)
{
    int is_signed = (op >> 8) & 1;
    uint8_t dn = rv_dreg((op >> 9) & 7);
    struct rv_ea ea;
    int owned;

    if (((op >> 3) & 7) == 1 || !rv_ea_decode(t, (op >> 3) & 7, op & 7, 2, &ea))
        return 0;
    uint8_t s = rv_ea_read(t, &ea, 2, &owned);
    uint8_t b = rv_tmp(t);
    rv_emit(t, is_signed ? rv_sext_h(b, s) : rv_zext_h(b, s));
    if (owned)
        rv_free(t, s);
    rv_ea_done(t, &ea);
    zero_divide_check(t, b);

    uint8_t a = rv_tmp(t), q = rv_tmp(t), r = rv_tmp(t), ov = rv_tmp(t);
    rv_emit(t, is_signed ? rv_mv(a, dn) : rv_zext_w(a, dn));
    rv_emit(t, is_signed ? rv_div(q, a, b) : rv_divu(q, a, b));
    rv_emit(t, is_signed ? rv_rem(r, a, b) : rv_remu(r, a, b));

    /* overflow: quotient outside 16 bits (unsigned) or -32768..32767 (signed) */
    if (is_signed) {
        rv_emit_li(t, ov, 32768);
        rv_emit(t, rv_add(ov, q, ov));
    } else {
        rv_emit(t, rv_mv(ov, q));
    }
    rv_emit(t, rv_srli(ov, ov, 16));
    rv_emit(t, rv_snez(ov, ov));

    /* Dn = remainder:quotient unless overflow */
    uint32_t *skip = t->ptr;
    rv_emit(t, rv_bnez(ov, 0));
    rv_emit(t, rv_slliw(a, r, 16));
    rv_emit(t, rv_zext_h(r, q));
    rv_emit(t, rv_or(dn, a, r));
    if (!t->overflow)
        *skip = rv_patch_b(*skip, (int32_t)((t->ptr - skip) * 4));

    /* N Z from the 16-bit quotient; on overflow N = 1, Z = 0 */
    rv_emit(t, rv_slli(a, q, 48));
    rv_emit(t, rv_sltz(r, a));
    rv_emit(t, rv_seqz(a, a));
    rv_emit(t, rv_or(r, r, ov));
    rv_emit(t, rv_andn(a, a, ov));
    set_nzv(t, r, a, ov);
    rv_free(t, a);
    rv_free(t, q);
    rv_free(t, r);
    rv_free(t, ov);
    rv_free(t, b);
    return 1;
}

/* MULU.L / MULS.L <ea>,Dl or <ea>,Dh:Dl (0x4c00) */
static int mul_l(struct rv_tc *t, uint16_t op, uint16_t ext)
{
    int is_signed = (ext >> 11) & 1, is64 = (ext >> 10) & 1;
    uint8_t dl = rv_dreg((ext >> 12) & 7), dh = rv_dreg(ext & 7);
    struct rv_ea ea;
    int owned;

    if (!rv_ea_decode(t, (op >> 3) & 7, op & 7, 4, &ea))
        return 0;
    uint8_t s = rv_ea_read(t, &ea, 4, &owned);
    uint8_t a = rv_tmp(t), b = rv_tmp(t), p = rv_tmp(t);
    rv_emit(t, is_signed ? rv_mv(a, dl) : rv_zext_w(a, dl));
    rv_emit(t, is_signed ? rv_mv(b, s) : rv_zext_w(b, s));
    rv_emit(t, rv_mul(p, a, b));            /* exact 64-bit product */
    if (owned)
        rv_free(t, s);
    rv_ea_done(t, &ea);

    uint8_t n = a, z = b, v = rv_tmp(t);
    if (is64) {
        rv_emit(t, rv_sltz(n, p));
        rv_emit(t, rv_seqz(z, p));
        rv_emit(t, rv_addi(v, 0, 0));
        rv_emit(t, rv_srai(dh, p, 32));
        rv_emit(t, rv_sext_w(dl, p));
    } else {
        rv_emit(t, rv_sext_w(dl, p));
        if (is_signed) {
            rv_emit(t, rv_xor(v, p, dl));   /* V: product does not fit in 32 bits */
        } else {
            rv_emit(t, rv_srli(v, p, 32));
        }
        rv_emit(t, rv_snez(v, v));
        rv_emit(t, rv_sltz(n, dl));
        rv_emit(t, rv_seqz(z, dl));
    }
    set_nzv(t, n, z, v);
    rv_free(t, a);
    rv_free(t, b);
    rv_free(t, p);
    rv_free(t, v);
    return 1;
}

/* DIVU.L / DIVS.L <ea>,Dq, <ea>,Dr:Dq (32 or 64-bit dividend) (0x4c40) */
static int div_l(struct rv_tc *t, uint16_t op, uint16_t ext)
{
    int is_signed = (ext >> 11) & 1, is64 = (ext >> 10) & 1;
    int nq = (ext >> 12) & 7, nr = ext & 7;
    uint8_t dq = rv_dreg(nq), dr = rv_dreg(nr);
    struct rv_ea ea;
    int owned;

    if (!rv_ea_decode(t, (op >> 3) & 7, op & 7, 4, &ea))
        return 0;
    uint8_t s = rv_ea_read(t, &ea, 4, &owned);
    uint8_t b = rv_tmp(t);
    rv_emit(t, is_signed ? rv_mv(b, s) : rv_zext_w(b, s));
    if (owned)
        rv_free(t, s);
    rv_ea_done(t, &ea);
    zero_divide_check(t, b);

    uint8_t a = rv_tmp(t), q = rv_tmp(t), r = rv_tmp(t), ov = rv_tmp(t);
    if (is64) {
        /* dividend Dr:Dq */
        rv_emit(t, rv_slli(a, dr, 32));
        rv_emit(t, rv_zext_w(q, dq));
        rv_emit(t, rv_or(a, a, q));
    } else {
        rv_emit(t, is_signed ? rv_mv(a, dq) : rv_zext_w(a, dq));
    }
    rv_emit(t, is_signed ? rv_div(q, a, b) : rv_divu(q, a, b));
    rv_emit(t, is_signed ? rv_rem(r, a, b) : rv_remu(r, a, b));

    /* overflow: quotient does not fit in 32 bits (signed or unsigned) */
    if (is_signed) {
        rv_emit(t, rv_sext_w(ov, q));
        rv_emit(t, rv_xor(ov, ov, q));
    } else {
        rv_emit(t, rv_srli(ov, q, 32));
    }
    rv_emit(t, rv_snez(ov, ov));

    uint32_t *skip = t->ptr;
    rv_emit(t, rv_bnez(ov, 0));
    if (is64 || nr != nq)
        rv_emit(t, rv_sext_w(dr, r));
    rv_emit(t, rv_sext_w(dq, q));
    if (!t->overflow)
        *skip = rv_patch_b(*skip, (int32_t)((t->ptr - skip) * 4));

    /* N Z from the quotient; on overflow N = 1, Z = 0 */
    rv_emit(t, rv_sltz(r, dq));
    rv_emit(t, rv_seqz(a, dq));
    rv_emit(t, rv_or(r, r, ov));
    rv_emit(t, rv_andn(a, a, ov));
    set_nzv(t, r, a, ov);
    rv_free(t, a);
    rv_free(t, q);
    rv_free(t, r);
    rv_free(t, ov);
    rv_free(t, b);
    return 1;
}

/* 0100 1100 0s ea + extension word */
int rv_emit_muldiv_l(struct rv_tc *t, uint16_t op)
{
    int mode = (op >> 3) & 7;
    if (mode == 1)
        return 0;
    uint16_t ext = rv_fetch16(t);
    if (ext & 0x83f8)
        return 0;                           /* reserved bits */
    return (op & 0x40) ? div_l(t, op, ext) : mul_l(t, op, ext);
}
