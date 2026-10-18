/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    TAS, EXG, CMPM, MOVEP, PACK and UNPK for the RV64 translator.

      TAS <ea>         0100 1010 11 <ea>     N Z of the byte, V = C = 0,
                                             then bit 7 set (one RMW access)
      EXG              1100 xxx1 0100 0yyy   Dx,Dy
                       1100 xxx1 0100 1yyy   Ax,Ay
                       1100 xxx1 1000 1yyy   Dx,Ay
      CMPM (Ay)+,(Ax)+ 1011 xxx1 ss00 1yyy   flags of CMP, X kept
      MOVEP            0000 ddd1 1m00 1aaa   m: 0 memory to Dd, 1 Dd to memory;
                                             bit 6: long; bytes at d16(Aa),
                                             +2, +4, +6, high byte first
      PACK             1000 yyy1 0100 mxxx   adjustment word follows
      UNPK             1000 yyy1 1000 mxxx

    PACK adds the adjustment to the 16-bit source and keeps its bits 11-8 and
    3-0 as one byte; UNPK spreads the two digits of a byte to bits 11-8 and
    3-0 of a word and adds the adjustment. In the memory forms the word is
    at -(Ax) as a big-endian word (high byte at the lower address) and the
    byte at -(Ay), which for A7 moves by 2, as in Emu68 on AArch64. Neither
    changes the flags.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

int rv_emit_tas(struct rv_tc *t, uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    struct rv_ea ea;
    int owned;

    if (mode == 1 || (mode == 7 && reg > 1))
        return 0;
    if (!rv_ea_decode(t, mode, reg, 1, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, 1, &owned);
    uint8_t live = t->live;
    t->live = CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C;
    rv_flags_nz(t, 1, v);
    t->live = live;
    uint8_t r = rv_tmp(t);
    rv_emit(t, rv_ori(r, v, 0x80));
    rv_ea_write(t, &ea, 1, r);
    rv_free(t, r);
    if (owned)
        rv_free(t, v);
    rv_ea_done(t, &ea);
    return 1;
}

int rv_emit_exg(struct rv_tc *t, uint16_t op)
{
    uint8_t rx, ry;

    switch (op & 0x01f8) {
    case 0x0140: rx = rv_dreg((op >> 9) & 7); ry = rv_dreg(op & 7); break;
    case 0x0148: rx = rv_areg((op >> 9) & 7); ry = rv_areg(op & 7); break;
    case 0x0188: rx = rv_dreg((op >> 9) & 7); ry = rv_areg(op & 7); break;
    default: return 0;
    }
    uint8_t k = rv_tmp(t);
    rv_emit(t, rv_mv(k, rx));
    rv_emit(t, rv_mv(rx, ry));
    rv_emit(t, rv_mv(ry, k));
    rv_free(t, k);
    return 1;
}

int rv_emit_cmpm(struct rv_tc *t, uint16_t op)
{
    static const int sizes[4] = { 1, 2, 4, 0 };
    int size = sizes[(op >> 6) & 3];
    struct rv_ea src, dst;
    int so, d_owned;

    if (!size)
        return 0;
    /* source first: with Ax = Ay the two reads are consecutive */
    rv_ea_decode(t, 3, op & 7, size, &src);
    uint8_t s = rv_ea_read(t, &src, size, &so);
    rv_ea_done(t, &src);
    rv_ea_decode(t, 3, (op >> 9) & 7, size, &dst);
    uint8_t d = rv_ea_read(t, &dst, size, &d_owned);
    rv_ea_done(t, &dst);
    uint8_t r = rv_tmp(t);
    rv_flags_sub(t, size, d, s, r, 0);
    rv_free(t, r);
    if (so)
        rv_free(t, s);
    if (d_owned)
        rv_free(t, d);
    return 1;
}

int rv_emit_movep(struct rv_tc *t, uint16_t op)
{
    int to_mem = op & 0x0080, n = (op & 0x0040) ? 4 : 2;
    uint8_t dn = rv_dreg((op >> 9) & 7);
    int16_t d16 = (int16_t)rv_fetch16(t);
    uint8_t a = rv_tmp(t), b = rv_tmp(t);

    rv_emit_li(t, b, d16);
    rv_emit(t, rv_addw(a, rv_areg(op & 7), b));

    if (to_mem) {
        for (int i = 0; i < n; i++) {
            rv_emit(t, rv_srli(b, dn, 8 * (n - 1 - i)));
            rv_store68(t, 1, b, a, 2 * i);
        }
    } else {
        uint8_t v = rv_tmp(t);
        rv_emit(t, rv_addi(v, RV_ZERO, 0));
        for (int i = 0; i < n; i++) {
            rv_load68(t, 1, b, a, 2 * i);
            rv_emit(t, rv_slli(v, v, 8));
            rv_emit(t, rv_or(v, v, b));
        }
        if (n == 4) {
            rv_emit(t, rv_sext_w(dn, v));
        } else {
            struct rv_ea d = { .kind = EA_DREG, .reg = dn };
            rv_ea_write(t, &d, 2, v);
        }
        rv_free(t, v);
    }
    rv_free(t, a);
    rv_free(t, b);
    return 1;
}

/* PACK (0x8140) and UNPK (0x8180), register or -(An) forms */
int rv_emit_pack_unpk(struct rv_tc *t, uint16_t op)
{
    int unpk = (op & 0x01f0) == 0x0180, mem = op & 0x08;
    int rx = op & 7, ry = (op >> 9) & 7;
    uint16_t adj = rv_fetch16(t);
    uint8_t v = rv_tmp(t), k = rv_tmp(t);
    struct rv_ea src, dst;
    int owned;

    /* source: a word for PACK, a byte for UNPK */
    if (mem) {
        rv_ea_decode(t, 4, rx, unpk ? 1 : 2, &src);
    } else {
        src = (struct rv_ea){ .kind = EA_DREG, .reg = rv_dreg(rx) };
    }
    uint8_t s = rv_ea_read(t, &src, unpk ? 1 : 2, &owned);
    rv_emit(t, rv_mv(v, s));
    if (owned)
        rv_free(t, s);
    if (mem)
        rv_ea_done(t, &src);

    if (!unpk) {
        rv_emit_li(t, k, adj);
        rv_emit(t, rv_add(v, v, k));
        rv_emit(t, rv_andi(k, v, 0x0f));
        rv_emit(t, rv_srli(v, v, 4));
        rv_emit(t, rv_andi(v, v, 0xf0));
        rv_emit(t, rv_or(v, v, k));             /* bits 11-8 and 3-0 */
    } else {
        rv_emit(t, rv_andi(k, v, 0x0f));
        rv_emit(t, rv_andi(v, v, 0xf0));
        rv_emit(t, rv_slli(v, v, 4));
        rv_emit(t, rv_or(v, v, k));
        rv_emit_li(t, k, adj);
        rv_emit(t, rv_add(v, v, k));
    }
    rv_free(t, k);

    if (mem) {
        rv_ea_decode(t, 4, ry, unpk ? 2 : 1, &dst);
    } else {
        dst = (struct rv_ea){ .kind = EA_DREG, .reg = rv_dreg(ry) };
    }
    rv_ea_write(t, &dst, unpk ? 2 : 1, v);
    if (mem)
        rv_ea_done(t, &dst);
    rv_free(t, v);
    return 1;
}
