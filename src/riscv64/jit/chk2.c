/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    CHK2 and CMP2 (68020) for the RV64 translator.

      0000 0ss0 11 <ea>, extension D/A rrr C000 0000 0000 (C = 1: CHK2)

    <ea> (control addressing) holds the lower bound followed by the upper
    bound. The result follows TG68K.C, whose microcode makes three
    subtractions (TG68KdotC_Kernel.vhd chk20 to chk24, TG68K_ALU.vhd opcCHK2):

      cU = borrow of UB - LB at the operation size, on the bounds as stored
      F  = R - LB, S = R - UB (unsigned borrow C and zero Z of each)

      cU = 0 (LB <= UB unsigned): C = F.C | (!S.C & !S.Z), R outside [LB, UB]
      cU = 1 (LB > UB unsigned):  C = (F.C ^ S.C) & !F.Z & !S.Z, which is
                                  the signed test for such a pair
      Z = F.Z | S.Z, V = 0, N = !cU (N and V are undefined in the PRM), X kept

    For a data register R and the bounds are taken at the operation size.
    For an address register the bounds are sign-extended and compared with
    all 32 bits of An; cU still comes from the bounds as stored. CHK2 takes
    exception 6 (format $2) when C is set.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

int rv_emit_chk2(struct rv_tc *t, uint16_t op)
{
    static const int sizes[4] = { 1, 2, 4, 0 };
    int size = sizes[(op >> 9) & 3];
    int mode = (op >> 3) & 7, reg = op & 7;
    struct rv_ea ea;

    if (!size || mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3))
        return 0;
    uint16_t ext = rv_fetch16(t);
    int is_a = ext & 0x8000, is_chk = ext & 0x0800;
    uint8_t rn = is_a ? rv_areg((ext >> 12) & 7) : rv_dreg((ext >> 12) & 7);
    if (!rv_ea_decode(t, mode, reg, 0, &ea))
        return 0;

    uint8_t a = rv_ea_address(t, &ea);
    rv_ea_done(t, &ea);
    uint8_t lb = rv_tmp(t), ub = rv_tmp(t);
    rv_load68(t, size, lb, a, 0);               /* b/w zero-extended, l canonical */
    rv_load68(t, size, ub, a, size);
    rv_free(t, a);

    uint8_t cu = rv_tmp(t), r = rv_tmp(t), fz = rv_tmp(t), sz = rv_tmp(t);
    /* zero-extended (b/w) and canonical (l) values keep the unsigned order */
    rv_emit(t, rv_sltu(cu, ub, lb));

    if (is_a) {
        rv_emit(t, rv_mv(r, rn));
        if (size == 1) {
            rv_emit(t, rv_sext_b(lb, lb));
            rv_emit(t, rv_sext_b(ub, ub));
        } else if (size == 2) {
            rv_emit(t, rv_sext_h(lb, lb));
            rv_emit(t, rv_sext_h(ub, ub));
        }
    } else if (size == 1) {
        rv_emit(t, rv_andi(r, rn, 0xff));
    } else if (size == 2) {
        rv_emit(t, rv_zext_h(r, rn));
    } else {
        rv_emit(t, rv_mv(r, rn));
    }

    rv_emit(t, rv_xor(fz, r, lb));
    rv_emit(t, rv_seqz(fz, fz));                /* F.Z */
    rv_emit(t, rv_xor(sz, r, ub));
    rv_emit(t, rv_seqz(sz, sz));                /* S.Z */
    rv_emit(t, rv_sltu(lb, r, lb));             /* F.C */
    rv_emit(t, rv_sltu(ub, r, ub));             /* S.C */
    rv_free(t, r);

    uint8_t cs = rv_tmp(t);
    /* unsigned: r = F.C | !(S.C | S.Z) */
    rv_emit(t, rv_or(r = rv_tmp(t), ub, sz));
    rv_emit(t, rv_xori(r, r, 1));
    rv_emit(t, rv_or(r, r, lb));
    /* signed: cs = (F.C ^ S.C) & !(F.Z | S.Z) */
    rv_emit(t, rv_xor(cs, lb, ub));
    rv_emit(t, rv_or(fz, fz, sz));              /* Z */
    rv_emit(t, rv_andn(cs, cs, fz));
    /* C = cU ? cs : r */
    rv_emit(t, rv_xor(cs, cs, r));
    rv_emit(t, rv_and(cs, cs, cu));
    rv_emit(t, rv_xor(cs, cs, r));
    rv_free(t, lb);
    rv_free(t, ub);
    rv_free(t, sz);
    rv_free(t, r);

    /* SR: N = !cU, Z, V = 0, C */
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));
    rv_emit(t, rv_xori(cu, cu, 1));
    rv_emit(t, rv_slli(cu, cu, 3));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, cu));
    rv_emit(t, rv_slli(fz, fz, 2));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, fz));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, cs));
    rv_free(t, cu);
    rv_free(t, fz);

    if (is_chk) {
        uint32_t *br = t->ptr;
        rv_emit(t, rv_beqz(cs, 0));
        rv_free(t, cs);
        rv_emit_exception(t, 6, 2, t->cursor, t->pc);
        rv_emit(t, rv_ret());
        if (!t->overflow)
            *br = rv_patch_b(*br, (int32_t)((t->ptr - br) * 4));
    } else {
        rv_free(t, cs);
    }
    return 1;
}
