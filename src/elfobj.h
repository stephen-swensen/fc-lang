#pragma once
#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* An ELF64 relocatable object (x86-64) built in memory: what the native
 * backend writes in place of assembly. The constants are defined here rather
 * than taken from <elf.h>, so fcc builds on hosts without it. */

enum {
    SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4,
    SHT_NOBITS = 8, SHT_GROUP = 17, SHT_X86_64_UNWIND = 0x70000001,
};
enum { SHF_WRITE = 0x1, SHF_ALLOC = 0x2, SHF_EXECINSTR = 0x4, SHF_MERGE = 0x10,
       SHF_STRINGS = 0x20, SHF_INFO_LINK = 0x40, SHF_GROUP = 0x200 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2 };
enum {
    R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_PLT32 = 4, R_X86_64_32 = 10,
    R_X86_64_REX_GOTPCRELX = 42,
};

typedef struct {
    uint64_t off;
    uint32_t type;
    int sym;                /* an index into Obj.syms */
    int64_t addend;
} ObjReloc;

typedef struct {
    char *name;
    uint32_t type;
    uint64_t flags, align, entsize;
    unsigned char *data;    /* NULL for SHT_NOBITS */
    uint64_t size, cap;
    ObjReloc *rel;
    int nrel, caprel;
    int sym;                /* its section symbol, or 0 */
} ObjSection;

typedef struct {
    char *name;             /* NULL for a section symbol */
    int sec;                /* a section index, or SHN_UNDEF / SHN_ABS / SHN_COMMON */
    uint64_t value, size;
    uint8_t type, bind, other;
} ObjSymbol;

/* Index 0 of both arrays is the null entry. */
typedef struct {
    ObjSection *secs;
    int nsec, capsec;
    ObjSymbol *syms;
    int nsym, capsym;
    HMap globals;           /* name -> index of each symbol made non-local */
} Obj;

void obj_init(Obj *o);
void obj_free(Obj *o);

int obj_section(Obj *o, const char *name, uint32_t type, uint64_t flags, uint64_t align);
/* Append bytes (zeros when p is NULL) to a section; returns their offset. A
 * NOBITS section only grows. */
uint64_t obj_put(Obj *o, int sec, const void *p, uint64_t n);
/* Pad to a multiple of `align` (raising the section's alignment). */
void obj_align(Obj *o, int sec, uint64_t align);
/* A symbol's binding stays local or non-local from here on: the index of
 * non-local names is kept as symbols are added. */
int obj_symbol(Obj *o, const char *name, int sec, uint64_t value, uint64_t size,
               uint8_t type, uint8_t bind);
/* The non-local symbol of that name, or 0. */
int obj_find(Obj *o, const char *name);
int obj_section_symbol(Obj *o, int sec);
void obj_reloc(Obj *o, int sec, uint64_t off, uint32_t type, int sym, int64_t addend);

/* Add the sections, symbols and relocations of a relocatable object (one
 * compiled by the C compiler), resolving its global symbols against those
 * already present by name as a linker does: a strong definition replaces a
 * weak or common one, a common symbol takes the larger size and alignment,
 * and two strong definitions are an error. False with a message in *err on a
 * malformed object or a duplicate definition. */
bool obj_merge(Obj *o, const unsigned char *elf, size_t n, char **err);

/* Write the object file: the sections in order, a .rela section for each
 * that has relocations, then the symbol and string tables. */
bool obj_write(Obj *o, FILE *f);
