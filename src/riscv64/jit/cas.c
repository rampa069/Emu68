/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    CAS and CAS2 (68020) for the RV64 translator.

      CAS   Dc,Du,<ea>       0000 1ss0 11 <ea>, ext 0000 000u uu00 0ccc
      CAS2  Dc1:Dc2,Du1:Du2,(Rn1):(Rn2)
                             0000 1ss0 1111 1100, ext Rn1 Du1 Dc1, ext Rn2 Du2 Dc2

    The flags are those of CMP Dc,<ea>. If the operands are equal Du is
    written to <ea>, otherwise <ea> is loaded into Dc. CAS2 compares
    (Rn1) with Dc1 first and only compares (Rn2) with Dc2 when they are
    equal; both memory operands are written only when both compares are
    equal, and both are loaded into Dc1 and Dc2 otherwise.

    There is a single m68k CPU, so no locked bus cycle is needed.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

static const int sizes[4] = { 0, 1, 2, 4 };

/* d at size into Dn, keeping the rest of Dn */
static void to_dreg(struct rv_tc *t, int n, int size, uint8_t v)
{
    struct rv_ea d = { .kind = EA_DREG, .reg = rv_dreg(n) };
    rv_ea_write(t, &d, size, v);
}

static int cas(struct rv_tc *t, uint16_t op, int size)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    struct rv_ea ea;
    int owned;

    if (mode < 2 || (mode == 7 && reg > 1))
        return 0;                                   /* memory alterable only */
    uint16_t ext = rv_fetch16(t);
    int dc = ext & 7, du = (ext >> 6) & 7;
    if (!rv_ea_decode(t, mode, reg, size, &ea))
        return 0;
    uint8_t v = rv_ea_read(t, &ea, size, &owned);
    uint8_t r = rv_tmp(t), k = rv_tmp(t);
    uint8_t live = t->live;

    t->live = CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C;   /* Z is read below */
    rv_flags_sub(t, size, v, rv_dreg(dc), r, 0);
    t->live = live;
    rv_free(t, r);
    rv_emit(t, rv_bexti(k, RVR_SR, 2));
    uint32_t *beq = t->ptr;
    rv_emit(t, rv_bnez(k, 0));
    rv_free(t, k);
    to_dreg(t, dc, size, v);                        /* not equal: Dc = <ea> */
    uint32_t *jend = t->ptr;
    rv_emit(t, rv_j_(0));
    uint32_t *eq = t->ptr;
    rv_ea_write(t, &ea, size, rv_dreg(du));         /* equal: <ea> = Du */
    if (!t->overflow) {
        *beq = rv_patch_b(*beq, (int32_t)((eq - beq) * 4));
        *jend = rv_patch_j(*jend, (int32_t)((t->ptr - jend) * 4));
    }
    if (owned)
        rv_free(t, v);
    rv_ea_done(t, &ea);
    return 1;
}

static int cas2(struct rv_tc *t, int size)
{
    uint16_t e1 = rv_fetch16(t), e2 = rv_fetch16(t);
    uint8_t rn1 = (e1 & 0x8000) ? rv_areg((e1 >> 12) & 7) : rv_dreg((e1 >> 12) & 7);
    uint8_t rn2 = (e2 & 0x8000) ? rv_areg((e2 >> 12) & 7) : rv_dreg((e2 >> 12) & 7);
    int dc1 = e1 & 7, du1 = (e1 >> 6) & 7, dc2 = e2 & 7, du2 = (e2 >> 6) & 7;
    uint8_t v1 = rv_tmp(t), v2 = rv_tmp(t);
    uint8_t r = rv_tmp(t), k = rv_tmp(t);
    uint8_t live = t->live;

    t->live = CCR_X | CCR_N | CCR_Z | CCR_V | CCR_C;   /* Z is read below */

    /*
        Both operands are read before anything is written. Rn1 and Rn2 are
        only changed after the last memory access (when one of them is also
        Dc1 or Dc2), so they serve as the addresses directly; copying them
        would need two more temporaries than flags_sub leaves.
    */
    rv_load68(t, size, v1, rn1, 0);
    rv_load68(t, size, v2, rn2, 0);

    rv_flags_sub(t, size, v1, rv_dreg(dc1), r, 0);
    rv_emit(t, rv_bexti(k, RVR_SR, 2));
    uint32_t *ne1 = t->ptr;
    rv_emit(t, rv_beqz(k, 0));
    rv_flags_sub(t, size, v2, rv_dreg(dc2), r, 0);
    rv_emit(t, rv_bexti(k, RVR_SR, 2));
    uint32_t *ne2 = t->ptr;
    rv_emit(t, rv_beqz(k, 0));
    rv_store68(t, size, rv_dreg(du1), rn1, 0);      /* both equal */
    rv_store68(t, size, rv_dreg(du2), rn2, 0);
    uint32_t *jend = t->ptr;
    rv_emit(t, rv_j_(0));
    uint32_t *ne = t->ptr;
    to_dreg(t, dc2, size, v2);                      /* Dc1 last: it wins if they are */
    to_dreg(t, dc1, size, v1);                      /* the same register (PRM) */
    if (!t->overflow) {
        *ne1 = rv_patch_b(*ne1, (int32_t)((ne - ne1) * 4));
        *ne2 = rv_patch_b(*ne2, (int32_t)((ne - ne2) * 4));
        *jend = rv_patch_j(*jend, (int32_t)((t->ptr - jend) * 4));
    }
    t->live = live;
    rv_free(t, v1);
    rv_free(t, v2);
    rv_free(t, r);
    rv_free(t, k);
    return 1;
}

/* 0000 1ss0 11xx xxxx with ss != 00 */
int rv_emit_cas(struct rv_tc *t, uint16_t op)
{
    int size = sizes[(op >> 9) & 3];

    if (!size)
        return 0;
    if ((op & 0x3f) == 0x3c)
        return size == 1 ? 0 : cas2(t, size);
    return cas(t, op, size);
}
