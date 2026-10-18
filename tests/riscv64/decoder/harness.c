/*
    Host harness for tests/riscv64/decoder: decodes a flat binary of RV64
    instructions with rv_decode_mem() and prints one line per instruction.
*/
#include <stdio.h>
#include <stdint.h>
#include "riscv64/insn_decode.h"

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 20];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f)
        return 2;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 1 < n;) {
        uint16_t lo = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        uint16_t hi = i + 3 < n ? (uint16_t)(buf[i + 2] | (buf[i + 3] << 8)) : 0;
        struct rv_mem_insn d;
        if (rv_decode_mem(lo, hi, &d)) {
            printf("mem %u %u %u %u %u %u %d\n", d.length, d.is_store, d.size,
                   d.is_signed, d.reg, d.base, d.offset);
            i += d.length;
        } else {
            unsigned len = (lo & 3) == 3 ? 4 : 2;
            printf("none %u\n", len);
            i += len;
        }
    }
    return 0;
}
