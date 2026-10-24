#pragma once
#include "elfobj.h"
#include <stdbool.h>
#include <stdint.h>

/* x86-64 machine code for the native backend, encoded directly: no assembly
 * text and no assembler. Instructions go into code buffers along with the
 * things only the final layout can settle (labels, branches, references to
 * symbols, source lines, unwind points); asm_finish lays the text out,
 * choosing each branch's short or long form as GNU as would, and produces an
 * ELF relocatable object with .eh_frame and DWARF line information.
 *
 * The encoding of every instruction is the one GNU as picks for the same
 * AT&T source line, so the object matches what `as` made of the assembly the
 * backend used to write. The instruction functions take their operands in
 * AT&T order: source first, destination last. */

typedef enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 } Reg;
#define RIP 16          /* only as the base of a memory operand */

typedef enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A,
               CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G } Cond;

typedef enum { OPND_REG, OPND_XMM, OPND_MEM, OPND_IMM } OpndKind;

typedef struct {
    uint8_t kind;
    int8_t base;            /* REG/XMM: the register; MEM: the base register or RIP */
    int8_t index;           /* MEM: the index register, or -1 */
    uint8_t scale;          /* MEM: 1, 2, 4 or 8 */
    bool got;               /* RIP-relative: the symbol's GOT entry */
    int32_t sym;            /* RIP-relative: the label or symbol addressed */
    int64_t disp;           /* MEM: displacement (RIP: addend); IMM: the value */
} Opnd;

static inline Opnd R(Reg r) { Opnd o = { OPND_REG, (int8_t)r, -1, 1, false, 0, 0 }; return o; }
static inline Opnd X(int n) { Opnd o = { OPND_XMM, (int8_t)n, -1, 1, false, 0, 0 }; return o; }
static inline Opnd M(Reg base, int64_t disp) { Opnd o = { OPND_MEM, (int8_t)base, -1, 1, false, 0, disp }; return o; }
static inline Opnd MI(Reg base, Reg index, int scale, int64_t disp) {
    Opnd o = { OPND_MEM, (int8_t)base, (int8_t)index, (uint8_t)scale, false, 0, disp };
    return o;
}
static inline Opnd IMM(int64_t v) { Opnd o = { OPND_IMM, -1, -1, 1, false, 0, v }; return o; }
/* `sym(%rip)` for a label or symbol, plus an addend. */
static inline Opnd RIPREL(int sym, int64_t addend) { Opnd o = { OPND_MEM, RIP, -1, 1, false, sym, addend }; return o; }
/* `sym@GOTPCREL(%rip)`. */
static inline Opnd GOTREL(int sym) { Opnd o = { OPND_MEM, RIP, -1, 1, true, sym, 0 }; return o; }

typedef struct AsmEv AsmEv;

/* A code buffer: the bytes, and the events placed among them. */
typedef struct {
    unsigned char *p;
    uint32_t len, cap;
    AsmEv *ev;
    int nev, capev;
} Code;

void code_reset(Code *c);
void code_free(Code *c);
/* Append src (bytes and events) to dst. */
void code_append(Code *dst, const Code *src);

/* ---- Labels and symbols ----
 * Both are ids in one space. A label is internal to the object; a symbol has
 * a name, which the symbol table and relocations use. */
int asm_label(void);
int asm_sym(const char *name);
void asm_sym_global(int sym);

/* The sections the backend writes. */
enum { ASM_TEXT = 1, ASM_DATA, ASM_BSS, ASM_RODATA, ASM_RELRO };

/* ---- Instructions ---- */

typedef enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP } AluOp;
typedef enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 } ShiftOp;
typedef enum { UN_NOT = 2, UN_NEG = 3, UN_MUL = 4, UN_DIV = 6, UN_IDIV = 7, UN_INC = 8, UN_DEC = 9 } UnOp;
typedef enum {
    SSE_MOVSS, SSE_MOVSD, SSE_MOVAPS, SSE_XORPS,
    SSE_ADDSS, SSE_ADDSD, SSE_SUBSS, SSE_SUBSD, SSE_MULSS, SSE_MULSD, SSE_DIVSS, SSE_DIVSD,
    SSE_UCOMISS, SSE_UCOMISD, SSE_CVTSS2SD, SSE_CVTSD2SS,
    SSE_CVTTSD2SIQ, SSE_CVTSI2SDQ, SSE_CVTSI2SSQ,
} SseOp;
typedef enum { OP_CQO, OP_LEAVE, OP_RET, OP_MFENCE, OP_REP_MOVSB, OP_REP_STOSB, OP_UD2, OP_PUSH_RBP } Op0;

/* mov of width w (1, 2, 4, 8). With an XMM operand, movq (w 8) or movd (w 4). */
void x_mov(Code *c, int w, Opnd src, Opnd dst);
/* movabsq $imm64, %reg: always the ten-byte form. */
void x_movabs(Code *c, int64_t imm, Opnd dst);
void x_alu(Code *c, AluOp op, int w, Opnd src, Opnd dst);
void x_test(Code *c, int w, Opnd src, Opnd dst);
void x_lea(Code *c, Opnd mem, Opnd dst);
/* count: IMM(n), or R(RCX) for %cl. */
void x_shift(Code *c, ShiftOp op, int w, Opnd count, Opnd dst);
void x_unary(Code *c, UnOp op, int w, Opnd dst);
void x_imul(Code *c, int w, Opnd src, Opnd dst);
void x_imul3(Code *c, int w, int64_t imm, Opnd src, Opnd dst);
/* movz/movs from sw bytes to dw bytes (movzbl, movzwl, movsbq, movswq, movslq). */
void x_movx(Code *c, int sw, int dw, bool sign, Opnd src, Opnd dst);
void x_setcc(Code *c, Cond cc, Opnd dst);
void x_cmov(Code *c, Cond cc, int w, Opnd src, Opnd dst);
void x_btc(Code *c, int w, int bit, Opnd dst);
void x_sse(Code *c, SseOp op, Opnd src, Opnd dst);
void x_op0(Code *c, Op0 op);

void x_jcc(Code *c, Cond cc, int label);
/* jmp to a label, or to a function defined in this object (relaxed); to any
 * other symbol, through the PLT. */
void x_jmp(Code *c, int target);
/* Remove a jmp to `label` that ends c, where control would fall through to
 * the label anyway; whether there was one. */
bool x_drop_final_jmp(Code *c, int label);
void x_jmp_plt(Code *c, int sym);
void x_call(Code *c, int sym, bool plt);
void x_call_ind(Code *c, Opnd target);
void x_jmp_ind(Code *c, Opnd target);

/* ---- Placement ---- */

void x_label(Code *c, int label);
/* Pad with no-ops to a multiple of `align`. */
void x_align(Code *c, int align);
/* Start and end a function symbol (its type and size). */
void x_func(Code *c, int sym);
void x_func_end(Code *c, int sym);
/* The source line of the following code (DWARF line table). */
void x_loc(Code *c, int file, int line);
void asm_file(int file, const char *path);

/* Unwind information for the standard frame. */
typedef enum {
    CFI_START,          /* .cfi_startproc */
    CFI_PUSHED,         /* after push %rbp: cfa = rsp+16, rbp saved at cfa-16 */
    CFI_FRAME,          /* after mov %rsp,%rbp: cfa = rbp+16 */
    CFI_LEFT,           /* after leave: remember the state; cfa = rsp+8 */
    CFI_RESTORE,        /* restore the remembered state */
    CFI_END,            /* .cfi_endproc */
    CFI_SAVED,          /* a callee-saved register stored in the frame (x_cfi_saved) */
} CfiOp;
void x_cfi(Code *c, CfiOp op);
/* .cfi_offset: register r is saved at cfa_off from the CFA. */
void x_cfi_saved(Code *c, Reg r, int32_t cfa_off);

/* ---- Data ---- */

void d_align(int sec, int64_t align);
/* Define a label or symbol at the section's current end. */
void d_def(int sec, int sym);
void d_bytes(int sec, const void *p, int64_t n);
void d_zero(int sec, int64_t n);
/* A w-byte little-endian integer. */
void d_int(int sec, uint64_t v, int w);
/* An 8-byte absolute address: sym + addend. */
void d_addr(int sec, int sym, int64_t addend);
/* The same, into zero bytes already written at `off`. */
void d_addr_at(int sec, int64_t off, int sym, int64_t addend);
int64_t d_size(int sec);

/* ---- The object ---- */

/* Start an object in *o (obj_init'ed by the caller): the data functions write
 * into its sections from here on. */
void asm_init(Obj *o);
/* Lay out `text` and complete the object: its code, symbols, relocations,
 * unwind tables and line information. */
void asm_finish(Code *text, const char *comp_dir, const char *producer);
void asm_free(void);
