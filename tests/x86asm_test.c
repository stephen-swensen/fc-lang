/* The native backend's instruction encoder (src/x86asm.c) against GNU as.
 *
 * Every instruction form the encoder offers, over the registers and the
 * memory operands that need different encodings (%rsp and %r12 as a base take
 * a SIB byte, %rbp and %r13 a displacement, %r8-%r15 a REX prefix, %spl-%dil
 * an empty one), is encoded here and written as an AT&T line; `as` assembles
 * the lines, and each instruction must come out byte for byte the same.
 * Branches, calls and RIP-relative operands are left out: their bytes are
 * settled at layout, and the backend's tests cover them.
 *
 * Run by `make test-native` (and `make test-x86asm`). Needs `as` and
 * `objcopy` on PATH. */

#define _DEFAULT_SOURCE     /* mkdtemp */

#include "x86asm.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOT 16             /* each instruction padded to this with int3 */

typedef struct {
    char *text;
    unsigned char bytes[SLOT];
    int len;
} Item;

static Item *g_items;
static int g_n, g_cap;
static Code g_c;
static int g_bad;

/* The instruction just encoded into g_c, as `as` should read it. */
static void item(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_c.nev != 0 || g_c.len == 0 || g_c.len >= SLOT) {
        fprintf(stderr, "x86asm_test: '%s' did not encode to plain bytes\n", buf);
        g_bad++;
    } else {
        if (g_n == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 1024;
            g_items = realloc(g_items, (size_t)g_cap * sizeof *g_items);
        }
        Item *it = &g_items[g_n++];
        it->text = strdup(buf);
        it->len = (int)g_c.len;
        memset(it->bytes, 0xcc, SLOT);
        memcpy(it->bytes, g_c.p, g_c.len);
    }
    code_reset(&g_c);
}

/* ---- AT&T operand text ---- */

static const char *const RQ[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *const RL[16] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *const RW[16] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *const RB[16] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };

static char g_txt[8][64];
static int g_txt_i;

/* An operand at width w (1, 2, 4, 8). */
static const char *T(Opnd o, int w) {
    char *s = g_txt[g_txt_i++ % 8];
    switch (o.kind) {
    case OPND_REG:
        snprintf(s, 64, "%%%s", (w == 8 ? RQ : w == 4 ? RL : w == 2 ? RW : RB)[o.base]);
        break;
    case OPND_XMM:
        snprintf(s, 64, "%%xmm%d", o.base);
        break;
    case OPND_IMM:
        snprintf(s, 64, "$%lld", (long long)o.disp);
        break;
    case OPND_MEM:
        if (o.index < 0) snprintf(s, 64, "%lld(%%%s)", (long long)o.disp, RQ[o.base]);
        else snprintf(s, 64, "%lld(%%%s,%%%s,%d)", (long long)o.disp, RQ[o.base], RQ[o.index], o.scale);
        break;
    }
    return s;
}

static char sfx(int w) { return w == 8 ? 'q' : w == 4 ? 'l' : w == 2 ? 'w' : 'b'; }

/* ---- The operands tried ---- */

static Opnd g_mem[64];
static int g_nmem;

static void mems(void) {
    static const int64_t disps[] = { 127, -128, 128, -129, 0x1000, -0x12345 };
    for (int b = 0; b < 16; b++) {
        g_mem[g_nmem++] = M((Reg)b, 0);
        g_mem[g_nmem++] = M((Reg)b, -8);
        if (b == RSP || b == RBP || b == R12 || b == R13)
            for (size_t d = 0; d < sizeof disps / sizeof *disps; d++) g_mem[g_nmem++] = M((Reg)b, disps[d]);
    }
    g_mem[g_nmem++] = MI(RAX, RCX, 1, 0);
    g_mem[g_nmem++] = MI(RDX, R9, 8, 24);
    g_mem[g_nmem++] = MI(RBP, RAX, 4, -16);
    g_mem[g_nmem++] = MI(R13, R12, 2, 0);
}

static const int64_t IMMS[] = { 0, 1, -1, 2, 127, -128, 128, -129, 255, 1000, -1000,
                                0x7fffffff, -0x7fffffff - 1 };
#define NIMMS (int)(sizeof IMMS / sizeof *IMMS)

/* An immediate representable at width w, as as reads it. */
static bool imm_ok(int64_t v, int w) {
    if (w == 1) return v >= -128 && v <= 255;
    if (w == 2) return v >= -32768 && v <= 65535;
    return v >= INT32_MIN && v <= INT32_MAX;
}

/* ---- The forms ---- */

static void alu(void) {
    static const char *const names[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
    static const int ws[] = { 1, 2, 4, 8 };
    for (int op = 0; op < 8; op++)
        for (int wi = 0; wi < 4; wi++) {
            int w = ws[wi];
            for (int a = 0; a < 16; a++)
                for (int b = 0; b < 16; b += 5) {
                    x_alu(&g_c, (AluOp)op, w, R((Reg)a), R((Reg)b));
                    item("%s%c %s, %s", names[op], sfx(w), T(R((Reg)a), w), T(R((Reg)b), w));
                }
            for (int m = 0; m < g_nmem; m++) {
                x_alu(&g_c, (AluOp)op, w, g_mem[m], R((Reg)(m % 16)));
                item("%s%c %s, %s", names[op], sfx(w), T(g_mem[m], w), T(R((Reg)(m % 16)), w));
                x_alu(&g_c, (AluOp)op, w, R((Reg)(m % 16)), g_mem[m]);
                item("%s%c %s, %s", names[op], sfx(w), T(R((Reg)(m % 16)), w), T(g_mem[m], w));
            }
            for (int i = 0; i < NIMMS; i++) {
                if (!imm_ok(IMMS[i], w)) continue;
                for (int r = 0; r < 16; r += 3) {
                    x_alu(&g_c, (AluOp)op, w, IMM(IMMS[i]), R((Reg)r));
                    item("%s%c %s, %s", names[op], sfx(w), T(IMM(IMMS[i]), w), T(R((Reg)r), w));
                }
                Opnd m = g_mem[i % g_nmem];
                x_alu(&g_c, (AluOp)op, w, IMM(IMMS[i]), m);
                item("%s%c %s, %s", names[op], sfx(w), T(IMM(IMMS[i]), w), T(m, w));
            }
        }
}

static void mov(void) {
    static const int ws[] = { 1, 2, 4, 8 };
    for (int wi = 0; wi < 4; wi++) {
        int w = ws[wi];
        for (int a = 0; a < 16; a++)
            for (int b = 0; b < 16; b += 3) {
                x_mov(&g_c, w, R((Reg)a), R((Reg)b));
                item("mov%c %s, %s", sfx(w), T(R((Reg)a), w), T(R((Reg)b), w));
            }
        for (int m = 0; m < g_nmem; m++) {
            Reg r = (Reg)((m * 7) % 16);
            x_mov(&g_c, w, g_mem[m], R(r));
            item("mov%c %s, %s", sfx(w), T(g_mem[m], w), T(R(r), w));
            x_mov(&g_c, w, R(r), g_mem[m]);
            item("mov%c %s, %s", sfx(w), T(R(r), w), T(g_mem[m], w));
        }
        for (int i = 0; i < NIMMS; i++) {
            if (!imm_ok(IMMS[i], w)) continue;
            for (int r = 0; r < 16; r += 5) {
                x_mov(&g_c, w, IMM(IMMS[i]), R((Reg)r));
                item("mov%c %s, %s", sfx(w), T(IMM(IMMS[i]), w), T(R((Reg)r), w));
            }
            Opnd m = g_mem[(i * 3) % g_nmem];
            x_mov(&g_c, w, IMM(IMMS[i]), m);
            item("mov%c %s, %s", sfx(w), T(IMM(IMMS[i]), w), T(m, w));
        }
    }
    /* movl zero-extends: the backend's way to an unsigned 32-bit constant */
    x_mov(&g_c, 4, IMM(0xffffffffLL), R(RAX));
    item("movl $0xffffffff, %%eax");
    static const int64_t big[] = { 0x80000000LL, -0x80000001LL, 0x123456789abcdefLL, -1 };
    for (int i = 0; i < 4; i++)
        for (int r = 0; r < 16; r += 7) {
            x_movabs(&g_c, big[i], R((Reg)r));
            item("movabsq $%lld, %s", (long long)big[i], T(R((Reg)r), 8));
        }
}

static void test_lea(void) {
    static const int ws[] = { 1, 2, 4, 8 };
    for (int wi = 0; wi < 4; wi++) {
        int w = ws[wi];
        for (int a = 0; a < 16; a += 3)
            for (int b = 0; b < 16; b += 2) {
                x_test(&g_c, w, R((Reg)a), R((Reg)b));
                item("test%c %s, %s", sfx(w), T(R((Reg)a), w), T(R((Reg)b), w));
            }
        for (int i = 0; i < NIMMS; i++) {
            if (!imm_ok(IMMS[i], w)) continue;
            for (int r = 0; r < 16; r += 5) {
                x_test(&g_c, w, IMM(IMMS[i]), R((Reg)r));
                item("test%c %s, %s", sfx(w), T(IMM(IMMS[i]), w), T(R((Reg)r), w));
            }
        }
        for (int m = 0; m < g_nmem; m += 3) {
            x_test(&g_c, w, R(RCX), g_mem[m]);
            item("test%c %s, %s", sfx(w), T(R(RCX), w), T(g_mem[m], w));
        }
    }
    for (int m = 0; m < g_nmem; m++) {
        Reg r = (Reg)((m * 5) % 16);
        x_lea(&g_c, g_mem[m], R(r));
        item("leaq %s, %s", T(g_mem[m], 8), T(R(r), 8));
    }
}

static void shifts_unary(void) {
    static const char *const sh[] = { [SH_SHL] = "shl", [SH_SHR] = "shr", [SH_SAR] = "sar" };
    static const ShiftOp ops[] = { SH_SHL, SH_SHR, SH_SAR };
    static const int ws[] = { 1, 4, 8 };
    static const int counts[] = { 1, 2, 7, 31, 63 };
    for (int o = 0; o < 3; o++)
        for (int wi = 0; wi < 3; wi++) {
            int w = ws[wi];
            for (int r = 0; r < 16; r += 3) {
                x_shift(&g_c, ops[o], w, R(RCX), R((Reg)r));
                item("%s%c %%cl, %s", sh[ops[o]], sfx(w), T(R((Reg)r), w));
                for (int k = 0; k < 5; k++) {
                    if (counts[k] >= w * 8) continue;
                    x_shift(&g_c, ops[o], w, IMM(counts[k]), R((Reg)r));
                    item("%s%c $%d, %s", sh[ops[o]], sfx(w), counts[k], T(R((Reg)r), w));
                }
            }
            x_shift(&g_c, ops[o], w, IMM(3), g_mem[2]);
            item("%s%c $3, %s", sh[ops[o]], sfx(w), T(g_mem[2], w));
        }
    static const char *const un[] = { [UN_NOT] = "not", [UN_NEG] = "neg", [UN_MUL] = "mul",
                                      [UN_DIV] = "div", [UN_IDIV] = "idiv", [UN_INC] = "inc",
                                      [UN_DEC] = "dec" };
    static const UnOp uops[] = { UN_NOT, UN_NEG, UN_MUL, UN_DIV, UN_IDIV, UN_INC, UN_DEC };
    for (int o = 0; o < 7; o++)
        for (int wi = 0; wi < 3; wi++) {
            int w = ws[wi];
            for (int r = 0; r < 16; r += 3) {
                x_unary(&g_c, uops[o], w, R((Reg)r));
                item("%s%c %s", un[uops[o]], sfx(w), T(R((Reg)r), w));
            }
            for (int m = 0; m < g_nmem; m += 4) {
                x_unary(&g_c, uops[o], w, g_mem[m]);
                item("%s%c %s", un[uops[o]], sfx(w), T(g_mem[m], w));
            }
        }
}

static void mul_ext(void) {
    for (int a = 0; a < 16; a += 3)
        for (int b = 0; b < 16; b += 2) {
            x_imul(&g_c, 8, R((Reg)a), R((Reg)b));
            item("imulq %s, %s", T(R((Reg)a), 8), T(R((Reg)b), 8));
        }
    for (int m = 0; m < g_nmem; m++) {
        Reg r = (Reg)((m * 3) % 16);
        x_imul(&g_c, 8, g_mem[m], R(r));
        item("imulq %s, %s", T(g_mem[m], 8), T(R(r), 8));
        x_imul3(&g_c, 8, IMMS[m % NIMMS], g_mem[m], R(r));
        item("imulq $%lld, %s, %s", (long long)IMMS[m % NIMMS], T(g_mem[m], 8), T(R(r), 8));
    }
    for (int i = 0; i < NIMMS; i++)
        for (int r = 0; r < 16; r += 5) {
            x_imul3(&g_c, 8, IMMS[i], R((Reg)r), R((Reg)((r + 1) % 16)));
            item("imulq $%lld, %s, %s", (long long)IMMS[i], T(R((Reg)r), 8), T(R((Reg)((r + 1) % 16)), 8));
        }
    static const struct { int sw, dw; bool sign; const char *name; } ext[] = {
        { 1, 4, false, "movzbl" }, { 2, 4, false, "movzwl" }, { 1, 8, true, "movsbq" },
        { 2, 8, true, "movswq" }, { 4, 8, true, "movslq" },
    };
    for (int k = 0; k < 5; k++) {
        for (int a = 0; a < 16; a++) {
            Reg b = (Reg)((a * 5 + 3) % 16);
            x_movx(&g_c, ext[k].sw, ext[k].dw, ext[k].sign, R((Reg)a), R(b));
            item("%s %s, %s", ext[k].name, T(R((Reg)a), ext[k].sw), T(R(b), ext[k].dw));
        }
        for (int m = 0; m < g_nmem; m++) {
            Reg r = (Reg)((m * 7 + 1) % 16);
            x_movx(&g_c, ext[k].sw, ext[k].dw, ext[k].sign, g_mem[m], R(r));
            item("%s %s, %s", ext[k].name, T(g_mem[m], ext[k].sw), T(R(r), ext[k].dw));
        }
    }
    static const char *const cc[] = { "o", "no", "b", "ae", "e", "ne", "be", "a",
                                      "s", "ns", "p", "np", "l", "ge", "le", "g" };
    for (int k = 0; k < 16; k++) {
        for (int r = 0; r < 16; r++) {
            x_setcc(&g_c, (Cond)k, R((Reg)r));
            item("set%s %s", cc[k], T(R((Reg)r), 1));
        }
        x_cmov(&g_c, (Cond)k, 8, R(RCX), R(RAX));
        item("cmov%sq %%rcx, %%rax", cc[k]);
        x_cmov(&g_c, (Cond)k, 8, g_mem[k % g_nmem], R(R9));
        item("cmov%sq %s, %%r9", cc[k], T(g_mem[k % g_nmem], 8));
    }
    for (int r = 0; r < 16; r += 5) {
        x_btc(&g_c, 8, 63, R((Reg)r));
        item("btcq $63, %s", T(R((Reg)r), 8));
    }
}

static void sse(void) {
    static const char *const names[] = {
        [SSE_MOVSS] = "movss", [SSE_MOVSD] = "movsd", [SSE_MOVAPS] = "movaps", [SSE_XORPS] = "xorps",
        [SSE_ADDSS] = "addss", [SSE_ADDSD] = "addsd", [SSE_SUBSS] = "subss", [SSE_SUBSD] = "subsd",
        [SSE_MULSS] = "mulss", [SSE_MULSD] = "mulsd", [SSE_DIVSS] = "divss", [SSE_DIVSD] = "divsd",
        [SSE_UCOMISS] = "ucomiss", [SSE_UCOMISD] = "ucomisd", [SSE_CVTSS2SD] = "cvtss2sd",
        [SSE_CVTSD2SS] = "cvtsd2ss",
    };
    for (int op = SSE_MOVSS; op <= SSE_CVTSD2SS; op++) {
        for (int a = 0; a < 16; a += 3)
            for (int b = 0; b < 16; b += 4) {
                x_sse(&g_c, (SseOp)op, X(a), X(b));
                item("%s %%xmm%d, %%xmm%d", names[op], a, b);
            }
        for (int m = 0; m < g_nmem; m += 2) {
            x_sse(&g_c, (SseOp)op, g_mem[m], X(m % 16));
            item("%s %s, %%xmm%d", names[op], T(g_mem[m], 8), m % 16);
        }
    }
    for (int m = 0; m < g_nmem; m += 2) {
        x_sse(&g_c, SSE_MOVSS, X(m % 16), g_mem[m]);
        item("movss %%xmm%d, %s", m % 16, T(g_mem[m], 4));
        x_sse(&g_c, SSE_MOVSD, X(m % 16), g_mem[m]);
        item("movsd %%xmm%d, %s", m % 16, T(g_mem[m], 8));
    }
    for (int r = 0; r < 16; r += 3)
        for (int x = 0; x < 16; x += 5) {
            x_sse(&g_c, SSE_CVTTSD2SIQ, X(x), R((Reg)r));
            item("cvttsd2si %%xmm%d, %s", x, T(R((Reg)r), 8));
            x_sse(&g_c, SSE_CVTSI2SDQ, R((Reg)r), X(x));
            item("cvtsi2sdq %s, %%xmm%d", T(R((Reg)r), 8), x);
            x_sse(&g_c, SSE_CVTSI2SSQ, R((Reg)r), X(x));
            item("cvtsi2ssq %s, %%xmm%d", T(R((Reg)r), 8), x);
            /* movq and movd between the register files */
            x_mov(&g_c, 8, R((Reg)r), X(x));
            item("movq %s, %%xmm%d", T(R((Reg)r), 8), x);
            x_mov(&g_c, 8, X(x), R((Reg)r));
            item("movq %%xmm%d, %s", x, T(R((Reg)r), 8));
            x_mov(&g_c, 4, R((Reg)r), X(x));
            item("movd %s, %%xmm%d", T(R((Reg)r), 4), x);
            x_mov(&g_c, 4, X(x), R((Reg)r));
            item("movd %%xmm%d, %s", x, T(R((Reg)r), 4));
            x_mov(&g_c, 8, X(x), X(r));
            item("movq %%xmm%d, %%xmm%d", x, r);
        }
    for (int m = 0; m < g_nmem; m += 2) {
        x_mov(&g_c, 8, X(m % 16), g_mem[m]);
        item("movq %%xmm%d, %s", m % 16, T(g_mem[m], 8));
        x_mov(&g_c, 8, g_mem[m], X(m % 16));
        item("movq %s, %%xmm%d", T(g_mem[m], 8), m % 16);
        x_mov(&g_c, 4, g_mem[m], X(m % 16));
        item("movd %s, %%xmm%d", T(g_mem[m], 4), m % 16);
    }
}

static void misc(void) {
    static const struct { Op0 op; const char *text; } ops[] = {
        { OP_CQO, "cqto" }, { OP_LEAVE, "leave" }, { OP_RET, "ret" }, { OP_MFENCE, "mfence" },
        { OP_REP_MOVSB, "rep movsb" }, { OP_REP_STOSB, "rep stosb" }, { OP_UD2, "ud2" },
        { OP_PUSH_RBP, "pushq %rbp" },
    };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
        x_op0(&g_c, ops[i].op);
        item("%s", ops[i].text);
    }
    for (int r = 0; r < 16; r += 3) {
        x_call_ind(&g_c, R((Reg)r));
        item("call *%s", T(R((Reg)r), 8));
        x_jmp_ind(&g_c, R((Reg)r));
        item("jmp *%s", T(R((Reg)r), 8));
    }
    for (int m = 0; m < g_nmem; m += 3) {
        x_call_ind(&g_c, g_mem[m]);
        item("call *%s", T(g_mem[m], 8));
    }
}

static unsigned char *slurp(const char *path, long *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc((size_t)*n + 1);
    if (fread(p, 1, (size_t)*n, f) != (size_t)*n) { free(p); p = NULL; }
    fclose(f);
    return p;
}

int main(void) {
    mems();
    alu();
    mov();
    test_lea();
    shifts_unary();
    mul_ext();
    sse();
    misc();

    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/x86asm_test.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char s_path[600], o_path[600], b_path[600], cmd[4096];
    snprintf(s_path, sizeof s_path, "%s/ref.s", dir);
    snprintf(o_path, sizeof o_path, "%s/ref.o", dir);
    snprintf(b_path, sizeof b_path, "%s/ref.bin", dir);
    FILE *f = fopen(s_path, "w");
    if (!f) { perror(s_path); return 1; }
    fprintf(f, "\t.text\n");
    for (int i = 0; i < g_n; i++) fprintf(f, "\t%s\n\t.balign %d, 0xcc\n", g_items[i].text, SLOT);
    fclose(f);
    snprintf(cmd, sizeof cmd, "as --64 -o '%s' '%s' && objcopy -O binary -j .text '%s' '%s'",
             o_path, s_path, o_path, b_path);
    int failed = g_bad;
    long n = 0;
    unsigned char *ref = system(cmd) == 0 ? slurp(b_path, &n) : NULL;
    if (!ref) {
        fprintf(stderr, "x86asm_test: as or objcopy failed on %s\n", s_path);
        return 1;
    }
    if (n != (long)g_n * SLOT) {
        fprintf(stderr, "x86asm_test: as wrote %ld bytes for %d instructions\n", n, g_n);
        failed++;
    }
    for (int i = 0; i < g_n && (long)(i + 1) * SLOT <= n; i++) {
        if (memcmp(ref + (long)i * SLOT, g_items[i].bytes, SLOT) == 0) continue;
        if (++failed > 20) continue;
        const unsigned char *r = ref + (long)i * SLOT;
        int rlen = SLOT;
        while (rlen > 0 && r[rlen - 1] == 0xcc) rlen--;     /* the padding */
        fprintf(stderr, "  FAIL  %s\n    as:  ", g_items[i].text);
        for (int k = 0; k < rlen; k++) fprintf(stderr, "%02x ", r[k]);
        fprintf(stderr, "\n    fcc: ");
        for (int k = 0; k < g_items[i].len; k++) fprintf(stderr, "%02x ", g_items[i].bytes[k]);
        fprintf(stderr, "\n");
    }
    free(ref);
    if (failed) {
        fprintf(stderr, "x86asm_test: %d of %d instructions differ from as (kept %s)\n", failed, g_n, dir);
        return 1;
    }
    unlink(s_path);
    unlink(o_path);
    unlink(b_path);
    rmdir(dir);
    printf("x86asm: %d instructions encoded as GNU as encodes them\n", g_n);
    for (int i = 0; i < g_n; i++) free(g_items[i].text);
    free(g_items);
    code_free(&g_c);
    return 0;
}
