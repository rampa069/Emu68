/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    NEG NEGX NOT and ADDX SUBX for the RV64 translator.

    All of them reduce to d + s + x or d - s - x with x = 0 or X, computed on
    operands shifted to the top of the register (as in sw/flag-recipes), so
    one routine covers bytes, words and longs:

      carry  = carry (borrow) out of either partial sum; at most one occurs
      V      = (d^r) & (s^r) < 0 for addition, (d^s) & (d^r) < 0 for subtraction
      X = C  = carry

    NEG is 0 - d, NEGX is 0 - d - X. For NEGX, ADDX and SUBX, Z is only
    cleared when the result is not zero (multi-precision arithmetic), for
    NEG it is set from the result. NOT sets N and Z, clears V and C and
    leaves X alone.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

static const int sizes[4] = { 1, 2, 4, 0 };

/*
    r_out = d + s + x or d - s - x at the given size (zero-extended), flags
    updated. use_x: include X; sticky_z: Z only cleared, never set.
*/
static void addsub_x(struct rv_tc *t, int size, uint8_t d, uint8_t s, int is_sub,
                     int use_x, int sticky_z, uint8_t r_out)
{
    uint8_t sh = (uint8_t)(64 - 8 * size);
    uint8_t ds = rv_tmp(t), ss = rv_tmp(t), r = rv_tmp(t), c = rv_tmp(t), k = rv_tmp(t);
    int fl = (t->live & 0x1f) != 0;                 /* no live flag: result only */

    rv_emit(t, rv_slli(ds, d, sh));
    rv_emit(t, rv_slli(ss, s, sh));
    if (is_sub) {
        rv_emit(t, rv_sub(r, ds, ss));
        if (fl)
            rv_emit(t, rv_sltu(c, ds, ss));         /* borrow of d - s */
    } else {
        rv_emit(t, rv_add(r, ds, ss));
        if (fl)
            rv_emit(t, rv_sltu(c, r, ds));          /* carry of d + s */
    }
    if (use_x) {
        rv_emit(t, rv_bexti(k, RVR_SR, 4));
        rv_emit(t, rv_slli(k, k, sh));              /* X at the operand's bit 0 */
        if (is_sub) {
            if (fl)
                rv_emit(t, rv_sltu(r_out, r, k));   /* borrow of (d - s) - x */
            rv_emit(t, rv_sub(r, r, k));
        } else {
            rv_emit(t, rv_add(r, r, k));
            if (fl)
                rv_emit(t, rv_sltu(r_out, r, k));   /* carry of (d + s) + x */
        }
        if (fl)
            rv_emit(t, rv_or(c, c, r_out));
    }
    if (!fl) {
        rv_emit(t, rv_srli(r_out, r, sh));          /* result, zero-extended */
        goto done;
    }

    /* V */
    if (is_sub) {
        rv_emit(t, rv_xor(k, ds, ss));
        rv_emit(t, rv_xor(ss, ds, r));
    } else {
        rv_emit(t, rv_xor(k, ds, r));
        rv_emit(t, rv_xor(ss, ss, r));
    }
    rv_emit(t, rv_and(k, k, ss));
    rv_emit(t, rv_sltz(k, k));                      /* k = V */

    rv_emit(t, rv_sltz(ds, r));                     /* ds = N */
    rv_emit(t, rv_srli(r_out, r, sh));              /* result, zero-extended */
    rv_emit(t, rv_seqz(ss, r));                     /* ss = Z of this result */
    if (sticky_z) {
        rv_emit(t, rv_bexti(r, RVR_SR, 2));
        rv_emit(t, rv_and(ss, ss, r));
    }

    /* SR = (SR & ~0x1f) | X N Z V C, X = C */
    rv_emit(t, rv_sh1add(k, k, c));
    rv_emit(t, rv_sh2add(k, ss, k));
    rv_emit(t, rv_sh3add(k, ds, k));
    rv_emit(t, rv_slli(c, c, 4));
    rv_emit(t, rv_or(k, k, c));
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -32));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));

done:
    rv_free(t, ds);
    rv_free(t, ss);
    rv_free(t, r);
    rv_free(t, c);
    rv_free(t, k);
}

/* NEGX 0x40, NEG 0x44, NOT 0x46 (size in bits 7-6, not 11) */
int rv_emit_neg_not(struct rv_tc *t, uint16_t op)
{
    int size = sizes[(op >> 6) & 3];
    int mode = (op >> 3) & 7;
    struct rv_ea ea;
    int owned;

    if (!size || mode == 1 || (mode == 7 && (op & 7) > 1))
        return 0;
    if (!rv_ea_decode(t, mode, op & 7, size, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, size, &owned);
    uint8_t r = rv_tmp(t);

    switch (op & 0xff00) {
    case 0x4600:                                    /* NOT */
        rv_emit(t, rv_not(r, v));
        rv_flags_nz(t, size, r);
        break;
    case 0x4400:                                    /* NEG */
        addsub_x(t, size, RV_ZERO, v, 1, 0, 0, r);
        break;
    default:                                        /* NEGX */
        addsub_x(t, size, RV_ZERO, v, 1, 1, 1, r);
        break;
    }
    if (size == 4)
        rv_emit(t, rv_sext_w(r, r));
    rv_ea_write(t, &ea, size, r);
    rv_free(t, r);
    if (owned)
        rv_free(t, v);
    rv_ea_done(t, &ea);
    return 1;
}

/* ADDX 1101 xxx1 ss00 mYYY, SUBX 1001 xxx1 ss00 mYYY */
int rv_emit_addx_subx(struct rv_tc *t, uint16_t op)
{
    int size = sizes[(op >> 6) & 3];
    int is_sub = (op >> 12) == 0x9;
    int rx = (op >> 9) & 7, ry = op & 7;

    if (!size)
        return 0;

    uint8_t r = rv_tmp(t);
    if (!(op & 0x08)) {
        /* Dy,Dx */
        uint8_t dx = rv_dreg(rx);
        addsub_x(t, size, dx, rv_dreg(ry), is_sub, 1, 1, r);
        if (size == 4)
            rv_emit(t, rv_sext_w(r, r));
        struct rv_ea d = { .kind = EA_DREG, .reg = dx };
        rv_ea_write(t, &d, size, r);
    } else {
        /* -(Ay),-(Ax): source first */
        struct rv_ea src, dst;
        int so, d_owned;
        rv_ea_decode(t, 4, ry, size, &src);
        uint8_t s = rv_ea_read(t, &src, size, &so);
        rv_ea_done(t, &src);
        rv_ea_decode(t, 4, rx, size, &dst);
        uint8_t d = rv_ea_read(t, &dst, size, &d_owned);
        addsub_x(t, size, d, s, is_sub, 1, 1, r);
        rv_ea_write(t, &dst, size, r);
        rv_ea_done(t, &dst);
        if (so)
            rv_free(t, s);
        if (d_owned)
            rv_free(t, d);
    }
    rv_free(t, r);
    return 1;
}
