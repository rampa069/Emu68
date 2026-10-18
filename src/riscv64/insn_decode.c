/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "riscv64/insn_decode.h"

#define BIT(v, n)       (((v) >> (n)) & 1u)
#define BITS(v, hi, lo) (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static int decode32(uint32_t w, struct rv_mem_insn *d)
{
    uint32_t op = w & 0x7f, f3 = BITS(w, 14, 12);

    d->length = 4;
    d->base = BITS(w, 19, 15);

    if (op == 0x03) {           /* LOAD */
        static const uint8_t size[8] = { 1, 2, 4, 8, 1, 2, 4, 0 };
        if (size[f3] == 0)
            return 0;
        d->is_store = 0;
        d->size = size[f3];
        d->is_signed = f3 < 4;
        d->reg = BITS(w, 11, 7);
        d->offset = (int32_t)w >> 20;
        return 1;
    }
    if (op == 0x23) {           /* STORE */
        if (f3 > 3)
            return 0;
        d->is_store = 1;
        d->size = 1u << f3;
        d->is_signed = 0;
        d->reg = BITS(w, 24, 20);
        d->offset = ((int32_t)(w & 0xfe000000) >> 20) | (int32_t)BITS(w, 11, 7);
        return 1;
    }
    return 0;
}

static int decode16(uint16_t h, struct rv_mem_insn *d)
{
    uint32_t q = h & 3, f3 = BITS(h, 15, 13);

    d->length = 2;
    d->is_signed = 0;

    if (q == 0) {
        /* c.lw / c.ld / c.sw / c.sd: rd'/rs2' in [4:2], rs1' in [9:7] */
        d->base = 8 + BITS(h, 9, 7);
        d->reg = 8 + BITS(h, 4, 2);
        switch (f3) {
        case 2: case 6:         /* c.lw, c.sw: uimm[5:3|2|6] */
            d->size = 4;
            d->offset = (BITS(h, 12, 10) << 3) | (BIT(h, 6) << 2) | (BIT(h, 5) << 6);
            break;
        case 3: case 7:         /* c.ld, c.sd: uimm[5:3|7:6] */
            d->size = 8;
            d->offset = (BITS(h, 12, 10) << 3) | (BITS(h, 6, 5) << 6);
            break;
        default:
            return 0;
        }
        d->is_store = f3 >= 6;
        d->is_signed = f3 == 2;     /* c.lw sign-extends; c.ld is full width */
        return 1;
    }

    if (q == 2) {
        d->base = 2;            /* sp */
        switch (f3) {
        case 2:                 /* c.lwsp: uimm[5|4:2|7:6] */
            if (BITS(h, 11, 7) == 0)
                return 0;
            d->reg = BITS(h, 11, 7);
            d->size = 4;
            d->is_signed = 1;
            d->is_store = 0;
            d->offset = (BIT(h, 12) << 5) | (BITS(h, 6, 4) << 2) | (BITS(h, 3, 2) << 6);
            return 1;
        case 3:                 /* c.ldsp: uimm[5|4:3|8:6] */
            if (BITS(h, 11, 7) == 0)
                return 0;
            d->reg = BITS(h, 11, 7);
            d->size = 8;
            d->is_store = 0;
            d->offset = (BIT(h, 12) << 5) | (BITS(h, 6, 5) << 3) | (BITS(h, 4, 2) << 6);
            return 1;
        case 6:                 /* c.swsp: uimm[5:2|7:6] */
            d->reg = BITS(h, 6, 2);
            d->size = 4;
            d->is_store = 1;
            d->offset = (BITS(h, 12, 9) << 2) | (BITS(h, 8, 7) << 6);
            return 1;
        case 7:                 /* c.sdsp: uimm[5:3|8:6] */
            d->reg = BITS(h, 6, 2);
            d->size = 8;
            d->is_store = 1;
            d->offset = (BITS(h, 12, 10) << 3) | (BITS(h, 9, 7) << 6);
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

int rv_decode_mem(uint16_t lo, uint16_t hi, struct rv_mem_insn *d)
{
    if ((lo & 3) == 3)
        return decode32((uint32_t)lo | ((uint32_t)hi << 16), d);
    return decode16(lo, d);
}
