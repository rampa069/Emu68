/*
    Generator for the rv64.h encoder test.

    For every encoder it writes random instances as GNU assembler source to
    test.s and the words produced by rv64.h to expected.bin. run.sh assembles
    test.s and compares the two byte for byte.

    It also checks, without the assembler, that rv_li() builds every value it
    is given (by evaluating the emitted sequence) and that rv_patch_b() and
    rv_patch_j() give the same words as encoding with the new offset.

    Usage: gen <out-dir>
*/
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "riscv64/rv64.h"

#define N 64            /* random instances per encoder */

static FILE *fs, *fb;
static unsigned long count;
static uint64_t rng_state = 0x243f6a8885a308d3ULL;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dULL;
}

static int reg(void) { return (int)(rng() & 31); }
static int32_t imm12(void) { return (int32_t)(rng() & 0xfff) - 2048; }
static int32_t boff(void) { return ((int32_t)(rng() % 4096) - 2048) * 2; }
static int32_t joff(void) { return ((int32_t)(rng() % 1048576) - 524288) * 2; }

static void emit(uint32_t word, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fs, fmt, ap);
    va_end(ap);
    fputc('\n', fs);
    /* expected.bin holds the bytes as they sit in memory */
    fwrite(&word, 4, 1, fb);
    count++;
}

static const char *target(int32_t off)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), ". %c %d", off < 0 ? '-' : '+', off < 0 ? -off : off);
    return buf;
}

/* ---- families ---- */

#define R3(asm, fn) for (int i = 0; i < N; i++) { int d = reg(), a = reg(), b = reg(); \
    emit(fn(d, a, b), "%s x%d, x%d, x%d", asm, d, a, b); }
#define R2(asm, fn) for (int i = 0; i < N; i++) { int d = reg(), a = reg(); \
    emit(fn(d, a), "%s x%d, x%d", asm, d, a); }
#define I12(asm, fn) for (int i = 0; i < N; i++) { int d = reg(), a = reg(); int32_t m = imm12(); \
    emit(fn(d, a, m), "%s x%d, x%d, %d", asm, d, a, m); }
#define SH(asm, fn, bits) for (int i = 0; i < N; i++) { int d = reg(), a = reg(); int s = (int)(rng() % (bits)); \
    emit(fn(d, a, s), "%s x%d, x%d, %d", asm, d, a, s); }
#define LD(asm, fn) for (int i = 0; i < N; i++) { int d = reg(), a = reg(); int32_t m = imm12(); \
    emit(fn(d, a, m), "%s x%d, %d(x%d)", asm, d, m, a); }
#define ST(asm, fn) for (int i = 0; i < N; i++) { int s = reg(), a = reg(); int32_t m = imm12(); \
    emit(fn(s, a, m), "%s x%d, %d(x%d)", asm, s, m, a); }
#define BR(asm, fn) for (int i = 0; i < N; i++) { int a = reg(), b = reg(); int32_t o = boff(); \
    emit(fn(a, b, o), "%s x%d, x%d, %s", asm, a, b, target(o)); }
#define BRZ(asm, fn) for (int i = 0; i < N; i++) { int a = reg(); int32_t o = boff(); \
    emit(fn(a, o), "%s x%d, %s", asm, a, target(o)); }

static const char *ordering(int aq, int rl)
{
    return aq && rl ? ".aqrl" : aq ? ".aq" : rl ? ".rl" : "";
}

#define AMO(asm, fn) for (int i = 0; i < N; i++) { int d = reg(), s = reg(), a = reg(); \
    int aq = (int)(rng() & 1), rl = (int)(rng() & 1); \
    emit(fn(d, s, a, aq, rl), "%s%s x%d, x%d, (x%d)", asm, ordering(aq, rl), d, s, a); }

static int check_li(void)
{
    static const int64_t fixed[] = {
        0, 1, -1, 2047, 2048, -2048, -2049, 0x7ff, 0x800, 0xfff, 0x1000,
        0x12345678, 0x7fffffff, -0x80000000LL, 0x80000000LL, 0xffffffffLL,
        0x100000000LL, 0x123456789abcdef0LL, INT64_MAX, INT64_MIN,
        (int64_t)0xffffffff80000000ULL, (int64_t)0xffffffc000000000ULL,
        (int64_t)0xffffffc080200000ULL, 0x0000800000000000LL,
    };
    int bad = 0;
    for (int k = 0; k < 200000 + (int)(sizeof(fixed) / sizeof(fixed[0])); k++) {
        int64_t v;
        if (k < (int)(sizeof(fixed) / sizeof(fixed[0])))
            v = fixed[k];
        else {
            uint64_t r = rng();
            /* mix full random values with sparse and narrow ones */
            v = (k & 3) == 0 ? (int64_t)r : (k & 3) == 1 ? (int64_t)(r & 0xffff000000000fffULL)
              : (k & 3) == 2 ? (int64_t)(int32_t)r : (int64_t)(r >> (r & 63));
        }
        uint32_t buf[16];
        int n = rv_li(buf, 10, v);
        if (n < 1 || n > 8) {
            printf("li %" PRIx64 ": %d instructions\n", (uint64_t)v, n);
            bad++;
            continue;
        }
        /* evaluate: only addi, addiw, lui, slli on x10 can appear */
        int64_t x = 0;
        for (int i = 0; i < n; i++) {
            uint32_t w = RV_INSN(buf[i]);
            uint32_t op = w & 0x7f, f3 = (w >> 12) & 7, rs1 = (w >> 15) & 31;
            int64_t src = rs1 == 0 ? 0 : x;
            int32_t im = (int32_t)w >> 20;
            if (((w >> 7) & 31) != 10) { bad++; break; }
            if (op == RV_OP_LUI)
                x = (int64_t)(int32_t)(w & 0xfffff000u);
            else if (op == RV_OP_IMM && f3 == 0)
                x = src + im;
            else if (op == RV_OP_IMM && f3 == 1)
                x = (int64_t)((uint64_t)src << ((w >> 20) & 63));
            else if (op == RV_OP_IMM_32 && f3 == 0)
                x = (int64_t)(int32_t)((uint32_t)src + (uint32_t)im);
            else { printf("li: unexpected opcode %08x\n", w); bad++; break; }
        }
        if (x != v) {
            if (bad < 10)
                printf("li %" PRIx64 ": got %" PRIx64 " in %d instructions\n", (uint64_t)v, (uint64_t)x, n);
            bad++;
        }
    }
    printf("rv_li: %d values, %d wrong\n", 200000 + (int)(sizeof(fixed) / sizeof(fixed[0])), bad);
    return bad;
}

static int check_patch(void)
{
    int bad = 0;
    for (int i = 0; i < 100000; i++) {
        int a = reg(), b = reg(), d = reg();
        int32_t o1 = boff(), o2 = boff(), j1 = joff(), j2 = joff();
        if (rv_patch_b(rv_bltu(a, b, o1), o2) != rv_bltu(a, b, o2)) bad++;
        if (rv_patch_j(rv_jal(d, j1), j2) != rv_jal(d, j2)) bad++;
    }
    printf("rv_patch_b/rv_patch_j: %s\n", bad ? "FAIL" : "100000 pairs ok");
    return bad;
}

int main(int argc, char **argv)
{
    char path[1024];
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out-dir>\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/test.s", argv[1]);
    fs = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/expected.bin", argv[1]);
    fb = fopen(path, "wb");
    if (!fs || !fb) {
        perror("open");
        return 2;
    }
    fprintf(fs, "    .option norvc\n    .option norelax\n    .text\n");

    /* RV64I */
    for (int i = 0; i < N; i++) { int d = reg(); uint32_t u = (uint32_t)(rng() & 0xfffff);
        emit(rv_lui(d, u), "lui x%d, %u", d, u);
        emit(rv_auipc(d, u), "auipc x%d, %u", d, u); }
    for (int i = 0; i < N; i++) { int d = reg(); int32_t o = joff();
        emit(rv_jal(d, o), "jal x%d, %s", d, target(o)); }
    LD("jalr", rv_jalr)
    BR("beq", rv_beq) BR("bne", rv_bne) BR("blt", rv_blt) BR("bge", rv_bge)
    BR("bltu", rv_bltu) BR("bgeu", rv_bgeu)
    LD("lb", rv_lb) LD("lh", rv_lh) LD("lw", rv_lw) LD("ld", rv_ld)
    LD("lbu", rv_lbu) LD("lhu", rv_lhu) LD("lwu", rv_lwu)
    ST("sb", rv_sb) ST("sh", rv_sh) ST("sw", rv_sw) ST("sd", rv_sd)
    I12("addi", rv_addi) I12("slti", rv_slti) I12("sltiu", rv_sltiu)
    I12("xori", rv_xori) I12("ori", rv_ori) I12("andi", rv_andi)
    SH("slli", rv_slli, 64) SH("srli", rv_srli, 64) SH("srai", rv_srai, 64)
    R3("add", rv_add) R3("sub", rv_sub) R3("sll", rv_sll) R3("slt", rv_slt)
    R3("sltu", rv_sltu) R3("xor", rv_xor) R3("srl", rv_srl) R3("sra", rv_sra)
    R3("or", rv_or) R3("and", rv_and)
    I12("addiw", rv_addiw)
    SH("slliw", rv_slliw, 32) SH("srliw", rv_srliw, 32) SH("sraiw", rv_sraiw, 32)
    R3("addw", rv_addw) R3("subw", rv_subw) R3("sllw", rv_sllw)
    R3("srlw", rv_srlw) R3("sraw", rv_sraw)

    /* fences and system */
    emit(rv_fence(RV_FENCE_RW, RV_FENCE_RW), "fence rw, rw");
    emit(rv_fence(RV_FENCE_IORW, RV_FENCE_IORW), "fence iorw, iorw");
    emit(rv_fence(RV_FENCE_R, RV_FENCE_W), "fence r, w");
    emit(rv_fence(RV_FENCE_W | RV_FENCE_O, RV_FENCE_I | RV_FENCE_R), "fence ow, ir");
    emit(rv_fence_i(), "fence.i");
    emit(rv_pause(), "pause");
    emit(rv_ecall(), "ecall");
    emit(rv_ebreak(), "ebreak");
    emit(rv_sret(), "sret");
    emit(rv_wfi(), "wfi");
    for (int i = 0; i < N; i++) { int a = reg(), b = reg();
        emit(rv_sfence_vma(a, b), "sfence.vma x%d, x%d", a, b); }

    /* Zicsr */
    for (int i = 0; i < N; i++) { int d = reg(), a = reg(); uint16_t c = (uint16_t)(rng() & 0xfff);
        emit(rv_csrrw(d, c, a), "csrrw x%d, %u, x%d", d, c, a);
        emit(rv_csrrs(d, c, a), "csrrs x%d, %u, x%d", d, c, a);
        emit(rv_csrrc(d, c, a), "csrrc x%d, %u, x%d", d, c, a);
        emit(rv_csrrwi(d, c, a), "csrrwi x%d, %u, %d", d, c, a);
        emit(rv_csrrsi(d, c, a), "csrrsi x%d, %u, %d", d, c, a);
        emit(rv_csrrci(d, c, a), "csrrci x%d, %u, %d", d, c, a); }

    /* M */
    R3("mul", rv_mul) R3("mulh", rv_mulh) R3("mulhsu", rv_mulhsu) R3("mulhu", rv_mulhu)
    R3("div", rv_div) R3("divu", rv_divu) R3("rem", rv_rem) R3("remu", rv_remu)
    R3("mulw", rv_mulw) R3("divw", rv_divw) R3("divuw", rv_divuw)
    R3("remw", rv_remw) R3("remuw", rv_remuw)

    /* A */
    for (int i = 0; i < N; i++) { int d = reg(), a = reg(); int aq = (int)(rng() & 1), rl = (int)(rng() & 1);
        emit(rv_lr_w(d, a, aq, rl), "lr.w%s x%d, (x%d)", ordering(aq, rl), d, a);
        emit(rv_lr_d(d, a, aq, rl), "lr.d%s x%d, (x%d)", ordering(aq, rl), d, a); }
    AMO("sc.w", rv_sc_w) AMO("sc.d", rv_sc_d) AMO("amoswap.w", rv_amoswap_w)
    AMO("amoadd.w", rv_amoadd_w) AMO("amoand.w", rv_amoand_w)
    AMO("amoor.w", rv_amoor_w) AMO("amoxor.w", rv_amoxor_w)

    /* Zba */
    R3("add.uw", rv_add_uw) R3("sh1add", rv_sh1add) R3("sh2add", rv_sh2add)
    R3("sh3add", rv_sh3add) R3("sh1add.uw", rv_sh1add_uw) R3("sh2add.uw", rv_sh2add_uw)
    R3("sh3add.uw", rv_sh3add_uw) SH("slli.uw", rv_slli_uw, 64)

    /* Zbb */
    R3("andn", rv_andn) R3("orn", rv_orn) R3("xnor", rv_xnor)
    R2("clz", rv_clz) R2("ctz", rv_ctz) R2("cpop", rv_cpop)
    R2("sext.b", rv_sext_b) R2("sext.h", rv_sext_h)
    R2("clzw", rv_clzw) R2("ctzw", rv_ctzw) R2("cpopw", rv_cpopw) R2("zext.h", rv_zext_h)
    R3("max", rv_max) R3("maxu", rv_maxu) R3("min", rv_min) R3("minu", rv_minu)
    R3("rol", rv_rol) R3("ror", rv_ror) R3("rolw", rv_rolw) R3("rorw", rv_rorw)
    SH("rori", rv_rori, 64) SH("roriw", rv_roriw, 32)
    R2("orc.b", rv_orc_b) R2("rev8", rv_rev8)

    /* Zbs */
    R3("bclr", rv_bclr) R3("bext", rv_bext) R3("binv", rv_binv) R3("bset", rv_bset)
    SH("bclri", rv_bclri, 64) SH("bexti", rv_bexti, 64)
    SH("binvi", rv_binvi, 64) SH("bseti", rv_bseti, 64)

    /* Zicond */
    R3("czero.eqz", rv_czero_eqz) R3("czero.nez", rv_czero_nez)

    /* pseudo-instructions */
    emit(rv_nop(), "nop");
    emit(rv_ret(), "ret");
    R2("mv", rv_mv) R2("not", rv_not) R2("neg", rv_neg) R2("negw", rv_negw)
    R2("sext.w", rv_sext_w) R2("zext.w", rv_zext_w) R2("zext.b", rv_zext_b)
    R2("seqz", rv_seqz) R2("snez", rv_snez) R2("sltz", rv_sltz) R2("sgtz", rv_sgtz)
    BRZ("beqz", rv_beqz) BRZ("bnez", rv_bnez) BRZ("bltz", rv_bltz) BRZ("bgez", rv_bgez)
    for (int i = 0; i < N; i++) { int32_t o = joff(); int a = reg();
        emit(rv_j_(o), "j %s", target(o));
        emit(rv_jr(a), "jr x%d", a); }

    fclose(fs);
    fclose(fb);
    printf("encoders: %lu instructions generated\n", count);

    return (check_li() || check_patch()) ? 1 : 0;
}
