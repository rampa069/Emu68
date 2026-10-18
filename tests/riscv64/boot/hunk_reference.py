#!/usr/bin/env python3
"""
Reference AmigaOS HUNK loader reproducing the memory layout of
src/HunkLoader.c, used to check the RV64 boot path independently.

Layout: every hunk is a SegList (BE32 h_Size, BE32 h_Next) followed by its
data, allocated from a pool that starts at the load address and advances in
32-byte steps. h_Next holds the address of the next hunk's h_Next field.
RELOC32 adds the address of the target hunk's data to each longword.

Usage: hunk_reference.py <hunk-file> <load-address> <memory-dump>
Exits 0 when the dump matches the reference image.
"""
import struct
import sys

HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3EC, 0x3F0, 0x3F1, 0x3F2
HUNK_HEADER = 0x3F3


def words_of(data):
    return list(struct.unpack(">%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))


def build(path, base):
    w = words_of(open(path, "rb").read())
    if w[0] != HUNK_HEADER or w[1] != 0:
        raise SystemExit("not a HUNK executable without resident libraries")
    first, last = w[3], w[4]
    sizes = w[5 : 5 + last - first + 1]
    if any((s & 0xC0000000) == 0xC0000000 for s in sizes):
        raise SystemExit("extended memory flags are not handled")
    pos = 5 + len(sizes)

    # Allocate SegLists exactly as _my_malloc does.
    hunks, pool = [], base
    for s in sizes:
        size = 4 * (s & 0x3FFFFFFF)
        hunks.append((pool, size))
        pool += (size + 8 + 31) & ~31
    image = bytearray(pool - base)

    def put32(addr, value):
        struct.pack_into(">I", image, addr - base, value & 0xFFFFFFFF)

    def get32(addr):
        return struct.unpack_from(">I", image, addr - base)[0]

    for i, (addr, size) in enumerate(hunks):
        put32(addr, size)
        nxt = hunks[i + 1][0] + 4 if i + 1 < len(hunks) else 0
        put32(addr + 4, nxt)

    block = first
    while pos < len(w) and block <= last:
        kind = w[pos] & 0x3FFFFFFF
        data_addr = hunks[block - first][0] + 8
        if kind in (HUNK_CODE, HUNK_DATA):
            n = w[pos + 1]
            for k in range(n):
                put32(data_addr + 4 * k, w[pos + 2 + k])
            pos += 2 + n
        elif kind == HUNK_BSS:
            pos += 2
        elif kind == HUNK_RELOC32:
            pos += 1
            while w[pos] != 0:
                count, target = w[pos], w[pos + 1]
                ref = hunks[target - first][0] + 8
                for off in w[pos + 2 : pos + 2 + count]:
                    put32(data_addr + off, get32(data_addr + off) + ref)
                pos += 2 + count
            pos += 1
        elif kind == HUNK_SYMBOL:
            pos += 1
            while w[pos] != 0:
                pos += 1 + w[pos] + 1
            pos += 1
        elif kind == HUNK_DEBUG:
            pos += 2 + w[pos + 1]
        elif kind == HUNK_END:
            pos += 1
            block += 1
        else:
            raise SystemExit("unsupported hunk type %x" % kind)
    return bytes(image), hunks


def main():
    path, base, dump = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    want, hunks = build(path, base)
    got = open(dump, "rb").read()[: len(want)]
    print("reference: %d hunks, %d bytes from 0x%08x" % (len(hunks), len(want), base))
    if got == want:
        print("hunk load: memory matches the reference loader")
        return 0
    bad = [i for i in range(len(want)) if i >= len(got) or got[i] != want[i]]
    print("hunk load: %d bytes differ, first at 0x%08x" % (len(bad), base + bad[0]))
    for i in bad[:8]:
        print("  0x%08x got %s want %02x" % (base + i, "%02x" % got[i] if i < len(got) else "--", want[i]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
