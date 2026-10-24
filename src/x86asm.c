/* x86asm.c: the native backend's assembler, without assembly text.
 *
 * Instructions are encoded into code buffers as they are generated. What
 * depends on the final layout is recorded as an event at its byte position:
 * a label or function start, an alignment, a branch (which takes no bytes
 * until its form is chosen), a 32-bit reference to a symbol, a source line,
 * an unwind point. asm_finish relaxes the branches the way GNU as does (all
 * short at first, lengthened until every one reaches), copies the bytes out
 * with the branches and padding in place, resolves references inside .text
 * and turns the rest into relocations. */

#include "x86asm.h"
#include "common.h"
#include <string.h>

typedef enum { EV_LABEL, EV_FUNC, EV_FUNC_END, EV_ALIGN, EV_JMP, EV_JCC, EV_REF, EV_LOC, EV_CFI } EvKind;

/* How a 32-bit reference resolves. */
typedef enum {
    RT_PC,          /* sym(%rip): in .text, patched; elsewhere R_X86_64_PC32 */
    RT_BRANCH,      /* call sym: in .text, patched; elsewhere R_X86_64_PLT32 */
    RT_PLT,         /* sym@PLT: always R_X86_64_PLT32 */
    RT_GOT,         /* sym@GOTPCREL(%rip) in a movq: R_X86_64_REX_GOTPCRELX */
} RefType;

struct AsmEv {
    uint32_t pos;           /* byte position in the buffer */
    uint8_t kind;
    uint8_t cc;             /* EV_JCC: the condition */
    uint8_t size;           /* branches: the chosen length; EV_REF: bytes after the field */
    uint8_t rtype;          /* EV_REF */
    int32_t sym;            /* the symbol defined or referenced; EV_ALIGN: the alignment;
                             * EV_CFI: the op; EV_LOC: the file */
    int32_t line;           /* EV_LOC; EV_CFI saved: the register */
    int64_t addend;         /* EV_REF; EV_CFI saved: the offset from the CFA */
    uint32_t addr;          /* the address in .text, once laid out */
};

typedef struct {
    char *name;             /* NULL: a label */
    int sec;                /* the defining section, or 0 */
    int64_t off, size;
    bool func, global, used;
    int objsym;
} AsmSym;

static Obj *g_obj;
static AsmSym *g_syms;
static int g_nsym, g_capsym;

/* Named symbols by name: open addressing over symbol ids. */
static int *g_names;
static size_t g_names_cap, g_names_n;

static char **g_files;
static int g_nfiles;

/* Data references, resolved once .text is laid out. */
typedef struct { int sec; int64_t off; int sym; int64_t addend; } DataRef;
static DataRef *g_drefs;
static int g_ndref, g_capdref;

/* ================================================================
 * Symbols
 * ================================================================ */

static uint64_t name_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static int new_sym(const char *name) {
    AsmSym s = { 0 };
    s.name = name ? str_dup(name) : NULL;
    if (g_nsym == 0) DA_APPEND(g_syms, g_nsym, g_capsym, s);     /* id 0: none */
    DA_APPEND(g_syms, g_nsym, g_capsym, s);
    return g_nsym - 1;
}

int asm_label(void) { return new_sym(NULL); }

int asm_sym(const char *name) {
    if (g_names_n * 2 + 2 > g_names_cap) {
        size_t nc = g_names_cap ? g_names_cap * 2 : 1024;
        int *nt = xcalloc(nc, sizeof(int));
        for (size_t i = 0; i < g_names_cap; i++) {
            if (!g_names[i]) continue;
            size_t j = (size_t)name_hash(g_syms[g_names[i]].name) & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = g_names[i];
        }
        free(g_names);
        g_names = nt;
        g_names_cap = nc;
    }
    size_t j = (size_t)name_hash(name) & (g_names_cap - 1);
    while (g_names[j]) {
        if (strcmp(g_syms[g_names[j]].name, name) == 0) return g_names[j];
        j = (j + 1) & (g_names_cap - 1);
    }
    int id = new_sym(name);
    g_names[j] = id;
    g_names_n++;
    return id;
}

void asm_sym_global(int sym) { g_syms[sym].global = true; }

void asm_file(int file, const char *path) {
    if (file >= g_nfiles) {
        g_files = xrealloc(g_files, sizeof(char *) * (size_t)(file + 1));
        for (int i = g_nfiles; i <= file; i++) g_files[i] = NULL;
        g_nfiles = file + 1;
    }
    free(g_files[file]);
    g_files[file] = str_dup(path);
}

/* ================================================================
 * Code buffers
 * ================================================================ */

void code_reset(Code *c) { c->len = 0; c->nev = 0; }

void code_free(Code *c) {
    free(c->p);
    free(c->ev);
    memset(c, 0, sizeof *c);
}

static void cput(Code *c, const void *p, uint32_t n) {
    if (c->len + n > c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 65536;
        while (nc < c->len + n) nc *= 2;
        c->p = xrealloc(c->p, nc);
        c->cap = nc;
    }
    memcpy(c->p + c->len, p, n);
    c->len += n;
}

static void c1(Code *c, unsigned b) { unsigned char x = (unsigned char)b; cput(c, &x, 1); }

static void cle(Code *c, uint64_t v, int w) {
    unsigned char t[8];
    for (int i = 0; i < w; i++) t[i] = (unsigned char)(v >> (8 * i));
    cput(c, t, (uint32_t)w);
}

static AsmEv *cev(Code *c, EvKind kind) {
    AsmEv e = { 0 };
    e.pos = c->len;
    e.kind = (uint8_t)kind;
    DA_APPEND(c->ev, c->nev, c->capev, e);
    return &c->ev[c->nev - 1];
}

void code_append(Code *dst, const Code *src) {
    uint32_t base = dst->len;
    if (src->len) cput(dst, src->p, src->len);
    for (int i = 0; i < src->nev; i++) {
        AsmEv e = src->ev[i];
        e.pos += base;
        DA_APPEND(dst->ev, dst->nev, dst->capev, e);
    }
}

/* A 32-bit field referring to sym, at the current position. `tail`: the
 * instruction bytes that follow the field. */
static void ref32(Code *c, RefType rt, int sym, int64_t addend, int tail) {
    AsmEv *e = cev(c, EV_REF);
    e->rtype = (uint8_t)rt;
    e->sym = sym;
    e->addend = addend;
    e->size = (uint8_t)tail;
    g_syms[sym].used = true;
    cle(c, 0, 4);
}

/* ================================================================
 * Encoding
 * ================================================================ */

static void bad(const char *what) {
    fprintf(stderr, "fcc: internal error: native backend: no encoding for %s\n", what);
    abort();
}

static bool fits8(int64_t v) { return v >= -128 && v <= 127; }
static bool fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* The REX prefix, if one is needed: W for 64-bit operands, R/X/B for the
 * high registers, and a bare REX for %spl..%dil in byte operations. */
static void rex(Code *c, bool w, int reg, Opnd rm, bool byte_regs, bool need) {
    int r = 0x40 | (w ? 8 : 0) | (reg >= 8 ? 4 : 0);
    if (rm.kind == OPND_MEM) {
        if (rm.index >= 8) r |= 2;
        if (rm.base >= 8 && rm.base != RIP) r |= 1;
    } else if (rm.kind == OPND_REG || rm.kind == OPND_XMM) {
        if (rm.base >= 8) r |= 1;
        if (byte_regs && rm.kind == OPND_REG && rm.base >= 4 && rm.base < 8) need = true;
    }
    if (r != 0x40 || need) c1(c, (unsigned)r);
}

/* ModRM, SIB and displacement for reg-field `reg` and operand rm. */
static void modrm(Code *c, int reg, Opnd rm, int tail) {
    reg &= 7;
    if (rm.kind == OPND_REG || rm.kind == OPND_XMM) {
        c1(c, 0xC0u | (unsigned)reg << 3 | (unsigned)(rm.base & 7));
        return;
    }
    if (rm.base == RIP) {
        c1(c, 0x05u | (unsigned)reg << 3);
        ref32(c, rm.got ? RT_GOT : RT_PC, rm.sym, rm.disp, tail);
        return;
    }
    if (!fits32(rm.disp)) bad("a displacement beyond 32 bits");
    int b = rm.base & 7;
    bool sib = rm.index >= 0 || b == 4;
    int mod = rm.disp == 0 && b != 5 ? 0 : fits8(rm.disp) ? 1 : 2;
    c1(c, (unsigned)mod << 6 | (unsigned)reg << 3 | (unsigned)(sib ? 4 : b));
    if (sib) {
        int ss = rm.scale == 8 ? 3 : rm.scale == 4 ? 2 : rm.scale == 2 ? 1 : 0;
        int ix = rm.index >= 0 ? rm.index & 7 : 4;
        c1(c, (unsigned)ss << 6 | (unsigned)ix << 3 | (unsigned)b);
    }
    if (mod == 1) c1(c, (unsigned)(rm.disp & 0xff));
    else if (mod == 2) cle(c, (uint64_t)rm.disp, 4);
}

/* [prefix] [REX] opcode modrm: the common shape. `reg` is a register or an
 * opcode extension; `reg_byte`: it is a byte register. */
static void op_rm(Code *c, int prefix, bool w, const char *op, int oplen, int reg, bool reg_byte,
                  Opnd rm, bool rm_byte, int tail) {
    if (prefix) c1(c, (unsigned)prefix);
    rex(c, w, reg, rm, rm_byte, reg_byte && reg >= 4 && reg < 8);
    cput(c, op, (uint32_t)oplen);
    modrm(c, reg, rm, tail);
}

static int pfx16(int w) { return w == 2 ? 0x66 : 0; }

void x_mov(Code *c, int w, Opnd src, Opnd dst) {
    if (src.kind == OPND_XMM || dst.kind == OPND_XMM) {
        bool q = w == 8;
        if (src.kind == OPND_XMM && dst.kind == OPND_MEM && q)
            op_rm(c, 0x66, false, "\x0f\xd6", 2, src.base, false, dst, false, 0);
        else if (dst.kind == OPND_XMM && src.kind == OPND_MEM && q)
            op_rm(c, 0xF3, false, "\x0f\x7e", 2, dst.base, false, src, false, 0);
        else if (dst.kind == OPND_XMM && src.kind == OPND_XMM)
            op_rm(c, 0xF3, false, "\x0f\x7e", 2, dst.base, false, src, false, 0);
        else if (dst.kind == OPND_XMM)      /* from a register or (movd) memory */
            op_rm(c, 0x66, q, "\x0f\x6e", 2, dst.base, false, src, false, 0);
        else
            op_rm(c, 0x66, q, "\x0f\x7e", 2, src.base, false, dst, false, 0);
        return;
    }
    if (src.kind == OPND_IMM) {
        if (dst.kind == OPND_REG) {
            if (w == 8) {
                if (!fits32(src.disp)) { x_movabs(c, src.disp, dst); return; }
                op_rm(c, 0, true, "\xc7", 1, 0, false, dst, false, 0);
                cle(c, (uint64_t)src.disp, 4);
                return;
            }
            if (w == 2) c1(c, 0x66);
            if (dst.base >= 8) c1(c, 0x41);
            else if (w == 1 && dst.base >= 4) c1(c, 0x40);
            c1(c, (w == 1 ? 0xB0u : 0xB8u) + (unsigned)(dst.base & 7));
            cle(c, (uint64_t)src.disp, w == 8 ? 4 : w);
            return;
        }
        int isz = w == 8 ? 4 : w;
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xc6" : "\xc7", 1, 0, false, dst, false, isz);
        cle(c, (uint64_t)src.disp, isz);
        return;
    }
    if (src.kind == OPND_REG && (dst.kind == OPND_REG || dst.kind == OPND_MEM)) {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\x88" : "\x89", 1, src.base, w == 1, dst, w == 1, 0);
        return;
    }
    if (src.kind == OPND_MEM && dst.kind == OPND_REG) {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\x8a" : "\x8b", 1, dst.base, w == 1, src, false, 0);
        return;
    }
    bad("mov");
}

void x_movabs(Code *c, int64_t imm, Opnd dst) {
    if (dst.kind != OPND_REG) bad("movabs");
    c1(c, dst.base >= 8 ? 0x49u : 0x48u);
    c1(c, 0xB8u + (unsigned)(dst.base & 7));
    cle(c, (uint64_t)imm, 8);
}

void x_alu(Code *c, AluOp op, int w, Opnd src, Opnd dst) {
    if (src.kind == OPND_IMM) {
        int64_t v = src.disp;
        if (w == 1) {
            if (dst.kind == OPND_REG && dst.base == RAX) { c1(c, (unsigned)op * 8 + 4); c1(c, (unsigned)(v & 0xff)); return; }
            op_rm(c, 0, false, "\x80", 1, (int)op, false, dst, true, 1);
            c1(c, (unsigned)(v & 0xff));
            return;
        }
        if (fits8(v)) {
            op_rm(c, pfx16(w), w == 8, "\x83", 1, (int)op, false, dst, false, 1);
            c1(c, (unsigned)(v & 0xff));
            return;
        }
        int isz = w == 2 ? 2 : 4;
        if (dst.kind == OPND_REG && dst.base == RAX) {
            if (w == 2) c1(c, 0x66);
            if (w == 8) c1(c, 0x48);
            c1(c, (unsigned)op * 8 + 5);
        } else {
            op_rm(c, pfx16(w), w == 8, "\x81", 1, (int)op, false, dst, false, isz);
        }
        cle(c, (uint64_t)v, isz);
        return;
    }
    char opc = (char)(op * 8 + (w == 1 ? 0 : 1));
    if (src.kind == OPND_REG && (dst.kind == OPND_REG || dst.kind == OPND_MEM)) {
        op_rm(c, pfx16(w), w == 8, &opc, 1, src.base, w == 1, dst, w == 1, 0);
        return;
    }
    if (src.kind == OPND_MEM && dst.kind == OPND_REG) {
        opc = (char)(opc + 2);
        op_rm(c, pfx16(w), w == 8, &opc, 1, dst.base, w == 1, src, false, 0);
        return;
    }
    bad("alu");
}

void x_test(Code *c, int w, Opnd src, Opnd dst) {
    if (src.kind == OPND_IMM) {
        int isz = w == 8 ? 4 : w;
        if (dst.kind == OPND_REG && dst.base == RAX) {
            if (w == 2) c1(c, 0x66);
            if (w == 8) c1(c, 0x48);
            c1(c, w == 1 ? 0xA8u : 0xA9u);
        } else {
            op_rm(c, pfx16(w), w == 8, w == 1 ? "\xf6" : "\xf7", 1, 0, false, dst, w == 1, isz);
        }
        cle(c, (uint64_t)src.disp, isz);
        return;
    }
    if (src.kind == OPND_REG) {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\x84" : "\x85", 1, src.base, w == 1, dst, w == 1, 0);
        return;
    }
    bad("test");
}

void x_lea(Code *c, Opnd mem, Opnd dst) {
    if (mem.kind != OPND_MEM || dst.kind != OPND_REG) bad("lea");
    op_rm(c, 0, true, "\x8d", 1, dst.base, false, mem, false, 0);
}

void x_shift(Code *c, ShiftOp op, int w, Opnd count, Opnd dst) {
    if (count.kind == OPND_REG) {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xd2" : "\xd3", 1, (int)op, false, dst, w == 1, 0);
    } else if (count.disp == 1) {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xd0" : "\xd1", 1, (int)op, false, dst, w == 1, 0);
    } else {
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xc0" : "\xc1", 1, (int)op, false, dst, w == 1, 1);
        c1(c, (unsigned)(count.disp & 0xff));
    }
}

void x_unary(Code *c, UnOp op, int w, Opnd dst) {
    if (op == UN_INC || op == UN_DEC)
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xfe" : "\xff", 1, op == UN_INC ? 0 : 1, false, dst, w == 1, 0);
    else
        op_rm(c, pfx16(w), w == 8, w == 1 ? "\xf6" : "\xf7", 1, (int)op, false, dst, w == 1, 0);
}

void x_imul(Code *c, int w, Opnd src, Opnd dst) {
    op_rm(c, pfx16(w), w == 8, "\x0f\xaf", 2, dst.base, false, src, false, 0);
}

void x_imul3(Code *c, int w, int64_t imm, Opnd src, Opnd dst) {
    if (fits8(imm)) {
        op_rm(c, pfx16(w), w == 8, "\x6b", 1, dst.base, false, src, false, 1);
        c1(c, (unsigned)(imm & 0xff));
    } else {
        int isz = w == 2 ? 2 : 4;
        op_rm(c, pfx16(w), w == 8, "\x69", 1, dst.base, false, src, false, isz);
        cle(c, (uint64_t)imm, isz);
    }
}

void x_movx(Code *c, int sw, int dw, bool sign, Opnd src, Opnd dst) {
    if (sw == 4) {
        if (!sign || dw != 8) bad("movx");
        op_rm(c, 0, true, "\x63", 1, dst.base, false, src, false, 0);
        return;
    }
    const char *op = sw == 1 ? (sign ? "\x0f\xbe" : "\x0f\xb6") : (sign ? "\x0f\xbf" : "\x0f\xb7");
    op_rm(c, dw == 2 ? 0x66 : 0, dw == 8, op, 2, dst.base, false, src, sw == 1, 0);
}

void x_setcc(Code *c, Cond cc, Opnd dst) {
    char op[2] = { 0x0f, (char)(0x90 + cc) };
    op_rm(c, 0, false, op, 2, 0, false, dst, true, 0);
}

void x_cmov(Code *c, Cond cc, int w, Opnd src, Opnd dst) {
    char op[2] = { 0x0f, (char)(0x40 + cc) };
    op_rm(c, pfx16(w), w == 8, op, 2, dst.base, false, src, false, 0);
}

void x_btc(Code *c, int w, int bit, Opnd dst) {
    op_rm(c, pfx16(w), w == 8, "\x0f\xba", 2, 7, false, dst, false, 1);
    c1(c, (unsigned)bit);
}

void x_sse(Code *c, SseOp op, Opnd src, Opnd dst) {
    static const struct { uint8_t pfx, opc; } T[] = {
        [SSE_MOVSS] = { 0xF3, 0x10 }, [SSE_MOVSD] = { 0xF2, 0x10 },
        [SSE_MOVAPS] = { 0, 0x28 }, [SSE_XORPS] = { 0, 0x57 },
        [SSE_ADDSS] = { 0xF3, 0x58 }, [SSE_ADDSD] = { 0xF2, 0x58 },
        [SSE_SUBSS] = { 0xF3, 0x5C }, [SSE_SUBSD] = { 0xF2, 0x5C },
        [SSE_MULSS] = { 0xF3, 0x59 }, [SSE_MULSD] = { 0xF2, 0x59 },
        [SSE_DIVSS] = { 0xF3, 0x5E }, [SSE_DIVSD] = { 0xF2, 0x5E },
        [SSE_UCOMISS] = { 0, 0x2E }, [SSE_UCOMISD] = { 0x66, 0x2E },
        [SSE_CVTSS2SD] = { 0xF3, 0x5A }, [SSE_CVTSD2SS] = { 0xF2, 0x5A },
        [SSE_CVTTSD2SIQ] = { 0xF2, 0x2C }, [SSE_CVTSI2SDQ] = { 0xF2, 0x2A },
        [SSE_CVTSI2SSQ] = { 0xF3, 0x2A },
    };
    char opc[2] = { 0x0f, (char)T[op].opc };
    bool w = op == SSE_CVTTSD2SIQ || op == SSE_CVTSI2SDQ || op == SSE_CVTSI2SSQ;
    if ((op == SSE_MOVSS || op == SSE_MOVSD) && dst.kind == OPND_MEM) {
        opc[1] = 0x11;
        op_rm(c, T[op].pfx, false, opc, 2, src.base, false, dst, false, 0);
        return;
    }
    if (dst.kind != OPND_XMM && dst.kind != OPND_REG) bad("sse");
    op_rm(c, T[op].pfx, w, opc, 2, dst.base, false, src, false, 0);
}

void x_op0(Code *c, Op0 op) {
    switch (op) {
    case OP_CQO: c1(c, 0x48); c1(c, 0x99); return;
    case OP_LEAVE: c1(c, 0xC9); return;
    case OP_RET: c1(c, 0xC3); return;
    case OP_MFENCE: c1(c, 0x0F); c1(c, 0xAE); c1(c, 0xF0); return;
    case OP_REP_MOVSB: c1(c, 0xF3); c1(c, 0xA4); return;
    case OP_REP_STOSB: c1(c, 0xF3); c1(c, 0xAA); return;
    case OP_UD2: c1(c, 0x0F); c1(c, 0x0B); return;
    case OP_PUSH_RBP: c1(c, 0x55); return;
    }
}

void x_jcc(Code *c, Cond cc, int label) {
    AsmEv *e = cev(c, EV_JCC);
    e->cc = (uint8_t)cc;
    e->sym = label;
}

void x_jmp(Code *c, int target) {
    cev(c, EV_JMP)->sym = target;
    g_syms[target].used = true;
}

bool x_drop_final_jmp(Code *c, int label) {
    if (c->nev == 0) return false;
    AsmEv *e = &c->ev[c->nev - 1];
    if (e->kind != EV_JMP || e->sym != label || e->pos != c->len) return false;
    c->nev--;
    return true;
}

void x_jmp_plt(Code *c, int sym) { c1(c, 0xE9); ref32(c, RT_PLT, sym, 0, 0); }

void x_call(Code *c, int sym, bool plt) { c1(c, 0xE8); ref32(c, plt ? RT_PLT : RT_BRANCH, sym, 0, 0); }

void x_call_ind(Code *c, Opnd target) { op_rm(c, 0, false, "\xff", 1, 2, false, target, false, 0); }

void x_jmp_ind(Code *c, Opnd target) { op_rm(c, 0, false, "\xff", 1, 4, false, target, false, 0); }

void x_label(Code *c, int label) { cev(c, EV_LABEL)->sym = label; }

void x_align(Code *c, int align) { cev(c, EV_ALIGN)->sym = align; }

void x_func(Code *c, int sym) {
    g_syms[sym].func = true;
    cev(c, EV_FUNC)->sym = sym;
}

void x_func_end(Code *c, int sym) { cev(c, EV_FUNC_END)->sym = sym; }

void x_loc(Code *c, int file, int line) {
    AsmEv *e = cev(c, EV_LOC);
    e->sym = file;
    e->line = line;
}

void x_cfi(Code *c, CfiOp op) { cev(c, EV_CFI)->sym = (int32_t)op; }

void x_cfi_saved(Code *c, Reg r, int32_t cfa_off) {
    AsmEv *e = cev(c, EV_CFI);
    e->sym = CFI_SAVED;
    e->line = (int32_t)r;
    e->addend = cfa_off;
}

/* ================================================================
 * Data
 * ================================================================ */

void d_align(int sec, int64_t align) { obj_align(g_obj, sec, (uint64_t)align); }

void d_def(int sec, int sym) {
    g_syms[sym].sec = sec;
    g_syms[sym].off = (int64_t)g_obj->secs[sec].size;
}

void d_bytes(int sec, const void *p, int64_t n) { obj_put(g_obj, sec, p, (uint64_t)n); }

void d_zero(int sec, int64_t n) { obj_put(g_obj, sec, NULL, (uint64_t)n); }

void d_int(int sec, uint64_t v, int w) {
    unsigned char t[8];
    for (int i = 0; i < w; i++) t[i] = (unsigned char)(v >> (8 * i));
    obj_put(g_obj, sec, t, (uint64_t)w);
}

void d_addr_at(int sec, int64_t off, int sym, int64_t addend) {
    DataRef r = { sec, off, sym, addend };
    g_syms[sym].used = true;
    DA_APPEND(g_drefs, g_ndref, g_capdref, r);
}

void d_addr(int sec, int sym, int64_t addend) {
    d_addr_at(sec, (int64_t)obj_put(g_obj, sec, NULL, 8), sym, addend);
}

int64_t d_size(int sec) { return (int64_t)g_obj->secs[sec].size; }

/* ================================================================
 * Layout
 * ================================================================ */

static const unsigned char NOPS[11][11] = {
    { 0x90 },
    { 0x66, 0x90 },
    { 0x0f, 0x1f, 0x00 },
    { 0x0f, 0x1f, 0x40, 0x00 },
    { 0x0f, 0x1f, 0x44, 0x00, 0x00 },
    { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
    { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 },
    { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static bool in_text(int sym) { return sym > 0 && g_syms[sym].sec == ASM_TEXT; }

/* The Obj symbol a relocation against `sym` uses, adjusting the addend: a
 * local definition goes through its section's symbol, as `as` does. */
static int reloc_target(int sym, int64_t *addend) {
    AsmSym *s = &g_syms[sym];
    if (!s->sec && !s->name) {
        fprintf(stderr, "fcc: internal error: native backend: a reference to a label never placed\n");
        abort();
    }
    if (s->sec && !s->global) {
        *addend += s->off;
        return obj_section_symbol(g_obj, s->sec);
    }
    return s->objsym;
}

/* The named symbol a call or GOT reference goes through. */
static int reloc_named(int sym) {
    if (!g_syms[sym].objsym) {
        fprintf(stderr, "fcc: internal error: native backend: a call through a label\n");
        abort();
    }
    return g_syms[sym].objsym;
}

static void put_le(unsigned char *p, uint64_t v, int w) {
    for (int i = 0; i < w; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Lay out the text, choosing the branch forms; fill in every event's address. */
static void layout(Code *t) {
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind == EV_LABEL || e->kind == EV_FUNC) g_syms[e->sym].sec = ASM_TEXT;
    }
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind == EV_JMP) e->size = in_text(e->sym) ? 2 : 5;
        else if (e->kind == EV_JCC) e->size = 2;
    }
    for (;;) {
        int64_t delta = 0;
        for (int i = 0; i < t->nev; i++) {
            AsmEv *e = &t->ev[i];
            e->addr = (uint32_t)(e->pos + delta);
            switch (e->kind) {
            case EV_LABEL: case EV_FUNC: g_syms[e->sym].off = e->addr; break;
            case EV_ALIGN: {
                uint32_t a = (uint32_t)e->sym;
                e->size = (uint8_t)((a - e->addr % a) % a);
                delta += e->size;
                break;
            }
            case EV_JMP: case EV_JCC: delta += e->size; break;
            default: break;
            }
        }
        bool grew = false;
        for (int i = 0; i < t->nev; i++) {
            AsmEv *e = &t->ev[i];
            if ((e->kind != EV_JMP && e->kind != EV_JCC) || e->size != 2) continue;
            if (!in_text(e->sym)) {
                fprintf(stderr, "fcc: internal error: native backend: a branch to an undefined label\n");
                abort();
            }
            int64_t d = g_syms[e->sym].off - ((int64_t)e->addr + 2);
            if (!fits8(d)) { e->size = e->kind == EV_JMP ? 5 : 6; grew = true; }
        }
        if (!grew) break;
    }
}

/* ---- DWARF helpers ---- */

static void sec_uleb(int sec, uint64_t v) {
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        obj_put(g_obj, sec, &b, 1);
    } while (v);
}

static void sec_sleb(int sec, int64_t v) {
    for (;;) {
        unsigned char b = v & 0x7f;
        v >>= 7;    /* arithmetic */
        bool done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        if (!done) b |= 0x80;
        obj_put(g_obj, sec, &b, 1);
        if (done) break;
    }
}

static void sec_le(int sec, uint64_t v, int w) {
    unsigned char t[8];
    put_le(t, v, w);
    obj_put(g_obj, sec, t, (uint64_t)w);
}

static void sec_str(int sec, const char *s) { obj_put(g_obj, sec, s, strlen(s) + 1); }

static void sec_patch32(int sec, uint64_t off, uint32_t v) { put_le(g_obj->secs[sec].data + off, v, 4); }

/* An 8-byte .text address field with its relocation. */
static void sec_text_addr(int sec, int64_t addr) {
    uint64_t at = g_obj->secs[sec].size;
    sec_le(sec, 0, 8);
    obj_reloc(g_obj, sec, at, R_X86_64_64, obj_section_symbol(g_obj, ASM_TEXT), addr);
}

static void sec_secoff(int sec, int target) {
    uint64_t at = g_obj->secs[sec].size;
    sec_le(sec, 0, 4);
    obj_reloc(g_obj, sec, at, R_X86_64_32, obj_section_symbol(g_obj, target), 0);
}

/* ---- Unwind tables ---- */

typedef struct { uint32_t addr; int op, reg; int32_t off; } CfiPoint;

/* The DWARF number of each register. */
static const uint8_t DWARF_REG[16] = { 0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15 };

static void cfa_advance(int sec, uint32_t delta) {
    if (delta == 0) return;
    if (delta < 64) { unsigned char b = (unsigned char)(0x40 | delta); obj_put(g_obj, sec, &b, 1); }
    else if (delta < 256) { sec_le(sec, 0x02, 1); sec_le(sec, delta, 1); }
    else if (delta < 65536) { sec_le(sec, 0x03, 1); sec_le(sec, delta, 2); }
    else { sec_le(sec, 0x04, 1); sec_le(sec, delta, 4); }
}

static uint64_t eh_fde(int sec, uint32_t cie, uint32_t start, uint32_t end, CfiPoint *pts, int n) {
    uint64_t at = g_obj->secs[sec].size;
    sec_le(sec, 0, 4);                              /* length, patched */
    sec_le(sec, at + 4 - cie, 4);                   /* CIE pointer */
    uint64_t pc = g_obj->secs[sec].size;
    sec_le(sec, 0, 4);
    obj_reloc(g_obj, sec, pc, R_X86_64_PC32, obj_section_symbol(g_obj, ASM_TEXT), start);
    sec_le(sec, end - start, 4);
    sec_uleb(sec, 0);                               /* no augmentation data */
    uint32_t loc = start;
    for (int i = 0; i < n; i++) {
        cfa_advance(sec, pts[i].addr - loc);
        loc = pts[i].addr;
        switch (pts[i].op) {
        case CFI_PUSHED:
            sec_le(sec, 0x0e, 1); sec_uleb(sec, 16);            /* def_cfa_offset 16 */
            sec_le(sec, 0x80 | 6, 1); sec_uleb(sec, 2);          /* offset rbp, cfa-16 */
            break;
        case CFI_FRAME:
            sec_le(sec, 0x0d, 1); sec_uleb(sec, 6);             /* def_cfa_register rbp */
            break;
        case CFI_LEFT:
            sec_le(sec, 0x0a, 1);                                /* remember_state */
            sec_le(sec, 0x0c, 1); sec_uleb(sec, 7); sec_uleb(sec, 8);   /* def_cfa rsp, 8 */
            break;
        case CFI_RESTORE:
            sec_le(sec, 0x0b, 1);                                /* restore_state */
            break;
        case CFI_SAVED:                                          /* offset reg, cfa+off */
            sec_le(sec, 0x80 | DWARF_REG[pts[i].reg], 1); sec_uleb(sec, (uint64_t)(-pts[i].off / 8));
            break;
        }
    }
    while ((g_obj->secs[sec].size - at) % 4) sec_le(sec, 0, 1);    /* DW_CFA_nop */
    sec_patch32(sec, at, (uint32_t)(g_obj->secs[sec].size - at - 4));
    return at;
}

static void build_eh_frame(Code *t) {
    int sec = 0;
    uint32_t cie = 0, start = 0;
    CfiPoint *pts = NULL;
    int npts = 0, cap = 0;
    bool open = false;
    uint64_t last = 0;
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind != EV_CFI) continue;
        if (!sec) {
            sec = obj_section(g_obj, ".eh_frame", SHT_PROGBITS, SHF_ALLOC, 8);
            /* The CIE: "zR", code align 1, data align -8, return address
             * r16, pc-relative sdata4 pointers; cfa = rsp+8, rip at cfa-8. */
            sec_le(sec, 0x14, 4);
            sec_le(sec, 0, 4);
            sec_le(sec, 1, 1);
            sec_str(sec, "zR");
            sec_uleb(sec, 1);
            sec_sleb(sec, -8);
            sec_uleb(sec, 16);
            sec_uleb(sec, 1);
            sec_le(sec, 0x1b, 1);
            sec_le(sec, 0x0c, 1); sec_uleb(sec, 7); sec_uleb(sec, 8);
            sec_le(sec, 0x80 | 16, 1); sec_uleb(sec, 1);
            sec_le(sec, 0, 2);
        }
        switch (e->sym) {
        case CFI_START: open = true; start = e->addr; npts = 0; break;
        case CFI_END:
            if (open) last = eh_fde(sec, cie, start, e->addr, pts, npts);
            open = false;
            break;
        default: {
            CfiPoint p = { e->addr, e->sym, e->line, (int32_t)e->addend };
            DA_APPEND(pts, npts, cap, p);
            break;
        }
        }
    }
    /* The last entry is padded to end the section on 8 bytes. */
    if (sec && last) {
        while (g_obj->secs[sec].size % 8) sec_le(sec, 0, 1);
        sec_patch32(sec, last, (uint32_t)(g_obj->secs[sec].size - last - 4));
    }
    free(pts);
}

/* ---- Line information and the compilation unit ---- */

typedef struct { uint32_t addr; int file, line; } LineRow;

static void build_debug(Code *t, uint32_t text_size, const char *comp_dir, const char *producer) {
    LineRow *rows = NULL;
    int nrows = 0, caprows = 0;
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind != EV_LOC) continue;
        LineRow r = { e->addr, e->sym, e->line };
        DA_APPEND(rows, nrows, caprows, r);
    }
    if (!nrows) return;

    /* .debug_line, version 3, laid out as `as` lays it out. */
    int ls = obj_section(g_obj, ".debug_line", SHT_PROGBITS, 0, 1);
    sec_le(ls, 0, 4);
    sec_le(ls, 3, 2);
    sec_le(ls, 0, 4);
    uint64_t hdr_start = g_obj->secs[ls].size;
    static const unsigned char std_lens[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    sec_le(ls, 1, 1);           /* minimum instruction length */
    sec_le(ls, 1, 1);           /* default is_stmt */
    sec_le(ls, (uint8_t)-5, 1); /* line base */
    sec_le(ls, 14, 1);          /* line range */
    sec_le(ls, 13, 1);          /* opcode base */
    obj_put(g_obj, ls, std_lens, 12);
    /* Directories: each file's directory part, once. */
    char **dirs = xcalloc((size_t)(g_nfiles + 1), sizeof(char *));
    int *fdir = xcalloc((size_t)(g_nfiles + 1), sizeof(int));
    int ndirs = 0;
    for (int f = 1; f < g_nfiles; f++) {
        if (!g_files[f]) continue;
        const char *slash = strrchr(g_files[f], '/');
        if (!slash) continue;
        size_t dl = slash > g_files[f] ? (size_t)(slash - g_files[f]) : 1;    /* "/" stays */
        int d = 0;
        for (int k = 0; k < ndirs && !d; k++)
            if (strlen(dirs[k]) == dl && strncmp(dirs[k], g_files[f], dl) == 0) d = k + 1;
        if (!d) { dirs[ndirs++] = str_ndup(g_files[f], dl); d = ndirs; }
        fdir[f] = d;
    }
    for (int k = 0; k < ndirs; k++) sec_str(ls, dirs[k]);
    sec_le(ls, 0, 1);
    for (int f = 1; f < g_nfiles; f++) {
        const char *name = g_files[f] ? g_files[f] : "<unknown>";
        const char *slash = strrchr(name, '/');
        sec_str(ls, slash ? slash + 1 : name);
        sec_uleb(ls, (uint64_t)fdir[f]);
        sec_uleb(ls, 0);
        sec_uleb(ls, 0);
    }
    sec_le(ls, 0, 1);
    sec_patch32(ls, 6, (uint32_t)(g_obj->secs[ls].size - hdr_start));
    /* The program: one sequence over .text. */
    sec_le(ls, 0, 1); sec_uleb(ls, 9); sec_le(ls, 2, 1);     /* DW_LNE_set_address */
    sec_text_addr(ls, rows[0].addr);
    uint32_t addr = rows[0].addr;
    int64_t line = 1;
    int file = 1;
    for (int i = 0; i < nrows; i++) {
        if (rows[i].file != file) { sec_le(ls, 4, 1); sec_uleb(ls, (uint64_t)rows[i].file); file = rows[i].file; }
        int64_t dl = rows[i].line - line;
        uint64_t da = rows[i].addr - addr;
        if (dl < -5 || dl > 8) { sec_le(ls, 3, 1); sec_sleb(ls, dl); dl = 0; }
        uint64_t op = (uint64_t)(dl + 5) + 14 * da + 13;
        if (op > 255 && da >= 17 && (uint64_t)(dl + 5) + 14 * (da - 17) + 13 <= 255) {
            sec_le(ls, 8, 1);                                   /* DW_LNS_const_add_pc */
            da -= 17;
            op = (uint64_t)(dl + 5) + 14 * da + 13;
        }
        if (op > 255) {
            sec_le(ls, 2, 1); sec_uleb(ls, da);                 /* DW_LNS_advance_pc */
            op = (uint64_t)(dl + 5) + 13;
        }
        sec_le(ls, op, 1);
        addr = rows[i].addr;
        line = rows[i].line;
    }
    if (text_size > addr) { sec_le(ls, 2, 1); sec_uleb(ls, text_size - addr); }
    sec_le(ls, 0, 1); sec_uleb(ls, 1); sec_le(ls, 1, 1);     /* DW_LNE_end_sequence */
    sec_patch32(ls, 0, (uint32_t)(g_obj->secs[ls].size - 4));

    /* .debug_abbrev: the unit and its functions. */
    int as = obj_section(g_obj, ".debug_abbrev", SHT_PROGBITS, 0, 1);
    static const unsigned char abbrev[] = {
        1, 0x11, 1,  0x10, 0x06,  0x11, 0x01,  0x12, 0x01,  0x03, 0x08,  0x1b, 0x08,  0x25, 0x08,
        0x13, 0x05,  0, 0,
        2, 0x2e, 0,  0x03, 0x08,  0x3f, 0x0c,  0x11, 0x01,  0x12, 0x01,  0, 0,
        0,
    };
    obj_put(g_obj, as, abbrev, sizeof abbrev);

    /* .debug_info, version 3. */
    int is = obj_section(g_obj, ".debug_info", SHT_PROGBITS, 0, 1);
    sec_le(is, 0, 4);
    sec_le(is, 3, 2);
    sec_secoff(is, as);
    sec_le(is, 8, 1);
    sec_uleb(is, 1);
    sec_secoff(is, ls);
    sec_text_addr(is, 0);
    sec_text_addr(is, text_size);
    sec_str(is, g_nfiles > 1 && g_files[1] ? g_files[1] : "");
    sec_str(is, comp_dir ? comp_dir : "");
    sec_str(is, producer);
    sec_le(is, 0x8001, 2);      /* DW_LANG_Mips_Assembler, as `as` marks it */
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind != EV_FUNC) continue;
        AsmSym *s = &g_syms[e->sym];
        sec_uleb(is, 2);
        sec_str(is, s->name);
        sec_le(is, s->global ? 1 : 0, 1);
        sec_text_addr(is, s->off);
        sec_text_addr(is, s->off + s->size);
    }
    sec_le(is, 0, 1);
    sec_patch32(is, 0, (uint32_t)(g_obj->secs[is].size - 4));

    /* .debug_aranges: the one range. */
    int ar = obj_section(g_obj, ".debug_aranges", SHT_PROGBITS, 0, 1);
    sec_le(ar, 44, 4);
    sec_le(ar, 2, 2);
    sec_secoff(ar, is);
    sec_le(ar, 8, 1);
    sec_le(ar, 0, 1);
    sec_le(ar, 0, 4);
    sec_text_addr(ar, 0);
    sec_le(ar, text_size, 8);
    sec_le(ar, 0, 8);
    sec_le(ar, 0, 8);

    for (int k = 0; k < ndirs; k++) free(dirs[k]);
    free(dirs);
    free(fdir);
    free(rows);
}

/* ================================================================
 * The object
 * ================================================================ */

void asm_init(Obj *o) {
    g_obj = o;
    obj_section(o, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16);
    obj_section(o, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 1);
    obj_section(o, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 1);
    obj_section(o, ".rodata", SHT_PROGBITS, SHF_ALLOC, 1);
    obj_section(o, ".data.rel.ro", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 1);
}

void asm_finish(Code *t, const char *comp_dir, const char *producer) {
    layout(t);

    /* The bytes, with branches and padding in place. */
    uint32_t prev = 0;
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->pos > prev) { obj_put(g_obj, ASM_TEXT, t->p + prev, e->pos - prev); prev = e->pos; }
        if (e->kind == EV_ALIGN) {
            int n = e->size;
            while (n > 11) { obj_put(g_obj, ASM_TEXT, NOPS[10], 11); n -= 11; }
            if (n) obj_put(g_obj, ASM_TEXT, NOPS[n - 1], (uint64_t)n);
        } else if (e->kind == EV_JMP || e->kind == EV_JCC) {
            unsigned char b[6];
            int64_t target = in_text(e->sym) ? g_syms[e->sym].off : 0;
            int64_t d = target - ((int64_t)e->addr + e->size);
            if (e->size == 2) {
                b[0] = e->kind == EV_JMP ? 0xEB : (unsigned char)(0x70 + e->cc);
                b[1] = (unsigned char)(d & 0xff);
            } else if (e->kind == EV_JMP) {
                b[0] = 0xE9;
                put_le(b + 1, in_text(e->sym) ? (uint64_t)d : 0, 4);
            } else {
                b[0] = 0x0F; b[1] = (unsigned char)(0x80 + e->cc);
                put_le(b + 2, (uint64_t)d, 4);
            }
            obj_put(g_obj, ASM_TEXT, b, e->size);
        } else if (e->kind == EV_FUNC_END) {
            g_syms[e->sym].size = (int64_t)e->addr - g_syms[e->sym].off;
        }
    }
    if (t->len > prev) obj_put(g_obj, ASM_TEXT, t->p + prev, t->len - prev);
    uint32_t text_size = (uint32_t)g_obj->secs[ASM_TEXT].size;

    /* Symbols: functions and named data, the rest undefined. */
    for (int i = 1; i < g_nsym; i++) {
        AsmSym *s = &g_syms[i];
        if (!s->name || (!s->sec && !s->used)) continue;
        s->objsym = obj_symbol(g_obj, s->name, s->sec, s->sec ? (uint64_t)s->off : 0,
                               s->func ? (uint64_t)s->size : 0,
                               s->func ? STT_FUNC : STT_NOTYPE,
                               s->global || !s->sec ? STB_GLOBAL : STB_LOCAL);
    }

    /* References from .text. */
    unsigned char *text = g_obj->secs[ASM_TEXT].data;
    for (int i = 0; i < t->nev; i++) {
        AsmEv *e = &t->ev[i];
        if (e->kind == EV_JMP && e->size == 5 && !in_text(e->sym)) {
            int64_t add = -4;
            obj_reloc(g_obj, ASM_TEXT, e->addr + 1, R_X86_64_PLT32, reloc_named(e->sym), add);
            continue;
        }
        if (e->kind != EV_REF) continue;
        int64_t tail = e->size;
        if ((e->rtype == RT_PC || e->rtype == RT_BRANCH) && in_text(e->sym) && !g_syms[e->sym].global) {
            int64_t d = g_syms[e->sym].off + e->addend - ((int64_t)e->addr + 4 + tail);
            put_le(text + e->addr, (uint64_t)d, 4);
            continue;
        }
        int64_t add = e->addend - 4 - tail;
        uint32_t type = e->rtype == RT_GOT ? R_X86_64_REX_GOTPCRELX
                      : e->rtype == RT_PC ? R_X86_64_PC32 : R_X86_64_PLT32;
        int target = e->rtype == RT_PC ? reloc_target(e->sym, &add) : reloc_named(e->sym);
        obj_reloc(g_obj, ASM_TEXT, e->addr, type, target, add);
    }
    /* References from data. */
    for (int i = 0; i < g_ndref; i++) {
        DataRef *r = &g_drefs[i];
        int64_t add = r->addend;
        int target = reloc_target(r->sym, &add);
        obj_reloc(g_obj, r->sec, (uint64_t)r->off, R_X86_64_64, target, add);
    }

    obj_section(g_obj, ".note.GNU-stack", SHT_PROGBITS, 0, 1);
    build_eh_frame(t);
    build_debug(t, text_size, comp_dir, producer);
}

void asm_free(void) {
    for (int i = 1; i < g_nsym; i++) free(g_syms[i].name);
    free(g_syms); g_syms = NULL; g_nsym = g_capsym = 0;
    free(g_names); g_names = NULL; g_names_cap = g_names_n = 0;
    for (int i = 0; i < g_nfiles; i++) free(g_files[i]);
    free(g_files); g_files = NULL; g_nfiles = 0;
    free(g_drefs); g_drefs = NULL; g_ndref = g_capdref = 0;
    g_obj = NULL;
}
