#!/usr/bin/env python3
"""
Check src/riscv64/insn_decode.c against the GNU assembler.

Random integer loads and stores (RV64I and RVC forms) and some instructions
that must be rejected are assembled with riscv64-elf-as; the host harness
decodes the result and every field is compared with what was generated.

Usage: tests/riscv64/decoder/run.py [build-dir]
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build")
CROSS = os.environ.get("CROSS", "riscv64-elf-")
HOSTCC = os.environ.get("HOSTCC", "cc")
rnd = random.Random(0x68000)

LOADS = {"lb": (1, 1), "lh": (2, 1), "lw": (4, 1), "ld": (8, 1),
         "lbu": (1, 0), "lhu": (2, 0), "lwu": (4, 0)}
STORES = {"sb": 1, "sh": 2, "sw": 4, "sd": 8}

asm, want = [".option norvc"], []


def r():
    return rnd.randrange(32)


def cr():
    return 8 + rnd.randrange(8)


for _ in range(3000):
    k = rnd.randrange(3)
    off = rnd.randrange(-2048, 2048)
    if k == 0:
        m = rnd.choice(list(LOADS))
        rd, rs1 = r(), r()
        size, signed = LOADS[m]
        asm.append(f"{m} x{rd}, {off}(x{rs1})")
        want.append(f"mem 4 0 {size} {signed} {rd} {rs1} {off}")
    elif k == 1:
        m = rnd.choice(list(STORES))
        rs2, rs1 = r(), r()
        asm.append(f"{m} x{rs2}, {off}(x{rs1})")
        want.append(f"mem 4 1 {STORES[m]} 0 {rs2} {rs1} {off}")
    else:
        asm.append(f"add x{r()}, x{r()}, x{r()}")
        want.append("none 4")

asm.append(".option rvc")
for _ in range(3000):
    k = rnd.randrange(9)
    if k in (0, 1):     # c.lw / c.sw
        reg, base, off = cr(), cr(), 4 * rnd.randrange(32)
        m = "c.lw" if k == 0 else "c.sw"
        asm.append(f"{m} x{reg}, {off}(x{base})")
        want.append(f"mem 2 {k} 4 {1 - k} {reg} {base} {off}")
    elif k in (2, 3):   # c.ld / c.sd
        reg, base, off = cr(), cr(), 8 * rnd.randrange(32)
        m = "c.ld" if k == 2 else "c.sd"
        asm.append(f"{m} x{reg}, {off}(x{base})")
        want.append(f"mem 2 {k - 2} 8 0 {reg} {base} {off}")
    elif k == 4:        # c.lwsp
        reg, off = 1 + rnd.randrange(31), 4 * rnd.randrange(64)
        asm.append(f"c.lwsp x{reg}, {off}(sp)")
        want.append(f"mem 2 0 4 1 {reg} 2 {off}")
    elif k == 5:        # c.ldsp
        reg, off = 1 + rnd.randrange(31), 8 * rnd.randrange(64)
        asm.append(f"c.ldsp x{reg}, {off}(sp)")
        want.append(f"mem 2 0 8 0 {reg} 2 {off}")
    elif k == 6:        # c.swsp
        reg, off = r(), 4 * rnd.randrange(64)
        asm.append(f"c.swsp x{reg}, {off}(sp)")
        want.append(f"mem 2 1 4 0 {reg} 2 {off}")
    elif k == 7:        # c.sdsp
        reg, off = r(), 8 * rnd.randrange(64)
        asm.append(f"c.sdsp x{reg}, {off}(sp)")
        want.append(f"mem 2 1 8 0 {reg} 2 {off}")
    else:               # must be rejected
        asm.append(f"c.addi x{1 + rnd.randrange(31)}, {1 + rnd.randrange(31)}")
        want.append("none 2")

os.makedirs(OUT, exist_ok=True)
src, obj, binf, exe = (os.path.join(OUT, n) for n in ("test.s", "test.o", "test.bin", "harness"))
with open(src, "w") as f:
    f.write("\n".join(asm) + "\n")
subprocess.run([CROSS + "as", "-march=rv64gc", "-mno-relax", src, "-o", obj], check=True)
subprocess.run([CROSS + "objcopy", "-O", "binary", "-j", ".text", obj, binf], check=True)
subprocess.run([HOSTCC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "include"),
                os.path.join(HERE, "harness.c"), os.path.join(ROOT, "src", "riscv64", "insn_decode.c"),
                "-o", exe], check=True)
got = subprocess.run([exe, binf], check=True, capture_output=True, text=True).stdout.splitlines()

src_lines = [l for l in asm if not l.startswith(".")]
bad = [(s, g, w) for s, g, w in zip(src_lines, got, want) if g != w]
if len(got) != len(want):
    print(f"decoder: decoded {len(got)} instructions, expected {len(want)}")
    sys.exit(1)
if bad:
    print(f"decoder: {len(bad)} of {len(want)} differ")
    for s, g, w in bad[:10]:
        print(f"  {s:28s} got '{g}' want '{w}'")
    sys.exit(1)
mem = sum(1 for w in want if w.startswith("mem"))
print(f"decoder: {len(want)} instructions match the assembler ({mem} loads/stores, {len(want) - mem} rejected)")
