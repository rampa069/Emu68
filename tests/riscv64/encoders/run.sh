#!/usr/bin/env bash
# Check include/riscv64/rv64.h against the GNU assembler.
#
# Builds gen.c for the host, writes random instances of every encoder as
# assembler source plus the expected words, assembles the source with
# riscv64-elf-as and compares the result byte for byte.
#
# Usage: tests/riscv64/encoders/run.sh [build-dir]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
OUT="${1:-${HERE}/build}"
HOSTCC="${HOSTCC:-cc}"
CROSS="${CROSS:-riscv64-elf-}"
MARCH="rv64ima_zicsr_zifencei_zihintpause_zba_zbb_zbs_zicond"

mkdir -p "${OUT}"
"${HOSTCC}" -std=c11 -O1 -Wall -Wextra -Werror -I"${ROOT}/include" \
    "${HERE}/gen.c" -o "${OUT}/gen"
"${OUT}/gen" "${OUT}"

"${CROSS}as" -march="${MARCH}" -mno-relax "${OUT}/test.s" -o "${OUT}/test.o"
"${CROSS}objcopy" -O binary -j .text "${OUT}/test.o" "${OUT}/actual.bin"

if cmp -s "${OUT}/expected.bin" "${OUT}/actual.bin"; then
    echo "encoders: match the GNU assembler ($(( $(wc -c < "${OUT}/actual.bin") / 4 )) words)"
    exit 0
fi

echo "encoders: MISMATCH against the GNU assembler"
# Show the first few differing words with their source lines.
python3 -I - "${OUT}" <<'PY'
import struct, sys
out = sys.argv[1]
exp = open(f"{out}/expected.bin", "rb").read()
act = open(f"{out}/actual.bin", "rb").read()
src = [l.rstrip() for l in open(f"{out}/test.s") if l.strip() and not l.strip().startswith(".")]
shown = 0
for i in range(max(len(exp), len(act)) // 4):
    e = exp[4*i:4*i+4]; a = act[4*i:4*i+4]
    if e != a:
        ev = struct.unpack("<I", e)[0] if len(e) == 4 else None
        av = struct.unpack("<I", a)[0] if len(a) == 4 else None
        print(f"  {src[i] if i < len(src) else '?':40s} rv64.h {ev:08x} as {av:08x}" if ev is not None and av is not None else f"  length differs at word {i}")
        shown += 1
        if shown == 10:
            break
PY
exit 1
