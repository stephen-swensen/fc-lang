/* repl.c: the interactive session, `fcc --interactive` (see repl.h).
 *
 * Each input (a line, a block, a paste, an .fcx file) holds declarations and
 * statements. Each of them is a unit: it is type-checked against what the
 * units before it declared and compiled by the native backend into an object
 * of its own (x86gen_emit_unit). Nothing earlier is checked or compiled
 * again. A whole input compiles before any of it runs, so an input with an
 * error runs none of its statements; then its units run one by one, in order.
 *
 * The front end sees a unit as a little program of one top-level declaration:
 *   - a `let` binding a function is a function;
 *   - any other `let` is a session binding, a global whose initializer runs
 *     when the unit runs (Decl.let.is_session);
 *   - a destructuring `let` is entered as a unit for each name it binds,
 *     after a hidden one that holds the value (entered_items);
 *   - any other statement is a session binding named `it`, which binds
 *     nothing when the statement yields no value (is_session_stmt);
 *   - a type, module, error group or import is as at the top of a file.
 * The unit's top-level symbol table is chained to the one before it
 * (SymbolTable.outer), so a name resolves to its newest declaration, and its
 * global declarations get C names under a root of their own
 * (pass1_set_global_root), so declaring a name again makes a new symbol and
 * leaves what used the old one as it was, as in F#'s fsi.
 *
 * Two processes keep the session. This one, the supervisor, holds the
 * terminal and the compiler, and links each unit's object itself, at
 * addresses in a region reserved before anything else ran, resolving the
 * names of earlier units and of the C library (dlsym). The holder, forked
 * from it at the start, holds the running program: it writes the linked
 * bytes where the supervisor says, and runs each unit in a child of its own.
 * A unit whose child finishes hands the session to that child, and the
 * holder before it exits; a unit whose child dies (an abort, Ctrl+C) leaves
 * the holder as it was, so the session goes on as if the statement had never
 * run. A unit's runner calls on into the holder's loop rather than returning
 * (see X86Unit), so what a statement built on its stack lives on, as in a
 * program's main. */

/* RTLD_DEFAULT, MAP_NORESERVE and prctl(). Must precede every #include. */
#define _GNU_SOURCE

#include "repl.h"

#if !defined(__linux__) || !defined(__x86_64__)

#include "diag.h"

int repl_main(const CompileArgs *ca) {
    (void)ca;
    diag_error_simple("--interactive is supported only on Linux x86-64");
    return 1;
}

#else /* Linux x86-64 */

#include "diag.h"
#include "elflink.h"
#include "elfobj.h"
#include "fcc_version.h"   /* FCC_DATADIR */
#include "lexer.h"
#include "lineedit.h"
#include "monomorph.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include "types.h"
#include "version.h"
#include "x86gen.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <ucontext.h>
#include <unistd.h>

/* ================================================================
 * The session
 * ================================================================ */

/* The address space for the session's code and data: every unit is linked
 * into it, so any two are within the +-2 GB a PC-relative reference reaches. */
#define REGION_SIZE ((uint64_t)1 << 30)

typedef struct {
    Arena arena;
    InternTable intern;
    MonoTable mono;
    FileImportScopes scopes;
    int session_scope;          /* the inputs' import scope in `scopes`, -1 before one */
    SymbolTable *top;           /* the newest unit's table */
    Decl **env;                 /* every top-level declaration so far */
    int env_n, env_cap;
    const char **gnames;        /* the generic declaration names so far */
    int gn_n, gn_cap;
    const Flag *flags;
    int flag_count;
    const char *const *include_dirs;
    int ninclude;
    int next_unit;
    /* the program */
    uint64_t region, region_used;
    HMap syms;                  /* symbol name (interned) -> address; 0 = undefined */
    struct { const char *name; int64_t old; } *undo;
    int undo_n, undo_cap;
    int cmd, resp;              /* to and from the holder */
    bool interactive;           /* stdin is a terminal */
} Session;

/* An address the holder fills: with `data`, or with zeros when it is NULL. */
typedef struct {
    uint64_t addr, size;
    unsigned char *data;
} Block;

/* What to put back when a unit is undone. */
typedef struct {
    SymbolTable *top;
    int env_n, imports, mono, errors, gn_n, undo_n;
    uint64_t region_used;
} Mark;

typedef struct {
    Mark mark;                  /* the session before the unit */
    int item;                   /* the item of its input it was entered for */
    SymbolTable *table;
    Obj obj;
    const char *runner;
    unsigned char *desc;        /* the bound value's descriptor, or NULL */
    int desc_len;
    const char *value_sym;      /* the global holding it */
    char *head;                 /* shown before the value: "val x: i32 = " */
    char *after;                /* shown when the unit has run, or NULL */
    uint64_t runner_addr, value_addr;
    Block *blocks;
    int nblocks, blocks_cap;
} Unit;

typedef struct {
    Unit *items;
    int count, cap;
} Units;

static Session S;

static ImportTable *session_imports(void) {
    return S.session_scope < 0 ? NULL : &S.scopes.scopes[S.session_scope].imports;
}

static Mark mark_session(void) {
    ImportTable *imp = session_imports();
    return (Mark){ S.top, S.env_n, imp ? imp->count : 0, S.mono.count, error_code_count(),
                   S.gn_n, S.undo_n, S.region_used };
}

/* Undo the front end's record of what came after `m`: the declarations, and
 * the imports. The undone tables are dropped from the chain but not freed:
 * the generic instances their statements registered may still point into
 * them. */
static void undo_declarations(const Mark *m) {
    S.top = m->top;
    S.env_n = m->env_n;
    ImportTable *imp = session_imports();
    if (imp && imp->count > m->imports) imp->count = m->imports;
}

/* Undo everything after `m`, for an input that did not compile or link:
 * nothing of it reached the holder. */
static void undo_input(const Mark *m) {
    undo_declarations(m);
    S.mono.count = m->mono;
    S.mono.walking = 0;     /* an error can end discovery mid-walk */
    error_codes_truncate(m->errors);
    S.gn_n = m->gn_n;
    while (S.undo_n > m->undo_n) {
        S.undo_n--;
        const char *name = S.undo[S.undo_n].name;
        hmap_get(&S.syms, name, strlen(name))->val = S.undo[S.undo_n].old;
    }
    S.region_used = m->region_used;
}

static void unit_free(Unit *u) {
    obj_free(&u->obj);
    free(u->desc);
    free(u->head);
    free(u->after);
    for (int i = 0; i < u->nblocks; i++) free(u->blocks[i].data);
    free(u->blocks);
}

static void units_free(Units *us) {
    for (int i = 0; i < us->count; i++) unit_free(&us->items[i]);
    free(us->items);
    us->items = NULL;
    us->count = us->cap = 0;
}

/* ================================================================
 * Compiling a unit
 * ================================================================ */

/* The hidden session binding that holds what a destructuring `let` takes
 * apart (entered_items). No name in a program may have a `__`, so none can
 * refer to it. */
#define DESTRUCTURED "destructured__value"

/* A `return` at the prompt ends the session with its exit code, so it must
 * have one, an i32, as main's does. Lambda bodies have their own. */
static void check_prompt_return(Expr *e, void *ctx) {
    (void)ctx;
    if (e->kind == EXPR_FUNC) return;
    if (e->kind == EXPR_RETURN) {
        Expr *v = e->return_expr.value;
        if (!v)
            diag_error(e->loc, "a return at the prompt ends the session and needs its exit "
                       "code: return 0");
        else if (v->type && !type_is_error(v->type) && !type_eq(v->type, type_int32()))
            diag_error(v->loc, "a return at the prompt ends the session with an i32 exit "
                       "code, not %s", type_name(v->type));
    }
    expr_for_each_child(e, check_prompt_return, NULL);
}

/* What the session shows once the unit has run: each declaration's kind and
 * name, a function's type; the value of a binding is shown by the holder. */
static void describe_unit(Unit *u, Decl **decls, int n) {
    char *after = NULL;
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        switch (d->kind) {
        case DECL_LET: {
            Type *t = d->let.resolved_type;
            if (!t || t->kind == TYPE_VOID || t->kind == TYPE_NEVER) break;
            if (d->let.name == intern_cstr(&S.intern, DESTRUCTURED)) break;
            if (decl_is_function(d) || t->kind == TYPE_FUNC || !u->desc) {
                after = str_appendf(after, "val %s: %s\n", d->let.name, type_name(t));
            } else {
                u->head = str_sprintf("val %s: %s = ", d->let.name, type_name(t));
                u->value_sym = d->let.codegen_name;
            }
            break;
        }
        case DECL_STRUCT:
            after = str_appendf(after, "struct %s\n", mangled_source_name(d->struc.name));
            break;
        case DECL_UNION:
            after = str_appendf(after, "union %s\n", mangled_source_name(d->unio.name));
            break;
        case DECL_ENUM:
            after = str_appendf(after, "enum %s\n", mangled_source_name(d->enu.name));
            break;
        case DECL_MODULE:
            after = str_appendf(after, "%s %s\n", d->module.is_error_group ? "error" : "module",
                                d->module.name);
            break;
        case DECL_IMPORT:
        case DECL_EXTERN:
        case DECL_NAMESPACE:
        case DECL_ERROR:
            break;
        }
    }
    u->after = after;
}

static void *new_node(size_t size) {
    void *p = arena_alloc(&S.arena, size);
    memset(p, 0, size);
    return p;
}

static Expr *new_expr(ExprKind kind, SrcLoc loc) {
    Expr *e = new_node(sizeof *e);
    e->kind = kind;
    e->loc = loc;
    return e;
}

static Expr *new_ident(const char *name, SrcLoc loc) {
    Expr *e = new_expr(EXPR_IDENT, loc);
    e->ident.name = name;
    return e;
}

/* `{ let <pattern> = ...; value }` */
static Expr *new_destructure(Expr *let, Expr *value) {
    Expr *b = new_expr(EXPR_BLOCK, let->loc);
    b->block.stmts = new_node(2 * sizeof(Expr *));
    b->block.stmts[0] = let;
    b->block.stmts[1] = value;
    b->block.count = 2;
    return b;
}

static Decl *new_session_let(const char *name, bool is_mut, Expr *init, SrcLoc loc) {
    Decl *d = new_node(sizeof *d);
    d->kind = DECL_LET;
    d->loc = loc;
    d->let.name = name;
    d->let.is_mut = is_mut;
    d->let.init = init;
    return d;
}

static SessionItem session_let_item(Decl *d) {
    Decl **decls = new_node(sizeof(Decl *));
    decls[0] = d;
    return (SessionItem){ .decls = decls, .decl_count = 1 };
}

/* The names a destructuring pattern binds, in order. Only the patterns a
 * destructuring `let` takes have names in them; the others are errors, which
 * checking the `let` reports. */
static void pattern_names(Pattern *p, Pattern ***out, int *n, int *cap) {
    switch (p->kind) {
    case PAT_BINDING:
        DA_APPEND(*out, *n, *cap, p);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < p->tuple_pat.pattern_count; i++)
            pattern_names(p->tuple_pat.patterns[i], out, n, cap);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < p->struc.field_count; i++)
            pattern_names(p->struc.fields[i].pattern, out, n, cap);
        break;
    default:
        break;
    }
}

/* The items an item of the input is entered as: itself, unless it is a
 * destructuring `let`, which binds each of its names as a session binding.
 * `let {q, r} = v` is entered as a hidden binding of
 * `{ let {q, r} = v; {q, r} }`, its names as a tuple, then as `let q` and
 * `let r`, each taking its name out of that tuple. So the pattern is checked
 * once, as written, and each name is shown as it is bound. With one name no
 * tuple is needed (`let q = { let {q, _} = v; q }`), and one that binds none
 * is a statement. So is `let _ = v`, which binds nothing either: it is
 * entered as `{ let _ = v }`. */
static SessionItem *entered_items(SessionItem *item, int *n) {
    *n = 1;
    if (item->decl_count == 1 && decl_binds_nothing(item->decls[0])) {
        Decl *d = item->decls[0];
        Expr *let = new_expr(EXPR_LET, d->loc);
        let->let_expr.let_name = d->let.name;
        let->let_expr.let_is_mut = d->let.is_mut;
        let->let_expr.let_init = d->let.init;
        Expr *b = new_expr(EXPR_BLOCK, d->loc);
        b->block.stmts = new_node(sizeof(Expr *));
        b->block.stmts[0] = let;
        b->block.count = 1;
        SessionItem *out = new_node(sizeof *out);
        out->stmt = b;
        return out;
    }
    Expr *e = item->stmt;
    if (!e || e->kind != EXPR_LET_DESTRUCT) return item;
    Pattern **names = NULL;
    int count = 0, cap = 0;
    pattern_names(e->let_destruct.pattern, &names, &count, &cap);
    SessionItem *out = new_node((size_t)(count + 1) * sizeof *out);
    bool mut = e->let_destruct.is_mut;
    if (count == 0) {
        out[0].stmt = e;
    } else if (count == 1) {
        const char *name = names[0]->binding.name;
        out[0] = session_let_item(new_session_let(
            name, mut, new_destructure(e, new_ident(name, names[0]->loc)), e->loc));
    } else {
        const char *hidden = intern_cstr(&S.intern, DESTRUCTURED);
        Expr *tuple = new_expr(EXPR_TUPLE_LIT, e->loc);
        tuple->tuple_lit.elems = new_node((size_t)count * sizeof(Expr *));
        tuple->tuple_lit.elem_count = count;
        for (int i = 0; i < count; i++)
            tuple->tuple_lit.elems[i] = new_ident(names[i]->binding.name, names[i]->loc);
        out[0] = session_let_item(new_session_let(hidden, false, new_destructure(e, tuple),
                                                  e->loc));
        for (int i = 0; i < count; i++) {
            const char *name = names[i]->binding.name;
            SrcLoc loc = names[i]->loc;
            Pattern *take = new_node(sizeof *take);
            take->kind = PAT_TUPLE;
            take->loc = loc;
            take->tuple_pat.patterns = new_node((size_t)count * sizeof(Pattern *));
            take->tuple_pat.pattern_count = count;
            for (int j = 0; j < count; j++) {
                Pattern *p = new_node(sizeof *p);
                p->loc = loc;
                p->kind = i == j ? PAT_BINDING : PAT_WILDCARD;
                if (i == j) p->binding.name = name;
                take->tuple_pat.patterns[j] = p;
            }
            Expr *let = new_expr(EXPR_LET_DESTRUCT, loc);
            let->let_destruct.pattern = take;
            let->let_destruct.init = new_ident(hidden, loc);
            out[i + 1] = session_let_item(new_session_let(
                name, mut, new_destructure(let, new_ident(name, loc)), loc));
        }
        *n = count + 1;
    }
    free(names);
    return out;
}

/* The declarations a unit holds: an item's own, or for a statement a
 * binding of it (`it`). NULL after reporting an item the prompt does not
 * take. */
static Decl **unit_decls(SessionItem *item, int *n) {
    if (!item->stmt) {
        for (int i = 0; i < item->decl_count; i++) {
            Decl *d = item->decls[i];
            if (d->kind == DECL_NAMESPACE) {
                diag_error(d->loc, "a namespace declaration is not allowed at the prompt; "
                           "load a file that declares one");
                return NULL;
            }
        }
        *n = item->decl_count;
        return item->decls;
    }
    Expr *e = item->stmt;
    if (e->kind == EXPR_DEFER) {
        diag_error(e->loc, "defer is not allowed at the prompt: the session has no end "
                   "for it to run at");
        return NULL;
    }
    Decl *d = new_session_let(intern_cstr(&S.intern, "it"), false, e, e->loc);
    d->let.is_session_stmt = true;
    Decl **decls = new_node(sizeof(Decl *));
    decls[0] = d;
    *n = 1;
    return decls;
}

/* Type-check and compile one item. False after reporting an error. */
static bool compile_unit(SessionItem *item, const char *filename, Unit *u) {
    memset(u, 0, sizeof *u);
    u->mark = mark_session();
    int n = 0;
    Decl **decls = unit_decls(item, &n);
    if (!decls) return false;

    int id = S.next_unit++;
    const char *root = intern_sprintf(&S.intern, "fc__%d", id);
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (d->kind != DECL_LET) continue;
        d->let.is_session = true;
        d->let.codegen_name = intern_sprintf(&S.intern, "%s__%s", root, d->let.name);
    }
    Program *prog = arena_alloc(&S.arena, sizeof(Program));
    prog->decls = decls;
    prog->decl_count = n;
    prog->generic_names = S.gnames;
    prog->generic_name_count = S.gn_n;

    SymbolTable *t = xcalloc(1, sizeof *t);
    t->outer = S.top;
    S.top = t;
    u->table = t;
    /* The inputs share one import scope, whichever file they came from. */
    if (S.session_scope < 0) {
        FileImportScope scope = { .filename = filename };
        DA_APPEND(S.scopes.scopes, S.scopes.count, S.scopes.capacity, scope);
        S.session_scope = S.scopes.count - 1;
    }
    S.scopes.scopes[S.session_scope].filename = filename;

    int mono0 = S.mono.count;
    pass1_set_global_root(root);
    pass1_collect(prog, t, &S.intern, &S.scopes, false);
    pass2_check(prog, t, &S.intern, &S.mono, &S.scopes, &S.arena);
    for (int i = 0; i < n; i++)
        if (decls[i]->kind == DECL_LET && decls[i]->let.init && !decl_is_function(decls[i]))
            check_prompt_return(decls[i]->let.init, NULL);
    if (diag_error_count() > 0) return false;
    mono_discover_transitive_from(&S.mono, mono0, &S.arena, &S.intern, t);
    mono_finalize_types_from(&S.mono, mono0, &S.arena, &S.intern);
    if (diag_error_count() > 0) return false;

    /* A statement that yields no value binds nothing. */
    if (item->stmt) {
        Type *st = decls[0]->let.resolved_type;
        if (st->kind == TYPE_VOID || st->kind == TYPE_NEVER) t->count = 0;
    }
    for (int i = 0; i < n; i++) DA_APPEND(S.env, S.env_n, S.env_cap, decls[i]);

    u->runner = intern_sprintf(&S.intern, "fc_run_%d", id);
    X86Unit xu = { .env = S.env, .env_count = S.env_n, .mono_first = mono0,
                   .runner = u->runner };
    obj_init(&u->obj);
    x86gen_emit_unit(prog, &u->obj, &S.mono, &S.arena, &S.intern, t, S.include_dirs,
                     S.ninclude, &xu);
    u->desc = xu.desc;
    u->desc_len = xu.desc_len;
    if (diag_error_count() > 0) return false;
    describe_unit(u, decls, n);
    return true;
}

/* ================================================================
 * Linking
 * ================================================================ */

static uint64_t region_alloc(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t at = (S.region_used + align - 1) & ~(align - 1);
    if (at + size > REGION_SIZE)
        diag_fatal_simple("the session has used all %llu MB of its space for code and data",
                          (unsigned long long)(REGION_SIZE >> 20));
    S.region_used = at + size;
    return S.region + at;
}

static void define_symbol(const char *name, uint64_t addr) {
    HEnt *e = hmap_put(&S.syms, name, strlen(name));
    if (S.undo_n == S.undo_cap) {
        S.undo_cap = S.undo_cap ? S.undo_cap * 2 : 256;
        S.undo = xrealloc(S.undo, sizeof *S.undo * (size_t)S.undo_cap);
    }
    S.undo[S.undo_n].name = name;
    S.undo[S.undo_n].old = e->val;
    S.undo_n++;
    e->val = (int64_t)addr;
}

static void add_block(Unit *u, uint64_t addr, uint64_t size, unsigned char *data) {
    Block b = { addr, size, data };
    DA_APPEND(u->blocks, u->nblocks, u->blocks_cap, b);
}

static bool fits_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

/* Place the unit's object in the region and resolve its references: to its
 * own symbols, to the units before it, and to the libraries (dlsym). A
 * library symbol is out of a 32-bit reach, so a call to one goes through a
 * jump stub and an address through a slot in the region. False after
 * reporting an undefined symbol. */
static bool link_unit(Unit *u) {
    Obj *o = &u->obj;
    uint64_t *secaddr = xcalloc((size_t)o->nsec, sizeof(uint64_t));
    unsigned char **secdata = xcalloc((size_t)o->nsec, sizeof(unsigned char *));
    for (int i = 1; i < o->nsec; i++) {
        ObjSection *sec = &o->secs[i];
        if (!(sec->flags & SHF_ALLOC)) continue;
        secaddr[i] = region_alloc(sec->size ? sec->size : 1, sec->align);
        if (sec->type != SHT_NOBITS && sec->size) {
            secdata[i] = xmalloc(sec->size);
            memcpy(secdata[i], sec->data, sec->size);
        }
    }
    uint64_t *symaddr = xcalloc((size_t)o->nsym, sizeof(uint64_t));
    bool *far = xcalloc((size_t)o->nsym, sizeof(bool));
    bool ok = true;
    for (int i = 1; i < o->nsym; i++) {
        ObjSymbol *s = &o->syms[i];
        if (s->sec == SHN_UNDEF) {
            if (!s->name) continue;
            HEnt *e = hmap_get(&S.syms, s->name, strlen(s->name));
            if (e && e->val) { symaddr[i] = (uint64_t)e->val; continue; }
            void *p = dlsym(RTLD_DEFAULT, s->name);
            if (!p) {
                diag_error_simple("undefined symbol '%s': no library loaded into the session "
                                  "defines it (link one with -l)", s->name);
                ok = false;
                continue;
            }
            symaddr[i] = (uint64_t)(uintptr_t)p;
            far[i] = true;
        } else if (s->sec == SHN_ABS) {
            symaddr[i] = s->value;
        } else if (s->sec == SHN_COMMON) {
            symaddr[i] = region_alloc(s->size ? s->size : 1, s->value);
            add_block(u, symaddr[i], s->size ? s->size : 1, NULL);
        } else if (s->sec > 0 && s->sec < o->nsec) {
            symaddr[i] = secaddr[s->sec] + s->value;
        }
    }

    /* Slots and stubs, at most one of each per symbol. */
    uint64_t *slot = xcalloc((size_t)o->nsym, sizeof(uint64_t));
    uint64_t *stub = xcalloc((size_t)o->nsym, sizeof(uint64_t));
    for (int i = 1; ok && i < o->nsec; i++) {
        ObjSection *sec = &o->secs[i];
        if (!(sec->flags & SHF_ALLOC)) continue;
        for (int r = 0; r < sec->nrel; r++) {
            ObjReloc *rel = &sec->rel[r];
            int sy = rel->sym;
            uint64_t P = secaddr[i] + rel->off;
            uint64_t S_ = symaddr[sy];
            int64_t A = rel->addend;
            unsigned char *at = secdata[i] ? secdata[i] + rel->off : NULL;
            if (!at) continue;
            switch (rel->type) {
            case R_X86_64_64:
                put64(at, S_ + (uint64_t)A);
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)(S_ + (uint64_t)A - P);
                if (!fits_i32(v) && far[sy]) {
                    if (!stub[sy]) {
                        stub[sy] = region_alloc(16, 16);
                        unsigned char *code = xcalloc(16, 1);
                        code[0] = 0xff;     /* jmp *0(%rip) */
                        code[1] = 0x25;
                        put64(code + 6, S_);
                        add_block(u, stub[sy], 16, code);
                    }
                    v = (int64_t)(stub[sy] + (uint64_t)A - P);
                }
                if (!fits_i32(v)) {
                    diag_error_simple("the reference to '%s' is out of reach of the session's code",
                                      o->syms[sy].name ? o->syms[sy].name : "?");
                    ok = false;
                    break;
                }
                put32(at, (uint32_t)(int32_t)v);
                break;
            }
            case R_X86_64_GOTPCREL:
            case R_X86_64_GOTPCRELX:
            case R_X86_64_REX_GOTPCRELX: {
                if (!slot[sy]) {
                    slot[sy] = region_alloc(8, 8);
                    unsigned char *v = xmalloc(8);
                    put64(v, S_);
                    add_block(u, slot[sy], 8, v);
                }
                put32(at, (uint32_t)(int32_t)(int64_t)(slot[sy] + (uint64_t)A - P));
                break;
            }
            case R_X86_64_PC64:
                put64(at, S_ + (uint64_t)A - P);
                break;
            default:
                diag_error_simple("the session cannot load a relocation of type %u "
                                  "(in section %s)", rel->type, sec->name);
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        for (int i = 1; i < o->nsec; i++) {
            ObjSection *sec = &o->secs[i];
            if (!(sec->flags & SHF_ALLOC)) continue;
            add_block(u, secaddr[i], sec->size ? sec->size : 1, secdata[i]);
            secdata[i] = NULL;
        }
        for (int i = 1; i < o->nsym; i++) {
            ObjSymbol *s = &o->syms[i];
            if (!s->name || s->sec == SHN_UNDEF || s->type == STT_SECTION || s->type == STT_FILE)
                continue;
            define_symbol(intern_cstr(&S.intern, s->name), symaddr[i]);
        }
        HEnt *r = hmap_get(&S.syms, u->runner, strlen(u->runner));
        u->runner_addr = r ? (uint64_t)r->val : 0;
        if (u->value_sym) {
            HEnt *v = hmap_get(&S.syms, u->value_sym, strlen(u->value_sym));
            u->value_addr = v ? (uint64_t)v->val : 0;
        }
    }
    for (int i = 0; i < o->nsec; i++) free(secdata[i]);
    free(secdata);
    free(secaddr);
    free(symaddr);
    free(far);
    free(slot);
    free(stub);
    return ok;
}

/* ================================================================
 * The holder
 * ================================================================ */

static bool read_full(int fd, void *p, size_t n) {
    char *c = p;
    while (n > 0) {
        ssize_t r = read(fd, c, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        c += r;
        n -= (size_t)r;
    }
    return true;
}

static void write_full(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n > 0) {
        ssize_t w = write(fd, c, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        c += w;
        n -= (size_t)w;
    }
}

/* ---- Showing a value: a descriptor (X86Desc) read over memory ---- */

typedef struct {
    const unsigned char *d, *end;
} Desc;

static uint64_t desc_num(Desc *d, int width) {
    uint64_t v = 0;
    for (int i = 0; i < width && d->d < d->end; i++) v |= (uint64_t)*d->d++ << (8 * i);
    return v;
}

static const char *desc_str(Desc *d, int *len) {
    *len = (int)desc_num(d, 4);
    const char *s = (const char *)d->d;
    d->d += *len;
    return s;
}

static int64_t load_int(const unsigned char *p, int width, bool sign) {
    switch (width) {
    case 1: return sign ? (int64_t)*(const int8_t *)p : (int64_t)*p;
    case 2: { uint16_t v; memcpy(&v, p, 2); return sign ? (int64_t)(int16_t)v : (int64_t)v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return sign ? (int64_t)(int32_t)v : (int64_t)v; }
    default: { int64_t v; memcpy(&v, p, 8); return v; }
    }
}

static void show_float(double x, bool f32) {
    char buf[64];
    for (int prec = f32 ? 6 : 15; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, x);
        if (f32 ? (float)strtod(buf, NULL) == (float)x : strtod(buf, NULL) == x) break;
    }
    fputs(buf, stdout);
    if (!strpbrk(buf, ".eEni")) fputs(".0", stdout);
}

static void show_bytes(const char *open, const unsigned char *s, size_t n, bool cut) {
    fputs(open, stdout);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c < 32 || c == 127) printf("\\x%02x", c);
        else putchar(c);
    }
    fputs(cut ? "\"..." : "\"", stdout);
}

enum { SHOW_ELEMS = 20, SHOW_BYTES = 400 };

/* Show the value at `v` the descriptor at d->d describes, and move past the
 * descriptor; with `v` NULL, only move past it. */
static void show(Desc *d, const unsigned char *v) {
    if (d->d >= d->end) return;
    unsigned op = *d->d++;
    int nl;
    switch (op) {
    case XD_INT:
    case XD_UINT: {
        int w = (int)desc_num(d, 4);
        if (!v) return;
        int64_t x = load_int(v, w, op == XD_INT);
        if (op == XD_INT || w < 8) printf("%" PRId64, x);
        else printf("%" PRIu64, (uint64_t)x);
        return;
    }
    case XD_FLOAT: {
        int w = (int)desc_num(d, 4);
        if (!v) return;
        if (w == 4) { float f; memcpy(&f, v, 4); show_float(f, true); }
        else { double f; memcpy(&f, v, 8); show_float(f, false); }
        return;
    }
    case XD_BOOL:
        if (v) fputs(*v ? "true" : "false", stdout);
        return;
    case XD_PTR:
        if (v) { uint64_t p; memcpy(&p, v, 8); printf("0x%" PRIx64, p); }
        return;
    case XD_FUNC:
        if (v) fputs("<fun>", stdout);
        return;
    case XD_STR: {
        int lw = (int)desc_num(d, 4);
        if (!v) return;
        const unsigned char *p;
        memcpy(&p, v, 8);
        uint64_t len = (uint64_t)load_int(v + 8, lw, false);
        bool cut = len > SHOW_BYTES;
        show_bytes("\"", p, cut ? SHOW_BYTES : (size_t)len, cut);
        return;
    }
    case XD_CSTR: {
        if (!v) return;
        const unsigned char *p;
        memcpy(&p, v, 8);
        size_t len = strnlen((const char *)p, SHOW_BYTES + 1);
        show_bytes("c\"", p, len > SHOW_BYTES ? SHOW_BYTES : len, len > SHOW_BYTES);
        return;
    }
    case XD_SLICE:
    case XD_ARRAY: {
        uint64_t count = 0;
        const unsigned char *p = v;
        if (op == XD_SLICE) {
            int lw = (int)desc_num(d, 4);
            if (v) { memcpy(&p, v, 8); count = (uint64_t)load_int(v + 8, lw, false); }
        } else {
            count = desc_num(d, 4);
        }
        uint64_t es = desc_num(d, 4);
        const unsigned char *elem = d->d;
        if (v) {
            fputs("{", stdout);
            for (uint64_t i = 0; i < count && i < SHOW_ELEMS; i++) {
                Desc e = { elem, d->end };
                fputs(i ? ", " : " ", stdout);
                show(&e, p + i * es);
            }
            if (count > SHOW_ELEMS) printf(", ... (%" PRIu64 " in all)", count);
            fputs(count ? " }" : "}", stdout);
        }
        show(d, NULL);
        return;
    }
    case XD_STRUCT:
    case XD_TUPLE: {
        if (op == XD_STRUCT) {
            const char *name = desc_str(d, &nl);
            if (v) printf("%.*s ", nl, name);
        }
        uint64_t n = desc_num(d, 4);
        if (v) fputs("{", stdout);
        for (uint64_t i = 0; i < n; i++) {
            if (v) fputs(i ? ", " : " ", stdout);
            if (op == XD_STRUCT) {
                const char *fname = desc_str(d, &nl);
                if (v) printf("%.*s = ", nl, fname);
            }
            uint64_t off = desc_num(d, 4);
            show(d, v ? v + off : NULL);
        }
        if (v) fputs(n ? " }" : "}", stdout);
        return;
    }
    case XD_UNION: {
        const char *name = desc_str(d, &nl);
        int name_len = nl;
        uint64_t poff = desc_num(d, 4);
        uint64_t n = desc_num(d, 4);
        int32_t tag = 0;
        if (v) memcpy(&tag, v, 4);
        for (uint64_t i = 0; i < n; i++) {
            const char *vname = desc_str(d, &nl);
            bool has = desc_num(d, 1) != 0;
            bool this = v && (uint64_t)tag == i;
            if (this) printf("%.*s.%.*s", name_len, name, nl, vname);
            if (has) {
                if (this) fputs("(", stdout);
                show(d, this ? v + poff : NULL);
                if (this) fputs(")", stdout);
            }
        }
        return;
    }
    case XD_ENUM: {
        const char *name = desc_str(d, &nl);
        int name_len = nl;
        int w = (int)desc_num(d, 4);
        bool sign = desc_num(d, 1) != 0;
        uint64_t n = desc_num(d, 4);
        int64_t x = v ? load_int(v, w, sign) : 0;
        uint64_t mask = w >= 8 ? UINT64_MAX : ((uint64_t)1 << (8 * w)) - 1;
        bool found = false;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t bits = desc_num(d, 8);
            const char *vname = desc_str(d, &nl);
            if (v && !found && (bits & mask) == ((uint64_t)x & mask)) {
                printf("%.*s.%.*s", name_len, name, nl, vname);
                found = true;
            }
        }
        if (v && !found) printf("%.*s(%" PRId64 ")", name_len, name, x);
        return;
    }
    case XD_OPT_NULL:
    case XD_OPT: {
        bool some = false;
        if (op == XD_OPT) {
            uint64_t flag = desc_num(d, 4);
            some = v && v[flag];
        } else if (v) {
            uint64_t p;
            memcpy(&p, v, 8);
            some = p != 0;
        }
        if (v && !some) fputs("none", stdout);
        if (some) fputs("some(", stdout);
        show(d, some ? v : NULL);
        if (some) fputs(")", stdout);
        return;
    }
    case XD_RESULT: {
        uint64_t off = desc_num(d, 4);
        bool has = desc_num(d, 1) != 0;
        int32_t tag = 0;
        if (v) memcpy(&tag, v, 4);
        bool ok = v && tag == 0;
        if (v && !ok) printf("err(%" PRId32 ")", tag);
        if (ok) fputs(has ? "ok(" : "ok(void())", stdout);
        if (has) show(d, ok ? v + off : NULL);
        if (ok && has) fputs(")", stdout);
        return;
    }
    case XD_OPAQUE: {
        const char *name = desc_str(d, &nl);
        if (v) printf("<%.*s>", nl, name);
        return;
    }
    default:
        d->d = d->end;
        return;
    }
}

static void printer_fault(int sig) {
    (void)sig;
    _exit(2);
}

/* Print `head` and the value, in a process of its own, so that a value
 * holding a dangling pointer cannot take the session down with it: a fault
 * ends the printer quietly. The printer writes nothing until it has the
 * whole line. */
static void show_value(const char *head, const unsigned char *desc, size_t len, uint64_t addr) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, printer_fault);
        signal(SIGBUS, printer_fault);
        static char line[1 << 16];
        setvbuf(stdout, line, _IOFBF, sizeof line);
        fputs(head, stdout);
        Desc d = { desc, desc + len };
        show(&d, (const unsigned char *)(uintptr_t)addr);
        fputs("\n", stdout);
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("%s<unreadable: it points at memory the program does not have>\n", head);
        fflush(stdout);
    }
}

/* ---- The loop ---- */

typedef struct {
    char *head;
    unsigned char *desc;
    uint32_t desc_len;
    uint64_t value;
} Pending;

static Pending g_pending;       /* what the running statement shows when it finishes */
static int g_life = -1;         /* the running statement's line to the holder that forked it */

static void holder_loop(void);

/* A unit's runner calls this when its initializers have run: the statement
 * finished. Its process takes over the session from the holder that forked
 * it, and goes on serving the supervisor. */
static void holder_next(void) {
    fflush(stdout);
    fflush(stderr);
    if (g_pending.desc && g_pending.value)
        show_value(g_pending.head, g_pending.desc, g_pending.desc_len, g_pending.value);
    signal(SIGINT, SIG_IGN);
    write_full(g_life, "c", 1);
    close(g_life);
    g_life = -1;
    write_full(S.resp, "D", 1);
    holder_loop();
    _exit(0);
}

static void holder_run(uint64_t runner) {
    int life[2];
    if (pipe(life) != 0) {
        int status = 0;
        write_full(S.resp, "X", 1);
        write_full(S.resp, &status, sizeof status);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        close(life[0]);
        g_life = life[1];
        signal(SIGINT, SIG_DFL);
        int (*fn)(void (*)(void)) = (int (*)(void (*)(void)))(uintptr_t)runner;
        int code = fn(holder_next);
        /* The runner returned: a `return` at the prompt. */
        exit(code);
    }
    close(life[1]);
    char c;
    ssize_t r;
    do r = read(life[0], &c, 1); while (r < 0 && errno == EINTR);
    if (r == 1) _exit(0);       /* the statement finished; its process holds the session */
    int status = 0;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    close(life[0]);
    write_full(S.resp, "X", 1);
    write_full(S.resp, &status, sizeof status);
}

static void holder_loop(void) {
    for (;;) {
        char op;
        if (!read_full(S.cmd, &op, 1)) _exit(0);
        if (op == 'W' || op == 'Z') {
            uint64_t a[2];
            if (!read_full(S.cmd, a, sizeof a)) _exit(1);
            void *at = (void *)(uintptr_t)a[0];
            if (op == 'Z') memset(at, 0, a[1]);
            else if (!read_full(S.cmd, at, a[1])) _exit(1);
        } else if (op == 'R') {
            uint64_t a[2];
            uint32_t lens[2];
            if (!read_full(S.cmd, a, sizeof a) || !read_full(S.cmd, lens, sizeof lens)) _exit(1);
            free(g_pending.desc);
            free(g_pending.head);
            g_pending.desc = lens[0] ? xmalloc(lens[0]) : NULL;
            g_pending.desc_len = lens[0];
            g_pending.head = xmalloc(lens[1] + 1);
            if (lens[0] && !read_full(S.cmd, g_pending.desc, lens[0])) _exit(1);
            if (!read_full(S.cmd, g_pending.head, lens[1])) _exit(1);
            g_pending.head[lens[1]] = 0;
            g_pending.value = a[1];
            holder_run(a[0]);
        } else {
            _exit(0);
        }
    }
}

/* The holder runs on a stack of its own, which grows by a frame for every
 * statement the session runs and needs more room than the process's. Not
 * under AddressSanitizer, which cannot follow the switch: there the process's
 * stack is room enough for the tests. */
#if defined(__SANITIZE_ADDRESS__)
#define SANITIZED_ADDRESSES 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SANITIZED_ADDRESSES 1
#endif
#endif

static ucontext_t g_holder_ctx;

static void holder_entry(void) {
    holder_loop();
    _exit(0);
}

static void holder_start(void) {
#ifdef SANITIZED_ADDRESSES
    holder_entry();
#endif
    size_t size = (size_t)256 << 20;
    void *stack = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) holder_entry();
    mprotect(stack, 4096, PROT_NONE);   /* the guard: an overflow faults */
    getcontext(&g_holder_ctx);
    g_holder_ctx.uc_stack.ss_sp = stack;
    g_holder_ctx.uc_stack.ss_size = size;
    g_holder_ctx.uc_link = NULL;
    makecontext(&g_holder_ctx, holder_entry, 0);
    setcontext(&g_holder_ctx);
    _exit(1);
}

/* ================================================================
 * Running inputs
 * ================================================================ */

typedef enum { RUN_OK, RUN_FAILED, RUN_COMPILE_ERROR, RUN_END } RunResult;

static void send_blocks(Unit *u) {
    for (int i = 0; i < u->nblocks; i++) {
        Block *b = &u->blocks[i];
        char op = b->data ? 'W' : 'Z';
        uint64_t a[2] = { b->addr, b->size };
        write_full(S.cmd, &op, 1);
        write_full(S.cmd, a, sizeof a);
        if (b->data) write_full(S.cmd, b->data, b->size);
    }
}

/* Collect the holders that have exited (this process adopts them). */
static void reap(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* Run a unit in the holder. RUN_OK when it finished; otherwise *status is
 * how its process ended: an exit is the end of the session (RUN_END), a
 * signal a failure (RUN_FAILED). */
static RunResult run_unit(Unit *u, int *status) {
    fflush(stdout);
    fflush(stderr);
    const char *head = u->head ? u->head : "";
    uint32_t lens[2] = { (uint32_t)(u->head ? u->desc_len : 0), (uint32_t)strlen(head) };
    uint64_t a[2] = { u->runner_addr, u->head ? u->value_addr : 0 };
    write_full(S.cmd, "R", 1);
    write_full(S.cmd, a, sizeof a);
    write_full(S.cmd, lens, sizeof lens);
    if (lens[0]) write_full(S.cmd, u->desc, lens[0]);
    write_full(S.cmd, head, lens[1]);
    char r;
    if (!read_full(S.resp, &r, 1)) {
        *status = SIGKILL;
        reap();
        return RUN_END;
    }
    if (r == 'D') {
        reap();
        if (u->after) fputs(u->after, stdout);
        fflush(stdout);
        return RUN_OK;
    }
    read_full(S.resp, status, sizeof *status);
    reap();
    return WIFEXITED(*status) ? RUN_END : RUN_FAILED;
}

/* ================================================================
 * An input sent from a file
 * ================================================================ */

/* An input may begin with a line `#line N "file"`, as an editor sends a
 * selection: its lines are then the file's, from line N on. Takes the line
 * off *src and sets *file (interned) and *line. False after reporting a
 * `#line` that is not of that form. */
static bool take_line_directive(const char **src, const char **file, int *line) {
    const char *p = *src;
    if (strncmp(p, "#line", 5) != 0 || (p[5] && p[5] != ' ' && p[5] != '\n')) return true;
    p += 5;
    while (*p == ' ') p++;
    long n = 0;
    while (*p >= '0' && *p <= '9' && n < 100000000) n = n * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    char *path = NULL;
    if (n > 0 && *p == '"') {
        size_t len = 0, cap = 64;
        path = xmalloc(cap);
        for (p++; *p && *p != '"' && *p != '\n'; p++) {
            if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
            if (len + 2 > cap) path = xrealloc(path, cap *= 2);
            path[len++] = *p;
        }
        path[len] = 0;
        if (*p == '"' && len > 0) p++;
        else { free(path); path = NULL; }
    }
    while (*p == ' ') p++;
    if (!path || (*p && *p != '\n')) {
        free(path);
        diag_error((SrcLoc){ .line = 1, .col = 1 }, "a #line directive is written "
                   "#line N \"file\": the input's lines are the file's, from line N");
        return false;
    }
    *file = intern_cstr(&S.intern, path);
    *line = (int)n;
    *src = *p ? p + 1 : p;
    free(path);
    return true;
}

/* The indentation every line of an input has, its blank lines aside: an
 * input copied from inside a function is read as if it were at the left
 * margin. */
static int common_indent(const char *s) {
    int least = -1;
    while (*s) {
        int n = 0;
        while (s[n] == ' ') n++;
        if (s[n] && s[n] != '\n' && s[n] != '\r' && (least < 0 || n < least)) least = n;
        const char *nl = strchr(s, '\n');
        if (!nl) break;
        s = nl + 1;
    }
    return least < 0 ? 0 : least;
}

/* `s` with the first `k` columns of each line dropped (a blank line may have
 * fewer), in the session's arena: tokens point into it. */
static const char *dedent(const char *s, int k) {
    char *out = arena_alloc(&S.arena, strlen(s) + 1), *w = out;
    while (*s) {
        for (int i = 0; i < k && *s == ' '; i++) s++;
        while (*s && *s != '\n') *w++ = *s++;
        if (*s) *w++ = *s++;
    }
    *w = 0;
    return out;
}

/* The top-level declarations of the file an input came from (#line), so
 * that a name it uses but the session does not have yet is shown where the
 * file declares it. */
typedef struct {
    const char *name;
    int line;
} FileDecl;

static struct {
    const char *file;
    FileDecl *decls;
    int n, cap;
} g_from;

static void discard_diag(SrcLoc loc, const char *msg, void *ud) {
    (void)loc; (void)msg; (void)ud;
}

/* Read the names the file declares at its left margin: `let [mut] x`,
 * `struct x`, `union x`, `enum x`, `module x`, `error x`. The file may be
 * mid-edit, so what does not lex is passed over in silence. */
static void read_file_decls(const char *file) {
    g_from.file = file;
    g_from.n = 0;
    char *text = read_file(file, NULL);
    if (!text) return;
    jmp_buf env;
    diag_set_sink(discard_diag, NULL);
    diag_set_abort_jmp(&env);
    if (!setjmp(env)) {
        Lexer lexer = { 0 };
        int count = 0;
        lexer_init(&lexer, text, &S.intern, S.flags, S.flag_count);
        Token *t = lexer_tokenize(&lexer, &count);
        for (int i = 0; i + 1 < count; i++) {
            if (t[i].col != 1) continue;
            int j = i + 1;
            if (t[i].kind == TOK_LET && t[j].kind == TOK_MUT && j + 1 < count) j++;
            else if (t[i].kind != TOK_LET && t[i].kind != TOK_STRUCT && t[i].kind != TOK_UNION &&
                     t[i].kind != TOK_ENUM && t[i].kind != TOK_MODULE &&
                     t[i].kind != TOK_ERROR_KW) continue;
            if (t[j].kind != TOK_IDENT) continue;
            const char *name = intern(&S.intern, t[j].start, t[j].length);
            DA_APPEND(g_from.decls, g_from.n, g_from.cap, ((FileDecl){ name, t[j].line }));
        }
        free(t);
    }
    diag_set_abort_jmp(NULL);
    diag_set_sink(NULL, NULL);
    diag_reset_counts();
    free(text);
}

/* Print a diagnostic as diag does, but say of a name the session lacks and
 * the file declares that it is not defined yet, and where it is. */
static void from_file_diag(SrcLoc loc, const char *msg, void *ud) {
    (void)ud;
    if (loc.line == 0) {
        fprintf(stderr, "fcc: error: %s\n", msg);
        return;
    }
    static const char *const unknown[] = { "undefined name '", "unknown type name '" };
    for (size_t i = 0; i < sizeof unknown / sizeof unknown[0]; i++) {
        size_t pre = strlen(unknown[i]), len = strlen(msg);
        if (strncmp(msg, unknown[i], pre) != 0 || len < pre + 2 || msg[len - 1] != '\'') continue;
        const char *name = intern(&S.intern, msg + pre, (int)(len - pre - 1));
        for (int k = 0; k < g_from.n; k++) {
            if (g_from.decls[k].name != name) continue;
            fprintf(stderr, "%s:%d:%d: error: '%s' is not defined in this session yet "
                    "(%s declares it at line %d)\n", loc.filename, loc.line, loc.col, name,
                    g_from.file, g_from.decls[k].line);
            return;
        }
    }
    fprintf(stderr, "%s:%d:%d: error: %s\n", loc.filename, loc.line, loc.col, msg);
}

static jmp_buf g_fatal;
static Units g_units;           /* the input being run (outside the frame setjmp saves) */
static Mark g_start;            /* the session before it */

/* Compile, load and run the lines of one input, the first of them line
 * `first_line` of `filename`. On RUN_END, *code is the session's exit
 * status. The source and tokens are kept for the session: the AST points
 * into the source (an assertion's text), and a generic function's body is
 * compiled again by each statement that needs a new instance of it. */
static RunResult run_lines(const char *src, const char *filename, int first_line, int *code) {
    g_start = mark_session();
    Units *us = &g_units;
    RunResult result = RUN_OK;
    if (setjmp(g_fatal)) {
        diag_set_abort_jmp(NULL);
        undo_input(&g_start);
        units_free(&g_units);
        return RUN_COMPILE_ERROR;
    }
    diag_set_abort_jmp(&g_fatal);
    diag_set_filename(filename);
    /* An input indented as a whole is lexed at the left margin, and its
     * tokens put back in the columns they were typed in. */
    int indent = common_indent(src);
    Lexer lexer = { 0 };
    int count = 0;
    lexer_init(&lexer, indent ? dedent(src, indent) : src, &S.intern, S.flags, S.flag_count);
    lexer.line = lexer.start_line = first_line;
    Token *tokens = lexer_tokenize(&lexer, &count);
    for (int i = 0; i < count; i++) tokens[i].col += indent;
    parser_collect_generic_names(tokens, count, &S.intern, &S.gnames, &S.gn_n, &S.gn_cap);
    int nitems = 0;
    SessionItem *items = parse_session_input(tokens, count, filename, S.gnames, S.gn_n,
                                             &S.arena, &S.intern, &nitems);
    bool ok = diag_error_count() == 0;
    for (int i = 0; ok && i < nitems; i++) {
        int n = 0;
        SessionItem *entered = entered_items(&items[i], &n);
        for (int j = 0; ok && j < n; j++) {
            Unit u;
            bool compiled = compile_unit(&entered[j], filename, &u);
            u.item = i;
            DA_APPEND(us->items, us->count, us->cap, u);
            ok = compiled;
        }
    }
    for (int i = 0; ok && i < us->count; i++) ok = link_unit(&us->items[i]);
    diag_set_abort_jmp(NULL);
    if (!ok) {
        undo_input(&g_start);
        units_free(us);
        return RUN_COMPILE_ERROR;
    }
    for (int i = 0; i < us->count; i++) send_blocks(&us->items[i]);
    for (int i = 0; i < us->count; i++) {
        int status = 0;
        RunResult r = run_unit(&us->items[i], &status);
        if (r == RUN_OK) continue;
        if (r == RUN_END) {
            *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + SIGKILL;
            result = RUN_END;
            break;
        }
        /* A failed statement is undone, with the statements after it: all
         * of it, though it was entered as several units. */
        int first = i;
        while (first > 0 && us->items[first - 1].item == us->items[i].item) first--;
        int later = us->items[us->count - 1].item - us->items[i].item;
        int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        *code = 128 + sig;
        if (S.interactive) {
            if (sig == SIGINT) fputs("\nInterrupted.\n", stderr);   /* after the terminal's ^C */
            else if (sig != SIGABRT) fprintf(stderr, "The statement was killed by %s.\n",
                                             strsignal(sig));
            if (later > 0)
                fprintf(stderr, "(%d later statement%s of this input did not run.)\n",
                        later, later == 1 ? "" : "s");
        }
        undo_declarations(&us->items[first].mark);
        result = RUN_FAILED;
        break;
    }
    units_free(us);
    fflush(stdout);
    fflush(stderr);
    return result;
}

/* Compile, load and run one input: its lines, or those after its #line. */
static RunResult run_input(const char *src, const char *filename, int *code) {
    diag_reset_counts();
    diag_set_filename(filename);
    const char *from = NULL;
    int first_line = 1;
    if (!take_line_directive(&src, &from, &first_line)) return RUN_COMPILE_ERROR;
    if (!from) return run_lines(src, filename, 1, code);
    read_file_decls(from);
    if (g_from.n > 0) diag_set_sink(from_file_diag, NULL);
    RunResult r = run_lines(src, from, first_line, code);
    diag_set_sink(NULL, NULL);
    return r;
}

/* ================================================================
 * Starting the session
 * ================================================================ */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The standard library's files: FCC_STDLIB_DIR, else the install's, else a
 * stdlib directory beside fcc's own (a build in the source tree). */
static void stdlib_files(char ***out, int *n, int *cap) {
    const char *dirs[3] = { getenv("FCC_STDLIB_DIR"), FCC_DATADIR "/fcc/stdlib", NULL };
    char exe[4096];
    char *beside = NULL;
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            beside = str_sprintf("%s/../stdlib", exe);
            dirs[2] = beside;
        }
    }
    for (int k = 0; k < 3; k++) {
        if (!dirs[k] || !*dirs[k]) continue;
        DIR *d = opendir(dirs[k]);
        if (!d) continue;
        int first = *n;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if (l < 4 || strcmp(e->d_name + l - 3, ".fc") != 0) continue;
            char *path = str_sprintf("%s/%s", dirs[k], e->d_name);
            DA_APPEND(*out, *n, *cap, path);
        }
        closedir(d);
        qsort(*out + first, (size_t)(*n - first), sizeof **out, cmp_str);
        if (*n > first) break;
    }
    free(beside);
}

static bool same_file(const char *a, const char *b) {
    char *ra = realpath(a, NULL), *rb = realpath(b, NULL);
    bool same = ra && rb && strcmp(ra, rb) == 0;
    free(ra);
    free(rb);
    return same;
}

/* Load the .fc inputs and the standard library, as one program at the top
 * of the session: their declarations as a file declares them, then their
 * initializers run. Their `main` is an ordinary function. */
static bool load_files(const char **paths, int n) {
    if (n == 0) return true;
    diag_reset_counts();
    if (setjmp(g_fatal)) {
        diag_set_abort_jmp(NULL);
        return false;
    }
    diag_set_abort_jmp(&g_fatal);
    Token **tokens = xcalloc((size_t)n, sizeof(Token *));
    int *counts = xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) {
        diag_set_filename(paths[i]);
        char *src = read_file(paths[i], NULL);
        if (!src) diag_fatal_simple("cannot open '%s'", paths[i]);
        Lexer lexer = { 0 };
        lexer_init(&lexer, src, &S.intern, S.flags, S.flag_count);
        tokens[i] = lexer_tokenize(&lexer, &counts[i]);
        parser_collect_generic_names(tokens[i], counts[i], &S.intern, &S.gnames, &S.gn_n,
                                     &S.gn_cap);
    }
    Program **files = arena_alloc(&S.arena, sizeof(Program *) * (size_t)n);
    for (int i = 0; i < n; i++)
        files[i] = parse_file(tokens[i], counts[i], paths[i], S.gnames, S.gn_n, &S.arena,
                              &S.intern);
    free(counts);
    if (diag_error_count() > 0) { diag_set_abort_jmp(NULL); return false; }
    Program *prog = program_merge(&S.arena, files, n);
    prog->generic_names = S.gnames;
    prog->generic_name_count = S.gn_n;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_LET && strcmp(d->let.name, "main") == 0)
            d->let.codegen_name = intern_cstr(&S.intern, "fc__main");
    }

    SymbolTable *t = xcalloc(1, sizeof *t);
    S.top = t;
    pass1_set_global_root(NULL);
    pass1_collect(prog, t, &S.intern, &S.scopes, false);
    pass2_check(prog, t, &S.intern, &S.mono, &S.scopes, &S.arena);
    if (diag_error_count() > 0) { diag_set_abort_jmp(NULL); return false; }
    mono_discover_transitive_from(&S.mono, 0, &S.arena, &S.intern, t);
    mono_finalize_types_from(&S.mono, 0, &S.arena, &S.intern);
    if (diag_error_count() > 0) { diag_set_abort_jmp(NULL); return false; }
    for (int i = 0; i < prog->decl_count; i++) DA_APPEND(S.env, S.env_n, S.env_cap, prog->decls[i]);

    Unit u = { 0 };
    u.table = t;
    u.runner = intern_sprintf(&S.intern, "fc_run_%d", S.next_unit++);
    X86Unit xu = { .env = S.env, .env_count = S.env_n, .mono_first = 0, .runner = u.runner };
    obj_init(&u.obj);
    x86gen_emit_unit(prog, &u.obj, &S.mono, &S.arena, &S.intern, t, S.include_dirs, S.ninclude,
                     &xu);
    free(xu.desc);
    bool ok = diag_error_count() == 0 && link_unit(&u);
    diag_set_abort_jmp(NULL);
    if (!ok) { unit_free(&u); return false; }
    send_blocks(&u);
    int status = 0;
    RunResult r = run_unit(&u, &status);
    unit_free(&u);
    if (r != RUN_OK) {
        fprintf(stderr, "fcc: the loaded files' initializers did not finish\n");
        return false;
    }
    return true;
}

/* Load the libraries into this process before the holder is forked, so that
 * the holder has them at the same addresses. The linker finds them, as it does
 * for an executable; dlopen would not read a linker script such as libm.so. */
static bool load_libraries(const CompileArgs *ca) {
    LinkArgs link = { (const char *const *)ca->link_inputs, ca->link_count,
                      (const char *const *)ca->lib_dirs, ca->lib_dir_count };
    char **libs = NULL;
    int nlibs = 0;
    bool ok = elf_shared_libs(&link, &libs, &nlibs);
    for (int i = 0; i < nlibs; i++) {
        if (ok) {
            /* Without a slash, dlopen would search for the name. */
            char *path = strchr(libs[i], '/') ? str_dup(libs[i]) : str_sprintf("./%s", libs[i]);
            if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {
                fprintf(stderr, "fcc: cannot load %s: %s\n", libs[i], dlerror());
                ok = false;
            }
            free(path);
        }
        free(libs[i]);
    }
    free(libs);
    return ok;
}

/* ================================================================
 * The prompt
 * ================================================================ */

static bool ends_with_word(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    if (n < wl || memcmp(s + n - wl, w, wl) != 0) return false;
    return n == wl || !(s[n - wl - 1] == '_' || (s[n - wl - 1] >= 'a' && s[n - wl - 1] <= 'z') ||
                        (s[n - wl - 1] >= 'A' && s[n - wl - 1] <= 'Z') ||
                        (s[n - wl - 1] >= '0' && s[n - wl - 1] <= '9'));
}

/* Entered text as the prompt reads it, with comments and literals skipped. */
typedef struct {
    const char *last;           /* the last character that is not blank, or NULL */
    const char *last_line;      /* the start of its line */
    int depth;                  /* the brackets left open */
    bool open_literal;          /* a literal not closed on its line */
} Scan;

static Scan scan_text(const char *text) {
    Scan s = { 0 };
    const char *line_start = text;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { line_start = p + 1; continue; }
        if (p[0] == '/' && p[1] == '/') {
            while (p[1] && p[1] != '\n') p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p;
            for (p++; *p && *p != q && *p != '\n'; p++) if (*p == '\\' && p[1]) p++;
            if (*p != q) { s.open_literal = true; return s; }
            s.last = p; s.last_line = line_start;
            continue;
        }
        if (*p == '(' || *p == '[' || *p == '{') s.depth++;
        if (*p == ')' || *p == ']' || *p == '}') s.depth--;
        if (*p != ' ' && *p != '\t' && *p != '\r') { s.last = p; s.last_line = line_start; }
    }
    return s;
}

/* Whether a line asks for the line after it to be part of its input: it ends
 * with `;`, or with `\`, which is dropped. Only the prompt reads a `\` so;
 * outside a literal it is not FC. */
static bool continues(char *line) {
    Scan s = scan_text(line);
    if (s.open_literal || !s.last) return false;
    if (*s.last == '\\') {
        char *at = line + (s.last - line);
        memmove(at, at + 1, strlen(at + 1) + 1);
        return true;
    }
    return *s.last == ';';
}

/* Whether the text entered so far opens a block: an open bracket, or a line
 * ending where a block or an operand must follow (`=`, `->`, `do`, `then`,
 * `else`, `with`, `in`, an operator, a comma). A literal not closed on its
 * line is the lexer's to report, so the input is complete. */
static bool wants_more(const char *text) {
    Scan sc = scan_text(text);
    if (sc.open_literal) return false;
    if (sc.depth > 0) return true;
    if (!sc.last) return false;
    size_t n = (size_t)(sc.last - sc.last_line) + 1;
    const char *s = sc.last_line;
    static const char *const words[] = { "do", "then", "else", "with", "in" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (ends_with_word(s, n, words[i])) return true;
    char c = s[n - 1];
    return strchr("=>+-*/%,|&^<", c) != NULL;
}

static bool blank(const char *s) {
    for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '\r') return false;
    return true;
}

/* Read one input at the prompt: a line, or a paste; lines while each asks
 * for the next (continues, or Alt+Enter); or a block, which runs to a blank
 * line. NULL at the end of the session. */
static char *read_input(void) {
    char *text = NULL;
    bool block = false;
    for (;;) {
        char *line = NULL;
        LineResult r = lineedit_read(text ? ". " : "> ", &line);
        if (r == LE_EOF) return text;
        if (r == LE_INTERRUPT) {
            if (!text) continue;
            free(text);
            return str_dup("");
        }
        if (r == LE_PASTE) {
            if (!text) return line;
            text = str_appendf(text, "\n%s", line);
            free(line);
            return text;
        }
        /* A blank line ends the input, unless Alt+Enter asked for more. */
        if (r == LE_LINE && blank(line)) {
            free(line);
            if (text) return text;
            continue;
        }
        lineedit_history_add(line);
        bool more = continues(line) || r == LE_MORE;
        if (text) {
            text = str_appendf(text, "\n%s", line);
            free(line);
        } else {
            text = line;
        }
        if (!block) block = wants_more(text);
        if (!block && !more) return text;
    }
}

static char *read_all(FILE *f) {
    size_t n = 0, cap = 4096;
    char *buf = xmalloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (cap - n - 1 == 0) buf = xrealloc(buf, cap *= 2);
    }
    buf[n] = 0;
    return buf;
}

/* Show an input read from a file as if it had been typed. */
static void echo_input(const char *text) {
    const char *p = text;
    bool first = true;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        printf("%s%.*s\n", first ? "> " : ". ", (int)len, p);
        first = false;
        p += len + (nl ? 1 : 0);
    }
    fflush(stdout);
}

static void end_session(void) {
    write_full(S.cmd, "Q", 1);
    close(S.cmd);
    close(S.resp);
    while (wait(NULL) > 0) {}
    lineedit_free();
}

int repl_main(const CompileArgs *ca) {
    memset(&S, 0, sizeof S);
    S.session_scope = -1;
    S.interactive = isatty(STDIN_FILENO);
    S.flags = ca->flags;
    S.flag_count = ca->flag_count;
    S.include_dirs = (const char *const *)ca->include_dirs;
    S.ninclude = ca->include_dir_count;
    g_len_repr = ca->len_repr;
    if (ca->backtraces) {
        fprintf(stderr, "fcc: --backtraces does not apply to --interactive\n");
        return 1;
    }
    arena_init(&S.arena);
    intern_init(&S.intern, &S.arena);
    error_codes_set_append(true);

    const char **files = NULL, **scripts = NULL;
    int nfiles = 0, files_cap = 0, nscripts = 0, scripts_cap = 0;
    for (int i = 0; i < ca->input_count; i++) {
        const char *p = ca->inputs[i];
        size_t l = strlen(p);
        if (l > 4 && strcmp(p + l - 4, ".fcx") == 0) DA_APPEND(scripts, nscripts, scripts_cap, p);
        else DA_APPEND(files, nfiles, files_cap, p);
    }
    char **std = NULL;
    int nstd = 0, std_cap = 0;
    stdlib_files(&std, &nstd, &std_cap);
    for (int i = 0; i < nstd; i++) {
        bool given = false;
        for (int j = 0; j < nfiles && !given; j++) given = same_file(std[i], files[j]);
        if (!given) DA_APPEND(files, nfiles, files_cap, std[i]);
    }

    /* Everything the program will reach must be in place before the holder
     * is forked: the region, and the libraries. */
    void *region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) {
        fprintf(stderr, "fcc: cannot reserve memory for the session's code: %s\n",
                strerror(errno));
        return 1;
    }
    S.region = (uint64_t)(uintptr_t)region;
    if (!dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL)) {
        fprintf(stderr, "fcc: cannot load the math library: %s\n", dlerror());
        return 1;
    }
    if (!load_libraries(ca)) return 1;

    int cmd[2], resp[2];
    if (pipe(cmd) != 0 || pipe(resp) != 0) {
        fprintf(stderr, "fcc: cannot start the session: %s\n", strerror(errno));
        return 1;
    }
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    signal(SIGINT, SIG_IGN);
    fflush(stdout);
    fflush(stderr);
    pid_t holder = fork();
    if (holder < 0) {
        fprintf(stderr, "fcc: cannot start the session: %s\n", strerror(errno));
        return 1;
    }
    if (holder == 0) {
        close(cmd[1]);
        close(resp[0]);
        S.cmd = cmd[0];
        S.resp = resp[1];
        holder_start();
    }
    close(cmd[0]);
    close(resp[1]);
    S.cmd = cmd[1];
    S.resp = resp[0];
    signal(SIGPIPE, SIG_IGN);     /* a holder gone is seen as an end of input */

    /* What arrives while the files load (an editor may send a selection as
     * soon as it starts the session) is read at the first prompt, and shown
     * then, so the terminal does not echo it now. */
    struct termios tty;
    bool quiet = S.interactive && tcgetattr(STDIN_FILENO, &tty) == 0;
    if (quiet) {
        struct termios noecho = tty;
        noecho.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    }
    int code = 0;
    bool loaded = load_files(files, nfiles);
    if (quiet) tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    if (!loaded) {
        end_session();
        return 1;
    }
    if (S.interactive)
        printf("FC %s interactive. Ctrl+D ends the session.\n", fcc_version_string());

    bool ended = false;
    for (int i = 0; i < nscripts && !ended; i++) {
        char *text = read_file(scripts[i], NULL);
        if (!text) {
            fprintf(stderr, "fcc: cannot open '%s'\n", scripts[i]);
            code = 1;
            if (!S.interactive) { ended = true; break; }
            continue;
        }
        if (S.interactive) echo_input(text);
        RunResult r = run_input(text, intern_cstr(&S.intern, scripts[i]), &code);
        if (r == RUN_END) ended = true;
        else if (!S.interactive && r != RUN_OK) {
            if (r == RUN_COMPILE_ERROR) code = 1;
            ended = true;
        }
    }
    if (!ended && !S.interactive) {
        char *text = read_all(stdin);
        RunResult r = run_input(text, intern_cstr(&S.intern, "stdin"), &code);
        if (r == RUN_COMPILE_ERROR) code = 1;
        else if (r == RUN_OK) code = 0;
        ended = true;
    }
    while (!ended) {
        char *text = read_input();
        if (!text) { code = 0; break; }
        if (blank(text)) { free(text); continue; }
        RunResult r = run_input(text, intern_cstr(&S.intern, "stdin"), &code);
        if (r == RUN_END) break;
        code = 0;
    }
    end_session();
    free(files);
    free(scripts);
    for (int i = 0; i < nstd; i++) free(std[i]);
    free(std);
    return code;
}

#endif /* Linux x86-64 */
