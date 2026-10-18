/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Shift and rotate instructions (line E) for the RV64 translator:
    ASL ASR LSL LSR ROL ROR ROXL ROXR, register forms with an immediate count
    (1-8) or a count in Dn (taken modulo 64), and the memory forms (word,
    shift by one).

    The operand is held zero-extended to 64 bits (n = 8, 16 or 32 bits), so
    the counts above the operand size that the register forms allow, up to
    63, need no special cases:

      LSL ASL   result = v << c,  C = bit n of (v << c)
      LSR       result = v >> c,  C = bit 0 of ((v << 1) >> c)
      ASR       as LSR on v sign-extended to 64 bits
      ROL ROR   rotation by c mod n, C = bit 0 / bit n-1 of the result
      ROXL ROXR rotation of the (n+1)-bit value X:v by c mod (n+1),
                C = X = bit n of the rotated value

    which also gives C = 0 for a zero count, as the 68k requires (C = X for
    ROXL/ROXR). X follows C when the count is not zero, except for ROL/ROR,
    which leave it alone. V is set by ASL when the most significant bit
    changes during the shift: the top c+1 bits of the operand are not all
    equal, or, for c >= n, the operand is not zero.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

enum shift_kind { SH_AS, SH_LS, SH_ROX, SH_RO };

/* rd = rs & ((1 << bits) - 1) for 1 <= bits <= 63 */
static void mask_low(struct rv_tc *t, uint8_t rd, uint8_t rs, int bits)
{
    if (bits == 8) {
        rv_emit(t, rv_andi(rd, rs, 0xff));
    } else if (bits == 16) {
        rv_emit(t, rv_zext_h(rd, rs));
    } else if (bits == 32) {
        rv_emit(t, rv_zext_w(rd, rs));
    } else {
        rv_emit(t, rv_slli(rd, rs, (uint8_t)(64 - bits)));
        rv_emit(t, rv_srli(rd, rd, (uint8_t)(64 - bits)));
    }
}

/*
    Emit the shift of the zero-extended n-bit value v by the count in c
    (0..63). Returns a new temporary with the zero-extended result and
    updates the condition codes. v and c are left unchanged.
*/
static uint8_t emit_shift(struct rv_tc *t, enum shift_kind kind, int left, int n, uint8_t v, uint8_t c)
{
    uint8_t res = rv_tmp(t), cf = rv_tmp(t), vf = rv_tmp(t), a = rv_tmp(t);
    /* no live flag among those written: result only */
    int fl = (t->live & (kind == SH_RO ? 0x0f : 0x1f)) != 0;

    if (fl)
        rv_emit(t, rv_addi(vf, 0, 0));              /* V = 0 unless ASL */

    switch (kind) {
    case SH_AS:
    case SH_LS:
        if (left) {
            rv_emit(t, rv_sll(a, v, c));
            if (fl)
                rv_emit(t, rv_bexti(cf, a, (uint8_t)n));
            mask_low(t, res, a, n);
            if (kind == SH_AS && fl) {
                /* V: top c+1 bits of v (aligned at bit 63) not all equal */
                uint8_t k = rv_tmp(t);
                rv_emit(t, rv_slli(a, v, (uint8_t)(64 - n)));
                rv_emit(t, rv_addi(k, 0, 63));
                rv_emit(t, rv_sub(k, k, c));
                rv_emit(t, rv_sra(a, a, k));
                rv_emit(t, rv_addi(a, a, 1));
                rv_emit(t, rv_sltiu(vf, a, 2));     /* 1 if they were all equal */
                rv_emit(t, rv_xori(vf, vf, 1));
                rv_free(t, k);
            }
        } else {
            uint8_t s = v;
            if (kind == SH_AS) {
                s = rv_tmp(t);
                rv_emit(t, n == 32 ? rv_sext_w(s, v) : n == 16 ? rv_sext_h(s, v) : rv_sext_b(s, v));
            }
            if (fl) {
                rv_emit(t, rv_slli(a, s, 1));
                rv_emit(t, kind == SH_AS ? rv_sra(a, a, c) : rv_srl(a, a, c));
                rv_emit(t, rv_andi(cf, a, 1));
            }
            rv_emit(t, kind == SH_AS ? rv_sra(a, s, c) : rv_srl(a, s, c));
            mask_low(t, res, a, n);
            if (s != v)
                rv_free(t, s);
        }
        break;

    case SH_RO: {
        uint8_t r = rv_tmp(t), k = rv_tmp(t);
        rv_emit(t, rv_andi(r, c, n - 1));
        rv_emit(t, rv_addi(k, 0, n));
        rv_emit(t, rv_sub(k, k, r));
        rv_emit(t, left ? rv_sll(a, v, r) : rv_srl(a, v, r));
        rv_emit(t, left ? rv_srl(k, v, k) : rv_sll(k, v, k));
        rv_emit(t, rv_or(a, a, k));
        mask_low(t, res, a, n);
        if (fl) {
            rv_emit(t, rv_bexti(cf, res, (uint8_t)(left ? 0 : n - 1)));
            rv_emit(t, rv_snez(k, c));              /* C = 0 for a zero count */
            rv_emit(t, rv_and(cf, cf, k));
        }
        rv_free(t, r);
        rv_free(t, k);
        break;
    }

    case SH_ROX: {
        uint8_t w = rv_tmp(t), r = rv_tmp(t), k = rv_tmp(t);
        rv_emit(t, rv_bexti(w, RVR_SR, 4));         /* w = X:v, n+1 bits */
        rv_emit(t, rv_slli(w, w, (uint8_t)n));
        rv_emit(t, rv_or(w, w, v));
        rv_emit(t, rv_addi(k, 0, n + 1));
        rv_emit(t, rv_remu(r, c, k));               /* rotation by c mod (n+1) */
        rv_emit(t, rv_sub(k, k, r));
        rv_emit(t, left ? rv_sll(a, w, r) : rv_srl(a, w, r));
        rv_emit(t, left ? rv_srl(k, w, k) : rv_sll(k, w, k));
        rv_emit(t, rv_or(a, a, k));
        mask_low(t, a, a, n + 1);
        if (fl)
            rv_emit(t, rv_bexti(cf, a, (uint8_t)n));    /* also X */
        mask_low(t, res, a, n);
        rv_free(t, w);
        rv_free(t, r);
        rv_free(t, k);
        break;
    }
    }

    uint8_t z = rv_tmp(t);
    if (!fl)
        goto done;

    /* SR: N Z V C, and X */
    uint8_t nz = a;
    rv_emit(t, rv_bexti(nz, res, (uint8_t)(n - 1)));       /* N */
    rv_emit(t, rv_seqz(z, res));
    rv_emit(t, rv_sh1add(vf, vf, cf));                      /* V<<1 | C */
    rv_emit(t, rv_sh2add(vf, z, vf));
    rv_emit(t, rv_sh3add(vf, nz, vf));

    if (kind == SH_RO) {
        rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));           /* X unchanged */
    } else {
        uint8_t x = z;
        if (kind == SH_ROX) {
            rv_emit(t, rv_mv(x, cf));
        } else {
            /* X = C if the count is not zero, else unchanged */
            uint8_t m = nz;
            rv_emit(t, rv_snez(m, c));
            rv_emit(t, rv_bexti(x, RVR_SR, 4));
            rv_emit(t, rv_xor(cf, cf, x));
            rv_emit(t, rv_and(cf, cf, m));
            rv_emit(t, rv_xor(x, x, cf));
        }
        rv_emit(t, rv_slli(x, x, 4));
        rv_emit(t, rv_or(vf, vf, x));
        rv_emit(t, rv_andi(RVR_SR, RVR_SR, -32));
    }
    rv_emit(t, rv_or(RVR_SR, RVR_SR, vf));

done:
    rv_free(t, cf);
    rv_free(t, vf);
    rv_free(t, a);
    rv_free(t, z);
    return res;
}

/* Register form: 1110 ccc d ss i tt rrr */
static int shift_reg(struct rv_tc *t, uint16_t op)
{
    static const int sizes[3] = { 1, 2, 4 };
    int size = sizes[(op >> 6) & 3];
    int n = 8 * size;
    uint8_t dn = rv_dreg(op & 7);
    uint8_t v = rv_tmp(t), c = rv_tmp(t);

    mask_low(t, v, dn, n);
    if (op & 0x20) {
        rv_emit(t, rv_andi(c, rv_dreg((op >> 9) & 7), 63));
    } else {
        int cnt = (op >> 9) & 7;
        rv_emit(t, rv_addi(c, 0, cnt ? cnt : 8));
    }
    uint8_t res = emit_shift(t, (enum shift_kind)((op >> 3) & 3), (op >> 8) & 1, n, v, c);
    if (size == 4)
        rv_emit(t, rv_sext_w(res, res));
    struct rv_ea d = { .kind = EA_DREG, .reg = dn };
    rv_ea_write(t, &d, size, res);
    rv_free(t, res);
    rv_free(t, v);
    rv_free(t, c);
    return 1;
}

/* Memory form: 1110 0tt d 11 ea, word shifted by one */
static int shift_mem(struct rv_tc *t, uint16_t op)
{
    struct rv_ea ea;
    int owned;
    int mode = (op >> 3) & 7;

    if (mode < 2 || (mode == 7 && (op & 7) > 1))
        return 0;
    if (!rv_ea_decode(t, mode, op & 7, 2, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, 2, &owned);
    uint8_t c = rv_tmp(t);
    rv_emit(t, rv_addi(c, 0, 1));
    uint8_t res = emit_shift(t, (enum shift_kind)((op >> 9) & 3), (op >> 8) & 1, 16, v, c);
    rv_ea_write(t, &ea, 2, res);
    rv_ea_done(t, &ea);
    rv_free(t, res);
    rv_free(t, c);
    if (owned)
        rv_free(t, v);
    return 1;
}

int rv_emit_lineE(struct rv_tc *t, uint16_t op)
{
    if ((op & 0xc0) != 0xc0)
        return shift_reg(t, op);
    if (op & 0x0800)
        return rv_emit_bitfield(t, op); /* 68020 bit fields (bitfield.c) */
    return shift_mem(t, op);
}
