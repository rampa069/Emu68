/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    68040 instructions of Line-F outside the FPU, for the RV64 translator.

      CINV    1111 0100 cc0 ss rrr   invalidate caches (cc: 01 data,
                                     10 instruction, 11 both; ss: 01 line,
                                     10 page, 11 all at (Ar))
      CPUSH   1111 0100 cc1 ss rrr   push and invalidate, same fields
      PFLUSH  1111 0101 000 oo rrr   flush ATC entries
      PTEST   1111 0101 01r 01 rrr   test a logical address (r: 1 read)
      MOVE16  1111 0110 0010 0xxx    (Ax)+,(Ay)+, extension 1yyy 0000 0000 0000
              1111 0110 000o orrr    with an absolute long address:
                                     00 (Ay)+,(xxx).L  01 (xxx).L,(Ay)+
                                     10 (Ay),(xxx).L   11 (xxx).L,(Ay)

    The host keeps no 68k data cache and no ATC, so the data cache halves of
    CINV and CPUSH, PFLUSH and PTEST do nothing; PTEST leaves MMUSR alone, as
    in Emu68 on AArch64. An instruction cache half drops every translated
    unit: chained units jump into each other directly, so dropping only the
    units of one line or page would leave stale jumps behind. The unit ends
    at the next instruction and rv_jit_stopped = RV_JIT_SERVICE_FLUSH asks
    the dispatcher, which runs outside any unit, to call rv_jit_flush.

    CINV, CPUSH, PFLUSH and PTEST are privileged on the 68040 (M68000PRM,
    section 6); a user-mode one takes the privilege violation. Emu68 on
    AArch64 runs them in user mode too. A cache field of 00 is a no-operation
    form; scope 00 of CINV and CPUSH is not an instruction, and like every
    other Line-F opcode left over it takes the Line-F exception (ops.c).

    MOVE16 copies the 16-byte line holding each address, the low four bits
    being ignored, and adds 16 to the address registers it post-increments;
    with Ax = Ay in (Ax)+,(Ay)+ the register goes up by 16 once, as in Emu68
    on AArch64. The copy is two 64-bit loads and stores, the same in either
    byte order. MOVE16 is not privileged.
*/

#include <stdint.h>
#include "support.h"
#include "jit_internal.h"

static void copy_line(struct rv_tc *t, uint8_t src, uint8_t dst)
{
    uint8_t s = rv_tmp(t), d = rv_tmp(t), w0 = rv_tmp(t), w1 = rv_tmp(t);

    /* canonical addresses stay canonical with the low bits cleared */
    rv_emit(t, rv_andi(s, src, -16));
    rv_emit(t, rv_andi(d, dst, -16));
    rv_emit(t, rv_ld(w0, s, 0));
    rv_emit(t, rv_ld(w1, s, 8));
    rv_emit(t, rv_sd(w0, d, 0));
    rv_emit(t, rv_sd(w1, d, 8));
    rv_free(t, s);
    rv_free(t, d);
    rv_free(t, w0);
    rv_free(t, w1);
}

static int move16(struct rv_tc *t, uint16_t op)
{
    if ((op & 0xfff8) == 0xf620) {
        /* the extension word's other bits are not checked: the 68040
           ignores them (Emu68 on AArch64 does not test them either) */
        uint16_t ext = rv_fetch16(t);
        int x = op & 7, y = (ext >> 12) & 7;
        uint8_t ax = rv_areg(x), ay = rv_areg(y);
        copy_line(t, ax, ay);
        rv_emit(t, rv_addiw(ax, ax, 16));
        if (y != x)
            rv_emit(t, rv_addiw(ay, ay, 16));
        return 1;
    }
    if ((op & 0xffe0) == 0xf600) {
        uint32_t abs = rv_fetch32(t);
        uint8_t ay = rv_areg(op & 7), m = rv_tmp(t);
        rv_emit_li(t, m, (int32_t)abs);
        if (op & 0x08)
            copy_line(t, m, ay);        /* (xxx).L to (Ay) */
        else
            copy_line(t, ay, m);        /* (Ay) to (xxx).L */
        rv_free(t, m);
        if (!(op & 0x10))
            rv_emit(t, rv_addiw(ay, ay, 16));
        return 1;
    }
    return 0;
}

int rv_emit_line_f_040(struct rv_tc *t, uint16_t op)
{
    if ((op & 0xff00) == 0xf400) {                   /* CINV, CPUSH */
        if ((op & 0x18) == 0)
            return 0;                               /* scope 00: Line-F */
        rv_emit_priv_check(t);                      /* caches 00: no operation */
        if (op & 0x80) {                            /* instruction cache */
            uint8_t k = rv_tmp(t), a = rv_tmp(t);
            rv_emit_li(t, a, (int64_t)(intptr_t)&rv_jit_stopped);
            rv_emit(t, rv_addi(k, RV_ZERO, RV_JIT_SERVICE_FLUSH));
            rv_emit(t, rv_sw(k, a, 0));
            rv_free(t, k);
            rv_free(t, a);
            rv_emit_li(t, RVR_PC, (int32_t)t->cursor);
            t->ended = 1;
        }
        return 1;
    }
    if ((op & 0xffe0) == 0xf500 ||                  /* PFLUSH */
        (op & 0xffd8) == 0xf548) {                  /* PTESTW, PTESTR */
        rv_emit_priv_check(t);
        return 1;
    }
    return move16(t, op);
}
