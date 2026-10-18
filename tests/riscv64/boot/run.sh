#!/usr/bin/env bash
# Boot the RV64 Emu68 runtime on QEMU riscv virt with a HUNK executable as
# initrd, run the MMIO self-test (emulated PL011 at m68k 0xf2201000) and
# check the loaded image against hunk_reference.py.
#
# Usage: tests/riscv64/boot/run.sh <Emu68.elf> <hunk-file>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${1:?usage: $0 <Emu68.elf> <hunk-file>}"
PROG="${2:?usage: $0 <Emu68.elf> <hunk-file>}"
QEMU="${QEMU:-qemu-system-riscv64}"

WORK="$(mktemp -d /tmp/e68rv.XXXX)"     # short path: UNIX socket limit
trap 'kill "${QPID:-}" 2>/dev/null || true; rm -rf "${WORK}"' EXIT

"${QEMU}" -M virt -m 512 -display none -no-shutdown \
    -monitor "unix:${WORK}/mon,server,nowait" -serial "file:${WORK}/serial" \
    -kernel "${ELF}" -initrd "${PROG}" -append "mmio_selftest norun" &
QPID=$!

for ((i = 0; i < 300; i++)); do
    grep -qE "stopping before the translator|\[TRAP\]" "${WORK}/serial" 2>/dev/null && break
    perl -e 'select(undef, undef, undef, 0.1)'
done
tr -d '\r' < "${WORK}/serial"

if grep -q "\[TRAP\]" "${WORK}/serial"; then
    echo "boot: trapped"
    exit 1
fi
# QEMU exits with status 0 even when SBI reports a failure, so check the log.
grep -q "\[MMIO\] self-test passed" "${WORK}/serial" || { echo "boot: MMIO self-test failed"; exit 1; }
grep -q "matches the HUNK_CODE data" "${WORK}/serial" || { echo "boot: entry check failed"; exit 1; }

phys=$(sed -n 's/.*m68k RAM 0x00000000-.* at physical \(0x[0-9a-f]*\).*/\1/p' "${WORK}/serial")
load=$(sed -n 's/.*Loading HUNK executable (\([0-9]*\) bytes in memory) at \(0x[0-9a-f]*\).*/\2/p' "${WORK}/serial")
size=$(sed -n 's/.*Loading HUNK executable (\([0-9]*\) bytes in memory).*/\1/p' "${WORK}/serial")
addr=$(printf '0x%x' $((phys + load)))

# HMP needs the file name quoted; keep the connection open while it runs.
{ printf 'pmemsave %s %s "%s"\n' "${addr}" "${size}" "${WORK}/dump.bin"; sleep 1; } \
    | nc -U "${WORK}/mon" >/dev/null
for ((i = 0; i < 50; i++)); do [[ -s "${WORK}/dump.bin" ]] && break; perl -e 'select(undef, undef, undef, 0.1)'; done

python3 -I "${HERE}/hunk_reference.py" "${PROG}" "${load}" "${WORK}/dump.bin"
