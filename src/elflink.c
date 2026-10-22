/* elflink.c: the native backend's linker.
 *
 * The backend's object, the objects, archives and shared libraries named on
 * the command line, then libm and libc, become a position-independent
 * executable for glibc on Linux x86-64: what `cc -pie -z now -z relro` makes
 * of the same inputs, with an entry point of our own (_start, which calls
 * __libc_start_main) in place of the C compiler's startup files. The steps:
 *
 *   1. Resolution. Objects merge into the backend's Obj (obj_merge). Archive
 *      members merge in while they define a symbol still undefined, whatever
 *      the order of the archives (as lld does, not GNU ld's left to right).
 *      What remains comes from the first shared library, in command-line
 *      order, that defines it at its default version.
 *   2. Scanning the relocations decides what each symbol needs: a GOT slot, a
 *      PLT stub, a copy (an imported variable addressed directly, as gcc's
 *      -fPIE code does), dynamic relocations.
 *   3. Layout, in four segments as GNU ld lays them out: headers and the
 *      dynamic linking tables (R); .plt and .text (RX); .rodata,
 *      .eh_frame_hdr and .eh_frame (R); .data.rel.ro, .dynamic and .got,
 *      read-only after relocation (RELRO), then .data and .bss (RW). File
 *      offsets equal addresses, each segment starting on a page.
 *   4. The file: the contents, relocated; the synthesized sections; the
 *      symbol table and section headers, for debuggers and profilers.
 *
 * Everything is bound at startup (BIND_NOW), so there is no lazy binding: an
 * imported function's PLT stub jumps through its GOT slot, which a GLOB_DAT
 * relocation fills. Libraries are linked as needed: one is a DT_NEEDED entry
 * only if a symbol came from it.
 *
 * Not supported, each reported as an error naming the input: thread-local
 * storage, IFUNC symbols, LTO objects, the large code model, absolute 32-bit
 * addresses in code or data (code not compiled as PIC or PIE), .ctors and
 * .dtors, thin archives, and linker script commands beyond GROUP, INPUT,
 * AS_NEEDED and OUTPUT_FORMAT. Such a program is built with `fcc --native -c`
 * and linked by the C compiler. */

/* mmap() and friends under -std=c11. Must precede every #include. */
#define _DEFAULT_SOURCE

#include "elflink.h"
#include "diag.h"

#if !defined(__linux__) || !defined(__x86_64__)

bool elf_link(Obj *o, const LinkArgs *a, const char *path) {
    (void)o; (void)a; (void)path;
    diag_error_simple("--native is supported only on Linux x86-64");
    return false;
}

#else /* Linux x86-64: the linker */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE 0x1000
#define INTERP "/lib64/ld-linux-x86-64.so.2"
#define NPHDR 10

/* ELF constants beyond elfobj.h's. */
enum { ET_REL = 1, ET_DYN = 3 };
enum { SHT_DYNAMIC = 6, SHT_DYNSYM = 11, SHT_INIT_ARRAY = 14, SHT_FINI_ARRAY = 15,
       SHT_PREINIT_ARRAY = 16, SHT_GNU_HASH = 0x6ffffff6, SHT_GNU_VERDEF = 0x6ffffffd,
       SHT_GNU_VERNEED = 0x6ffffffe, SHT_GNU_VERSYM = 0x6fffffff };
enum { SHF_TLS = 0x400 };
enum { STB_GNU_UNIQUE = 10 };
enum { STT_TLS = 6, STT_GNU_IFUNC = 10 };
enum { STV_HIDDEN = 2 };
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_PHDR = 6, PT_GNU_EH_FRAME = 0x6474e550,
       PT_GNU_STACK = 0x6474e551, PT_GNU_RELRO = 0x6474e552 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };
enum { DT_NULL = 0, DT_NEEDED = 1, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_SONAME = 14, DT_DEBUG = 21,
       DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_FLAGS = 30, DT_PREINIT_ARRAY = 32, DT_PREINIT_ARRAYSZ = 33, DT_GNU_HASH = 0x6ffffef5,
       DT_VERSYM = 0x6ffffff0, DT_RELACOUNT = 0x6ffffff9, DT_FLAGS_1 = 0x6ffffffb,
       DT_VERNEED = 0x6ffffffe, DT_VERNEEDNUM = 0x6fffffff };
enum { DF_BIND_NOW = 0x8, DF_1_NOW = 0x1, DF_1_PIE = 0x08000000 };
enum { R_X86_64_NONE = 0, R_X86_64_COPY = 5, R_X86_64_GLOB_DAT = 6, R_X86_64_RELATIVE = 8,
       R_X86_64_GOTPCREL = 9, R_X86_64_32S = 11, R_X86_64_PC64 = 24, R_X86_64_GOTPCRELX = 41 };

/* Where the linker looks for -l libraries after -L and LIBRARY_PATH: GNU ld's
 * list for x86-64, Debian's multiarch directories and the lib64 ones. */
static const char *const SYSTEM_DIRS[] = {
    "/usr/local/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
    "/usr/local/lib64", "/lib64", "/usr/lib64", "/usr/local/lib", "/lib", "/usr/lib",
};

/* ================================================================
 * Bytes
 * ================================================================ */

static uint64_t rd(const unsigned char *p, int w) {
    uint64_t v = 0;
    for (int i = w - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint64_t rd_be(const unsigned char *p, int w) {
    uint64_t v = 0;
    for (int i = 0; i < w; i++) v = (v << 8) | p[i];
    return v;
}

static void wr(unsigned char *p, uint64_t v, int w) {
    for (int i = 0; i < w; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t align_up(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

typedef struct { unsigned char *p; size_t len, cap; } Bytes;

static size_t by_put(Bytes *b, const void *p, size_t n) {
    size_t at = b->len;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
    if (p) memcpy(b->p + b->len, p, n);
    else memset(b->p + b->len, 0, n);
    b->len += n;
    return at;
}

static size_t by_str(Bytes *b, const char *s) { return by_put(b, s, strlen(s) + 1); }

static void by_le(Bytes *b, uint64_t v, int w) {
    unsigned char t[8];
    wr(t, v, w);
    by_put(b, t, (size_t)w);
}

/* ================================================================
 * The link's state
 * ================================================================ */

typedef struct {
    char *path;
    unsigned char *map;
    size_t size;
    dev_t dev;
    ino_t ino;
} File;

typedef struct {
    const char *name;               /* the file, for messages */
    char *needed;                   /* its DT_NEEDED name */
    const unsigned char *sym;       /* .dynsym */
    uint32_t nsym;
    const char *str;                /* .dynstr */
    uint64_t strsz;
    const unsigned char *versym;    /* .gnu.version, or NULL */
    const char **vername;           /* version index -> name; NULL for the base */
    int nvername;
    const unsigned char *hash;      /* .gnu.hash, or NULL */
    uint64_t hashsz;
    bool used;                      /* a symbol came from it */
    uint32_t needed_off;            /* its name in .dynstr */
} Dso;

typedef struct {
    const char *name;
    const unsigned char *b;
    size_t n;
    HMap index;                     /* symbol -> member header offset */
    const char *longnames;
    size_t nlong;
    size_t *loaded;
    int nloaded, caploaded;
} Archive;

enum { IN_DSO, IN_ARCHIVE };
typedef struct { int kind, idx; } Input;

/* The symbol's kind once resolved: defined in a section of the object (its
 * own, or a copy's), absolute (SHN_ABS, or a weak symbol defined nowhere,
 * which is 0), imported from a library, or an address the linker defines. */
enum { K_DEF, K_ABS, K_IMPORT, K_SPECIAL };
enum { SP_NONE, SP_GOT, SP_DYNAMIC, SP_BASE };

typedef struct {
    int dso;                /* 1 + the library it comes from, or 0 */
    uint32_t dsosym;        /* its .dynsym index there */
    bool copy;              /* imported, but its storage is a copy in .bss */
    bool copied;            /* the copy's R_X86_64_COPY is this symbol's */
    int special;            /* SP_*: an address the linker defines */
    int got, plt;           /* GOT slot and PLT stub, 1-based, or 0 */
    int dyn;                /* its .dynsym index, or 0 */
    uint64_t addr;
} SymLink;

/* A .dynsym entry: an imported symbol, or one a copy defines. */
typedef struct {
    const char *name;
    int sym;                /* the object's symbol: the import, or the copy */
    int dso;                /* 1 + the library whose version it carries, or 0 */
    uint32_t dsosym;
    bool def;
    uint32_t hash;
    int ver;
    uint32_t stroff;
} DynSym;

typedef struct { int dso; const char *name; int index; uint32_t stroff; } VerNeed;
typedef struct { int first; char *name; } Origin;   /* symbols from `first` on came from `name` */
typedef struct { uint64_t off; uint32_t type, sym; int64_t addend; } DynRel;

/* An output section: one the linker synthesizes, or a group of the object's
 * sections. */
typedef struct {
    const char *name;
    uint32_t type, link, info;
    uint64_t flags, addr, off, size, align, entsize;
    int *in;                /* the object's sections, in order */
    int nin, capin;
    int shndx;              /* its section header, or 0 when it has none */
} OSec;

static struct {
    Obj *o;
    File *files; int nfile, capfile;
    Dso *dsos; int ndso, capdso;
    Archive *ars; int nar, capar;
    Input *ins; int nin, capin;
    char **dirs; int ndir, capdir;
    Origin *orig; int norig, caporig;

    SymLink *sl;
    int *got; int ngot, capgot;
    int *plt; int nplt, capplt;
    int *copies; int ncopy, capcopy;
    DynSym *dyns; int ndyn, capdyn;
    VerNeed *vn; int nvn, capvn;
    DynRel *rels; int nrel, caprel;
    int nrelative, nsymbolic;
    int copysec, commsec;

    OSec *os; int nos, capos;
    int *secos;             /* object section -> output section, or -1 */
    uint64_t *secaddr, *secoff;
    Bytes dynstr, gnuhash, versym, verneed;
    int nverneed, nfde;
} L;

/* The output sections the linker makes; the rest come from the object. */
static int O_INTERP, O_GNUHASH, O_DYNSYM, O_DYNSTR, O_VERSYM, O_VERNEED, O_RELADYN, O_PLT,
           O_TEXT, O_RODATA, O_EHHDR, O_EHFRAME, O_PREINIT, O_INIT, O_FINI, O_RELRO,
           O_DYNAMIC, O_GOT, O_DATA, O_BSS;

static void unsupported(const char *origin, const char *what) {
    diag_error_simple("%s: %s is not supported by fcc's linker (build an object with "
                      "-c and link it with cc)", origin, what);
}

/* Where the object's symbol i first appeared: NULL for the program itself. */
static const char *origin_of(int i) {
    const char *name = NULL;
    for (int k = 0; k < L.norig && L.orig[k].first <= i; k++) name = L.orig[k].name;
    return name;
}

/* ================================================================
 * Input files
 * ================================================================ */

/* Map a file, once: -2 for a second mention of the same file, -1 if it
 * cannot be read, else its index. */
static int open_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    for (int i = 0; i < L.nfile; i++)
        if (L.files[i].dev == st.st_dev && L.files[i].ino == st.st_ino) { close(fd); return -2; }
    void *m = NULL;
    if (st.st_size > 0) {
        m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) { close(fd); return -1; }
    }
    close(fd);
    File f = { str_dup(path), m, (size_t)st.st_size, st.st_dev, st.st_ino };
    DA_APPEND(L.files, L.nfile, L.capfile, f);
    return L.nfile - 1;
}

static bool supported_reloc(uint32_t t) {
    return t == R_X86_64_NONE || t == R_X86_64_64 || t == R_X86_64_PC32 ||
           t == R_X86_64_PLT32 || t == R_X86_64_GOTPCREL || t == R_X86_64_32 ||
           t == R_X86_64_32S || t == R_X86_64_PC64 || t == R_X86_64_GOTPCRELX ||
           t == R_X86_64_REX_GOTPCRELX;
}

static void unsupported_reloc(const char *origin, uint32_t t) {
    if ((t >= 16 && t <= 23) || (t >= 34 && t <= 36)) unsupported(origin, "thread-local storage");
    else if (t >= 25 && t <= 31) unsupported(origin, "the large code model");
    else {
        char *what = str_sprintf("relocation type %u", t);
        unsupported(origin, what);
        free(what);
    }
}

/* Reject, naming the input, what the link cannot carry out: obj_merge would
 * take it in silently. */
static bool check_object(const unsigned char *b, size_t n, const char *origin) {
    if (n < 64 || memcmp(b, "\177ELF\2\1", 6) != 0 || rd(b + 18, 2) != 62) {
        diag_error_simple("%s: not an x86-64 ELF object", origin);
        return false;
    }
    uint64_t shoff = rd(b + 40, 8);
    uint64_t shnum = rd(b + 60, 2), shstrndx = rd(b + 62, 2);
    if (shoff > n || shnum > (n - shoff) / 64 || shstrndx >= shnum) {
        diag_error_simple("%s: truncated section headers", origin);
        return false;
    }
    const unsigned char *sh = b + shoff;
    for (uint64_t i = 0; i < shnum; i++) {
        const unsigned char *h = sh + 64 * i;
        uint64_t off = rd(h + 24, 8), size = rd(h + 32, 8);
        if (rd(h + 4, 4) != SHT_NOBITS && (off > n || size > n - off)) {
            diag_error_simple("%s: a section lies outside the file", origin);
            return false;
        }
    }
    const unsigned char *strs = sh + 64 * shstrndx;
    uint64_t stroff = rd(strs + 24, 8), strsz = rd(strs + 32, 8);
    for (uint64_t i = 1; i < shnum; i++) {
        const unsigned char *h = sh + 64 * i;
        uint64_t nm = rd(h, 4), flags = rd(h + 8, 8);
        const char *name = nm < strsz && memchr(b + stroff + nm, 0, strsz - nm)
                         ? (const char *)b + stroff + nm : "";
        if (strncmp(name, ".gnu.lto_", 9) == 0) {
            unsupported(origin, "an LTO object (compiled with -flto), which holds no machine code,");
            return false;
        }
        if (!(flags & SHF_ALLOC)) continue;
        if (flags & SHF_TLS) {
            unsupported(origin, "thread-local storage");
            return false;
        }
        if (strncmp(name, ".ctors", 6) == 0 || strncmp(name, ".dtors", 6) == 0) {
            unsupported(origin, "a .ctors or .dtors section");
            return false;
        }
    }
    for (uint64_t i = 1; i < shnum; i++) {
        const unsigned char *h = sh + 64 * i;
        uint32_t type = (uint32_t)rd(h + 4, 4);
        uint64_t off = rd(h + 24, 8), size = rd(h + 32, 8);
        if (type == SHT_SYMTAB) {
            for (uint64_t k = 24; k + 24 <= size; k += 24) {
                const unsigned char *s = b + off + k;
                int st = s[4] & 0xf;
                if (st == STT_TLS) {
                    unsupported(origin, "thread-local storage");
                    return false;
                }
                if (st == STT_GNU_IFUNC && rd(s + 6, 2) != SHN_UNDEF) {
                    unsupported(origin, "an IFUNC symbol");
                    return false;
                }
            }
        } else if (type == SHT_RELA) {
            uint64_t target = rd(h + 44, 4);
            if (target >= shnum || !(rd(sh + 64 * target + 8, 8) & SHF_ALLOC)) continue;
            for (uint64_t k = 0; k + 24 <= size; k += 24) {
                uint32_t t = (uint32_t)rd(b + off + k + 8, 4);
                if (t == R_X86_64_32 || t == R_X86_64_32S) {
                    diag_error_simple("%s: absolute 32-bit addresses, which a position-independent "
                                      "executable cannot have (compile it with -fPIC or -fPIE)", origin);
                    return false;
                }
                if (!supported_reloc(t)) {
                    unsupported_reloc(origin, t);
                    return false;
                }
            }
        }
    }
    return true;
}

/* Merge an object; `origin` (owned) names it in messages. */
static bool merge_object(const unsigned char *b, size_t n, char *origin) {
    DA_APPEND(L.orig, L.norig, L.caporig, ((Origin){ L.o->nsym, origin }));
    if (!check_object(b, n, origin)) return false;
    char *err = NULL;
    if (!obj_merge(L.o, b, n, &err)) {
        diag_error_simple("%s: %s", origin, err);
        free(err);
        return false;
    }
    return true;
}

/* ---- Shared libraries ---- */

static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

static const unsigned char *dsym(const Dso *d, uint32_t i) { return d->sym + 24 * (size_t)i; }

static int dso_verindex(const Dso *d, uint32_t i) {
    return d->versym ? (int)rd(d->versym + 2 * (size_t)i, 2) : 1;
}

/* The version a symbol of the library has, or NULL for none. */
static const char *dso_version(const Dso *d, uint32_t i) {
    int v = dso_verindex(d, i) & 0x7fff;
    return v >= 2 && v < d->nvername ? d->vername[v] : NULL;
}

static const char *dso_name(const Dso *d, uint32_t i) {
    uint64_t nm = rd(dsym(d, i), 4);
    return nm < d->strsz ? d->str + nm : "";
}

/* Symbol i is a definition of `name` that a link can bind to: defined,
 * global, at its default version. */
static bool dso_match(const Dso *d, uint32_t i, const char *name) {
    const unsigned char *s = dsym(d, i);
    int bind = s[4] >> 4;
    if (rd(s + 6, 2) == SHN_UNDEF) return false;
    if (bind != STB_GLOBAL && bind != STB_WEAK && bind != STB_GNU_UNIQUE) return false;
    int v = dso_verindex(d, i);
    if ((v & 0x8000) || v == 0) return false;
    return strcmp(dso_name(d, i), name) == 0;
}

/* The library's definition of `name`, or 0. */
static uint32_t dso_find(const Dso *d, const char *name) {
    if (d->hash) {
        uint32_t nb = (uint32_t)rd(d->hash, 4), symoff = (uint32_t)rd(d->hash + 4, 4);
        uint32_t bloom = (uint32_t)rd(d->hash + 8, 4);
        const unsigned char *buckets = d->hash + 16 + 8 * (size_t)bloom;
        const unsigned char *chain = buckets + 4 * (size_t)nb;
        uint64_t nchain = (d->hashsz - (uint64_t)(chain - d->hash)) / 4;
        uint32_t h = gnu_hash(name);
        uint32_t i = (uint32_t)rd(buckets + 4 * (size_t)(h % nb), 4);
        if (i < symoff) return 0;
        for (; i < d->nsym && i - symoff < nchain; i++) {
            uint32_t h2 = (uint32_t)rd(chain + 4 * (size_t)(i - symoff), 4);
            if ((h | 1) == (h2 | 1) && dso_match(d, i, name)) return i;
            if (h2 & 1) break;
        }
        return 0;
    }
    for (uint32_t i = 1; i < d->nsym; i++)
        if (dso_match(d, i, name)) return i;
    return 0;
}

static bool add_dso(const char *path, const unsigned char *b, size_t n, bool searched) {
    uint64_t shoff = rd(b + 40, 8), shnum = rd(b + 60, 2);
    if (shoff > n || shnum > (n - shoff) / 64) {
        diag_error_simple("%s: truncated section headers", path);
        return false;
    }
    const unsigned char *sh = b + shoff;
    Dso d = { 0 };
    d.name = path;
    const unsigned char *dyn = NULL, *verdef = NULL;
    uint64_t dynsz = 0, verdefsz = 0, strsec = 0, versymsz = 0;
    for (uint64_t i = 0; i < shnum; i++) {
        const unsigned char *h = sh + 64 * i;
        uint32_t type = (uint32_t)rd(h + 4, 4);
        uint64_t off = rd(h + 24, 8), size = rd(h + 32, 8);
        if (type == SHT_NOBITS) continue;
        if (off > n || size > n - off) {
            diag_error_simple("%s: a section lies outside the file", path);
            return false;
        }
        switch (type) {
        case SHT_DYNSYM: d.sym = b + off; d.nsym = (uint32_t)(size / 24); strsec = rd(h + 40, 4); break;
        case SHT_GNU_VERSYM: d.versym = b + off; versymsz = size; break;
        case SHT_GNU_VERDEF: verdef = b + off; verdefsz = size; break;
        case SHT_GNU_HASH: d.hash = b + off; d.hashsz = size; break;
        case SHT_DYNAMIC: dyn = b + off; dynsz = size; break;
        }
    }
    if (!d.sym || strsec >= shnum) {
        diag_error_simple("%s: a shared library without a dynamic symbol table", path);
        return false;
    }
    d.str = (const char *)b + rd(sh + 64 * strsec + 24, 8);
    d.strsz = rd(sh + 64 * strsec + 32, 8);
    if (d.strsz == 0 || d.str[d.strsz - 1] != 0) {
        diag_error_simple("%s: a malformed string table", path);
        return false;
    }
    if (d.versym && versymsz < 2 * (uint64_t)d.nsym) d.versym = NULL;
    if (d.hash && (d.hashsz < 16 || rd(d.hash, 4) == 0 ||
                   16 + 8 * rd(d.hash + 8, 4) + 4 * rd(d.hash, 4) > d.hashsz))
        d.hash = NULL;

    const char *soname = NULL;
    for (uint64_t k = 0; dyn && k + 16 <= dynsz; k += 16) {
        uint64_t tag = rd(dyn + k, 8), val = rd(dyn + k + 8, 8);
        if (tag == DT_NULL) break;
        if (tag == DT_SONAME && val < d.strsz) soname = d.str + val;
    }
    const char *base = strrchr(path, '/');
    d.needed = str_dup(soname ? soname : searched && base ? base + 1 : path);

    /* The version definitions: index -> name, the base (the library's own
     * name) left out, since a symbol at it is unversioned. */
    for (uint64_t off = 0; verdef && off + 20 <= verdefsz; ) {
        const unsigned char *v = verdef + off;
        int flags = (int)rd(v + 2, 2), ndx = (int)(rd(v + 4, 2) & 0x7fff);
        uint64_t aux = rd(v + 12, 4), next = rd(v + 16, 4);
        if (!(flags & 1) && off + aux + 8 <= verdefsz) {
            uint64_t nm = rd(v + aux, 4);
            if (nm < d.strsz) {
                if (ndx >= d.nvername) {
                    int nn = ndx + 8;
                    d.vername = xrealloc(d.vername, (size_t)nn * sizeof(char *));
                    for (int k = d.nvername; k < nn; k++) d.vername[k] = NULL;
                    d.nvername = nn;
                }
                d.vername[ndx] = d.str + nm;
            }
        }
        if (!next) break;
        off += next;
    }

    DA_APPEND(L.dsos, L.ndso, L.capdso, d);
    DA_APPEND(L.ins, L.nin, L.capin, ((Input){ IN_DSO, L.ndso - 1 }));
    return true;
}

/* ---- Archives ---- */

static uint64_t ar_size(const unsigned char *h) {
    uint64_t v = 0;
    for (int i = 48; i < 58 && h[i] >= '0' && h[i] <= '9'; i++) v = v * 10 + (uint64_t)(h[i] - '0');
    return v;
}

/* The symbol index: big-endian, w-byte count and member offsets, then the
 * names. */
static bool ar_index(Archive *a, const unsigned char *p, uint64_t size, int w) {
    if (size < (uint64_t)w) return false;
    uint64_t count = rd_be(p, w);
    if (count > (size - (uint64_t)w) / (uint64_t)w) return false;
    const unsigned char *nm = p + w + w * count, *end = p + size;
    for (uint64_t k = 0; k < count; k++) {
        const unsigned char *z = memchr(nm, 0, (size_t)(end - nm));
        if (!z) return false;
        HEnt *e = hmap_put(&a->index, (const char *)nm, (size_t)(z - nm));
        if (!e->val) e->val = (int64_t)rd_be(p + w + w * k, w);
        nm = z + 1;
    }
    return true;
}

static bool add_archive(const char *path, const unsigned char *b, size_t n) {
    Archive a = { 0 };
    a.name = path;
    a.b = b;
    a.n = n;
    bool indexed = false, members = false;
    for (size_t off = 8; off + 60 <= n; ) {
        const unsigned char *h = b + off;
        uint64_t size = ar_size(h);
        if (memcmp(h + 58, "`\n", 2) != 0 || size > n - off - 60) {
            diag_error_simple("%s: a malformed archive", path);
            hmap_free(&a.index);
            return false;
        }
        const unsigned char *data = h + 60;
        bool ok = true;
        if (h[0] == '/' && h[1] == ' ') { ok = ar_index(&a, data, size, 4); indexed = true; }
        else if (memcmp(h, "/SYM64/ ", 8) == 0) { ok = ar_index(&a, data, size, 8); indexed = true; }
        else if (h[0] == '/' && h[1] == '/') { a.longnames = (const char *)data; a.nlong = size; }
        else members = true;
        if (!ok) {
            diag_error_simple("%s: a malformed archive symbol index", path);
            hmap_free(&a.index);
            return false;
        }
        off += 60 + size + (size & 1);
    }
    if (members && !indexed) {
        diag_error_simple("%s: the archive has no symbol index (run ranlib on it)", path);
        return false;
    }
    DA_APPEND(L.ars, L.nar, L.capar, a);
    DA_APPEND(L.ins, L.nin, L.capin, ((Input){ IN_ARCHIVE, L.nar - 1 }));
    return true;
}

static char *member_name(const Archive *a, const unsigned char *h) {
    if (h[0] == '/' && h[1] >= '0' && h[1] <= '9' && a->longnames) {
        size_t at = 0;
        for (int i = 1; i < 16 && h[i] >= '0' && h[i] <= '9'; i++) at = at * 10 + (size_t)(h[i] - '0');
        if (at < a->nlong) {
            const char *s = a->longnames + at;
            size_t k = 0;
            while (at + k < a->nlong && s[k] != '/' && s[k] != '\n') k++;
            return str_ndup(s, k);
        }
    }
    size_t k = 0;
    while (k < 16 && h[k] != '/' && h[k] != ' ') k++;
    return str_ndup((const char *)h, k);
}

static bool load_member(Archive *a, size_t off) {
    DA_APPEND(a->loaded, a->nloaded, a->caploaded, off);
    if (off < 8 || off + 60 > a->n || memcmp(a->b + off + 58, "`\n", 2) != 0 ||
        ar_size(a->b + off) > a->n - off - 60) {
        diag_error_simple("%s: the symbol index names no member", a->name);
        return false;
    }
    const unsigned char *h = a->b + off;
    char *mem = member_name(a, h);
    char *origin = str_sprintf("%s(%s)", a->name, mem);
    free(mem);
    return merge_object(h + 60, (size_t)ar_size(h), origin);
}

/* ---- Linker scripts ---- */

static int load_file(const char *path, bool searching, int depth);

/* -lNAME: libNAME.so, else libNAME.a, in each directory in turn; -l:FILE
 * names the file. An ELF file for another machine is passed over. */
static bool find_lib(const char *name, int depth) {
    for (int d = 0; d < L.ndir; d++) {
        for (int k = 0; k < 2; k++) {
            char *cand = name[0] == ':' ? (k ? NULL : str_sprintf("%s/%s", L.dirs[d], name + 1))
                                        : str_sprintf("%s/lib%s%s", L.dirs[d], name, k ? ".a" : ".so");
            if (!cand) continue;
            int r = access(cand, R_OK) == 0 ? load_file(cand, true, depth) : 0;
            free(cand);
            if (r != 0) return r > 0;
        }
    }
    diag_error_simple("cannot find -l%s", name);
    return false;
}

static bool script_item(const char *script, const char *t, int depth) {
    if (t[0] == '-' && t[1] == 'l') return find_lib(t + 2, depth);
    const char *p = t[0] == '=' ? t + 1 : t;        /* in the sysroot, which is / */
    if (p[0] == '/' || access(p, F_OK) == 0) return load_file(p, false, depth) > 0;
    for (int d = 0; d < L.ndir; d++) {
        char *cand = str_sprintf("%s/%s", L.dirs[d], p);
        int r = access(cand, R_OK) == 0 ? load_file(cand, false, depth) : 0;
        free(cand);
        if (r != 0) return r > 0;
    }
    diag_error_simple("%s: cannot find '%s'", script, t);
    return false;
}

/* A GNU ld script of the kind libc.so is: GROUP, INPUT and AS_NEEDED lists
 * of files and -l names, and OUTPUT_FORMAT. Every library is linked as
 * needed anyway, so AS_NEEDED changes nothing. */
static bool run_script(const char *path, const unsigned char *b, size_t n, int depth) {
    for (size_t i = 0; i < n; i++)
        if (b[i] == 0 || (b[i] < 32 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t')) {
            diag_error_simple("%s: not an object file, archive, shared library or linker script",
                              path);
            return false;
        }
    char **tok = NULL;
    int ntok = 0, captok = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = b[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && b[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(b[i] == '*' && b[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        size_t s = i;
        if (c == '(' || c == ')' || c == ',') i++;
        else
            while (i < n && !strchr(" \t\n\r(),", b[i])) i++;
        DA_APPEND(tok, ntok, captok, str_ndup((const char *)b + s, i - s));
    }
    bool ok = true;
    for (int i = 0; i < ntok && ok; ) {
        const char *cmd = tok[i++];
        bool files = strcmp(cmd, "GROUP") == 0 || strcmp(cmd, "INPUT") == 0;
        if (!files && strcmp(cmd, "OUTPUT_FORMAT") != 0) {
            char *what = str_sprintf("the linker script command '%s'", cmd);
            unsupported(path, what);
            free(what);
            ok = false;
            break;
        }
        if (i >= ntok || strcmp(tok[i], "(") != 0) {
            diag_error_simple("%s: '(' expected after %s", path, cmd);
            ok = false;
            break;
        }
        i++;
        for (int level = 1; i < ntok && level > 0 && ok; i++) {
            const char *t = tok[i];
            if (strcmp(t, "(") == 0) level++;
            else if (strcmp(t, ")") == 0) level--;
            else if (files && strcmp(t, ",") != 0 && strcmp(t, "AS_NEEDED") != 0)
                ok = script_item(path, t, depth);
        }
    }
    for (int i = 0; i < ntok; i++) free(tok[i]);
    free(tok);
    return ok;
}

/* Load an input: 1 when loaded (or loaded before), 0 when `searching` and it
 * is an ELF file for another machine, -1 after reporting an error. */
static int load_file(const char *path, bool searching, int depth) {
    int fi = open_file(path);
    if (fi == -2) return 1;
    if (fi < 0) {
        diag_error_simple("cannot read '%s'", path);
        return -1;
    }
    const char *name = L.files[fi].path;
    const unsigned char *b = L.files[fi].map;
    size_t n = L.files[fi].size;
    if (n >= 8 && memcmp(b, "!<arch>\n", 8) == 0) return add_archive(name, b, n) ? 1 : -1;
    if (n >= 8 && memcmp(b, "!<thin>\n", 8) == 0) {
        unsupported(name, "a thin archive");
        return -1;
    }
    if (n >= 4 && memcmp(b, "\177ELF", 4) == 0) {
        if (n < 64 || b[4] != 2 || b[5] != 1 || rd(b + 18, 2) != 62) {
            if (searching) return 0;
            diag_error_simple("%s: not an x86-64 ELF file", name);
            return -1;
        }
        uint64_t type = rd(b + 16, 2);
        if (type == ET_REL) return merge_object(b, n, str_dup(name)) ? 1 : -1;
        if (type == ET_DYN) return add_dso(name, b, n, searching) ? 1 : -1;
        diag_error_simple("%s: neither an object file nor a shared library", name);
        return -1;
    }
    if (depth >= 8) {
        diag_error_simple("%s: linker scripts nested too deeply", name);
        return -1;
    }
    return run_script(name, b, n, depth + 1) ? 1 : -1;
}

/* ================================================================
 * Resolution
 * ================================================================ */

/* The entry point, as glibc's crt1 has it: __libc_start_main(main, argc,
 * argv, init, fini, rtld_fini, stack_end), with no init or fini (glibc 2.34
 * and later run the constructors themselves). */
static void add_start(void) {
    static const unsigned char code[] = {
        0x31, 0xed,                         /* xor %ebp,%ebp: the outermost frame */
        0x49, 0x89, 0xd1,                   /* mov %rdx,%r9: rtld_fini */
        0x5e,                               /* pop %rsi: argc */
        0x48, 0x89, 0xe2,                   /* mov %rsp,%rdx: argv */
        0x48, 0x83, 0xe4, 0xf0,             /* and $-16,%rsp */
        0x50,                               /* push %rax */
        0x54,                               /* push %rsp: stack_end */
        0x45, 0x31, 0xc0,                   /* xor %r8d,%r8d: fini */
        0x31, 0xc9,                         /* xor %ecx,%ecx: init */
        0x48, 0x8b, 0x3d, 0, 0, 0, 0,       /* mov main@GOTPCREL(%rip),%rdi */
        0xff, 0x15, 0, 0, 0, 0,             /* call *__libc_start_main@GOTPCREL(%rip) */
        0xf4,                               /* hlt */
    };
    Obj *o = L.o;
    if (obj_find(o, "_start") && o->syms[obj_find(o, "_start")].sec != SHN_UNDEF) {
        diag_error_simple("the program defines _start, the linker's entry point");
        return;
    }
    int sec = obj_section(o, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16);
    obj_put(o, sec, code, sizeof code);
    int start = obj_find(o, "_start");
    if (start) {
        o->syms[start].sec = sec;
        o->syms[start].size = sizeof code;
        o->syms[start].type = STT_FUNC;
    } else {
        obj_symbol(o, "_start", sec, 0, sizeof code, STT_FUNC, STB_GLOBAL);
    }
    int main_sym = obj_find(o, "main");
    if (!main_sym) main_sym = obj_symbol(o, "main", SHN_UNDEF, 0, 0, STT_NOTYPE, STB_GLOBAL);
    int lsm = obj_find(o, "__libc_start_main");
    if (!lsm) lsm = obj_symbol(o, "__libc_start_main", SHN_UNDEF, 0, 0, STT_NOTYPE, STB_GLOBAL);
    obj_reloc(o, sec, 23, R_X86_64_REX_GOTPCRELX, main_sym, -4);
    obj_reloc(o, sec, 29, R_X86_64_GOTPCRELX, lsm, -4);
}

static bool member_loaded(const Archive *a, size_t off) {
    for (int i = 0; i < a->nloaded; i++) if (a->loaded[i] == off) return true;
    return false;
}

/* Merge archive members while one defines a symbol that is still undefined
 * and that no library before the archive defines. A weak reference does not
 * pull a member in. */
static void resolve_archives(void) {
    Obj *o = L.o;
    bool pulled = true;
    while (pulled && diag_error_count() == 0) {
        pulled = false;
        for (int i = 1; i < o->nsym; i++) {
            ObjSymbol *s = &o->syms[i];
            if (s->bind != STB_GLOBAL || !s->name || s->sec != SHN_UNDEF) continue;
            const char *name = s->name;
            for (int k = 0; k < L.nin; k++) {
                Input *in = &L.ins[k];
                if (in->kind == IN_DSO) {
                    if (dso_find(&L.dsos[in->idx], name)) break;
                    continue;
                }
                Archive *a = &L.ars[in->idx];
                HEnt *e = hmap_get(&a->index, name, strlen(name));
                if (!e || member_loaded(a, (size_t)e->val)) continue;
                load_member(a, (size_t)e->val);
                pulled = true;
                break;
            }
        }
    }
}

static int kind(int i) {
    const ObjSymbol *s = &L.o->syms[i];
    const SymLink *l = &L.sl[i];
    if (l->special) return K_SPECIAL;
    if (s->sec > 0 && s->sec < L.o->nsec) return K_DEF;
    if (l->dso) return K_IMPORT;
    return K_ABS;
}

/* A symbol the linker defines, as GNU ld and the C startup files do. */
static bool linker_symbol(int i) {
    Obj *o = L.o;
    const char *name = o->syms[i].name;
    if (strcmp(name, "__dso_handle") == 0) {
        /* The handle __cxa_atexit files an executable's handlers under: 0,
         * as crtbegin.o defines it for an executable. */
        int sec = obj_section(o, ".rodata", SHT_PROGBITS, SHF_ALLOC, 8);
        obj_put(o, sec, NULL, 8);
        ObjSymbol *s = &o->syms[i];
        s->sec = sec; s->value = 0; s->size = 8; s->type = STT_OBJECT; s->other = STV_HIDDEN;
        return true;
    }
    int sp = strcmp(name, "_GLOBAL_OFFSET_TABLE_") == 0 ? SP_GOT
           : strcmp(name, "_DYNAMIC") == 0 ? SP_DYNAMIC
           : strcmp(name, "__ehdr_start") == 0 || strcmp(name, "__executable_start") == 0 ? SP_BASE
           : SP_NONE;
    L.sl[i].special = sp;
    return sp != SP_NONE;
}

/* What is still undefined comes from the libraries. */
static void resolve_imports(void) {
    Obj *o = L.o;
    for (int i = 1; i < o->nsym; i++) {
        ObjSymbol *s = &o->syms[i];
        if (s->bind == STB_LOCAL || !s->name || s->sec != SHN_UNDEF) continue;
        if (linker_symbol(i)) continue;
        s = &o->syms[i];
        bool found = false;
        for (int k = 0; k < L.nin && !found; k++) {
            if (L.ins[k].kind != IN_DSO) continue;
            Dso *d = &L.dsos[L.ins[k].idx];
            uint32_t j = dso_find(d, s->name);
            if (!j) continue;
            found = true;
            if ((dsym(d, j)[4] & 0xf) == STT_TLS) {
                char *what = str_sprintf("the thread-local variable '%s'", s->name);
                unsupported(d->name, what);
                free(what);
                break;
            }
            L.sl[i].dso = L.ins[k].idx + 1;
            L.sl[i].dsosym = j;
            d->used = true;
        }
        if (found || s->bind == STB_WEAK) continue;
        const char *from = origin_of(i);
        if (from) diag_error_simple("%s: undefined symbol '%s'", from, s->name);
        else diag_error_simple("undefined symbol '%s' (is a -l library missing?)", s->name);
    }
}

/* Common symbols (tentative definitions, from -fcommon) get .bss space. */
static void allocate_commons(void) {
    Obj *o = L.o;
    for (int i = 1; i < o->nsym; i++) {
        if (o->syms[i].sec != SHN_COMMON) continue;
        if (!L.commsec) L.commsec = obj_section(o, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 1);
        ObjSymbol *s = &o->syms[i];
        obj_align(o, L.commsec, s->value ? s->value : 1);
        s->value = obj_put(o, L.commsec, NULL, s->size);
        s->sec = L.commsec;
    }
}

/* ================================================================
 * Relocation scan
 * ================================================================ */

enum { RELAX_NONE, RELAX_LEA, RELAX_CALL, RELAX_JMP };

/* A load from the GOT slot of a symbol defined here becomes a lea of the
 * symbol, and an indirect call or jump through it a direct one: GNU ld's
 * relaxations, which the GOTPCRELX relocations allow. */
static int relax_kind(const ObjSection *s, const ObjReloc *r) {
    if (r->type != R_X86_64_GOTPCRELX && r->type != R_X86_64_REX_GOTPCRELX) return RELAX_NONE;
    int k = kind(r->sym);
    if ((k != K_DEF && k != K_SPECIAL) || r->addend != -4 || r->off < 3 || !s->data)
        return RELAX_NONE;
    unsigned char op = s->data[r->off - 2], modrm = s->data[r->off - 1];
    if (op == 0x8b && (modrm & 0xc7) == 0x05) return RELAX_LEA;
    if (r->type == R_X86_64_GOTPCRELX && op == 0xff && modrm == 0x15) return RELAX_CALL;
    if (r->type == R_X86_64_GOTPCRELX && op == 0xff && modrm == 0x25) return RELAX_JMP;
    return RELAX_NONE;
}

static bool is_branch(const ObjSection *s, uint64_t off) {
    if (!s->data || off < 2) return false;
    unsigned char a = s->data[off - 2], b = s->data[off - 1];
    return b == 0xe8 || b == 0xe9 || (a == 0x0f && (b & 0xf0) == 0x80);
}

static void need_dyn(int i) {
    if (L.sl[i].dyn) return;
    DynSym d = { L.o->syms[i].name, i, L.sl[i].dso, L.sl[i].dsosym, false, 0, 0, 0 };
    DA_APPEND(L.dyns, L.ndyn, L.capdyn, d);
    L.sl[i].dyn = -1;           /* numbered once the table is ordered */
}

static void need_got(int i) {
    if (L.sl[i].got) return;
    DA_APPEND(L.got, L.ngot, L.capgot, i);
    L.sl[i].got = L.ngot;
    int k = kind(i);
    if (k == K_IMPORT) { need_dyn(i); L.nsymbolic++; }
    else if (k != K_ABS) L.nrelative++;
}

static void need_plt(int i) {
    need_got(i);
    if (L.sl[i].plt) return;
    DA_APPEND(L.plt, L.nplt, L.capplt, i);
    L.sl[i].plt = L.nplt;
}

static void export_copy(const char *name, int sym, int dso, uint32_t dsosym) {
    DynSym d = { name, sym, dso, dsosym, true, gnu_hash(name), 0, 0 };
    DA_APPEND(L.dyns, L.ndyn, L.capdyn, d);
}

/* Give an imported variable storage of its own in .bss, which the dynamic
 * linker fills from the library's (R_X86_64_COPY) and which the library then
 * uses too: the executable exports it under every name the library has for
 * it (environ, __environ, ...). */
static void make_copy(int i) {
    Obj *o = L.o;
    SymLink *l = &L.sl[i];
    const Dso *d = &L.dsos[l->dso - 1];
    const unsigned char *ds = dsym(d, l->dsosym);
    uint64_t value = rd(ds + 8, 8), size = rd(ds + 16, 8), shndx = rd(ds + 6, 2);
    if (!size) {
        diag_error_simple("%s: cannot copy the variable '%s', whose size is unknown "
                          "(compile its user with -fPIC)", d->name, o->syms[i].name);
        return;
    }
    uint64_t align = value & (~value + 1);
    if (!align || align > 64) align = 64;
    if (!L.copysec) L.copysec = obj_section(o, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 1);
    obj_align(o, L.copysec, align);
    uint64_t at = obj_put(o, L.copysec, NULL, size);
    ObjSymbol *s = &o->syms[i];
    s->sec = L.copysec; s->value = at; s->size = size; s->type = STT_OBJECT;
    l->copy = l->copied = true;
    DA_APPEND(L.copies, L.ncopy, L.capcopy, i);
    export_copy(s->name, i, l->dso, l->dsosym);
    for (uint32_t j = 1; j < d->nsym; j++) {
        const unsigned char *a = dsym(d, j);
        if (j == l->dsosym || rd(a + 8, 8) != value || rd(a + 6, 2) != shndx) continue;
        const char *name = dso_name(d, j);
        if (!dso_match(d, j, name)) continue;
        int k = obj_find(o, name);
        if (k && L.sl[k].dso == l->dso && kind(k) == K_IMPORT) {
            ObjSymbol *ks = &o->syms[k];
            ks->sec = L.copysec; ks->value = at; ks->size = size; ks->type = STT_OBJECT;
            L.sl[k].copy = true;
            export_copy(name, k, l->dso, j);
        } else {
            export_copy(name, i, l->dso, j);
        }
    }
}

/* The sections the executable keeps: allocated ones, and the debug
 * information. */
static bool kept(int sec) {
    const ObjSection *s = &L.o->secs[sec];
    return (s->flags & SHF_ALLOC) || strncmp(s->name, ".debug_", 7) == 0;
}

static void scan_relocs(void) {
    Obj *o = L.o;
    /* Copies first: they make imported variables local for the rest. */
    for (int si = 1; si < o->nsec; si++) {
        ObjSection *s = &o->secs[si];
        if (!(s->flags & SHF_ALLOC)) continue;
        for (int ri = 0; ri < s->nrel; ri++) {
            ObjReloc *r = &s->rel[ri];
            if ((r->type != R_X86_64_PC32 && r->type != R_X86_64_PC64) || kind(r->sym) != K_IMPORT)
                continue;
            const Dso *d = &L.dsos[L.sl[r->sym].dso - 1];
            int t = dsym(d, L.sl[r->sym].dsosym)[4] & 0xf;
            if (t != STT_FUNC && t != STT_GNU_IFUNC) make_copy(r->sym);
            else if (r->type != R_X86_64_PC32 || !is_branch(s, r->off))
                diag_error_simple("the address of the library function '%s' is taken PC-relative "
                                  "in %s (compile it with -fPIC or -fPIE)", o->syms[r->sym].name, s->name);
        }
    }
    for (int si = 1; si < o->nsec; si++) {
        ObjSection *s = &o->secs[si];
        if (!(s->flags & SHF_ALLOC)) continue;
        if (s->flags & SHF_TLS) {
            unsupported(s->name, "thread-local storage");
            continue;
        }
        /* Objects from the command line were checked as they came in; this
         * catches what the program's own object (and its shim) might hold,
         * once per section. */
        for (int ri = 0; ri < s->nrel; ri++) {
            ObjReloc *r = &s->rel[ri];
            int k = kind(r->sym);
            if (k == K_DEF && !kept(o->syms[r->sym].sec)) {
                diag_error_simple("%s refers to the discarded section %s", s->name,
                                  o->secs[o->syms[r->sym].sec].name);
                break;
            }
            if (r->type == R_X86_64_64 && k != K_ABS && !(s->flags & SHF_WRITE)) {
                diag_error_simple("an address in the read-only section %s needs a dynamic "
                                  "relocation (compile it with -fPIC or -fPIE)", s->name);
                break;
            }
            if (r->type == R_X86_64_32 || r->type == R_X86_64_32S) {
                diag_error_simple("an absolute 32-bit address in %s (compile it with -fPIC or "
                                  "-fPIE)", s->name);
                break;
            }
            if (!supported_reloc(r->type)) {
                unsupported_reloc(s->name, r->type);
                break;
            }
            switch (r->type) {
            case R_X86_64_64:
                if (k == K_ABS) break;
                if (k == K_IMPORT) { need_dyn(r->sym); L.nsymbolic++; }
                else L.nrelative++;
                break;
            case R_X86_64_PC32: case R_X86_64_PC64: case R_X86_64_PLT32:
                if (k == K_IMPORT) need_plt(r->sym);
                break;
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX:
                if (!relax_kind(s, r)) need_got(r->sym);
                break;
            }
        }
    }
    L.nsymbolic += L.ncopy;
}

/* ================================================================
 * The dynamic linking tables
 * ================================================================ */


static uint32_t elf_hash(const char *s) {
    uint32_t h = 0;
    for (; *s; s++) {
        h = (h << 4) + (unsigned char)*s;
        uint32_t g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t g_nbucket;

/* The .gnu.version index of a library's version, 1 (none) if it has none. */
static int version_index(int dso, const char *name) {
    if (!name) return 1;
    for (int k = 0; k < L.nvn; k++)
        if (L.vn[k].dso == dso && strcmp(L.vn[k].name, name) == 0) return L.vn[k].index;
    return 1;
}

static int cmp_export(const void *a, const void *b) {
    const DynSym *x = a, *y = b;
    uint32_t bx = x->hash % g_nbucket, by = y->hash % g_nbucket;
    if (bx != by) return bx < by ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* .dynsym's order (the null entry, the imports, then the exported copies by
 * hash bucket), .dynstr, .gnu.hash, .gnu.version and .gnu.version_r: none of
 * them depends on an address. */
static void build_dynamic_tables(void) {
    DynSym *tab = xcalloc((size_t)L.ndyn + 1, sizeof(DynSym));
    int n = 1;
    for (int k = 0; k < L.ndyn; k++) if (!L.dyns[k].def) tab[n++] = L.dyns[k];
    int first_exp = n;
    for (int k = 0; k < L.ndyn; k++) if (L.dyns[k].def) tab[n++] = L.dyns[k];
    uint32_t nexp = (uint32_t)(n - first_exp);
    g_nbucket = nexp / 4 + 1;
    qsort(tab + first_exp, nexp, sizeof(DynSym), cmp_export);
    free(L.dyns);
    L.dyns = tab;
    L.ndyn = L.capdyn = n;
    for (int k = 1; k < n; k++) {
        DynSym *d = &L.dyns[k];
        if (!d->def || strcmp(d->name, L.o->syms[d->sym].name) == 0) L.sl[d->sym].dyn = k;
    }

    /* The versions the symbols carry, by library in DT_NEEDED order. */
    for (int k = 0; k < L.nin; k++) {
        if (L.ins[k].kind != IN_DSO) continue;
        int dso = L.ins[k].idx + 1;
        for (int j = 1; j < n; j++) {
            if (L.dyns[j].dso != dso) continue;
            const char *v = dso_version(&L.dsos[dso - 1], L.dyns[j].dsosym);
            if (!v || version_index(dso, v) != 1) continue;
            VerNeed vn = { dso, v, L.nvn + 2, 0 };
            DA_APPEND(L.vn, L.nvn, L.capvn, vn);
        }
    }
    for (int j = 1; j < n; j++) {
        DynSym *d = &L.dyns[j];
        d->ver = d->dso ? version_index(d->dso, dso_version(&L.dsos[d->dso - 1], d->dsosym)) : 1;
    }

    by_put(&L.dynstr, "", 1);
    for (int k = 0; k < L.nin; k++) {
        if (L.ins[k].kind != IN_DSO) continue;
        Dso *d = &L.dsos[L.ins[k].idx];
        if (d->used) d->needed_off = (uint32_t)by_str(&L.dynstr, d->needed);
    }
    for (int j = 1; j < n; j++) L.dyns[j].stroff = (uint32_t)by_str(&L.dynstr, L.dyns[j].name);
    for (int k = 0; k < L.nvn; k++) L.vn[k].stroff = (uint32_t)by_str(&L.dynstr, L.vn[k].name);

    /* .gnu.hash: one bloom word, shift 6. */
    uint64_t bloom = 0;
    for (int j = first_exp; j < n; j++) {
        uint32_t h = L.dyns[j].hash;
        bloom |= (uint64_t)1 << (h % 64) | (uint64_t)1 << ((h >> 6) % 64);
    }
    by_le(&L.gnuhash, g_nbucket, 4);
    by_le(&L.gnuhash, (uint64_t)first_exp, 4);
    by_le(&L.gnuhash, 1, 4);
    by_le(&L.gnuhash, 6, 4);
    by_le(&L.gnuhash, bloom, 8);
    for (uint32_t b = 0; b < g_nbucket; b++) {
        int first = 0;
        for (int j = first_exp; j < n && !first; j++) if (L.dyns[j].hash % g_nbucket == b) first = j;
        by_le(&L.gnuhash, (uint64_t)first, 4);
    }
    for (int j = first_exp; j < n; j++) {
        uint32_t h = L.dyns[j].hash;
        bool last = j + 1 == n || L.dyns[j + 1].hash % g_nbucket != h % g_nbucket;
        by_le(&L.gnuhash, (h & ~1u) | (last ? 1u : 0u), 4);
    }

    if (L.nvn) {
        by_le(&L.versym, 0, 2);
        for (int j = 1; j < n; j++) by_le(&L.versym, (uint64_t)L.dyns[j].ver, 2);
        int nlib = 0;
        for (int k = 0; k < L.nvn; k++) if (k == 0 || L.vn[k].dso != L.vn[k - 1].dso) nlib++;
        int lib = 0;
        for (int k = 0; k < L.nvn; ) {
            int dso = L.vn[k].dso, cnt = 0;
            while (k + cnt < L.nvn && L.vn[k + cnt].dso == dso) cnt++;
            lib++;
            by_le(&L.verneed, 1, 2);                                /* vn_version */
            by_le(&L.verneed, (uint64_t)cnt, 2);
            by_le(&L.verneed, L.dsos[dso - 1].needed_off, 4);
            by_le(&L.verneed, 16, 4);                               /* vn_aux */
            by_le(&L.verneed, lib < nlib ? 16 + 16 * (uint64_t)cnt : 0, 4);
            for (int a = 0; a < cnt; a++) {
                VerNeed *v = &L.vn[k + a];
                by_le(&L.verneed, elf_hash(v->name), 4);
                by_le(&L.verneed, 0, 2);                            /* vna_flags */
                by_le(&L.verneed, (uint64_t)v->index, 2);
                by_le(&L.verneed, v->stroff, 4);
                by_le(&L.verneed, a + 1 < cnt ? 16 : 0, 4);
            }
            k += cnt;
        }
        L.nverneed = nlib;
    }
}

/* The .dynamic entries, with the addresses once there are some. */
static void dynamic_entries(Bytes *b) {
    for (int k = 0; k < L.nin; k++) {
        if (L.ins[k].kind != IN_DSO || !L.dsos[L.ins[k].idx].used) continue;
        by_le(b, DT_NEEDED, 8); by_le(b, L.dsos[L.ins[k].idx].needed_off, 8);
    }
    static const int arrays[3][3] = {
        { 0, DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ },
        { 1, DT_INIT_ARRAY, DT_INIT_ARRAYSZ },
        { 2, DT_FINI_ARRAY, DT_FINI_ARRAYSZ },
    };
    for (int k = 0; k < 3; k++) {
        OSec *s = &L.os[arrays[k][0] == 0 ? O_PREINIT : arrays[k][0] == 1 ? O_INIT : O_FINI];
        if (!s->nin) continue;
        by_le(b, (uint64_t)arrays[k][1], 8); by_le(b, s->addr, 8);
        by_le(b, (uint64_t)arrays[k][2], 8); by_le(b, s->size, 8);
    }
    by_le(b, DT_GNU_HASH, 8); by_le(b, L.os[O_GNUHASH].addr, 8);
    by_le(b, DT_STRTAB, 8); by_le(b, L.os[O_DYNSTR].addr, 8);
    by_le(b, DT_SYMTAB, 8); by_le(b, L.os[O_DYNSYM].addr, 8);
    by_le(b, DT_STRSZ, 8); by_le(b, L.dynstr.len, 8);
    by_le(b, DT_SYMENT, 8); by_le(b, 24, 8);
    by_le(b, DT_DEBUG, 8); by_le(b, 0, 8);
    if (L.nrelative + L.nsymbolic) {
        by_le(b, DT_RELA, 8); by_le(b, L.os[O_RELADYN].addr, 8);
        by_le(b, DT_RELASZ, 8); by_le(b, 24 * (uint64_t)(L.nrelative + L.nsymbolic), 8);
        by_le(b, DT_RELAENT, 8); by_le(b, 24, 8);
    }
    by_le(b, DT_FLAGS, 8); by_le(b, DF_BIND_NOW, 8);
    by_le(b, DT_FLAGS_1, 8); by_le(b, DF_1_NOW | DF_1_PIE, 8);
    if (L.nvn) {
        by_le(b, DT_VERSYM, 8); by_le(b, L.os[O_VERSYM].addr, 8);
        by_le(b, DT_VERNEED, 8); by_le(b, L.os[O_VERNEED].addr, 8);
        by_le(b, DT_VERNEEDNUM, 8); by_le(b, (uint64_t)L.nverneed, 8);
    }
    if (L.nrelative) { by_le(b, DT_RELACOUNT, 8); by_le(b, (uint64_t)L.nrelative, 8); }
    by_le(b, DT_NULL, 8); by_le(b, 0, 8);
}

/* ================================================================
 * Layout
 * ================================================================ */

static int new_os(const char *name, uint32_t type, uint64_t flags, uint64_t align, uint64_t entsize) {
    OSec s = { 0 };
    s.name = name;
    s.type = type;
    s.flags = flags;
    s.align = align;
    s.entsize = entsize;
    DA_APPEND(L.os, L.nos, L.capos, s);
    return L.nos - 1;
}

static int section_group(int i) {
    const ObjSection *s = &L.o->secs[i];
    if (s->type == SHT_PREINIT_ARRAY || strncmp(s->name, ".preinit_array", 14) == 0) return O_PREINIT;
    if (s->type == SHT_INIT_ARRAY || strncmp(s->name, ".init_array", 11) == 0) return O_INIT;
    if (s->type == SHT_FINI_ARRAY || strncmp(s->name, ".fini_array", 11) == 0) return O_FINI;
    if (strcmp(s->name, ".eh_frame") == 0) return O_EHFRAME;
    if (s->flags & SHF_EXECINSTR) return O_TEXT;
    if (!(s->flags & SHF_WRITE)) return O_RODATA;
    if (s->type == SHT_NOBITS) return O_BSS;
    if (strncmp(s->name, ".data.rel.ro", 12) == 0) return O_RELRO;
    return O_DATA;
}

/* .init_array.N runs before .init_array, by N, as GNU ld sorts them. */
static long init_priority(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return 65536;
    for (const char *p = dot + 1; *p; p++) if (*p < '0' || *p > '9') return 65536;
    return strtol(dot + 1, NULL, 10);
}

static void sort_by_priority(OSec *s) {
    for (int a = 1; a < s->nin; a++) {
        int v = s->in[a], b = a;
        long pv = init_priority(L.o->secs[v].name);
        while (b > 0 && init_priority(L.o->secs[s->in[b - 1]].name) > pv) {
            s->in[b] = s->in[b - 1];
            b--;
        }
        s->in[b] = v;
    }
}

static int count_fdes(const ObjSection *s) {
    int n = 0;
    for (uint64_t at = 0; s->data && at + 8 <= s->size; ) {
        uint64_t len = rd(s->data + at, 4);
        if (len == 0 || len == 0xffffffff || len > s->size - at - 4) break;
        if (rd(s->data + at + 4, 4) != 0) n++;
        at += 4 + len;
    }
    return n;
}

/* The output sections, and the object's sections in them. */
static void group_sections(void) {
    O_INTERP = new_os(".interp", SHT_PROGBITS, SHF_ALLOC, 1, 0);
    O_GNUHASH = new_os(".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, 8, 0);
    O_DYNSYM = new_os(".dynsym", SHT_DYNSYM, SHF_ALLOC, 8, 24);
    O_DYNSTR = new_os(".dynstr", SHT_STRTAB, SHF_ALLOC, 1, 0);
    O_VERSYM = new_os(".gnu.version", SHT_GNU_VERSYM, SHF_ALLOC, 2, 2);
    O_VERNEED = new_os(".gnu.version_r", SHT_GNU_VERNEED, SHF_ALLOC, 8, 0);
    O_RELADYN = new_os(".rela.dyn", SHT_RELA, SHF_ALLOC, 8, 24);
    O_PLT = new_os(".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16, 8);
    O_TEXT = new_os(".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16, 0);
    O_RODATA = new_os(".rodata", SHT_PROGBITS, SHF_ALLOC, 1, 0);
    O_EHHDR = new_os(".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, 4, 0);
    O_EHFRAME = new_os(".eh_frame", SHT_PROGBITS, SHF_ALLOC, 8, 0);
    O_PREINIT = new_os(".preinit_array", SHT_PREINIT_ARRAY, SHF_ALLOC | SHF_WRITE, 8, 8);
    O_INIT = new_os(".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE, 8, 8);
    O_FINI = new_os(".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE, 8, 8);
    O_RELRO = new_os(".data.rel.ro", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 1, 0);
    O_DYNAMIC = new_os(".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 8, 16);
    O_GOT = new_os(".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 8, 8);
    O_DATA = new_os(".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 1, 0);
    O_BSS = new_os(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 1, 0);

    Obj *o = L.o;
    L.secos = xmalloc((size_t)o->nsec * sizeof(int));
    L.secaddr = xcalloc((size_t)o->nsec, sizeof(uint64_t));
    L.secoff = xcalloc((size_t)o->nsec, sizeof(uint64_t));
    L.secos[0] = -1;
    for (int i = 1; i < o->nsec; i++) {
        ObjSection *s = &o->secs[i];
        int g = -1;
        if (s->flags & SHF_ALLOC) {
            if ((s->flags & SHF_WRITE) && (s->flags & SHF_EXECINSTR)) {
                diag_error_simple("the section %s is both writable and executable", s->name);
                continue;
            }
            g = section_group(i);
        } else if (strncmp(s->name, ".debug_", 7) == 0) {
            for (int k = O_BSS + 1; k < L.nos && g < 0; k++)
                if (strcmp(L.os[k].name, s->name) == 0) g = k;
            if (g < 0) g = new_os(s->name, SHT_PROGBITS, 0, 1, 0);
        }
        L.secos[i] = g;
        if (g < 0) continue;
        OSec *os = &L.os[g];
        DA_APPEND(os->in, os->nin, os->capin, i);
        if (g != O_EHFRAME && s->align > os->align) os->align = s->align;
        if (g == O_EHFRAME) L.nfde += count_fdes(s);
    }
    sort_by_priority(&L.os[O_PREINIT]);
    sort_by_priority(&L.os[O_INIT]);
    sort_by_priority(&L.os[O_FINI]);

    L.os[O_INTERP].size = sizeof INTERP;
    L.os[O_GNUHASH].size = L.gnuhash.len;
    L.os[O_DYNSYM].size = 24 * (uint64_t)L.ndyn;
    L.os[O_DYNSTR].size = L.dynstr.len;
    L.os[O_VERSYM].size = L.versym.len;
    L.os[O_VERNEED].size = L.verneed.len;
    L.os[O_RELADYN].size = 24 * (uint64_t)(L.nrelative + L.nsymbolic);
    L.os[O_PLT].size = 8 * (uint64_t)L.nplt;
    L.os[O_EHHDR].size = 12 + 8 * (uint64_t)L.nfde;
    L.os[O_GOT].size = 8 * (uint64_t)L.ngot;
    Bytes dyn = { 0 };
    dynamic_entries(&dyn);
    L.os[O_DYNAMIC].size = dyn.len;
    free(dyn.p);
}

static uint64_t place(int id, uint64_t at) {
    OSec *s = &L.os[id];
    at = align_up(at, s->align);
    s->addr = s->off = at;
    if (!s->nin && id != O_EHFRAME) return at + s->size;
    for (int k = 0; k < s->nin; k++) {
        int i = s->in[k];
        at = align_up(at, id == O_EHFRAME ? 4 : L.o->secs[i].align);
        L.secaddr[i] = L.secoff[i] = at;
        at += L.o->secs[i].size;
    }
    if (id == O_EHFRAME) at += 4;           /* the terminator */
    s->size = at - s->addr;
    return at;
}

static uint64_t g_seg[4][2];                /* each segment's start and file end */
static uint64_t g_relro_end, g_mem_end, g_alloc_end;

static void layout(void) {
    uint64_t at = 64 + NPHDR * 56;
    g_seg[0][0] = 0;
    for (int id = O_INTERP; id <= O_RELADYN; id++) at = place(id, at);
    g_seg[0][1] = at;
    at = g_seg[1][0] = align_up(at, PAGE);
    at = place(O_PLT, at);
    at = g_seg[1][1] = place(O_TEXT, at);
    at = g_seg[2][0] = align_up(at, PAGE);
    at = place(O_RODATA, at);
    at = place(O_EHHDR, at);
    at = g_seg[2][1] = place(O_EHFRAME, at);
    at = g_seg[3][0] = align_up(at, PAGE);
    for (int id = O_PREINIT; id <= O_GOT; id++) at = place(id, at);
    g_relro_end = at;
    /* The RELRO part ends on a page, so all of it is protected. */
    bool rw = false;
    for (int k = 0; k < L.os[O_DATA].nin; k++) rw |= L.o->secs[L.os[O_DATA].in[k]].size != 0;
    for (int k = 0; k < L.os[O_BSS].nin; k++) rw |= L.o->secs[L.os[O_BSS].in[k]].size != 0;
    if (rw) at = g_relro_end = align_up(at, PAGE);
    at = g_seg[3][1] = place(O_DATA, at);
    g_mem_end = place(O_BSS, at);
    g_alloc_end = at;
    for (int id = O_BSS + 1; id < L.nos; id++) {
        OSec *s = &L.os[id];
        at = align_up(at, s->align);
        s->off = at;
        s->addr = 0;
        for (int k = 0; k < s->nin; k++) {
            int i = s->in[k];
            uint64_t rel = align_up(at - s->off, L.o->secs[i].align);
            L.secaddr[i] = rel;
            L.secoff[i] = s->off + rel;
            at = s->off + rel + L.o->secs[i].size;
        }
        s->size = at - s->off;
    }
    g_alloc_end = at;       /* now the end of the debug sections too */
}

static void assign_addresses(void) {
    Obj *o = L.o;
    for (int i = 1; i < o->nsym; i++) {
        ObjSymbol *s = &o->syms[i];
        SymLink *l = &L.sl[i];
        switch (kind(i)) {
        case K_DEF: l->addr = L.secaddr[s->sec] + s->value; break;
        case K_ABS: l->addr = s->value; break;
        case K_SPECIAL:
            l->addr = l->special == SP_GOT ? L.os[O_GOT].addr
                    : l->special == SP_DYNAMIC ? L.os[O_DYNAMIC].addr : 0;
            break;
        case K_IMPORT: l->addr = l->plt ? L.os[O_PLT].addr + 8 * (uint64_t)(l->plt - 1) : 0; break;
        }
    }
}

/* ================================================================
 * The file
 * ================================================================ */

static void add_rel(uint64_t off, uint32_t type, uint32_t sym, int64_t addend) {
    DynRel r = { off, type, sym, addend };
    DA_APPEND(L.rels, L.nrel, L.caprel, r);
}

static uint64_t got_addr(int i) { return L.os[O_GOT].addr + 8 * (uint64_t)(L.sl[i].got - 1); }

static bool put32(unsigned char *loc, int64_t v, bool sign, const ObjSection *s) {
    if (sign ? (v < INT32_MIN || v > INT32_MAX) : (v < 0 || v > (int64_t)UINT32_MAX)) {
        diag_error_simple("a relocation in %s is out of range", s->name);
        return false;
    }
    wr(loc, (uint64_t)v, 4);
    return true;
}

static void apply_relocs(unsigned char *img) {
    Obj *o = L.o;
    for (int si = 1; si < o->nsec; si++) {
        ObjSection *s = &o->secs[si];
        if (L.secos[si] < 0 || s->type == SHT_NOBITS) continue;
        bool alloc = (s->flags & SHF_ALLOC) != 0;
        for (int ri = 0; ri < s->nrel; ri++) {
            ObjReloc *r = &s->rel[ri];
            int w = r->type == R_X86_64_64 || r->type == R_X86_64_PC64 ? 8 : 4;
            if (r->type == R_X86_64_NONE) continue;
            if (r->off > s->size || s->size - r->off < (uint64_t)w) {
                diag_error_simple("a relocation lies outside %s", s->name);
                return;
            }
            unsigned char *loc = img + L.secoff[si] + r->off;
            uint64_t P = L.secaddr[si] + r->off;
            int k = kind(r->sym);
            uint64_t S = L.sl[r->sym].addr;
            int64_t A = r->addend;
            switch (r->type) {
            case R_X86_64_64:
                if (!alloc || k == K_ABS) wr(loc, k == K_IMPORT ? 0 : S + (uint64_t)A, 8);
                else if (k == K_IMPORT) add_rel(P, R_X86_64_64, (uint32_t)L.sl[r->sym].dyn, A);
                else {
                    wr(loc, S + (uint64_t)A, 8);
                    add_rel(P, R_X86_64_RELATIVE, 0, (int64_t)(S + (uint64_t)A));
                }
                break;
            case R_X86_64_PC32: case R_X86_64_PLT32:
                put32(loc, (int64_t)(S + (uint64_t)A - P), true, s);
                break;
            case R_X86_64_PC64:
                wr(loc, S + (uint64_t)A - P, 8);
                break;
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX:
                switch (relax_kind(s, r)) {
                case RELAX_LEA:
                    loc[-2] = 0x8d;
                    put32(loc, (int64_t)(S + (uint64_t)A - P), true, s);
                    break;
                case RELAX_CALL:
                    loc[-2] = 0x67;                 /* addr32 call */
                    loc[-1] = 0xe8;
                    put32(loc, (int64_t)(S + (uint64_t)A - P), true, s);
                    break;
                case RELAX_JMP:
                    loc[-2] = 0xe9;                 /* jmp, then a nop */
                    put32(loc - 1, (int64_t)(S + (uint64_t)A - P + 1), true, s);
                    loc[3] = 0x90;
                    break;
                default:
                    put32(loc, (int64_t)(got_addr(r->sym) + (uint64_t)A - P), true, s);
                }
                break;
            case R_X86_64_32: case R_X86_64_32S:
                put32(loc, (int64_t)(S + (uint64_t)A), r->type == R_X86_64_32S, s);
                break;
            default:
                unsupported_reloc(s->name, r->type);
                return;
            }
        }
    }

    for (int j = 0; j < L.ngot; j++) {
        int i = L.got[j];
        uint64_t at = L.os[O_GOT].addr + 8 * (uint64_t)j;
        int k = kind(i);
        if (k == K_IMPORT) add_rel(at, R_X86_64_GLOB_DAT, (uint32_t)L.sl[i].dyn, 0);
        else {
            wr(img + at, L.sl[i].addr, 8);
            if (k != K_ABS) add_rel(at, R_X86_64_RELATIVE, 0, (int64_t)L.sl[i].addr);
        }
    }
    for (int j = 0; j < L.nplt; j++) {
        uint64_t at = L.os[O_PLT].addr + 8 * (uint64_t)j;
        unsigned char *p = img + at;
        p[0] = 0xff; p[1] = 0x25;                   /* jmp *slot(%rip) */
        wr(p + 2, got_addr(L.plt[j]) - (at + 6), 4);
        p[6] = 0x66; p[7] = 0x90;                   /* xchg %ax,%ax */
    }
    for (int j = 0; j < L.ncopy; j++) {
        int i = L.copies[j];
        add_rel(L.sl[i].addr, R_X86_64_COPY, (uint32_t)L.sl[i].dyn, 0);
    }
}

static int cmp_rel(const void *a, const void *b) {
    const DynRel *x = a, *y = b;
    bool rx = x->type == R_X86_64_RELATIVE, ry = y->type == R_X86_64_RELATIVE;
    if (rx != ry) return rx ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* ---- .eh_frame_hdr ---- */

static uint64_t uleb(const unsigned char *p, size_t n, size_t *i) {
    uint64_t v = 0;
    int shift = 0;
    while (*i < n) {
        unsigned char c = p[(*i)++];
        if (shift < 64) v |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
        if (!(c & 0x80)) break;
    }
    return v;
}

static int enc_size(int enc) {
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return -1;
    }
}

/* The FDE pointer encoding a CIE gives (its 'R' augmentation), or -1. `c`
 * is the CIE after its length and id. */
static int cie_encoding(const unsigned char *c, size_t n) {
    size_t i = 0;
    if (n < 2) return -1;
    int version = c[i++];
    const char *aug = (const char *)c + i;
    size_t alen = strnlen(aug, n - i);
    if (i + alen >= n) return -1;
    i += alen + 1;
    if (aug[0] != 'z') return aug[0] ? -1 : 0x00;
    uleb(c, n, &i);                                 /* code alignment */
    uleb(c, n, &i);                                 /* data alignment (sleb; only skipped) */
    if (version == 1) i++;
    else uleb(c, n, &i);                            /* return address register */
    uleb(c, n, &i);                                 /* augmentation length */
    for (const char *a = aug + 1; *a && i < n; a++) {
        if (*a == 'R') return c[i];
        if (*a == 'L') i++;
        else if (*a == 'P') {
            int sz = enc_size(c[i]);
            if (sz < 0 || (c[i] & 0x70) == 0x50) return -1;
            i += 1 + (size_t)sz;
        } else if (*a != 'S' && *a != 'B') return -1;
    }
    return 0x00;
}

static bool read_encoded(const unsigned char *p, uint64_t at, int enc, uint64_t *out) {
    int64_t v;
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: v = (int64_t)rd(p, 8); break;
    case 0x03: v = (int64_t)(uint32_t)rd(p, 4); break;
    case 0x0b: v = (int32_t)(uint32_t)rd(p, 4); break;
    case 0x02: v = (int64_t)(uint16_t)rd(p, 2); break;
    case 0x0a: v = (int16_t)(uint16_t)rd(p, 2); break;
    default: return false;
    }
    if ((enc & 0x70) == 0x10) v += (int64_t)at;
    else if (enc & 0x70) return false;
    *out = (uint64_t)v;
    return true;
}

typedef struct { int64_t pc, fde; } HdrEnt;

static int cmp_hdr(const void *a, const void *b) {
    const HdrEnt *x = a, *y = b;
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

/* The binary search table unwinders use (PT_GNU_EH_FRAME). Without it, when
 * an FDE's encoding is one it cannot read, they walk .eh_frame instead. */
static void build_eh_hdr(unsigned char *img) {
    OSec *h = &L.os[O_EHHDR], *e = &L.os[O_EHFRAME];
    unsigned char *p = img + h->off;
    p[0] = 1;                                       /* version */
    p[1] = 0x1b;                                    /* eh_frame_ptr: pcrel sdata4 */
    p[2] = p[3] = 0xff;                             /* no table, until there is one */
    wr(p + 4, e->addr - (h->addr + 4), 4);
    HdrEnt *t = xmalloc(((size_t)L.nfde + 1) * sizeof(HdrEnt));
    int n = 0;
    bool ok = true;
    uint64_t lastcie = 0;
    int lastenc = -1;
    for (uint64_t at = e->addr; ok && at + 8 <= e->addr + e->size - 4; ) {
        const unsigned char *q = img + at;
        uint64_t len = rd(q, 4), id = rd(q + 4, 4);
        if (len == 0) break;
        if (len == 0xffffffff || at + 4 + len > e->addr + e->size) { ok = false; break; }
        if (id != 0) {
            uint64_t cie = at + 4 - id;
            if (cie != lastcie) {
                if (cie < e->addr || cie + 8 > at) { ok = false; break; }
                lastcie = cie;
                lastenc = cie_encoding(img + cie + 8, (size_t)rd(img + cie, 4) - 4);
            }
            uint64_t pc;
            if (lastenc < 0 || n >= L.nfde || !read_encoded(q + 8, at + 8, lastenc, &pc)) {
                ok = false;
                break;
            }
            t[n].pc = (int64_t)pc - (int64_t)h->addr;
            t[n].fde = (int64_t)at - (int64_t)h->addr;
            n++;
        }
        at += 4 + len;
    }
    if (ok) {
        qsort(t, (size_t)n, sizeof(HdrEnt), cmp_hdr);
        p[2] = 0x03;                                /* fde_count: udata4 */
        p[3] = 0x3b;                                /* table: datarel sdata4 */
        wr(p + 8, (uint64_t)n, 4);
        for (int k = 0; k < n; k++) {
            wr(p + 12 + 8 * (size_t)k, (uint64_t)t[k].pc, 4);
            wr(p + 16 + 8 * (size_t)k, (uint64_t)t[k].fde, 4);
        }
    }
    free(t);
}

/* ---- The symbol table ---- */

static void symtab_entry(Bytes *tab, Bytes *str, const char *name, uint64_t value, uint64_t size,
                         int bind, int type, int other, int shndx) {
    by_le(tab, name ? by_str(str, name) : 0, 4);
    unsigned char info[2] = { (unsigned char)(bind << 4 | type), (unsigned char)other };
    by_put(tab, info, 2);
    by_le(tab, (uint64_t)shndx, 2);
    by_le(tab, value, 8);
    by_le(tab, size, 8);
}

/* The symbols, for debuggers and profilers: the object's defined ones, and
 * the imports. */
static int build_symtab(Bytes *tab, Bytes *str) {
    Obj *o = L.o;
    by_put(str, "", 1);
    by_put(tab, NULL, 24);
    int first_global = 1;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) first_global = (int)(tab->len / 24);
        for (int i = 1; i < o->nsym; i++) {
            ObjSymbol *s = &o->syms[i];
            if (!s->name || !*s->name || (s->bind == STB_LOCAL) != (pass == 0)) continue;
            if (s->type == STT_SECTION || s->type == STT_FILE) continue;
            int bind = s->bind == STB_LOCAL ? STB_LOCAL : s->bind == STB_WEAK ? STB_WEAK : STB_GLOBAL;
            switch (kind(i)) {
            case K_DEF: {
                int g = L.secos[s->sec];
                if (g < 0 || !L.os[g].shndx) break;
                symtab_entry(tab, str, s->name, L.sl[i].addr, s->size, bind, s->type, s->other,
                             L.os[g].shndx);
                break;
            }
            case K_ABS:
                symtab_entry(tab, str, s->name, s->value, s->size, bind, s->type, s->other,
                             s->sec == SHN_ABS ? SHN_ABS : SHN_UNDEF);
                break;
            case K_IMPORT: {
                const Dso *d = &L.dsos[L.sl[i].dso - 1];
                int t = dsym(d, L.sl[i].dsosym)[4] & 0xf;
                symtab_entry(tab, str, s->name, 0, 0, bind,
                             t == STT_GNU_IFUNC ? STT_FUNC : t, 0, SHN_UNDEF);
                break;
            }
            default:
                break;
            }
        }
    }
    return first_global;
}

/* ---- Writing ---- */

static void put_phdr(unsigned char *p, uint32_t type, uint32_t flags, uint64_t off, uint64_t addr,
                     uint64_t filesz, uint64_t memsz, uint64_t align) {
    wr(p, type, 4);
    wr(p + 4, flags, 4);
    wr(p + 8, off, 8);
    wr(p + 16, addr, 8);
    wr(p + 24, addr, 8);
    wr(p + 32, filesz, 8);
    wr(p + 40, memsz, 8);
    wr(p + 48, align, 8);
}

static bool write_file(const char *path, const unsigned char *img, size_t n) {
    /* A new file, not the old one rewritten: the old one may be running. */
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0777);
    if (fd < 0) {
        diag_error_simple("cannot create '%s': %s", path, strerror(errno));
        return false;
    }
    for (size_t done = 0; done < n; ) {
        ssize_t w = write(fd, img + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            diag_error_simple("cannot write '%s': %s", path, strerror(errno));
            close(fd);
            return false;
        }
        done += (size_t)w;
    }
    if (close(fd) != 0) {
        diag_error_simple("cannot write '%s': %s", path, strerror(errno));
        return false;
    }
    return true;
}

static void emit(const char *path) {
    Obj *o = L.o;

    /* Section headers: the output sections with contents, then the symbol
     * and string tables. */
    int nsh = 1;
    for (int id = 0; id < L.nos; id++) {
        OSec *s = &L.os[id];
        if (s->size || id == O_DYNAMIC) s->shndx = nsh++;
    }
    int symtab_ndx = nsh++, strtab_ndx = nsh++, shstr_ndx = nsh++;
    Bytes symtab = { 0 }, strtab = { 0 }, shstr = { 0 };
    int first_global = build_symtab(&symtab, &strtab);
    by_put(&shstr, "", 1);

    uint64_t symtab_off = align_up(g_alloc_end, 8);
    uint64_t strtab_off = symtab_off + symtab.len;
    uint64_t shstr_off = strtab_off + strtab.len;
    uint32_t *names = xcalloc((size_t)nsh, sizeof(uint32_t));
    for (int id = 0; id < L.nos; id++)
        if (L.os[id].shndx) names[L.os[id].shndx] = (uint32_t)by_str(&shstr, L.os[id].name);
    names[symtab_ndx] = (uint32_t)by_str(&shstr, ".symtab");
    names[strtab_ndx] = (uint32_t)by_str(&shstr, ".strtab");
    names[shstr_ndx] = (uint32_t)by_str(&shstr, ".shstrtab");
    uint64_t shoff = align_up(shstr_off + shstr.len, 8);
    size_t total = (size_t)(shoff + 64 * (uint64_t)nsh);
    unsigned char *img = xcalloc(total, 1);

    /* Contents. */
    for (int i = 1; i < o->nsec; i++) {
        ObjSection *s = &o->secs[i];
        if (L.secos[i] >= 0 && s->type != SHT_NOBITS && s->size)
            memcpy(img + L.secoff[i], s->data, (size_t)s->size);
    }
    apply_relocs(img);
    memcpy(img + L.os[O_INTERP].off, INTERP, sizeof INTERP);
    memcpy(img + L.os[O_GNUHASH].off, L.gnuhash.p, L.gnuhash.len);
    memcpy(img + L.os[O_DYNSTR].off, L.dynstr.p, L.dynstr.len);
    if (L.versym.len) memcpy(img + L.os[O_VERSYM].off, L.versym.p, L.versym.len);
    if (L.verneed.len) memcpy(img + L.os[O_VERNEED].off, L.verneed.p, L.verneed.len);
    for (int j = 1; j < L.ndyn; j++) {
        DynSym *d = &L.dyns[j];
        unsigned char *p = img + L.os[O_DYNSYM].off + 24 * (size_t)j;
        const Dso *dso = d->dso ? &L.dsos[d->dso - 1] : NULL;
        const unsigned char *ds = dso ? dsym(dso, d->dsosym) : NULL;
        int t = ds ? ds[4] & 0xf : STT_NOTYPE;
        int bind = !d->def && o->syms[d->sym].bind == STB_WEAK ? STB_WEAK : STB_GLOBAL;
        wr(p, d->stroff, 4);
        p[4] = (unsigned char)(bind << 4 | (t == STT_GNU_IFUNC ? STT_FUNC : t));
        if (d->def) {
            wr(p + 6, (uint64_t)L.os[L.secos[o->syms[d->sym].sec]].shndx, 2);
            wr(p + 8, L.sl[d->sym].addr, 8);
            wr(p + 16, o->syms[d->sym].size, 8);
        }
    }
    if ((int)L.nrel != L.nrelative + L.nsymbolic && diag_error_count() == 0)
        diag_error_simple("internal error: %d dynamic relocations, %d expected", L.nrel,
                          L.nrelative + L.nsymbolic);
    qsort(L.rels, (size_t)L.nrel, sizeof(DynRel), cmp_rel);
    for (int j = 0; j < L.nrel && j < L.nrelative + L.nsymbolic; j++) {
        unsigned char *p = img + L.os[O_RELADYN].off + 24 * (size_t)j;
        wr(p, L.rels[j].off, 8);
        wr(p + 8, (uint64_t)L.rels[j].sym << 32 | L.rels[j].type, 8);
        wr(p + 16, (uint64_t)L.rels[j].addend, 8);
    }
    Bytes dyn = { 0 };
    dynamic_entries(&dyn);
    memcpy(img + L.os[O_DYNAMIC].off, dyn.p, dyn.len);
    free(dyn.p);
    build_eh_hdr(img);
    memcpy(img + symtab_off, symtab.p, symtab.len);
    memcpy(img + strtab_off, strtab.p, strtab.len);
    memcpy(img + shstr_off, shstr.p, shstr.len);

    /* Section headers. */
    for (int id = 0; id < L.nos; id++) {
        OSec *s = &L.os[id];
        if (!s->shndx) continue;
        uint32_t link = 0, info = 0;
        if (id == O_GNUHASH || id == O_VERSYM || id == O_RELADYN) link = (uint32_t)L.os[O_DYNSYM].shndx;
        if (id == O_DYNSYM || id == O_VERNEED || id == O_DYNAMIC) link = (uint32_t)L.os[O_DYNSTR].shndx;
        if (id == O_DYNSYM) info = 1;
        if (id == O_VERNEED) info = (uint32_t)L.nverneed;
        unsigned char *h = img + shoff + 64 * (size_t)s->shndx;
        wr(h, names[s->shndx], 4);
        wr(h + 4, s->type, 4);
        wr(h + 8, s->flags, 8);
        wr(h + 16, s->addr, 8);
        wr(h + 24, s->off, 8);
        wr(h + 32, s->size, 8);
        wr(h + 40, link, 4);
        wr(h + 44, info, 4);
        wr(h + 48, s->align, 8);
        wr(h + 56, s->entsize, 8);
    }
    const struct { int ndx; uint32_t type; uint64_t off, size, align, entsize; uint32_t link, info; } tail[3] = {
        { symtab_ndx, SHT_SYMTAB, symtab_off, symtab.len, 8, 24, (uint32_t)strtab_ndx, (uint32_t)first_global },
        { strtab_ndx, SHT_STRTAB, strtab_off, strtab.len, 1, 0, 0, 0 },
        { shstr_ndx, SHT_STRTAB, shstr_off, shstr.len, 1, 0, 0, 0 },
    };
    for (int k = 0; k < 3; k++) {
        unsigned char *h = img + shoff + 64 * (size_t)tail[k].ndx;
        wr(h, names[tail[k].ndx], 4);
        wr(h + 4, tail[k].type, 4);
        wr(h + 24, tail[k].off, 8);
        wr(h + 32, tail[k].size, 8);
        wr(h + 40, tail[k].link, 4);
        wr(h + 44, tail[k].info, 4);
        wr(h + 48, tail[k].align, 8);
        wr(h + 56, tail[k].entsize, 8);
    }

    /* The ELF header and the program headers. */
    memcpy(img, "\177ELF\2\1\1", 7);
    wr(img + 16, ET_DYN, 2);
    wr(img + 18, 62, 2);                            /* EM_X86_64 */
    wr(img + 20, 1, 4);                             /* EV_CURRENT */
    wr(img + 24, L.sl[obj_find(o, "_start")].addr, 8);
    wr(img + 32, 64, 8);                            /* e_phoff */
    wr(img + 40, shoff, 8);
    wr(img + 52, 64, 2);                            /* e_ehsize */
    wr(img + 54, 56, 2);
    wr(img + 56, NPHDR, 2);
    wr(img + 58, 64, 2);
    wr(img + 60, (uint64_t)nsh, 2);
    wr(img + 62, (uint64_t)shstr_ndx, 2);
    unsigned char *ph = img + 64;
    OSec *in = &L.os[O_INTERP], *dy = &L.os[O_DYNAMIC], *eh = &L.os[O_EHHDR];
    put_phdr(ph, PT_PHDR, PF_R, 64, 64, NPHDR * 56, NPHDR * 56, 8);
    put_phdr(ph + 56, PT_INTERP, PF_R, in->off, in->addr, in->size, in->size, 1);
    put_phdr(ph + 112, PT_LOAD, PF_R, 0, 0, g_seg[0][1], g_seg[0][1], PAGE);
    put_phdr(ph + 168, PT_LOAD, PF_R | PF_X, g_seg[1][0], g_seg[1][0], g_seg[1][1] - g_seg[1][0],
             g_seg[1][1] - g_seg[1][0], PAGE);
    put_phdr(ph + 224, PT_LOAD, PF_R, g_seg[2][0], g_seg[2][0], g_seg[2][1] - g_seg[2][0],
             g_seg[2][1] - g_seg[2][0], PAGE);
    put_phdr(ph + 280, PT_LOAD, PF_R | PF_W, g_seg[3][0], g_seg[3][0], g_seg[3][1] - g_seg[3][0],
             g_mem_end - g_seg[3][0], PAGE);
    put_phdr(ph + 336, PT_DYNAMIC, PF_R | PF_W, dy->off, dy->addr, dy->size, dy->size, 8);
    put_phdr(ph + 392, PT_GNU_EH_FRAME, PF_R, eh->off, eh->addr, eh->size, eh->size, 4);
    put_phdr(ph + 448, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 16);
    put_phdr(ph + 504, PT_GNU_RELRO, PF_R, g_seg[3][0], g_seg[3][0], g_relro_end - g_seg[3][0],
             g_relro_end - g_seg[3][0], 1);

    if (diag_error_count() == 0) write_file(path, img, total);
    free(img);
    free(names);
    free(symtab.p);
    free(strtab.p);
    free(shstr.p);
}

/* ================================================================
 * The link
 * ================================================================ */

static void link_free(void) {
    for (int i = 0; i < L.nfile; i++) {
        if (L.files[i].map) munmap(L.files[i].map, L.files[i].size);
        free(L.files[i].path);
    }
    free(L.files);
    for (int i = 0; i < L.ndso; i++) {
        free(L.dsos[i].needed);
        free(L.dsos[i].vername);
    }
    free(L.dsos);
    for (int i = 0; i < L.nar; i++) {
        hmap_free(&L.ars[i].index);
        free(L.ars[i].loaded);
    }
    free(L.ars);
    free(L.ins);
    for (int i = 0; i < L.ndir; i++) free(L.dirs[i]);
    free(L.dirs);
    for (int i = 0; i < L.norig; i++) free(L.orig[i].name);
    free(L.orig);
    free(L.sl);
    free(L.got);
    free(L.plt);
    free(L.copies);
    free(L.dyns);
    free(L.vn);
    free(L.rels);
    for (int i = 0; i < L.nos; i++) free(L.os[i].in);
    free(L.os);
    free(L.secos);
    free(L.secaddr);
    free(L.secoff);
    free(L.dynstr.p);
    free(L.gnuhash.p);
    free(L.versym.p);
    free(L.verneed.p);
    memset(&L, 0, sizeof L);
}

bool elf_link(Obj *o, const LinkArgs *a, const char *path) {
    int errors = diag_error_count();
    memset(&L, 0, sizeof L);
    L.o = o;

    for (int i = 0; i < a->ndirs; i++) DA_APPEND(L.dirs, L.ndir, L.capdir, str_dup(a->dirs[i]));
    const char *lp = getenv("LIBRARY_PATH");
    for (const char *p = lp; p && *p; ) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len) DA_APPEND(L.dirs, L.ndir, L.capdir, str_ndup(p, len));
        p = colon ? colon + 1 : p + len;
    }
    for (size_t i = 0; i < sizeof SYSTEM_DIRS / sizeof SYSTEM_DIRS[0]; i++)
        DA_APPEND(L.dirs, L.ndir, L.capdir, str_dup(SYSTEM_DIRS[i]));

    add_start();
    for (int i = 0; i < a->ninputs && diag_error_count() == errors; i++) {
        const char *in = a->inputs[i];
        if (in[0] == '-' && in[1] == 'l') find_lib(in + 2, 0);
        else load_file(in, false, 0);
    }
    if (diag_error_count() == errors) find_lib("m", 0);
    if (diag_error_count() == errors) find_lib("c", 0);
    if (diag_error_count() == errors) resolve_archives();
    if (diag_error_count() == errors) {
        L.sl = xcalloc((size_t)o->nsym, sizeof(SymLink));
        resolve_imports();
    }
    if (diag_error_count() == errors) {
        allocate_commons();
        scan_relocs();
    }
    if (diag_error_count() == errors) {
        build_dynamic_tables();
        group_sections();
    }
    if (diag_error_count() == errors && (L.os[O_INIT].nin || L.os[O_PREINIT].nin)) {
        /* Before 2.34, glibc ran an executable's constructors from code the C
         * compiler's startup files supply. */
        int lsm = obj_find(o, "__libc_start_main");
        const char *v = L.sl[lsm].dso ? dso_version(&L.dsos[L.sl[lsm].dso - 1], L.sl[lsm].dsosym) : NULL;
        if (v && strcmp(v, "GLIBC_2.2.5") == 0)
            unsupported("the program", "a constructor (.init_array) with glibc before 2.34");
    }
    if (diag_error_count() == errors) {
        layout();
        assign_addresses();
        emit(path);
    }
    link_free();
    return diag_error_count() == errors;
}

#endif /* Linux x86-64 */
