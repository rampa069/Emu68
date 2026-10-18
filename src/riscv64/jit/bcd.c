/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    ABCD, SBCD and NBCD for the RV64 translator.

    The result follows TG68K.C (TG68K_ALU.vhd, "BCD handling with all
    undefined behavior"), which also defines it for operands that are not
    valid BCD. With A the destination byte, B the source byte and X:

      ABCD  S = A + B + X (9 bits). Add 6 when the low nibble carried
            (half carry) or S[3:0] >= 0xA; add 0x60 when S >= 0x100 or
            S[7:0] >= 0x9A. R = S + correction, C = S[8] | R[8].
      SBCD  D = A - B - X (9 bits, D[8] = borrow). Subtract 6 when the low
            nibble borrowed, 0x60 when D[8]. R = D - correction,
            C = D[8] | R[8].
      NBCD  SBCD with A = 0.

    X = C. N is bit 7 of the result and V is 0 (the PRM leaves both
    undefined; this is TG68K.C in 68020 mode). Z is only cleared, for
    multi-precision arithmetic.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

/* r_out = BCD a + b + X or a - b - X (bytes, zero-extended), flags set */
static void bcd(struct rv_tc *t, uint8_t a, uint8_t b, int is_sub, uint8_t r_out)
{
    uint8_t s = rv_tmp(t), h = rv_tmp(t), k = rv_tmp(t), c = rv_tmp(t);

    rv_emit(t, rv_bexti(k, RVR_SR, 4));
    if (!is_sub) {
        rv_emit(t, rv_add(s, a, b));
        rv_emit(t, rv_add(s, s, k));                /* S, at most 0x1ff */
        rv_emit(t, rv_xor(h, a, b));
        rv_emit(t, rv_xor(h, h, s));
        rv_emit(t, rv_bexti(h, h, 4));              /* half carry */
        rv_emit(t, rv_andi(k, s, 15));
        rv_emit(t, rv_sltiu(k, k, 10));
        rv_emit(t, rv_xori(k, k, 1));
        rv_emit(t, rv_or(h, h, k));                 /* low correction */
        rv_emit(t, rv_andi(k, s, 0xff));
        rv_emit(t, rv_sltiu(k, k, 0x9a));
        rv_emit(t, rv_xori(k, k, 1));
        rv_emit(t, rv_srli(c, s, 8));               /* S[8] */
        rv_emit(t, rv_or(k, k, c));                 /* high correction */
    } else {
        rv_emit(t, rv_sub(s, a, b));
        rv_emit(t, rv_sub(s, s, k));
        rv_emit(t, rv_andi(s, s, 0x1ff));           /* D, D[8] = borrow */
        rv_emit(t, rv_xor(h, a, b));
        rv_emit(t, rv_xor(h, h, s));
        rv_emit(t, rv_bexti(h, h, 4));              /* half borrow: low correction */
        rv_emit(t, rv_bexti(c, s, 8));
        rv_emit(t, rv_mv(k, c));                    /* high correction */
    }
    rv_emit(t, rv_sh1add(h, h, h));
    rv_emit(t, rv_slli(h, h, 1));                   /* 6 or 0 */
    rv_emit(t, rv_sh1add(k, k, k));
    rv_emit(t, rv_slli(k, k, 5));                   /* 0x60 or 0 */
    if (!is_sub) {
        rv_emit(t, rv_add(s, s, h));
        rv_emit(t, rv_add(s, s, k));
    } else {
        rv_emit(t, rv_sub(s, s, h));
        rv_emit(t, rv_sub(s, s, k));
    }
    rv_emit(t, rv_bexti(k, s, 8));
    rv_emit(t, rv_or(c, c, k));                     /* C */
    rv_emit(t, rv_andi(r_out, s, 0xff));

    /* SR: X N V C replaced, Z cleared if the result is not zero */
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, ~(CCR_X | CCR_N | CCR_V | CCR_C)));
    rv_emit(t, rv_snez(k, r_out));
    rv_emit(t, rv_slli(k, k, 2));
    rv_emit(t, rv_andn(RVR_SR, RVR_SR, k));
    rv_emit(t, rv_srli(k, r_out, 7));
    rv_emit(t, rv_slli(k, k, 3));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));           /* N */
    rv_emit(t, rv_slli(k, c, 4));
    rv_emit(t, rv_or(k, k, c));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));           /* X = C */

    rv_free(t, s);
    rv_free(t, h);
    rv_free(t, k);
    rv_free(t, c);
}

/* ABCD 1100 xxx1 0000 mYYY, SBCD 1000 xxx1 0000 mYYY */
int rv_emit_abcd_sbcd(struct rv_tc *t, uint16_t op)
{
    int is_sub = (op >> 12) == 0x8;
    int rx = (op >> 9) & 7, ry = op & 7;
    uint8_t a = rv_tmp(t), b = rv_tmp(t), r = rv_tmp(t);

    if (!(op & 0x08)) {
        /* Dy,Dx */
        uint8_t dx = rv_dreg(rx);
        rv_emit(t, rv_andi(a, dx, 0xff));
        rv_emit(t, rv_andi(b, rv_dreg(ry), 0xff));
        bcd(t, a, b, is_sub, r);
        struct rv_ea d = { .kind = EA_DREG, .reg = dx };
        rv_ea_write(t, &d, 1, r);
    } else {
        /* -(Ay),-(Ax): source first */
        struct rv_ea src, dst;
        int so, d_owned;
        rv_ea_decode(t, 4, ry, 1, &src);
        uint8_t s = rv_ea_read(t, &src, 1, &so);
        rv_emit(t, rv_mv(b, s));
        if (so)
            rv_free(t, s);
        rv_ea_done(t, &src);
        rv_ea_decode(t, 4, rx, 1, &dst);
        uint8_t d = rv_ea_read(t, &dst, 1, &d_owned);
        rv_emit(t, rv_mv(a, d));
        if (d_owned)
            rv_free(t, d);
        bcd(t, a, b, is_sub, r);
        rv_ea_write(t, &dst, 1, r);
        rv_ea_done(t, &dst);
    }
    rv_free(t, a);
    rv_free(t, b);
    rv_free(t, r);
    return 1;
}

/* NBCD 0100 1000 00 <ea>, data alterable */
int rv_emit_nbcd(struct rv_tc *t, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    struct rv_ea ea;
    int owned;

    if (mode == 1 || (mode == 7 && reg > 1))
        return 0;
    if (!rv_ea_decode(t, mode, reg, 1, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, 1, &owned);
    uint8_t b = rv_tmp(t), r = rv_tmp(t);
    rv_emit(t, rv_andi(b, v, 0xff));
    if (owned)
        rv_free(t, v);
    bcd(t, RV_ZERO, b, 1, r);
    rv_ea_write(t, &ea, 1, r);
    rv_ea_done(t, &ea);
    rv_free(t, b);
    rv_free(t, r);
    return 1;
}
