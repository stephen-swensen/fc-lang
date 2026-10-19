/* elfobj.c: ELF64 relocatable objects for x86-64, built in memory and
 * written as a file. The native backend fills one with its code and data;
 * the C header shim, compiled by the C compiler, is read back and merged into
 * it, so the result is a single object for the linker. */

#include "elfobj.h"
#include "common.h"
#include <string.h>

void obj_init(Obj *o) {
    memset(o, 0, sizeof *o);
    o->capsec = 16;
    o->secs = xcalloc((size_t)o->capsec, sizeof(ObjSection));
    o->nsec = 1;
    o->capsym = 64;
    o->syms = xcalloc((size_t)o->capsym, sizeof(ObjSymbol));
    o->nsym = 1;
}

void obj_free(Obj *o) {
    for (int i = 1; i < o->nsec; i++) {
        free(o->secs[i].name);
        free(o->secs[i].data);
        free(o->secs[i].rel);
    }
    for (int i = 1; i < o->nsym; i++) free(o->syms[i].name);
    free(o->secs);
    free(o->syms);
    memset(o, 0, sizeof *o);
}

int obj_section(Obj *o, const char *name, uint32_t type, uint64_t flags, uint64_t align) {
    ObjSection s = { 0 };
    s.name = str_dup(name);
    s.type = type;
    s.flags = flags;
    s.align = align ? align : 1;
    DA_APPEND(o->secs, o->nsec, o->capsec, s);
    return o->nsec - 1;
}

uint64_t obj_put(Obj *o, int sec, const void *p, uint64_t n) {
    ObjSection *s = &o->secs[sec];
    uint64_t at = s->size;
    if (n == 0) return at;
    if (s->type != SHT_NOBITS) {
        if (s->size + n > s->cap) {
            uint64_t nc = s->cap ? s->cap * 2 : 4096;
            while (nc < s->size + n) nc *= 2;
            s->data = xrealloc(s->data, (size_t)nc);
            s->cap = nc;
        }
        if (p) memcpy(s->data + s->size, p, (size_t)n);
        else memset(s->data + s->size, 0, (size_t)n);
    }
    s->size += n;
    return at;
}

void obj_align(Obj *o, int sec, uint64_t align) {
    ObjSection *s = &o->secs[sec];
    if (align <= 1) return;
    if (align > s->align) s->align = align;
    uint64_t pad = (align - s->size % align) % align;
    if (pad) obj_put(o, sec, NULL, pad);
}

int obj_symbol(Obj *o, const char *name, int sec, uint64_t value, uint64_t size,
               uint8_t type, uint8_t bind) {
    ObjSymbol s = { 0 };
    s.name = name ? str_dup(name) : NULL;
    s.sec = sec;
    s.value = value;
    s.size = size;
    s.type = type;
    s.bind = bind;
    DA_APPEND(o->syms, o->nsym, o->capsym, s);
    return o->nsym - 1;
}

int obj_section_symbol(Obj *o, int sec) {
    if (!o->secs[sec].sym)
        o->secs[sec].sym = obj_symbol(o, NULL, sec, 0, 0, STT_SECTION, STB_LOCAL);
    return o->secs[sec].sym;
}

void obj_reloc(Obj *o, int sec, uint64_t off, uint32_t type, int sym, int64_t addend) {
    ObjSection *s = &o->secs[sec];
    ObjReloc r = { off, type, sym, addend };
    DA_APPEND(s->rel, s->nrel, s->caprel, r);
}

/* ================================================================
 * Little-endian fields
 * ================================================================ */

typedef struct { unsigned char *p; size_t len, cap; } Bytes;

static void by_put(Bytes *b, const void *p, size_t n) {
    if (n == 0) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + n) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
    if (p) memcpy(b->p + b->len, p, n);
    else memset(b->p + b->len, 0, n);
    b->len += n;
}

static void by_le(Bytes *b, uint64_t v, int w) {
    unsigned char t[8];
    for (int i = 0; i < w; i++) t[i] = (unsigned char)(v >> (8 * i));
    by_put(b, t, (size_t)w);
}

static void by_align(Bytes *b, uint64_t a) {
    while (a > 1 && b->len % a) by_put(b, NULL, 1);
}

static uint64_t rd(const unsigned char *p, int w) {
    uint64_t v = 0;
    for (int i = w - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ================================================================
 * Writing
 * ================================================================ */

static uint32_t strtab_add(Bytes *t, const char *s) {
    uint32_t at = (uint32_t)t->len;
    by_put(t, s, strlen(s) + 1);
    return at;
}

bool obj_write(Obj *o, FILE *f) {
    int ncontent = o->nsec - 1;
    int nrela = 0;
    for (int i = 1; i < o->nsec; i++) if (o->secs[i].nrel) nrela++;
    int symtab_idx = 1 + ncontent + nrela, strtab_idx = symtab_idx + 1, shstr_idx = strtab_idx + 1;
    int nshdr = shstr_idx + 1;

    /* Symbols: the locals first, as ELF requires. */
    int *final = xcalloc((size_t)o->nsym, sizeof(int));
    int next = 1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 1; i < o->nsym; i++)
            if ((o->syms[i].bind == STB_LOCAL) == (pass == 0)) final[i] = next++;
    int first_global = 1;
    for (int i = 1; i < o->nsym; i++) if (o->syms[i].bind == STB_LOCAL) first_global++;

    Bytes strtab = { 0 }, symtab = { 0 }, shstr = { 0 };
    by_put(&strtab, "", 1);
    by_put(&shstr, "", 1);
    by_put(&symtab, NULL, 24 * (size_t)next);
    for (int i = 1; i < o->nsym; i++) {
        ObjSymbol *s = &o->syms[i];
        uint32_t nm = s->name && *s->name ? strtab_add(&strtab, s->name) : 0;
        unsigned char *e = symtab.p + 24 * (size_t)final[i];
        for (int k = 0; k < 4; k++) e[k] = (unsigned char)(nm >> (8 * k));
        e[4] = (unsigned char)(s->bind << 4 | (s->type & 0xf));
        e[5] = s->other;
        e[6] = (unsigned char)s->sec;
        e[7] = (unsigned char)(s->sec >> 8);
        for (int k = 0; k < 8; k++) e[8 + k] = (unsigned char)(s->value >> (8 * k));
        for (int k = 0; k < 8; k++) e[16 + k] = (unsigned char)(s->size >> (8 * k));
    }

    /* The file: header, section contents, section headers. */
    Bytes out = { 0 };
    by_put(&out, NULL, 64);
    uint64_t *offs = xcalloc((size_t)nshdr, sizeof(uint64_t));
    uint64_t *sizes = xcalloc((size_t)nshdr, sizeof(uint64_t));
    for (int i = 1; i < o->nsec; i++) {
        ObjSection *s = &o->secs[i];
        by_align(&out, s->align);
        offs[i] = out.len;
        sizes[i] = s->size;
        if (s->type != SHT_NOBITS && s->size) by_put(&out, s->data, (size_t)s->size);
    }
    int ri = 1 + ncontent;
    int *rela_target = xcalloc((size_t)nshdr, sizeof(int));
    for (int i = 1; i < o->nsec; i++) {
        ObjSection *s = &o->secs[i];
        if (!s->nrel) continue;
        by_align(&out, 8);
        offs[ri] = out.len;
        sizes[ri] = 24 * (uint64_t)s->nrel;
        for (int k = 0; k < s->nrel; k++) {
            ObjReloc *r = &s->rel[k];
            by_le(&out, r->off, 8);
            by_le(&out, (uint64_t)final[r->sym] << 32 | r->type, 8);
            by_le(&out, (uint64_t)r->addend, 8);
        }
        rela_target[ri++] = i;
    }
    by_align(&out, 8);
    offs[symtab_idx] = out.len;
    sizes[symtab_idx] = symtab.len;
    by_put(&out, symtab.p, symtab.len);
    offs[strtab_idx] = out.len;
    sizes[strtab_idx] = strtab.len;
    by_put(&out, strtab.p, strtab.len);

    uint32_t *names = xcalloc((size_t)nshdr, sizeof(uint32_t));
    for (int i = 1; i < o->nsec; i++) names[i] = strtab_add(&shstr, o->secs[i].name);
    for (int i = 1 + ncontent; i < symtab_idx; i++) {
        char *nm = str_sprintf(".rela%s", o->secs[rela_target[i]].name);
        names[i] = strtab_add(&shstr, nm);
        free(nm);
    }
    names[symtab_idx] = strtab_add(&shstr, ".symtab");
    names[strtab_idx] = strtab_add(&shstr, ".strtab");
    names[shstr_idx] = strtab_add(&shstr, ".shstrtab");
    offs[shstr_idx] = out.len;
    sizes[shstr_idx] = shstr.len;
    by_put(&out, shstr.p, shstr.len);

    by_align(&out, 8);
    uint64_t shoff = out.len;
    by_put(&out, NULL, 64);     /* the null section */
    for (int i = 1; i < nshdr; i++) {
        uint32_t type, link = 0, info = 0;
        uint64_t flags = 0, align, entsize = 0;
        if (i <= ncontent) {
            ObjSection *s = &o->secs[i];
            type = s->type; flags = s->flags; align = s->align; entsize = s->entsize;
        } else if (i < symtab_idx) {
            type = SHT_RELA; flags = SHF_INFO_LINK; align = 8; entsize = 24;
            link = (uint32_t)symtab_idx; info = (uint32_t)rela_target[i];
        } else if (i == symtab_idx) {
            type = SHT_SYMTAB; align = 8; entsize = 24;
            link = (uint32_t)strtab_idx; info = (uint32_t)first_global;
        } else {
            type = SHT_STRTAB; align = 1;
        }
        by_le(&out, names[i], 4);
        by_le(&out, type, 4);
        by_le(&out, flags, 8);
        by_le(&out, 0, 8);
        by_le(&out, offs[i], 8);
        by_le(&out, sizes[i], 8);
        by_le(&out, link, 4);
        by_le(&out, info, 4);
        by_le(&out, align, 8);
        by_le(&out, entsize, 8);
    }

    /* The header. */
    unsigned char *h = out.p;
    memcpy(h, "\177ELF\2\1\1", 7);
    h[16] = 1;                  /* ET_REL */
    h[18] = 62;                 /* EM_X86_64 */
    h[20] = 1;                  /* EV_CURRENT */
    for (int k = 0; k < 8; k++) h[40 + k] = (unsigned char)(shoff >> (8 * k));
    h[52] = 64;                 /* e_ehsize */
    h[58] = 64;                 /* e_shentsize */
    h[60] = (unsigned char)nshdr; h[61] = (unsigned char)(nshdr >> 8);
    h[62] = (unsigned char)shstr_idx; h[63] = (unsigned char)(shstr_idx >> 8);

    bool ok = fwrite(out.p, 1, out.len, f) == out.len;
    free(out.p); free(strtab.p); free(symtab.p); free(shstr.p);
    free(final); free(offs); free(sizes); free(rela_target); free(names);
    return ok;
}

/* ================================================================
 * Merging an object compiled by the C compiler
 * ================================================================ */

typedef struct {
    uint32_t name, type, link, info;
    uint64_t flags, off, size, align, entsize;
} Shdr;

static const char *strat(const unsigned char *elf, size_t n, const Shdr *tab, uint32_t at) {
    if (!tab || tab->off + at >= n) return "";
    const char *s = (const char *)elf + tab->off + at;
    size_t room = (size_t)(n - (tab->off + at));
    return memchr(s, 0, room) ? s : "";
}

bool obj_merge(Obj *o, const unsigned char *elf, size_t n, char **err) {
    *err = NULL;
    if (n < 64 || memcmp(elf, "\177ELF\2\1", 6) != 0 || rd(elf + 16, 2) != 1 || rd(elf + 18, 2) != 62) {
        *err = str_dup("not an x86-64 ELF relocatable object");
        return false;
    }
    uint64_t shoff = rd(elf + 40, 8);
    int shnum = (int)rd(elf + 60, 2), shstrndx = (int)rd(elf + 62, 2);
    if (shoff + (uint64_t)shnum * 64 > n || shstrndx >= shnum) {
        *err = str_dup("truncated section headers");
        return false;
    }
    Shdr *sh = xcalloc((size_t)(shnum ? shnum : 1), sizeof(Shdr));
    for (int i = 0; i < shnum; i++) {
        const unsigned char *p = elf + shoff + 64 * (uint64_t)i;
        sh[i] = (Shdr){ (uint32_t)rd(p, 4), (uint32_t)rd(p + 4, 4), (uint32_t)rd(p + 40, 4),
                        (uint32_t)rd(p + 44, 4), rd(p + 8, 8), rd(p + 24, 8), rd(p + 32, 8),
                        rd(p + 48, 8), rd(p + 56, 8) };
        if (sh[i].type != SHT_NOBITS && sh[i].off + sh[i].size > n) {
            *err = str_dup("a section lies outside the file");
            free(sh);
            return false;
        }
    }
    const Shdr *shstr = &sh[shstrndx];

    /* The allocated sections. Code and data join the section of the same
     * name, at the end (so there is one .text); anything else is copied as
     * a section of its own. Notes (stack and property markers) and group
     * headers are left out. secbase: where each one's contents now start. */
    int *secmap = xcalloc((size_t)(shnum ? shnum : 1), sizeof(int));
    uint64_t *secbase = xcalloc((size_t)(shnum ? shnum : 1), sizeof(uint64_t));
    int symtab = -1;
    for (int i = 1; i < shnum; i++) {
        if (sh[i].type == SHT_SYMTAB) symtab = i;
        if (!(sh[i].flags & SHF_ALLOC) || sh[i].type == SHT_GROUP || sh[i].type == 7 /* SHT_NOTE */)
            continue;
        const char *name = strat(elf, n, shstr, sh[i].name);
        uint64_t flags = sh[i].flags & ~(uint64_t)SHF_GROUP;
        int s = 0;
        if ((sh[i].type == SHT_PROGBITS || sh[i].type == SHT_NOBITS) && strcmp(name, ".eh_frame") != 0)
            for (int k = 1; k < o->nsec && !s; k++)
                if (strcmp(o->secs[k].name, name) == 0 && o->secs[k].type == sh[i].type &&
                    o->secs[k].flags == flags && o->secs[k].entsize == sh[i].entsize)
                    s = k;
        if (!s) {
            s = obj_section(o, name, sh[i].type, flags, sh[i].align);
            o->secs[s].entsize = sh[i].entsize;
        }
        obj_align(o, s, sh[i].align);
        secbase[i] = obj_put(o, s, sh[i].type == SHT_NOBITS ? NULL : elf + sh[i].off, sh[i].size);
        secmap[i] = s;
    }

    bool ok = true;
    int *symmap = NULL;
    uint64_t *symbase = NULL;
    if (symtab >= 0) {
        const Shdr *st = &sh[symtab];
        const Shdr *strs = st->link < (uint32_t)shnum ? &sh[st->link] : NULL;
        int nsym = (int)(st->size / 24);
        symmap = xcalloc((size_t)(nsym ? nsym : 1), sizeof(int));
        symbase = xcalloc((size_t)(nsym ? nsym : 1), sizeof(uint64_t));
        for (int i = 1; i < nsym && ok; i++) {
            const unsigned char *p = elf + st->off + 24 * (uint64_t)i;
            const char *name = strat(elf, n, strs, (uint32_t)rd(p, 4));
            uint8_t info = p[4], bind = info >> 4, type = info & 0xf;
            int shndx = (int)rd(p + 6, 2);
            uint64_t value = rd(p + 8, 8), size = rd(p + 16, 8);
            if (type == STT_FILE) continue;
            int sec = shndx;
            if (shndx != SHN_UNDEF && shndx != SHN_ABS && shndx != SHN_COMMON) {
                if (shndx >= shnum || !secmap[shndx]) continue;    /* in a dropped section */
                sec = secmap[shndx];
                value += secbase[shndx];
            }
            if (type == STT_SECTION) {
                /* Now the section's symbol plus where its contents went. */
                symmap[i] = obj_section_symbol(o, sec);
                symbase[i] = secbase[shndx];
                continue;
            }
            if (bind == STB_LOCAL) {
                symmap[i] = obj_symbol(o, name, sec, value, size, type, bind);
                o->syms[symmap[i]].other = p[5];
                continue;
            }
            int found = 0;
            for (int k = 1; k < o->nsym && !found; k++)
                if (o->syms[k].bind != STB_LOCAL && o->syms[k].name && strcmp(o->syms[k].name, name) == 0)
                    found = k;
            if (!found) {
                symmap[i] = obj_symbol(o, name, sec, value, size, type, bind);
                o->syms[symmap[i]].other = p[5];
            } else {
                ObjSymbol *e = &o->syms[found];
                if (sec != SHN_UNDEF) {
                    if (e->sec != SHN_UNDEF) {
                        *err = str_sprintf("symbol '%s' is defined twice", name);
                        ok = false;
                        break;
                    }
                    e->sec = sec; e->value = value; e->size = size; e->type = type; e->bind = bind;
                    e->other = p[5];
                }
                symmap[i] = found;
            }
        }
        for (int i = 1; i < shnum && ok; i++) {
            if (sh[i].type != SHT_RELA || sh[i].info >= (uint32_t)shnum || !secmap[sh[i].info]) continue;
            int target = secmap[sh[i].info];
            for (uint64_t k = 0; k + 24 <= sh[i].size; k += 24) {
                const unsigned char *p = elf + sh[i].off + k;
                uint64_t rinfo = rd(p + 8, 8);
                int si = (int)(rinfo >> 32);
                if (si >= nsym || (si && !symmap[si])) {
                    *err = str_dup("a relocation refers to a dropped symbol");
                    ok = false;
                    break;
                }
                obj_reloc(o, target, secbase[sh[i].info] + rd(p, 8), (uint32_t)rinfo, symmap[si],
                          (int64_t)rd(p + 16, 8) + (int64_t)symbase[si]);
            }
        }
    }
    free(symmap);
    free(symbase);
    free(secmap);
    free(secbase);
    free(sh);
    return ok;
}
