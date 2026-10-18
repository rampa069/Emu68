/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    68020 bit field instructions for the RV64 translator.

      1110 1ttt 11 <ea>, extension 0 rrr Do ooooo Dw wwwww
      ttt: 000 BFTST 001 BFEXTU 010 BFCHG 011 BFEXTS
           100 BFCLR 101 BFFFO  110 BFSET 111 BFINS

    Offset and width come from the extension word or from Do/Dw. The width
    is taken modulo 32 with 0 meaning 32. Bit 0 of a field is its most
    significant bit, counted from bit 31 of a register or from bit 7 of the
    byte at the effective address.

    Everything works on the field moved to the top of a 64-bit register, x:
    N is bit 63 of x and the field is x >> (64 - w).

      Dn      the register rotated left by offset mod 32 (the field may wrap
              around), shifted into the upper half
      memory  the byte address moves by offset >> 3 (signed for Do), and
              the 40 bits starting there (a field spans at most five bytes)
              are shifted left by offset & 7

    The field is read as five bytes even when it fits in fewer; writes store
    all five back, unchanged outside the field.

    N and Z come from the field (from the inserted value for BFINS), V and C
    are cleared, X is kept. BFFFO returns the full offset (Do as is, not
    modulo 32) plus the number of leading zeros in the field, or plus the
    width when it is zero, as TG68K.C does (bf_full_offset).
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

enum { BFTST, BFEXTU, BFCHG, BFEXTS, BFCLR, BFFFO, BFSET, BFINS };

static void set_nz(struct rv_tc *t, uint8_t x, uint8_t field)
{
    uint8_t k = rv_tmp(t);
    rv_emit(t, rv_andi(RVR_SR, RVR_SR, -16));
    rv_emit(t, rv_sltz(k, x));
    rv_emit(t, rv_slli(k, k, 3));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));
    rv_emit(t, rv_seqz(k, field));
    rv_emit(t, rv_slli(k, k, 2));
    rv_emit(t, rv_or(RVR_SR, RVR_SR, k));
    rv_free(t, k);
}

int rv_emit_bitfield(struct rv_tc *t, uint16_t op)
{
    int type = (op >> 8) & 7;
    int mode = (op >> 3) & 7, reg = op & 7;
    int writes = type == BFCHG || type == BFCLR || type == BFSET || type == BFINS;

    /* Dn or control addressing; PC-relative only for the read-only forms */
    if (mode == 1 || mode == 3 || mode == 4 || (mode == 7 && reg > (writes ? 1 : 3)))
        return 0;

    uint16_t ext = rv_fetch16(t);
    uint8_t dr = rv_dreg((ext >> 12) & 7);
    int o_reg = ext & 0x0800, w_reg = ext & 0x0020;
    int o_imm = (ext >> 6) & 31, w_imm = ext & 31;
    uint8_t d_o = rv_dreg((ext >> 6) & 7), d_w = rv_dreg(ext & 7);

    struct rv_ea ea;
    if (!rv_ea_decode(t, mode, reg, 0, &ea))
        return 0;

    /* sh = 64 - width */
    uint8_t sh = rv_tmp(t);
    if (w_reg) {
        rv_emit(t, rv_addi(sh, d_w, -1));
        rv_emit(t, rv_andi(sh, sh, 31));
        rv_emit(t, rv_sub(sh, RV_ZERO, sh));
        rv_emit(t, rv_addi(sh, sh, 63));
    } else {
        rv_emit(t, rv_addi(sh, RV_ZERO, 64 - (w_imm ? w_imm : 32)));
    }

    uint8_t x = rv_tmp(t), f = rv_tmp(t);
    uint8_t a = 0, bit = 0, x0 = 0;

    if (ea.kind == EA_DREG) {
        /* x = rol32(Dn, offset) << 32 */
        if (o_reg) {
            rv_emit(t, rv_rolw(x, ea.reg, d_o));    /* rolw uses offset mod 32 */
        } else if (o_imm) {
            rv_emit(t, rv_roriw(x, ea.reg, 32 - o_imm));
        } else {
            rv_emit(t, rv_mv(x, ea.reg));
        }
        rv_emit(t, rv_slli(x, x, 32));
    } else {
        a = rv_ea_address(t, &ea);
        rv_ea_done(t, &ea);
        bit = rv_tmp(t);
        x0 = rv_tmp(t);
        if (o_reg) {
            rv_emit(t, rv_srai(f, d_o, 3));
            rv_emit(t, rv_addw(a, a, f));
            rv_emit(t, rv_andi(bit, d_o, 7));
        } else {
            if (o_imm >> 3)
                rv_emit(t, rv_addiw(a, a, o_imm >> 3));
            rv_emit(t, rv_addi(bit, RV_ZERO, o_imm & 7));
        }
        /* x0 = the five bytes at a, in bits 63..24 */
        rv_load68(t, 4, x0, a, 0);
        rv_emit(t, rv_slli(x0, x0, 32));
        rv_load68(t, 1, f, a, 4);
        rv_emit(t, rv_slli(f, f, 24));
        rv_emit(t, rv_or(x0, x0, f));
        rv_emit(t, rv_sll(x, x0, bit));
    }

    /* the field, zero-extended, and the flags */
    if (type == BFINS) {
        rv_emit(t, rv_sll(f, dr, sh));              /* value to insert, top-aligned */
        rv_emit(t, rv_srl(x, f, sh));
        set_nz(t, f, x);
    } else {
        rv_emit(t, rv_srl(f, x, sh));
        set_nz(t, x, f);
    }

    switch (type) {
    case BFEXTU:
        rv_emit(t, rv_sext_w(dr, f));
        break;
    case BFEXTS:
        rv_emit(t, rv_sra(dr, x, sh));
        break;
    case BFFFO:
        /* stop bit just below the field: clz gives at most the width */
        rv_emit(t, rv_addi(f, RV_ZERO, 1));
        rv_emit(t, rv_addi(sh, sh, -1));
        rv_emit(t, rv_sll(f, f, sh));
        rv_emit(t, rv_or(x, x, f));
        rv_emit(t, rv_clz(x, x));
        if (o_reg)
            rv_emit(t, rv_addw(dr, d_o, x));
        else
            rv_emit(t, rv_addiw(dr, x, o_imm));
        break;
    default:
        break;
    }

    if (writes) {
        /* f: the field mask in the top bits; for BFINS x: the value at the field */
        if (type == BFINS)
            rv_emit(t, rv_sll(x, dr, sh));
        rv_emit(t, rv_addi(f, RV_ZERO, -1));
        rv_emit(t, rv_sll(f, f, sh));

        if (ea.kind == EA_DREG) {
            uint8_t dn = ea.reg;
            /* to bits 31..0, then rotate right by the offset */
            rv_emit(t, rv_srli(f, f, 32));
            if (type == BFINS)
                rv_emit(t, rv_srli(x, x, 32));
            if (o_reg) {
                rv_emit(t, rv_rorw(f, f, d_o));
                if (type == BFINS)
                    rv_emit(t, rv_rorw(x, x, d_o));
            } else {
                rv_emit(t, rv_roriw(f, f, o_imm));
                if (type == BFINS)
                    rv_emit(t, rv_roriw(x, x, o_imm));
            }
            switch (type) {
            case BFCHG: rv_emit(t, rv_xor(dn, dn, f)); break;
            case BFCLR: rv_emit(t, rv_andn(dn, dn, f)); break;
            case BFSET: rv_emit(t, rv_or(dn, dn, f)); break;
            default:
                rv_emit(t, rv_andn(dn, dn, f));
                rv_emit(t, rv_or(dn, dn, x));
                break;
            }
        } else {
            /* to the position of the field in x0 */
            rv_emit(t, rv_srl(f, f, bit));
            if (type == BFINS)
                rv_emit(t, rv_srl(x, x, bit));
            switch (type) {
            case BFCHG: rv_emit(t, rv_xor(x0, x0, f)); break;
            case BFCLR: rv_emit(t, rv_andn(x0, x0, f)); break;
            case BFSET: rv_emit(t, rv_or(x0, x0, f)); break;
            default:
                rv_emit(t, rv_andn(x0, x0, f));
                rv_emit(t, rv_or(x0, x0, x));
                break;
            }
            rv_emit(t, rv_srai(f, x0, 32));
            rv_store68(t, 4, f, a, 0);
            rv_emit(t, rv_srli(f, x0, 24));
            rv_store68(t, 1, f, a, 4);
        }
    }

    if (ea.kind == EA_DREG)
        rv_ea_done(t, &ea);
    else {
        rv_free(t, a);
        rv_free(t, bit);
        rv_free(t, x0);
    }
    rv_free(t, sh);
    rv_free(t, x);
    rv_free(t, f);
    return 1;
}
