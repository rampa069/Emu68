/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Decoder for the integer load and store instructions that can fault on
    an m68k access: RV64I loads and stores and their RVC forms. Plain C with
    no host dependencies, so tests/riscv64/decoder can check it on the build
    machine against the GNU assembler.
*/

#ifndef _EMU68_RISCV64_INSN_DECODE_H
#define _EMU68_RISCV64_INSN_DECODE_H

#include <stdint.h>

struct rv_mem_insn {
    uint8_t length;     /* 2 or 4 bytes */
    uint8_t is_store;
    uint8_t size;       /* access size in bytes: 1, 2, 4 or 8 */
    uint8_t is_signed;  /* loads only: sign-extend the result */
    uint8_t reg;        /* rd for loads, rs2 for stores */
    uint8_t base;       /* rs1 */
    int32_t offset;
};

/*
    Decode the instruction whose first 16 bits are lo and next 16 bits are
    hi (hi is ignored for compressed instructions). Returns 1 and fills d for
    an integer load or store, 0 for anything else.
*/
int rv_decode_mem(uint16_t lo, uint16_t hi, struct rv_mem_insn *d);

#endif /* _EMU68_RISCV64_INSN_DECODE_H */
