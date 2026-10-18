/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Condition code liveness for one translation unit, the RV64 counterpart
    of M68K_GetSRMask in src/M68k_SR.c.

    For every opcode rv_sr_usage gives the flags it reads and the flags it
    always overwrites. Walking the unit backwards from its end, where every
    flag is live, gives for each instruction the flags a later instruction
    may still read; the others are skipped (struct rv_tc, field live).

    The table must never claim that an instruction overwrites a flag it can
    leave alone, and never omit a flag it reads, or an earlier result would
    be lost. Anything not listed reads every flag and overwrites none, which
    is always safe. That default also covers every instruction that can take
    an exception (zero divide, TRAP, TRAPV, TRAPcc, CHK, ILLEGAL, Line-A/F,
    privilege violations), because the exception frame saves SR, and the
    SR and CCR instructions themselves.

    Unlike M68K_GetSRMask the scan does not follow branches out of the unit:
    the unit end keeps all flags live, so SR is exact at every unit boundary
    (the trace records it there, and interrupts will be taken there).
*/

#include <stdint.h>
#include "jit_internal.h"

#define X   CCR_X
#define N   CCR_N
#define Z   CCR_Z
#define V   CCR_V
#define C   CCR_C
#define ALL (X | N | Z | V | C)
#define NZVC (N | Z | V | C)

#define USE(needs, sets) (((uint32_t)(needs) << 8) | (uint32_t)(sets))
#define DEFAULT USE(ALL, 0)

/* Flags read by condition cc (Bcc, Scc, DBcc) */
static uint8_t cond_needs(int cc)
{
    static const uint8_t needs[16] = {
        0, 0, C | Z, C | Z, C, C, Z, Z, V, V, N, N, N | V, N | V, N | V | Z, N | V | Z
    };
    return needs[cc & 15];
}

/* needs << 8 | sets, for the flags XNZVC */
static uint32_t sr_usage(uint16_t op)
{
    int mode = (op >> 3) & 7, reg = op & 7;
    int size = (op >> 6) & 3;

    switch (op >> 12) {
    case 0x0:
        if (op & 0x0100)
            return mode == 1 ? DEFAULT : USE(0, Z);         /* MOVEP; BTST.. Dn,<ea> */
        switch (op & 0x0f00) {
        case 0x0800:                                        /* BTST.. #n,<ea> */
            return USE(0, Z);
        case 0x0000: case 0x0200: case 0x0a00:              /* ORI ANDI EORI */
            if (size == 3 || (mode == 7 && reg == 4))
                return DEFAULT;                             /* to CCR/SR, CMP2/CHK2 */
            return USE(0, NZVC);
        case 0x0400: case 0x0600:                           /* SUBI ADDI */
            return size == 3 ? DEFAULT : USE(0, ALL);
        case 0x0c00:                                        /* CMPI */
            return size == 3 ? DEFAULT : USE(0, NZVC);
        default:
            return DEFAULT;
        }
    case 0x1: case 0x2: case 0x3:                           /* MOVE, MOVEA */
        return ((op >> 6) & 7) == 1 ? USE(0, 0) : USE(0, NZVC);
    case 0x4:
        if ((op & 0xf1c0) == 0x41c0)                        /* LEA */
            return USE(0, 0);
        switch (op & 0xffc0) {
        case 0x4840:                                        /* SWAP, PEA */
            return mode == 0 ? USE(0, NZVC) : USE(0, 0);
        case 0x4880: case 0x48c0:                           /* EXT, MOVEM to memory */
            return mode == 0 ? USE(0, NZVC) : USE(0, 0);
        case 0x49c0:                                        /* EXTB.L */
            return mode == 0 ? USE(0, NZVC) : DEFAULT;
        case 0x4c80: case 0x4cc0:                           /* MOVEM to registers */
            return USE(0, 0);
        case 0x4c00:                                        /* MULU.L MULS.L */
            return USE(0, NZVC);
        case 0x4e80: case 0x4ec0:                           /* JSR JMP */
            return USE(0, 0);
        }
        if ((op & 0xfff8) == 0x4e50 || (op & 0xfff8) == 0x4e58)
            return USE(0, 0);                               /* LINK UNLK */
        if (op == 0x4e71 || op == 0x4e75)
            return USE(0, 0);                               /* NOP RTS */
        if (size == 3)
            return DEFAULT;                                 /* SR/CCR moves, TAS, ... */
        switch (op & 0xff00) {
        case 0x4000: return USE(X | Z, ALL);                /* NEGX: Z only cleared */
        case 0x4200: return USE(0, NZVC);                   /* CLR */
        case 0x4400: return USE(0, ALL);                    /* NEG */
        case 0x4600: return USE(0, NZVC);                   /* NOT */
        case 0x4a00: return USE(0, NZVC);                   /* TST */
        }
        return DEFAULT;
    case 0x5:
        if (size == 3) {
            if (mode == 1)                                  /* DBcc */
                return USE(cond_needs(op >> 8), 0);
            if (mode == 7 && reg >= 2)                      /* TRAPcc */
                return DEFAULT;
            return USE(cond_needs(op >> 8), 0);             /* Scc */
        }
        return mode == 1 ? USE(0, 0) : USE(0, ALL);         /* ADDQ SUBQ */
    case 0x6:
        return USE(cond_needs(op >> 8), 0);                 /* Bcc BRA BSR */
    case 0x7:
        return USE(0, NZVC);                                /* MOVEQ */
    case 0x8: case 0xc:
        if (size == 3)                                      /* DIVx.W: zero divide; MULx.W */
            return (op >> 12) == 0x8 ? DEFAULT : USE(0, NZVC);
        if ((op & 0x0130) == 0x0100)
            return DEFAULT;                                 /* SBCD PACK UNPK ABCD EXG */
        return USE(0, NZVC);                                /* OR AND */
    case 0x9: case 0xd:
        if (size == 3)
            return USE(0, 0);                               /* SUBA ADDA */
        if ((op & 0x0130) == 0x0100)
            return USE(X | Z, ALL);                         /* SUBX ADDX: Z only cleared */
        return USE(0, ALL);                                 /* SUB ADD */
    case 0xb:
        return USE(0, NZVC);                                /* CMP CMPA CMPM EOR */
    case 0xe:
        if (size == 3) {
            if (op & 0x0800)
                return DEFAULT;                             /* bitfields */
            switch ((op >> 9) & 3) {                        /* memory, count 1 */
            case 0: case 1: return USE(0, ALL);             /* ASx LSx */
            case 2:         return USE(X, ALL);             /* ROXx */
            default:        return USE(0, NZVC);            /* ROx */
            }
        }
        {
            int type = (op >> 3) & 3, imm = !(op & 0x20);
            if (type == 3)                                  /* ROx: X unaffected */
                return USE(0, NZVC);
            if (type == 2)                                  /* ROXx: C = X for count 0 */
                return USE(X, ALL);
            /* ASx LSx: X unaffected by a register count of 0 */
            return imm ? USE(0, ALL) : USE(0, NZVC);
        }
    default:                                                /* Line-A, Line-F */
        return DEFAULT;
    }
}

uint32_t rv_sr_usage(uint16_t opcode)
{
    return sr_usage(opcode);
}

/*
    live[i] = the flags instruction i must still compute: all of them except
    those it is known to overwrite and that nothing later in the unit, nor
    the unit exit, reads. An instruction outside the table (sets = 0) thus
    keeps every flag live, including the ones its emitter does write.
*/
void rv_flags_liveness(const uint16_t *opcodes, uint32_t count, uint8_t *live)
{
    uint8_t out = ALL;

    for (uint32_t i = count; i-- > 0;) {
        uint32_t u = sr_usage(opcodes[i]);
        uint8_t needs = (u >> 8) & ALL, sets = u & ALL;
        live[i] = (uint8_t)(ALL & ~(sets & ~out));
        out = needs | (out & ~sets);
    }
}
