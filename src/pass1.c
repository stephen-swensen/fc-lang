#include "pass1.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Copy a malloc'd name array (built by type_collect_vars via DA_APPEND) into the
 * arena so it is reclaimed with the AST. These type_params arrays are aliased
 * from decl/symbol fields and never freed piecemeal; arena-backing them closes a
 * per-analysis leak in the long-running server (the CLI frees the arena at exit). */
static const char **arena_dup_names(Arena *a, const char **src, int n) {
    const char **out = arena_alloc(a, sizeof(const char *) * (size_t)n);
    for (int i = 0; i < n; i++) out[i] = src[i];
    return out;
}

static uint8_t *arena_dup_kinds(Arena *a, const uint8_t *src, int n) {
    uint8_t *out = arena_alloc(a, (size_t)n);
    for (int i = 0; i < n; i++) out[i] = src[i];
    return out;
}

/* Internal kind value marking an already-reported type-vs-const conflict, so
 * the fixpoint doesn't re-report it every iteration. Finalizes to GP_TYPE. */
#define GP_CONFLICT 3

static void diag_kind_conflict(SrcLoc loc, const char *var, const char *owner_name) {
    diag_error(loc, "generic parameter %s is used both as a type and as a constant in '%s'",
               var, owner_name);
}

/* Detect generics: scan fields/params for type variables */
static void detect_generic_struct(Arena *arena, Decl *d, Symbol *sym) {
    if (d->struc.is_generic && d->struc.param_kinds) {
        /* Twin registration (mangled alias / global twin): share the decl's
         * arrays so kind-inference updates are visible through every Symbol. */
        sym->is_generic = true;
        sym->type_params = d->struc.type_params;
        sym->type_param_count = d->struc.type_param_count;
        sym->param_kinds = d->struc.param_kinds;
        return;
    }
    const char **vars = NULL;
    uint8_t *kinds = NULL;
    int vcount = 0, vcap = 0;
    const char *conflict = NULL;
    for (int i = 0; i < d->struc.field_count; i++)
        type_collect_vars_kinds(d->struc.fields[i].type, &vars, &kinds, &vcount, &vcap, &conflict);
    (void)conflict;  /* reported once by the infer_param_kinds fixpoint */
    if (vcount > 0) {
        const char **av = arena_dup_names(arena, vars, vcount);
        uint8_t *ak = arena_dup_kinds(arena, kinds, vcount);
        d->struc.is_generic = true;
        d->struc.type_params = av;
        d->struc.type_param_count = vcount;
        d->struc.param_kinds = ak;
        sym->is_generic = true;
        sym->type_params = av;
        sym->type_param_count = vcount;
        sym->param_kinds = ak;
    }
    free(vars);
    free(kinds);
}

static void detect_generic_union(Arena *arena, Decl *d, Symbol *sym) {
    if (d->unio.is_generic && d->unio.param_kinds) {
        sym->is_generic = true;
        sym->type_params = d->unio.type_params;
        sym->type_param_count = d->unio.type_param_count;
        sym->param_kinds = d->unio.param_kinds;
        return;
    }
    const char **vars = NULL;
    uint8_t *kinds = NULL;
    int vcount = 0, vcap = 0;
    const char *conflict = NULL;
    for (int i = 0; i < d->unio.variant_count; i++)
        type_collect_vars_kinds(d->unio.variants[i].payload, &vars, &kinds, &vcount, &vcap, &conflict);
    (void)conflict;  /* reported once by the infer_param_kinds fixpoint */
    if (vcount > 0) {
        const char **av = arena_dup_names(arena, vars, vcount);
        uint8_t *ak = arena_dup_kinds(arena, kinds, vcount);
        d->unio.is_generic = true;
        d->unio.type_params = av;
        d->unio.type_param_count = vcount;
        d->unio.param_kinds = ak;
        sym->is_generic = true;
        sym->type_params = av;
        sym->type_param_count = vcount;
        sym->param_kinds = ak;
    }
    free(vars);
    free(kinds);
}

/* ---- Reject non-uniform recursive type definitions (audit item 14) ----
 *
 * A generic type may refer to itself only *uniformly* — applied to its own type
 * parameters, in order (e.g. `next: node<'a>*` inside `struct node<'a>`). That is
 * an ordinary recursive data structure: a single, finite monomorphized instance
 * per concrete `'a`. A *non-uniform* self-reference — `node<node<'a>>`, `node<int32>`,
 * or reordered params `pair<'b, 'a>` — demands a different instance at every depth,
 * an infinite family no by-value monomorphizing backend can lay out. We reject it
 * at the definition site (conservative-but-complete), which also gives a precise
 * diagnostic; genuinely mutual/indirect non-uniform recursion that slips past this
 * direct-self check is still caught by the instantiation-depth cap in monomorph.c.
 *
 * Runs on raw parser output before any name mangling, so self-references (still
 * TYPE_STUB with the source name) compare equal to the type's own source name. */

/* The only uniform form: args are exactly params[0..pc) as type vars, in order. */
static bool self_ref_is_uniform(Type **args, int argc, const char **params, int pc) {
    if (argc != pc) return false;
    for (int i = 0; i < argc; i++) {
        if (!args[i] || args[i]->kind != TYPE_TYPE_VAR) return false;
        if (args[i]->type_var.name != params[i]) return false;
    }
    return true;
}

/* Return the offending node if `t` contains a non-uniform reference to
 * `self_name`, else NULL. Recurses through every type constructor and into
 * generic type arguments, but never into a referenced struct/union's *fields*
 * (those belong to that type's own definition) — so the walk stays over the
 * finite syntactic type tree and terminates even for recursive types. */
static Type *find_nonuniform_self_ref(Type *t, const char *self_name,
                                      const char **params, int pc) {
    if (!t) return NULL;
    Type *r;
    switch (t->kind) {
    case TYPE_POINTER:     return find_nonuniform_self_ref(t->pointer.pointee, self_name, params, pc);
    case TYPE_SLICE:       return find_nonuniform_self_ref(t->slice.elem, self_name, params, pc);
    case TYPE_OPTION:      return find_nonuniform_self_ref(t->option.inner, self_name, params, pc);
    case TYPE_RESULT:      return find_nonuniform_self_ref(t->result.inner, self_name, params, pc);
    case TYPE_FIXED_ARRAY: return find_nonuniform_self_ref(t->fixed_array.elem, self_name, params, pc);
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            if ((r = find_nonuniform_self_ref(t->func.param_types[i], self_name, params, pc))) return r;
        return find_nonuniform_self_ref(t->func.return_type, self_name, params, pc);
    case TYPE_STUB:
        if (t->stub.name == self_name && t->stub.type_arg_count > 0 &&
            !self_ref_is_uniform(t->stub.type_args, t->stub.type_arg_count, params, pc))
            return t;
        for (int i = 0; i < t->stub.type_arg_count; i++)
            if ((r = find_nonuniform_self_ref(t->stub.type_args[i], self_name, params, pc))) return r;
        return NULL;
    case TYPE_STRUCT:
        if (t->struc.name == self_name && t->struc.type_arg_count > 0 &&
            !self_ref_is_uniform(t->struc.type_args, t->struc.type_arg_count, params, pc))
            return t;
        for (int i = 0; i < t->struc.type_arg_count; i++)
            if ((r = find_nonuniform_self_ref(t->struc.type_args[i], self_name, params, pc))) return r;
        return NULL;
    case TYPE_UNION:
        if (t->unio.name == self_name && t->unio.type_arg_count > 0 &&
            !self_ref_is_uniform(t->unio.type_args, t->unio.type_arg_count, params, pc))
            return t;
        for (int i = 0; i < t->unio.type_arg_count; i++)
            if ((r = find_nonuniform_self_ref(t->unio.type_args[i], self_name, params, pc))) return r;
        return NULL;
    default: return NULL;
    }
}

/* Build the one legal self-reference form, "name<'a, 'b>", for the diagnostic.
 * Caller frees. Grown to fit — the type name and its parameter list are both
 * unbounded, and this form is quoted to the user as the spelling to write. */
static char *uniform_self_form(const char *name, const char **params, int pc) {
    char *buf = str_sprintf("%s<", name);
    for (int i = 0; i < pc; i++)
        buf = str_appendf(buf, "%s%s", i ? ", " : "", params[i]);
    return str_appendf(buf, ">");
}

static void check_type_def_recursion(Decl *d) {
    const char **params = NULL;
    int pc = 0, pcap = 0;
    const char *self_name = NULL;

    if (d->kind == DECL_STRUCT) {
        if (d->struc.is_extern) return;
        self_name = d->struc.name;
        for (int i = 0; i < d->struc.field_count; i++)
            type_collect_vars(d->struc.fields[i].type, &params, &pc, &pcap);
    } else if (d->kind == DECL_UNION) {
        self_name = d->unio.name;
        for (int i = 0; i < d->unio.variant_count; i++)
            type_collect_vars(d->unio.variants[i].payload, &params, &pc, &pcap);
    } else {
        return;
    }
    if (pc == 0) { free(params); return; }  /* not generic: no non-uniform form possible */

    /* Scan each field/variant type for a non-uniform self-reference. */
    Type *bad = NULL;
    if (d->kind == DECL_STRUCT) {
        for (int i = 0; i < d->struc.field_count && !bad; i++)
            bad = find_nonuniform_self_ref(d->struc.fields[i].type, self_name, params, pc);
    } else {
        for (int i = 0; i < d->unio.variant_count && !bad; i++)
            bad = find_nonuniform_self_ref(d->unio.variants[i].payload, self_name, params, pc);
    }
    if (bad) {
        char *form = uniform_self_form(self_name, params, pc);
        diag_error(d->loc,
            "non-uniform recursive type '%s' is not supported: a generic type may "
            "only refer to itself uniformly as '%s'; a self-reference with any other "
            "type arguments would require infinitely many monomorphized instances",
            self_name, form);
        free(form);
    }
    free(params);
}

/* Walk all struct/union definitions (descending into modules) for non-uniform
 * self-reference. Called before pass1 mangles any names. */
static void check_recursion_in_decls(Decl **decls, int count) {
    for (int i = 0; i < count; i++) {
        Decl *d = decls[i];
        if (d->kind == DECL_STRUCT || d->kind == DECL_UNION)
            check_type_def_recursion(d);
        else if (d->kind == DECL_MODULE)
            check_recursion_in_decls(d->module.decls, d->module.decl_count);
    }
}

/* True when `name` is one of the built-in type names (i32, str, any, char, …).
 * The single source of truth is type_from_name, which is exactly the lookup
 * every type position consults — so this predicate matches wins-before-any-user
 * -declaration behavior by construction. */
static bool is_builtin_type_name(const char *name) {
    return name && type_from_name(name, (int)strlen(name)) != NULL;
}

/* §7.8: a built-in type name is resolved before any user declaration in every
 * type and module position (a field's `: i32`, a cast target, `mod.member`
 * property lookup), so a user *type or module* named after one is permanently
 * unreachable — its uses bind to the built-in instead ("expected i32, got i32"
 * on a `struct i32`; "type 'i32' has no property …" on a `module i32`). Reject
 * such a declaration outright rather than let it sit shadowed and dead.
 *
 * Value bindings are deliberately exempt: a `let`, parameter, or loop variable
 * named `i32`/`any`/`char` lives in a separate namespace from the type, never
 * collides with it, and the standard library relies on the spelling (every
 * container's `any` combinator, `array_list.any(pred)`). This walks the module
 * tree so a nested `module m` containing `struct i32` is caught too, and runs
 * before any name mangling, while declaration names are still their source
 * spellings. Import aliases are checked at their resolution sites, where the
 * imported symbol's kind (type/module vs value) is known. */
static void check_builtin_type_name_decls(Decl **decls, int count) {
    for (int i = 0; i < count; i++) {
        Decl *d = decls[i];
        const char *name = NULL;
        const char *what = NULL;
        switch (d->kind) {
        case DECL_STRUCT:
            name = d->struc.name;
            what = d->struc.is_c_union ? "a union" : "a struct";
            break;
        case DECL_UNION:
            name = d->unio.name;
            what = "a union";
            break;
        case DECL_ENUM:
            name = d->enu.name;
            what = "an enum";
            break;
        case DECL_MODULE:
            name = d->module.name;
            what = d->module.is_error_group ? "an error group" : "a module";
            check_builtin_type_name_decls(d->module.decls, d->module.decl_count);
            break;
        case DECL_NAMESPACE:
            name = d->ns.name;
            what = "a namespace";
            break;
        default:
            break;
        }
        if (is_builtin_type_name(name))
            diag_error(d->loc,
                "'%s' is a built-in type name and cannot be used as %s name",
                name, what);
    }
}

static void detect_generic_func(Arena *arena, Decl *d, Symbol *sym) {
    if (!d->let.init || d->let.init->kind != EXPR_FUNC) return;
    Expr *fn = d->let.init;
    const char **vars = NULL;
    uint8_t *kinds = NULL;
    int vcount = 0, vcap = 0;
    const char *conflict = NULL;

    /* Collect from explicit type vars (kind unknown until an occurrence
     * pins it — a prefix var may be used only in the body) */
    for (int i = 0; i < fn->func.explicit_type_var_count; i++) {
        if (vcount >= vcap) {
            vcap = vcap ? vcap * 2 : 8;
            vars = realloc(vars, (size_t)vcap * sizeof(*vars));
            kinds = realloc(kinds, (size_t)vcap * sizeof(*kinds));
        }
        vars[vcount] = fn->func.explicit_type_vars[i];
        kinds[vcount] = GP_UNKNOWN;
        vcount++;
    }

    /* Collect from parameter types */
    for (int i = 0; i < fn->func.param_count; i++)
        type_collect_vars_kinds(fn->func.params[i].type, &vars, &kinds, &vcount, &vcap, &conflict);
    (void)conflict;  /* reported once by the infer_param_kinds fixpoint */

    if (vcount > 0) {
        sym->is_generic = true;
        sym->type_params = arena_dup_names(arena, vars, vcount);
        sym->type_param_count = vcount;
        sym->explicit_type_param_count = fn->func.explicit_type_var_count;
        sym->param_kinds = arena_dup_kinds(arena, kinds, vcount);
    }
    free(vars);
    free(kinds);
}

void symtab_init(SymbolTable *t) {
    t->symbols = NULL;
    t->count = 0;
    t->capacity = 0;
}

Symbol *symtab_lookup(SymbolTable *t, const char *name) {
    /* Since names are interned, pointer comparison works */
    for (int i = 0; i < t->count; i++) {
        if (t->symbols[i].name == name) {
            return &t->symbols[i];
        }
    }
    return NULL;
}

Symbol *symtab_lookup_kind(SymbolTable *t, const char *name, DeclKind kind) {
    for (int i = 0; i < t->count; i++) {
        if (t->symbols[i].name == name && t->symbols[i].kind == kind) {
            return &t->symbols[i];
        }
    }
    return NULL;
}

/* Namespace-filtered lookup by name and kind. Returns the entry whose
 * ns_prefix pointer matches (both NULL or same interned ptr). Needed for
 * global symtab where top-level types may be registered under the same name
 * across multiple namespaces. */
Symbol *symtab_lookup_kind_ns(SymbolTable *t, const char *name, DeclKind kind,
                               const char *ns_prefix) {
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->name == name && s->kind == kind && s->ns_prefix == ns_prefix) {
            return s;
        }
    }
    return NULL;
}

void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl) {
    Symbol sym = { .name = name, .ns_prefix = NULL, .kind = kind, .decl = decl,
                   .type = NULL, .members = NULL, .imports = NULL, .parent = NULL,
                   .is_private = false,
                   .is_generic = false, .type_params = NULL, .type_param_count = 0 };
    DA_APPEND(t->symbols, t->count, t->capacity, sym);
}

void symtab_free_nested(SymbolTable *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->imports) {
            free(s->imports->entries);
            free(s->imports);
            s->imports = NULL;
        }
        if (s->members) {
            symtab_free_nested(s->members);
            free(s->members->symbols);
            free(s->members);
            s->members = NULL;
        }
    }
}

/* ---- Import table helpers ---- */

/* Add or replace (shadow) an import ref in an import table */
static void import_table_add(ImportTable *tbl, const char *local_name,
                              const char *source_name, DeclKind kind,
                              SymbolTable *source_members, Symbol *msym) {
    /* Check for existing entry with same local_name AND kind (shadowing: replace).
     * Different kinds coexist — e.g., a struct and its companion module both
     * live in the table under the same name, found by kind-specific lookup. */
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].local_name == local_name && tbl->entries[i].kind == kind) {
            tbl->entries[i].source_name = source_name;
            tbl->entries[i].source_members = source_members;
            tbl->entries[i].ns_prefix = msym->ns_prefix;
            tbl->entries[i].module_members = msym->members;
            tbl->entries[i].is_generic = msym->is_generic;
            tbl->entries[i].type_params = msym->type_params;
            tbl->entries[i].type_param_count = msym->type_param_count;
            tbl->entries[i].explicit_type_param_count = msym->explicit_type_param_count;
            tbl->entries[i].param_kinds = msym->param_kinds;
            return;
        }
    }
    ImportRef ref = {
        .local_name = local_name,
        .source_name = source_name,
        .kind = kind,
        .source_members = source_members,
        .ns_prefix = msym->ns_prefix,
        .module_members = msym->members,
        .is_generic = msym->is_generic,
        .type_params = msym->type_params,
        .type_param_count = msym->type_param_count,
        .explicit_type_param_count = msym->explicit_type_param_count,
        .param_kinds = msym->param_kinds,
    };
    DA_APPEND(tbl->entries, tbl->count, tbl->capacity, ref);
}

/* Add a whole-module import to an import table.
 *
 * A top-level module lives in the global symtab, so this is just import_table_add
 * with source_members = the global symtab, source_name = the module's name, and
 * kind = DECL_MODULE (a module never carries generic metadata, so passing mod_sym
 * through leaves those fields empty). Delegating keeps the (local_name, kind)
 * dedup rule — a later module import shadows an earlier one, but a same-named
 * struct/union import coexists (the companion pattern) — owned in one place, so
 * the two entry points can't drift apart. */
static void import_table_add_module(ImportTable *tbl, const char *local_name,
                                     Symbol *mod_sym, SymbolTable *global_symtab) {
    import_table_add(tbl, local_name, mod_sym->name, DECL_MODULE, global_symtab, mod_sym);
}

/* Find or create a per-file import scope */
static ImportTable *get_file_imports(FileImportScopes *scopes, const char *filename) {
    for (int i = 0; i < scopes->count; i++) {
        if (scopes->scopes[i].filename == filename)
            return &scopes->scopes[i].imports;
    }
    FileImportScope scope = { .filename = filename };
    memset(&scope.imports, 0, sizeof(ImportTable));
    DA_APPEND(scopes->scopes, scopes->count, scopes->capacity, scope);
    return &scopes->scopes[scopes->count - 1].imports;
}

/* Process a member import (wildcard or named) into an ImportTable.
 * Handles: import * from MODULE, import NAME [as ALIAS] from MODULE.
 * Does NOT handle whole-module imports (import MODULE [as ALIAS]). */
static void process_member_import(Decl *d, ImportTable *target,
                                   SymbolTable *symtab, InternTable *intern  __attribute__((unused)),
                                   const char *current_ns) {
    const char *mod_name = d->import.from_module;
    const char *from_ns = d->import.from_namespace;

    if (from_ns && strcmp(from_ns, "global") == 0) {
        diag_error(d->loc, "cannot import from 'global::'; use 'import %s from %s' instead",
            d->import.is_wildcard ? "*" : d->import.name, mod_name);
        return;
    }

    /* Look up the source module: global symtab first, then import table
     * (a prior whole-module import in the same scope may have brought it in) */
    const char *lookup_ns = from_ns ? from_ns : current_ns;
    Symbol *mod_sym = symtab_lookup_module(symtab, mod_name, lookup_ns);
    /* Fall back to global namespace only if we're already in the global namespace.
     * Non-global namespaces cannot implicitly access global-namespace modules. */
    if (!mod_sym && !lookup_ns) mod_sym = symtab_lookup_module(symtab, mod_name, NULL);
    if (!mod_sym || mod_sym->kind != DECL_MODULE) {
        /* Check the target import table for a whole-module import */
        for (int k = 0; k < target->count; k++) {
            ImportRef *ref = &target->entries[k];
            if (ref->local_name == mod_name && ref->kind == DECL_MODULE && ref->module_members) {
                /* Found via import — resolve using namespace-aware lookup */
                mod_sym = ref->ns_prefix
                    ? symtab_lookup_module(ref->source_members, ref->source_name, ref->ns_prefix)
                    : symtab_lookup(ref->source_members, ref->source_name);
                break;
            }
        }
    }
    if (!mod_sym || mod_sym->kind != DECL_MODULE) {
        diag_error(d->loc, "unknown module '%s'", mod_name);
        return;
    }

    if (d->import.is_wildcard) {
        /* import * from MODULE: add all non-private members */
        SymbolTable *members = mod_sym->members;
        for (int j = 0; j < members->count; j++) {
            Symbol *msym = &members->symbols[j];
            if (msym->is_private) continue;
            import_table_add(target, msym->name, msym->name, msym->kind,
                             members, msym);
        }
    } else {
        /* import NAME [as ALIAS] from MODULE */
        Symbol *msym = symtab_lookup(mod_sym->members, d->import.name);
        if (!msym) {
            diag_error(d->loc, "module '%s' has no member '%s'",
                mod_name, d->import.name);
            return;
        }
        if (msym->is_private) {
            diag_error(d->loc, "cannot import private member '%s' from module '%s'",
                d->import.name, mod_name);
            return;
        }
        const char *import_name = d->import.alias ? d->import.alias : d->import.name;
        /* §7.8: an `as` alias may not rename a type or module onto a built-in
         * type name — that alias would be unreachable exactly as a declaration
         * with the name would be. Value members (a `let`) are exempt, matching
         * the rule for declarations. A non-aliased import can only carry a name
         * its declaration already had, which was checked there. */
        if (d->import.alias && is_builtin_type_name(import_name) &&
            (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION ||
             msym->kind == DECL_ENUM || msym->kind == DECL_MODULE)) {
            diag_error(d->loc,
                "import alias '%s' is a built-in type name and cannot name a type or module",
                import_name);
        }
        import_table_add(target, import_name, d->import.name, msym->kind,
                         mod_sym->members, msym);
        /* Type-associated module: if importing a type, also import its
         * associated module under the same name. */
        if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION ||
            msym->kind == DECL_ENUM) {
            Symbol *assoc_mod = symtab_lookup_kind(mod_sym->members,
                d->import.name, DECL_MODULE);
            if (assoc_mod && !assoc_mod->is_private) {
                import_table_add(target, import_name, d->import.name,
                                 DECL_MODULE, mod_sym->members, assoc_mod);
            }
        }
    }
}

/* Find a module symbol by name and namespace prefix */
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix) {
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->kind != DECL_MODULE) continue;
        if (s->name != name) continue;
        if (ns_prefix == s->ns_prefix) return s;  /* both NULL or same interned ptr */
    }
    return NULL;
}

/* Build a mangled name: prefix__name
 * Uses __ (double underscore) to separate namespace/module hierarchy levels.
 * This avoids collisions between e.g. namespace foo:: module bar (foo__bar)
 * and global module foo_bar (foo_bar). */
static const char *make_mangled(InternTable *intern, const char *prefix, const char *name) {
    return intern_sprintf(intern, "%s__%s", prefix, name);
}

/* The mangling prefix for a declaration path rooted at namespace `ns` (NULL =
 * the implicit global namespace).
 *
 * EVERY user-declared name that reaches C file scope is rooted at `fc__`: a
 * file-scope decl is `fc__name`, a module member `fc__mod__name`, a namespaced
 * one `fc__ns__mod__name`. Rooting the whole path — rather than only file-scope
 * decls — is what keeps the two schemes disjoint, so a module or namespace
 * named `fc` (whose members used to mangle to the same `fc__<name>` as a
 * file-scope decl, silently merging two globals into one) is just another path
 * component: `fc__fc__counter`.
 *
 * The reserved root also buys the converse: since `__` cannot appear in an FC
 * identifier, no user name can start with `fc__`, so any compiler-derived
 * spelling that does *not* (`fc_str`, `fc_eq_*`, `fc_tag_*`, `_l_x_3`) is
 * unreachable from source by construction. */
static const char *mangle_root(InternTable *intern, const char *ns) {
    return ns ? make_mangled(intern, "fc", ns) : "fc";
}

/* Canonicalize generic stub names in a type tree.
 * Walks through compound type wrappers and, for TYPE_STUB nodes with
 * type_arg_count > 0, updates the stub's name to the canonical mangled form
 * from the module's symbol table. This is a NAME-ONLY update: preserves
 * type_args, doesn't replace with full type, doesn't create circular
 * references. Mutates the type in place. */
static void canonicalize_stub_names(Type *t, SymbolTable *members) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_POINTER: canonicalize_stub_names(t->pointer.pointee, members); return;
    case TYPE_SLICE:   canonicalize_stub_names(t->slice.elem, members); return;
    case TYPE_OPTION:  canonicalize_stub_names(t->option.inner, members); return;
    case TYPE_RESULT:  canonicalize_stub_names(t->result.inner, members); return;
    case TYPE_FIXED_ARRAY: canonicalize_stub_names(t->fixed_array.elem, members); return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            canonicalize_stub_names(t->func.param_types[i], members);
        canonicalize_stub_names(t->func.return_type, members);
        return;
    case TYPE_STUB:
        if (t->stub.type_arg_count > 0 && t->stub.name) {
            /* Try struct first, then union — stubs are kind-agnostic */
            Symbol *sym = symtab_lookup_kind(members, t->stub.name, DECL_STRUCT);
            if (!sym)
                sym = symtab_lookup_kind(members, t->stub.name, DECL_UNION);
            if (sym && sym->type) {
                const char *canon = (sym->type->kind == TYPE_STRUCT)
                    ? sym->type->struc.name : sym->type->unio.name;
                const char *qname = (sym->type->kind == TYPE_STRUCT)
                    ? sym->type->struc.qualified_name : sym->type->unio.qualified_name;
                if (canon != t->stub.name) {
                    t->stub.name = canon;
                    if (qname) t->stub.qualified_name = qname;
                }
            }
        }
        return;
    case TYPE_STRUCT:
        /* Don't recurse into fields of real struct types */
        return;
    case TYPE_UNION:
        return;
    default: return;
    }
}

/* Resolve type stubs in a type tree against a symbol table.
 * Recursively walks the type tree (through pointers, slices, options, fixed arrays,
 * and function types) and replaces TYPE_STUB nodes with the actual type from the
 * symtab. Used to resolve references to sibling extern types within the same
 * from-module. */
static Type *resolve_type_stubs(Arena *arena, Type *t, SymbolTable *members) {
    if (!t) return t;
    if (t->kind == TYPE_POINTER) {
        Type *inner = resolve_type_stubs(arena, t->pointer.pointee, members);
        if (inner != t->pointer.pointee) {
            Type *r = arena_alloc(arena, sizeof(Type));
            *r = *t;
            r->pointer.pointee = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_SLICE) {
        Type *inner = resolve_type_stubs(arena, t->slice.elem, members);
        if (inner != t->slice.elem) {
            Type *r = arena_alloc(arena, sizeof(Type));
            *r = *t;
            r->slice.elem = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_OPTION) {
        Type *inner = resolve_type_stubs(arena, t->option.inner, members);
        if (inner != t->option.inner) {
            Type *r = arena_alloc(arena, sizeof(Type));
            *r = *t;
            r->option.inner = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_RESULT) {
        Type *inner = resolve_type_stubs(arena, t->result.inner, members);
        if (inner != t->result.inner) {
            Type *r = arena_alloc(arena, sizeof(Type));
            *r = *t;
            r->result.inner = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_FIXED_ARRAY) {
        Type *inner = resolve_type_stubs(arena, t->fixed_array.elem, members);
        if (inner != t->fixed_array.elem) {
            Type *r = arena_alloc(arena, sizeof(Type));
            *r = *t;
            r->fixed_array.elem = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_FUNC) {
        bool changed = false;
        Type **params = arena_alloc(arena, sizeof(Type*) * (size_t)t->func.param_count);
        for (int i = 0; i < t->func.param_count; i++) {
            params[i] = resolve_type_stubs(arena, t->func.param_types[i], members);
            if (params[i] != t->func.param_types[i]) changed = true;
        }
        Type *ret = resolve_type_stubs(arena, t->func.return_type, members);
        if (ret != t->func.return_type) changed = true;
        if (!changed) return t;
        Type *r = arena_alloc(arena, sizeof(Type));
        *r = *t;
        r->func.param_types = params;
        r->func.return_type = ret;
        return r;
    }
    if (t->kind == TYPE_STUB && t->stub.name && t->stub.type_arg_count == 0) {
        Symbol *sym = symtab_lookup_kind(members, t->stub.name, DECL_STRUCT);
        if (sym && sym->type) return sym->type;
        sym = symtab_lookup_kind(members, t->stub.name, DECL_UNION);
        if (sym && sym->type) return sym->type;
        sym = symtab_lookup_kind(members, t->stub.name, DECL_ENUM);
        if (sym && sym->type) return sym->type;
    }
    return t;
}

/* Register a struct type symbol and return the created type */
static Type *register_struct_sym(SymbolTable *tab, InternTable *intern, Decl *d) {
    /* File-scope (no namespace/module) struct types are mangled into the
     * compiler-reserved `fc__` namespace, exactly as module members get
     * `m__name`. The bare tag/typedef would otherwise reach C file scope raw
     * and collide with C keywords (`struct restrict`) or libc typedefs
     * (`FILE`, `size_t`). The symtab is still keyed by the source name so name
     * resolution is unchanged; the mangled name is also registered so that
     * canonicalized type stubs (e.g. from monomorphization) resolve directly.
     * qualified_name stays the source name for diagnostics. */
    const char *src_name = d->struc.name;
    const char *mangled = make_mangled(intern, "fc", src_name);
    d->struc.name = mangled;
    symtab_add(tab, src_name, DECL_STRUCT, d);
    /* Use the last added entry (not symtab_lookup which may find a module with same name) */
    Symbol *sym = &tab->symbols[tab->count - 1];
    Type *st = arena_alloc(intern->arena, sizeof(Type));
    st->kind = TYPE_STRUCT;
    st->struc.name = mangled;
    st->struc.qualified_name = src_name;
    st->struc.c_name = d->struc.c_name;
    st->struc.is_c_union = d->struc.is_c_union;
    st->struc.fields = d->struc.fields;
    st->struc.field_count = d->struc.field_count;
    st->struc.type_args = NULL;
    st->struc.type_arg_count = 0;
    sym->type = st;
    /* resolved_sym is bound in the final phase (set_type_resolved_syms), not here:
     * the symtab_add below — and every later one — may realloc tab->symbols, so a
     * &symbols[i] taken now would dangle. */
    detect_generic_struct(intern->arena, d, sym);
    symtab_add(tab, mangled, DECL_STRUCT, d);
    Symbol *sym2 = &tab->symbols[tab->count - 1];
    sym2->type = st;
    detect_generic_struct(intern->arena, d, sym2);
    return st;
}

/* Register a union type symbol and return the created type */
static Type *register_union_sym(SymbolTable *tab, InternTable *intern, Decl *d) {
    /* See register_struct_sym: file-scope union types are mangled into the
     * `fc__` namespace so their tag/typedef cannot collide with C keywords or
     * libc names at C file scope. */
    const char *src_name = d->unio.name;
    const char *mangled = make_mangled(intern, "fc", src_name);
    d->unio.name = mangled;
    symtab_add(tab, src_name, DECL_UNION, d);
    /* Use the last added entry (not symtab_lookup which may find a module with same name) */
    Symbol *sym = &tab->symbols[tab->count - 1];
    Type *ut = arena_alloc(intern->arena, sizeof(Type));
    ut->kind = TYPE_UNION;
    ut->unio.name = mangled;
    ut->unio.qualified_name = src_name;
    ut->unio.variants = d->unio.variants;
    ut->unio.variant_count = d->unio.variant_count;
    ut->unio.type_args = NULL;
    ut->unio.type_arg_count = 0;
    sym->type = ut;
    /* resolved_sym deferred to set_type_resolved_syms — see register_struct_sym. */
    detect_generic_union(intern->arena, d, sym);
    symtab_add(tab, mangled, DECL_UNION, d);
    Symbol *sym2 = &tab->symbols[tab->count - 1];
    sym2->type = ut;
    detect_generic_union(intern->arena, d, sym2);
    return ut;
}

/* Format an enum variant's canonical bits as a signed/unsigned decimal for
 * diagnostics. */
static void enum_val_str(char *buf, size_t cap, uint64_t bits, Type *repr) {
    uint64_t w = 0;
    switch (repr->kind) {
    case TYPE_INT8: case TYPE_UINT8:  w = 8;  break;
    case TYPE_INT16: case TYPE_UINT16: w = 16; break;
    case TYPE_INT64: case TYPE_UINT64: w = 64; break;
    default: w = 32; break;
    }
    uint64_t mask = (w == 64) ? UINT64_MAX : ((1ULL << w) - 1);
    if (type_is_signed(repr) && bits > (mask >> 1)) {
        snprintf(buf, cap, "-%llu", (unsigned long long)((mask - bits) + 1));
    } else {
        snprintf(buf, cap, "%llu", (unsigned long long)bits);
    }
}

/* Resolve and validate an enum's variant values in place: C-style
 * auto-numbering (first = 0, otherwise previous + 1), normalization to
 * two\'s-complement bits truncated to the repr width, repr-fit checks,
 * duplicate-value/name checks, and the mandatory zero variant (zero-filled
 * memory must be a valid value, so default(E) exists). All diag_error, never
 * fatal: a malformed enum still ends up with well-formed values so later
 * passes never chase garbage. Must run before the decl name is mangled so
 * messages show the source name. */
static void resolve_enum_values(Decl *d) {
    if (d->enu.values_resolved) return;
    d->enu.values_resolved = true;
    if (!d->enu.repr) d->enu.repr = type_int32();
    Type *repr = d->enu.repr;
    const char *ename = d->enu.name;
    int w;
    switch (repr->kind) {
    case TYPE_INT8: case TYPE_UINT8:  w = 8;  break;
    case TYPE_INT16: case TYPE_UINT16: w = 16; break;
    case TYPE_INT64: case TYPE_UINT64: w = 64; break;
    default: w = 32; break;
    }
    bool sign = type_is_signed(repr);
    uint64_t mask = (w == 64) ? UINT64_MAX : ((1ULL << w) - 1);
    uint64_t max_pos = sign ? (mask >> 1) : mask;   /* largest positive value */
    uint64_t min_mag = sign ? (mask >> 1) + 1 : 0;  /* |most negative| (signed) */

    if (d->enu.variant_count == 0) {
        diag_error(d->loc, "enum '%s' must declare at least one variant", ename);
        return;
    }

    bool have_prev = false;
    uint64_t prev = 0;
    bool *any_poisoned = calloc((size_t)d->enu.variant_count, sizeof(bool));
    for (int i = 0; i < d->enu.variant_count; i++) {
        EnumVariant *v = &d->enu.variants[i];
        uint64_t bits = 0;
        bool poisoned = false;
        if (v->has_explicit) {
            uint64_t mag = v->value_bits;
            if (v->negative && mag > 0) {
                if (!sign) {
                    diag_error(v->loc, "enum value -%llu does not fit repr %s",
                        (unsigned long long)mag, type_name(repr));
                    poisoned = true;
                } else if (mag > min_mag) {
                    diag_error(v->loc, "enum value -%llu does not fit repr %s",
                        (unsigned long long)mag, type_name(repr));
                    poisoned = true;
                } else {
                    bits = (0 - mag) & mask;
                }
            } else {
                if (mag > max_pos) {
                    diag_error(v->loc, "enum value %llu does not fit repr %s",
                        (unsigned long long)mag, type_name(repr));
                    poisoned = true;
                } else {
                    bits = mag & mask;
                }
            }
        } else if (!have_prev) {
            bits = 0;
        } else if (prev == max_pos) {
            diag_error(v->loc, "enum value for '%s' overflows repr %s "
                "(previous variant holds the largest %s value)",
                v->name, type_name(repr), type_name(repr));
            poisoned = true;
        } else {
            bits = (prev + 1) & mask;
        }
        v->value_bits = bits;
        if (poisoned) any_poisoned[i] = true;
        prev = bits;
        have_prev = true;
    }

    bool has_zero = false;
    for (int i = 0; i < d->enu.variant_count; i++) {
        EnumVariant *vi = &d->enu.variants[i];
        if (vi->value_bits == 0) has_zero = true;
        if (any_poisoned[i]) continue;  /* fit errors already reported; don't cascade */
        for (int j = 0; j < i; j++) {
            EnumVariant *vj = &d->enu.variants[j];
            if (any_poisoned[j]) continue;
            if (vi->name == vj->name) {
                diag_error(vi->loc, "duplicate variant name '%s' in enum '%s'",
                    vi->name, ename);
            } else if (vi->value_bits == vj->value_bits) {
                char valbuf[32];
                enum_val_str(valbuf, sizeof valbuf, vi->value_bits, repr);
                diag_error(vi->loc,
                    "duplicate value %s in enum '%s': variants '%s' and '%s'",
                    valbuf, ename, vj->name, vi->name);
            }
        }
    }
    if (!has_zero) {
        diag_error(d->loc, "enum '%s' must have a variant with value 0 "
            "(zero-filled memory must be a valid value; default(%s) is that variant)",
            ename, ename);
    }
    free(any_poisoned);
}

/* Register an enum type symbol and return the created type */
static Type *register_enum_sym(SymbolTable *tab, InternTable *intern, Decl *d) {
    /* See register_struct_sym: file-scope enum types are mangled into the
     * `fc__` namespace so their typedef cannot collide with C keywords or
     * libc names at C file scope. */
    resolve_enum_values(d);
    const char *src_name = d->enu.name;
    const char *mangled = make_mangled(intern, "fc", src_name);
    d->enu.name = mangled;
    symtab_add(tab, src_name, DECL_ENUM, d);
    Symbol *sym = &tab->symbols[tab->count - 1];
    Type *et = arena_alloc(intern->arena, sizeof(Type));
    et->kind = TYPE_ENUM;
    et->enu.name = mangled;
    et->enu.qualified_name = src_name;
    et->enu.repr = d->enu.repr;
    et->enu.variants = d->enu.variants;
    et->enu.variant_count = d->enu.variant_count;
    sym->type = et;
    /* resolved_sym deferred to set_type_resolved_syms — see register_struct_sym. */
    symtab_add(tab, mangled, DECL_ENUM, d);
    Symbol *sym2 = &tab->symbols[tab->count - 1];
    sym2->type = et;
    return et;
}

/* Register module members: compute mangled names, populate sub-symtab */
/* Build a qualified display name: prefix.name */
static const char *make_qualified(InternTable *intern, const char *prefix, const char *name) {
    int needed = snprintf(NULL, 0, "%s.%s", prefix, name) + 1;
    char *buf = malloc((size_t)needed);
    snprintf(buf, (size_t)needed, "%s.%s", prefix, name);
    const char *result = intern_cstr(intern, buf);
    free(buf);
    return result;
}

/* Protocol / result-return agreement for extern function declarations
 * (spec/result-type-design.md §C interop): a result return requires an error
 * protocol, a protocol requires a result return, and the declared payload
 * kind must fit the protocol — the sentinel must be comparable to the raw C
 * return (-1/0 need an integer or void payload, null needs a pointer),
 * `status` carries no payload at all (the return value IS the code), and
 * neg_errno/hresult read the code out of a signed return. */
static void validate_extern_protocol(Decl *d, const char *src_name) {
    ExternProtocol proto = d->ext.protocol;
    if (proto == EXT_PROTO_ERROR) return;  /* malformed clause already reported */
    Type *ret = d->ext.type->func.return_type;
    bool ret_result = ret && ret->kind == TYPE_RESULT;
    if (proto == EXT_PROTO_NONE) {
        if (ret_result)
            diag_error(d->loc, "extern '%s' returns %s but declares no error "
                "protocol; add `from <protocol>` after the type (errno(-1), "
                "errno(null), status, neg_errno, hresult, last_error(<sentinel>), "
                "wsa_error(-1))", src_name, type_name(ret));
        return;
    }
    if (!ret_result) {
        diag_error(d->loc, "extern '%s' declares an error protocol but returns "
            "%s, not a result type (T!)", src_name,
            ret ? type_name(ret) : "void");
        return;
    }
    Type *pay = ret->result.inner;
    bool pay_void = pay && pay->kind == TYPE_VOID;
    bool pay_int = pay && type_is_integer(pay);
    bool pay_sint = pay_int && type_is_signed(pay);
    bool pay_ptr = pay && (pay->kind == TYPE_POINTER || pay->kind == TYPE_ANY_PTR);
    switch (proto) {
    case EXT_PROTO_ERRNO_NEG1:
    case EXT_PROTO_LASTERR_NEG1:
    case EXT_PROTO_WSA_NEG1:
        if (!pay_void && !pay_sint)
            diag_error(d->loc, "extern '%s': the -1 sentinel needs a signed "
                "integer or void payload, got %s", src_name, type_name(pay));
        break;
    case EXT_PROTO_ERRNO_NULL:
    case EXT_PROTO_LASTERR_NULL:
        if (!pay_ptr)
            diag_error(d->loc, "extern '%s': the null sentinel needs a pointer "
                "payload, got %s", src_name, type_name(pay));
        break;
    case EXT_PROTO_STATUS:
        if (!pay_void)
            diag_error(d->loc, "extern '%s': the status protocol carries no "
                "payload (the return value IS the error code); declare void!, "
                "got %s", src_name, type_name(pay));
        break;
    case EXT_PROTO_NEG_ERRNO:
        if (!pay_void && !pay_sint)
            diag_error(d->loc, "extern '%s': neg_errno reads the code from a "
                "negative return; the payload must be a signed integer or void, "
                "got %s", src_name, type_name(pay));
        break;
    case EXT_PROTO_HRESULT:
        if (!pay_void && !(pay && pay->kind == TYPE_INT32))
            diag_error(d->loc, "extern '%s': hresult payload must be i32 (the "
                "success-mode HRESULT) or void, got %s", src_name, type_name(pay));
        break;
    case EXT_PROTO_LASTERR_0:
        if (!pay_void && !pay_int)
            diag_error(d->loc, "extern '%s': the 0 sentinel needs an integer "
                "or void payload, got %s", src_name, type_name(pay));
        break;
    default: break;
    }
}

static void register_module_members(Decl *d, const char *mangle_prefix,
                                    const char *display_prefix,
                                    SymbolTable *members, InternTable *intern,
                                    SymbolTable *global_symtab,
                                    const char *ns_prefix) {
    const char *mod_name = d->module.name;
    /* Validate: from-modules may only contain extern declarations,
     * and non-from modules may not contain extern declarations. */
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        if (d->module.from_lib && child->kind != DECL_EXTERN &&
            !(child->kind == DECL_STRUCT && child->struc.is_extern)) {
            diag_error(child->loc,
                "module '%s' has a 'from' clause — only extern declarations are allowed",
                mod_name);
        }
        if (!d->module.from_lib && child->kind == DECL_EXTERN) {
            diag_error(child->loc,
                "extern declaration in module '%s' requires a 'from' clause on the module",
                mod_name);
        }
        if (!d->module.from_lib && child->kind == DECL_STRUCT && child->struc.is_extern) {
            diag_error(child->loc,
                "extern %s in module '%s' requires a 'from' clause on the module",
                child->struc.is_c_union ? "union" : "struct", mod_name);
        }
    }
    /* Validate: imports must come before all other declarations in a module */
    {
        bool seen_non_import = false;
        for (int j = 0; j < d->module.decl_count; j++) {
            Decl *child = d->module.decls[j];
            if (child->kind == DECL_IMPORT) {
                if (seen_non_import) {
                    diag_error(child->loc,
                        "imports must appear at the top of module '%s', before other declarations",
                        mod_name);
                }
            } else {
                seen_non_import = true;
            }
        }
    }
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        switch (child->kind) {
        case DECL_LET: {
            const char *src_name = child->let.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->let.codegen_name = mangled;
            child->let.is_module_member = true;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc,
                    d->module.is_error_group
                        ? "redefinition of '%s' in error group '%s'"
                        : "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_LET, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                detect_generic_func(intern->arena, child, msym);
            }
            break;
        }
        case DECL_STRUCT: {
            const char *src_name = child->struc.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->struc.name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_STRUCT, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                Type *st = arena_alloc(intern->arena, sizeof(Type));
                st->kind = TYPE_STRUCT;
                st->struc.name = mangled;
                st->struc.qualified_name = make_qualified(intern, display_prefix, src_name);
                st->struc.c_name = child->struc.c_name;
                st->struc.is_c_union = child->struc.is_c_union;
                st->struc.fields = child->struc.fields;
                st->struc.field_count = child->struc.field_count;
                st->struc.type_args = NULL;
                st->struc.type_arg_count = 0;
                msym->type = st;
                detect_generic_struct(intern->arena, child, msym);
                /* Also register under mangled name so canonicalized type stubs
                 * (which use the mangled name) resolve in module_symtab directly,
                 * avoiding namespace-filter rejection in global_lookup_kind. */
                symtab_add(members, mangled, DECL_STRUCT, child);
                Symbol *msym2 = &members->symbols[members->count - 1];
                msym2->is_private = child->is_private;
                msym2->type = st;
                detect_generic_struct(intern->arena, child, msym2);
            }
            break;
        }
        case DECL_UNION: {
            const char *src_name = child->unio.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->unio.name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_UNION, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                Type *ut = arena_alloc(intern->arena, sizeof(Type));
                ut->kind = TYPE_UNION;
                ut->unio.name = mangled;
                ut->unio.qualified_name = make_qualified(intern, display_prefix, src_name);
                ut->unio.variants = child->unio.variants;
                ut->unio.variant_count = child->unio.variant_count;
                ut->unio.type_args = NULL;
                ut->unio.type_arg_count = 0;
                msym->type = ut;
                detect_generic_union(intern->arena, child, msym);
                /* Also register under mangled name (see struct case above) */
                symtab_add(members, mangled, DECL_UNION, child);
                Symbol *msym2 = &members->symbols[members->count - 1];
                msym2->is_private = child->is_private;
                msym2->type = ut;
                detect_generic_union(intern->arena, child, msym2);
            }
            break;
        }
        case DECL_ENUM: {
            resolve_enum_values(child);
            const char *src_name = child->enu.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->enu.name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_ENUM, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                Type *et = arena_alloc(intern->arena, sizeof(Type));
                et->kind = TYPE_ENUM;
                et->enu.name = mangled;
                et->enu.qualified_name = make_qualified(intern, display_prefix, src_name);
                et->enu.repr = child->enu.repr;
                et->enu.variants = child->enu.variants;
                et->enu.variant_count = child->enu.variant_count;
                msym->type = et;
                /* Also register under mangled name (see struct case above) */
                symtab_add(members, mangled, DECL_ENUM, child);
                Symbol *msym2 = &members->symbols[members->count - 1];
                msym2->is_private = child->is_private;
                msym2->type = et;
            }
            break;
        }
        case DECL_EXTERN: {
            const char *src_name = child->ext.alias ? child->ext.alias : child->ext.name;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_EXTERN, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->type = child->ext.type;
                msym->is_private = child->is_private;
            }
            /* Validate types for extern constants (non-function externs).
             * Function-type externs are extern function declarations.
             * Constants may only have scalar or pointer types — types that
             * map directly to C #define constant values. */
            Type *et = child->ext.type;
            if (et && et->kind != TYPE_FUNC) {
                const char *reason = NULL;
                switch (et->kind) {
                case TYPE_SLICE:
                case TYPE_FIXED_ARRAY:
                    reason = "slice"; break;
                case TYPE_OPTION:
                    reason = "option"; break;
                case TYPE_RESULT:
                    reason = "result"; break;
                case TYPE_STRUCT:
                    reason = "struct"; break;
                case TYPE_UNION:
                    reason = "union"; break;
                case TYPE_STUB:
                    reason = "struct"; break;  /* stubs may resolve to struct or union */
                case TYPE_VOID:
                    reason = "void"; break;
                default: break;
                }
                if (reason) {
                    diag_error(child->loc,
                        "extern constant '%s' cannot have %s type '%s'",
                        child->ext.name, reason, type_name(et));
                }
                if (child->ext.protocol != EXT_PROTO_NONE &&
                    child->ext.protocol != EXT_PROTO_ERROR) {
                    diag_error(child->loc, "extern constant '%s' cannot declare "
                        "an error protocol — protocols apply to extern functions "
                        "returning a result type (T!)", src_name);
                }
            }
            if (et && et->kind == TYPE_FUNC)
                validate_extern_protocol(child, src_name);
            break;
        }
        case DECL_MODULE: {
            /* Nested submodule: register as a module member */
            const char *sub_name = child->module.name;
            const char *sub_prefix = make_mangled(intern, mangle_prefix, sub_name);

            Symbol *existing = symtab_lookup(members, sub_name);
            if (existing && (existing->kind == DECL_STRUCT || existing->kind == DECL_UNION ||
                             existing->kind == DECL_ENUM)) {
                /* Type-associated module: struct/union with same name already registered.
                 * Add a second entry with kind=DECL_MODULE under the same name.
                 * Use symtab_lookup_kind to distinguish. */
                symtab_add(members, sub_name, DECL_MODULE, child);
                Symbol *sub_sym = &members->symbols[members->count - 1];
                sub_sym->is_private = child->is_private;
                sub_sym->ns_prefix = ns_prefix;
                SymbolTable *sub_members = malloc(sizeof(SymbolTable));
                symtab_init(sub_members);
                sub_sym->members = sub_members;
                register_module_members(child, sub_prefix, make_qualified(intern, display_prefix, sub_name), sub_members, intern, global_symtab, ns_prefix);
                break;
            }
            if (existing) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    sub_name, mod_name);
                break;
            }
            symtab_add(members, sub_name, DECL_MODULE, child);
            Symbol *sub_sym = &members->symbols[members->count - 1];
            sub_sym->is_private = child->is_private;
            sub_sym->ns_prefix = ns_prefix;
            SymbolTable *sub_members = malloc(sizeof(SymbolTable));
            symtab_init(sub_members);
            sub_sym->members = sub_members;
            register_module_members(child, sub_prefix, make_qualified(intern, display_prefix, sub_name), sub_members, intern, global_symtab, ns_prefix);
            break;
        }
        default:
            break;
        }
    }

    /* Resolve type stubs in all symbols against sibling types within the module.
     * A single comprehensive pass that walks every type tree in every symbol,
     * covering extern function signatures, struct field types, and any other
     * position where a type stub may appear. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if (!msym->type) continue;

        /* Resolve the symbol's top-level type (e.g. extern function signatures
         * whose param/return types reference sibling extern structs/unions) */
        Type *resolved = resolve_type_stubs(intern->arena, msym->type, members);
        if (resolved != msym->type) {
            msym->type = resolved;
            if (msym->kind == DECL_EXTERN && msym->decl)
                msym->decl->ext.type = resolved;
        }

        /* Resolve stubs within struct field types */
        if (msym->type->kind == TYPE_STRUCT) {
            Type *st = msym->type;
            for (int k = 0; k < st->struc.field_count; k++) {
                Type *fr = resolve_type_stubs(intern->arena, st->struc.fields[k].type, members);
                if (fr != st->struc.fields[k].type)
                    st->struc.fields[k].type = fr;
            }
        }

    }

    /* Canonicalize generic stub names in struct/union field types.
     * Updates parser names (e.g., "inner") to canonical mangled names (e.g., "m__inner")
     * so downstream code doesn't need module context to resolve them. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if (!msym->type) continue;
        if (msym->type->kind == TYPE_STRUCT) {
            for (int k = 0; k < msym->type->struc.field_count; k++)
                canonicalize_stub_names(msym->type->struc.fields[k].type, members);
        }
        if (msym->type->kind == TYPE_UNION) {
            for (int k = 0; k < msym->type->unio.variant_count; k++)
                canonicalize_stub_names(msym->type->unio.variants[k].payload, members);
        }
    }

    /* Set resolved_sym on each type now that the symtab is stable (no more reallocs).
     * This bridges pass1 symbol info to mono without requiring symtab re-lookup. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if (msym->kind == DECL_STRUCT && msym->type && msym->type->kind == TYPE_STRUCT)
            msym->type->struc.resolved_sym = msym;
        if (msym->kind == DECL_UNION && msym->type && msym->type->kind == TYPE_UNION)
            msym->type->unio.resolved_sym = msym;
        if (msym->kind == DECL_ENUM && msym->type && msym->type->kind == TYPE_ENUM)
            msym->type->enu.resolved_sym = msym;
    }

    /* Register module-scoped struct/union types in the global symbol table under
     * their mangled names, so resolve_type can find them from any context. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if ((msym->kind == DECL_STRUCT || msym->kind == DECL_UNION ||
             msym->kind == DECL_ENUM) && msym->type) {
            const char *mangled_name = (msym->kind == DECL_STRUCT)
                ? msym->type->struc.name
                : (msym->kind == DECL_UNION) ? msym->type->unio.name
                                             : msym->type->enu.name;
            symtab_add(global_symtab, mangled_name, msym->kind, msym->decl);
            Symbol *gsym = &global_symtab->symbols[global_symtab->count - 1];
            gsym->type = msym->type;
            gsym->is_generic = msym->is_generic;
            gsym->type_params = msym->type_params;
            gsym->type_param_count = msym->type_param_count;
            gsym->explicit_type_param_count = msym->explicit_type_param_count;
            gsym->param_kinds = msym->param_kinds;
        }
    }
}

/* Process imports for a single module symbol */
static void process_module_level_imports(Symbol *ms, SymbolTable *global_symtab,
                                          InternTable *intern) {
    Decl *mod_decl = ms->decl;
    if (!mod_decl) return;

    bool has_imports = false;
    for (int j = 0; j < mod_decl->module.decl_count; j++) {
        if (mod_decl->module.decls[j]->kind == DECL_IMPORT) {
            has_imports = true;
            break;
        }
    }
    if (!has_imports) return;

    ImportTable *imports = malloc(sizeof(ImportTable));
    memset(imports, 0, sizeof(ImportTable));
    ms->imports = imports;

    const char *mod_ns = ms->ns_prefix;
    for (int j = 0; j < mod_decl->module.decl_count; j++) {
        Decl *d = mod_decl->module.decls[j];
        if (d->kind != DECL_IMPORT) continue;

        const char *imp_mod = d->import.from_module;
        const char *imp_ns = d->import.from_namespace;

        if (imp_mod) {
            /* import X from MODULE or import * from MODULE */
            process_member_import(d, imports, global_symtab, intern, mod_ns);
        } else if (imp_ns && !imp_mod) {
            /* import MODULE from NS:: */
            if (d->import.is_wildcard) {
                diag_error(d->loc, "cannot wildcard-import from a namespace");
                continue;
            }
            if (strcmp(imp_ns, "global") == 0) {
                diag_error(d->loc, "cannot import from 'global::'; global-namespace modules are already visible by name");
                continue;
            }
            const char *name = d->import.name;
            Symbol *src = symtab_lookup_module(global_symtab, name, imp_ns);
            if (!src || src->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s' in namespace '%s'", name, imp_ns);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : name;
            if (d->import.alias && is_builtin_type_name(import_name)) {
                diag_error(d->loc,
                    "import alias '%s' is a built-in type name and cannot name a module",
                    import_name);
            }
            import_table_add_module(imports, import_name, src, global_symtab);
        } else {
            /* import MODULE [as ALIAS] — bare whole-module imports are not supported.
             * Same-namespace modules are already visible by name; cross-namespace
             * modules require 'import MODULE from ns::'. */
            diag_error(d->loc,
                "bare 'import %s' is not supported; use 'import %s from ns::' for cross-namespace or access members directly",
                d->import.name, d->import.name);
        }
    }
}

/* Point each top-level struct/union Type at its address-stable defining Symbol.
 * register_struct_sym / register_union_sym (and their namespace-scoped variants)
 * cannot set Type.resolved_sym safely at registration time: every later
 * symtab_add DA_APPENDs to the by-value `symbols` array and may realloc it,
 * leaving an earlier &symbols[i] dangling — a heap-use-after-free once mono reads
 * resolved_sym (ASan: discover_nested_types). Module members already defer this
 * to a post-population loop in register_module_members; top-level types get the
 * same treatment here, called at end of pass1 once all symtab mutations are done.
 * The non-NULL guard leaves module-scoped types' resolved_sym (already pointed at
 * their member Symbol) intact when their global-symtab alias is visited. */
static void set_type_resolved_syms(SymbolTable *tab) {
    for (int i = 0; i < tab->count; i++) {
        Symbol *s = &tab->symbols[i];
        if (s->kind == DECL_STRUCT && s->type && s->type->kind == TYPE_STRUCT
            && !s->type->struc.resolved_sym)
            s->type->struc.resolved_sym = s;
        else if (s->kind == DECL_UNION && s->type && s->type->kind == TYPE_UNION
                 && !s->type->unio.resolved_sym)
            s->type->unio.resolved_sym = s;
        else if (s->kind == DECL_ENUM && s->type && s->type->kind == TYPE_ENUM
                 && !s->type->enu.resolved_sym)
            s->type->enu.resolved_sym = s;
    }
}

/* ---- Error-code assignment (`error` groups) --------------------------------
 * The compiler owns the error-code space: every declared error constant gets a
 * deterministic code — fully-qualified names are sorted (strcmp) and numbered
 * sequentially from FC_ERROR_CODE_BASE. Runs at the end of pass1_collect, once
 * the whole program has been merged, by patching each synthesized member let's
 * EXPR_INT_LIT placeholder in place. The registry below backs error_name /
 * --backtraces name tables in codegen and the --emit-error-codes listing. */

typedef struct ErrEntry {
    const char *qualified;
    Decl *let_decl;
} ErrEntry;

static ErrEntry *g_err_entries = NULL;
static int g_err_count = 0, g_err_cap = 0;

int error_code_count(void) { return g_err_count; }

ErrorCodeInfo error_code_info(int idx) {
    ErrorCodeInfo info = { g_err_entries[idx].qualified, g_err_entries[idx].let_decl->loc };
    return info;
}

static int err_entry_cmp(const void *a, const void *b) {
    return strcmp(((const ErrEntry *)a)->qualified, ((const ErrEntry *)b)->qualified);
}

/* Collect the error-group members under module decl d, whose own display path
 * (including d's name, [ns::]mod.sub…) is display_prefix. */
static void collect_error_members(Decl *d, const char *display_prefix,
                                  InternTable *intern) {
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        if (d->module.is_error_group) {
            if (child->kind != DECL_LET) continue;
            ErrEntry e = { make_qualified(intern, display_prefix, child->let.name), child };
            DA_APPEND(g_err_entries, g_err_count, g_err_cap, e);
        } else if (child->kind == DECL_MODULE) {
            collect_error_members(child,
                make_qualified(intern, display_prefix, child->module.name), intern);
        }
    }
}

static void assign_error_codes(Program *prog, InternTable *intern) {
    g_err_count = 0;  /* rebuilt per collection — the LSP re-runs pass1 per edit */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind != DECL_MODULE) continue;
        const char *display;
        if (d->module.ns_prefix) {  /* stamped in Phase 1 */
            int needed = snprintf(NULL, 0, "%s::%s", d->module.ns_prefix, d->module.name) + 1;
            char *buf = malloc((size_t)needed);
            snprintf(buf, (size_t)needed, "%s::%s", d->module.ns_prefix, d->module.name);
            display = intern_cstr(intern, buf);
            free(buf);
        } else {
            display = d->module.name;
        }
        collect_error_members(d, display, intern);
    }
    if (g_err_count == 0) return;
    qsort(g_err_entries, (size_t)g_err_count, sizeof(ErrEntry), err_entry_cmp);
    if ((int64_t)g_err_count > (int64_t)INT32_MAX - FC_ERROR_CODE_BASE) {
        diag_error(g_err_entries[0].let_decl->loc,
            "too many declared errors (%d) — the error-code space is exhausted",
            g_err_count);
        return;
    }
    for (int i = 0; i < g_err_count; i++)
        g_err_entries[i].let_decl->let.init->int_lit.value =
            (uint64_t)(FC_ERROR_CODE_BASE + i);
}

/* Walk modules and set parent pointers on every member symbol — not just nested
 * modules but also member lets and types — so an on-demand type check can recover
 * a member's enclosing module Symbol (and through it, that module's scope and
 * file-level imports) from the member alone. Called at end of pass1 after all
 * symtab mutations are complete, so Symbol pointers are stable. */
static void set_module_parents(SymbolTable *tab, Symbol *parent) {
    for (int i = 0; i < tab->count; i++) {
        Symbol *s = &tab->symbols[i];
        s->parent = parent;
        if (s->kind == DECL_MODULE && s->members)
            set_module_parents(s->members, s);
    }
}

/* Process imports for all top-level modules in global symtab */
static void resolve_module_imports(SymbolTable *symtab, InternTable *intern) {
    for (int i = 0; i < symtab->count; i++) {
        Symbol *ms = &symtab->symbols[i];
        if (ms->kind != DECL_MODULE) continue;
        process_module_level_imports(ms, symtab, intern);
    }
}

/* Recursively process imports for nested submodules.
 * Each nested module now carries its enclosing namespace's ns_prefix on the
 * Symbol (set during register_module_members), so process_module_level_imports
 * reads the correct namespace context directly from ms->ns_prefix. */
static void resolve_nested_module_imports(SymbolTable *members,
                                           SymbolTable *global_symtab,
                                           InternTable *intern) {
    for (int i = 0; i < members->count; i++) {
        Symbol *ms = &members->symbols[i];
        if (ms->kind != DECL_MODULE || !ms->members) continue;
        process_module_level_imports(ms, global_symtab, intern);
        resolve_nested_module_imports(ms->members, global_symtab, intern);
    }
}

/* ---- Const-generic parameter kind inference (fixpoint) ----
 *
 * A parameter's kind (type vs const) is inferred from its occurrences. Direct
 * syntactic positions are decided at collection time (type slot vs array-size
 * slot); a var appearing in another generic's type-arg slot ("wide<'n>")
 * inherits that parameter's kind from the referenced symbol. Because symbols
 * reference each other in any order (forward refs, mutual refs, cross-module),
 * this runs as a whole-symtab fixpoint after collection. Unconstrained params
 * finalize to GP_TYPE (backward compatible); function params keep GP_UNKNOWN
 * for pass2's lazy body inference (a prefix var may be used only in the body). */

typedef struct {
    SymbolTable *global;
    Symbol *owner;
    bool changed;
} KindInferCtx;

static Symbol *kind_ref_sym(SymbolTable *global, const char *name) {
    Symbol *s = symtab_lookup_kind(global, name, DECL_STRUCT);
    if (!s) s = symtab_lookup_kind(global, name, DECL_UNION);
    return s;
}

static void kind_merge(KindInferCtx *kc, const char *var, uint8_t k) {
    if (k == GP_UNKNOWN) return;
    Symbol *o = kc->owner;
    if (!o->param_kinds) return;
    for (int i = 0; i < o->type_param_count; i++) {
        if (o->type_params[i] != var) continue;
        uint8_t cur = o->param_kinds[i];
        if (cur == GP_CONFLICT || cur == k) return;
        if (cur == GP_UNKNOWN) {
            o->param_kinds[i] = k;
            kc->changed = true;
        } else {
            o->param_kinds[i] = GP_CONFLICT;
            kc->changed = true;
            diag_kind_conflict(o->decl ? o->decl->loc : (SrcLoc){0}, var, o->name);
        }
        return;
    }
}

static void kind_walk_expr(KindInferCtx *kc, Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_TYPE_VAR_REF: kind_merge(kc, e->type_var_ref.name, GP_CONST); return;
    case EXPR_UNARY_PREFIX: kind_walk_expr(kc, e->unary_prefix.operand); return;
    case EXPR_BINARY:
        kind_walk_expr(kc, e->binary.left);
        kind_walk_expr(kc, e->binary.right);
        return;
    case EXPR_CAST: kind_walk_expr(kc, e->cast.operand); return;
    default: return;
    }
}

/* The kind a bare var would take in each type-arg slot of a reference to
 * symbol `r` (NULL when unknown): the referenced parameter's own kind. */
static uint8_t kind_arg_slot(Symbol *r, int i) {
    if (!r || !r->is_generic || i >= r->type_param_count) return GP_UNKNOWN;
    if (!r->param_kinds) return GP_TYPE;   /* generic with no const params */
    uint8_t k = r->param_kinds[i];
    return (k == GP_CONFLICT) ? GP_UNKNOWN : k;
}

static void kind_walk(KindInferCtx *kc, Type *t, uint8_t pos_kind) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_TYPE_VAR: kind_merge(kc, t->type_var.name, pos_kind); return;
    case TYPE_POINTER: kind_walk(kc, t->pointer.pointee, GP_TYPE); return;
    case TYPE_SLICE:   kind_walk(kc, t->slice.elem, GP_TYPE); return;
    case TYPE_OPTION:  kind_walk(kc, t->option.inner, GP_TYPE); return;
    case TYPE_RESULT:  kind_walk(kc, t->result.inner, GP_TYPE); return;
    case TYPE_FIXED_ARRAY:
        kind_walk(kc, t->fixed_array.elem, GP_TYPE);
        kind_walk(kc, t->fixed_array.size_ref, GP_CONST);
        return;
    case TYPE_CONST_EXPR: kind_walk_expr(kc, t->const_expr.expr); return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            kind_walk(kc, t->func.param_types[i], GP_TYPE);
        kind_walk(kc, t->func.return_type, GP_TYPE);
        return;
    case TYPE_STUB: {
        Symbol *r = kind_ref_sym(kc->global, t->stub.name);
        for (int i = 0; i < t->stub.type_arg_count; i++)
            kind_walk(kc, t->stub.type_args[i], kind_arg_slot(r, i));
        return;
    }
    case TYPE_STRUCT: {
        Symbol *r = t->struc.resolved_sym;
        if (!r) r = kind_ref_sym(kc->global, t->struc.name);
        for (int i = 0; i < t->struc.field_count; i++)
            kind_walk(kc, t->struc.fields[i].type, GP_TYPE);
        for (int i = 0; i < t->struc.type_arg_count; i++)
            kind_walk(kc, t->struc.type_args[i], kind_arg_slot(r, i));
        return;
    }
    case TYPE_UNION: {
        Symbol *r = t->unio.resolved_sym;
        if (!r) r = kind_ref_sym(kc->global, t->unio.name);
        for (int i = 0; i < t->unio.variant_count; i++)
            kind_walk(kc, t->unio.variants[i].payload, GP_TYPE);
        for (int i = 0; i < t->unio.type_arg_count; i++)
            kind_walk(kc, t->unio.type_args[i], kind_arg_slot(r, i));
        return;
    }
    default: return;
    }
}

static void kind_walk_sym(KindInferCtx *kc, Symbol *s) {
    if (!s->is_generic || !s->decl || !s->param_kinds) return;
    kc->owner = s;
    Decl *d = s->decl;
    if (d->kind == DECL_STRUCT) {
        for (int i = 0; i < d->struc.field_count; i++)
            kind_walk(kc, d->struc.fields[i].type, GP_TYPE);
    } else if (d->kind == DECL_UNION) {
        for (int i = 0; i < d->unio.variant_count; i++)
            kind_walk(kc, d->unio.variants[i].payload, GP_TYPE);
    } else if (d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC) {
        Expr *fn = d->let.init;
        for (int i = 0; i < fn->func.param_count; i++)
            kind_walk(kc, fn->func.params[i].type, GP_TYPE);
    }
}

static void kind_walk_table(KindInferCtx *kc, SymbolTable *t) {
    for (int i = 0; i < t->count; i++) {
        kind_walk_sym(kc, &t->symbols[i]);
        if (t->symbols[i].members)
            kind_walk_table(kc, t->symbols[i].members);
    }
}

static void kind_finalize_table(SymbolTable *t) {
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->is_generic && s->param_kinds) {
            bool is_func = s->kind == DECL_LET;
            for (int j = 0; j < s->type_param_count; j++) {
                if (s->param_kinds[j] == GP_CONFLICT)
                    s->param_kinds[j] = GP_TYPE;   /* error already reported */
                else if (s->param_kinds[j] == GP_UNKNOWN && !is_func)
                    s->param_kinds[j] = GP_TYPE;   /* unconstrained — backward compatible */
                /* function GP_UNKNOWN entries stay: pass2's body check pins them */
            }
        }
        if (s->members) kind_finalize_table(s->members);
    }
}

static void infer_param_kinds(SymbolTable *global) {
    KindInferCtx kc = { global, NULL, false };
    int rounds = 0;
    do {
        kc.changed = false;
        kind_walk_table(&kc, global);
    } while (kc.changed && ++rounds < 64);   /* bound: kinds only ever tighten */
    kind_finalize_table(global);
}

/* ---- C name collision backstop ----
 *
 * Rooting every declaration path at `fc__` (see mangle_root) separates the
 * user namespace from the compiler's, but it does not by itself make the path
 * scheme injective: FC has two hierarchies — namespaces and module nesting —
 * and both flatten onto the same `__` separator, so `namespace a:: module b`
 * and `module a = module b` spell one prefix, `fc__a__b`. Their members would
 * then be emitted as one C object, which is the silent-wrong-code failure this
 * whole family produces.
 *
 * Making the join unambiguous costs every generated name its readability
 * (length-prefixed components, as the monomorphizer's type mangling uses), so
 * the claim is checked instead: the first declaration to claim a C name keeps
 * it, and a second claimant is an error naming both sites. Complete by
 * construction — it judges the names actually emitted, not the shapes that
 * were anticipated. */
typedef struct { const char *cname; SrcLoc loc; bool is_extern; } CNameClaim;

/* Open-addressed set of claimed names, keyed by the claim itself. A linear scan
 * per declaration made this quadratic in the emitted-name count — invisible on a
 * small program, 26ms on ten thousand declarations, and paid again on every
 * keystroke the server analyzes a clean file. Sized and probed like the intern
 * table (power-of-two capacity, linear probing, grow at half load); a slot is
 * empty iff its `cname` is NULL, and nothing is ever removed. */
typedef struct {
    CNameClaim *slots;
    int count, capacity;
} CNameClaims;

/* The last path component of a mangled name — the declaration's source
 * spelling. Split non-overlapping from the left, the way make_mangled built it:
 * a component may start with `_`, so scanning for the *last* `__` would take
 * `fc__a___x` (component `_x`) apart one character off. */
static const char *mangled_tail(const char *cname) {
    const char *tail = cname;
    for (const char *p = cname; p[0] && p[1]; ) {
        if (p[0] == '_' && p[1] == '_') { tail = p + 2; p += 2; }
        else p++;
    }
    return tail;
}

/* Claims are interned, so the key is the pointer, not the bytes. Allocator
 * alignment leaves the low bits constant, which linear probing would turn into
 * one long run, so the value is passed through a mixing finalizer first. */
static uint32_t cname_hash(const char *cname) {
    uint64_t x = (uint64_t)(uintptr_t)cname;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static void claims_grow(CNameClaims *cl) {
    int newcap = cl->capacity ? cl->capacity * 2 : 64;
    CNameClaim *slots = calloc((size_t)newcap, sizeof(CNameClaim));
    if (!slots) {
        fprintf(stderr, "fcc: out of memory\n");
        exit(1);
    }
    for (int i = 0; i < cl->capacity; i++) {
        if (!cl->slots[i].cname) continue;
        uint32_t idx = cname_hash(cl->slots[i].cname) & (uint32_t)(newcap - 1);
        while (slots[idx].cname) idx = (idx + 1) & (uint32_t)(newcap - 1);
        slots[idx] = cl->slots[i];
    }
    free(cl->slots);
    cl->slots = slots;
    cl->capacity = newcap;
}

static void claim_c_name(CNameClaims *cl, const char *cname, SrcLoc loc,
                         bool is_extern) {
    if (!cname) return;
    /* Grow before probing: rehashing moves every slot, so a position found
     * first would be stale. Half load keeps probe runs short. */
    if ((cl->count + 1) * 2 >= cl->capacity) claims_grow(cl);

    uint32_t idx = cname_hash(cname) & (uint32_t)(cl->capacity - 1);
    for (;;) {
        CNameClaim *e = &cl->slots[idx];
        if (!e->cname) {                        /* first claimant keeps the name */
            e->cname = cname;
            e->loc = loc;
            e->is_extern = is_extern;
            cl->count++;
            return;
        }
        /* Mangled names and token spellings share one intern table, so pointer
         * equality is the comparison for both. */
        if (e->cname == cname) {
            /* Two externs naming one C symbol is the feature, not a collision —
             * that is how a header symbol gets a second FC alias. */
            if (is_extern && e->is_extern) return;
            diag_error(loc,
                "'%s' would be emitted as the C name '%s', which the declaration at "
                "%s:%d already claims — rename one, or move it to a module path that "
                "does not flatten onto the other's",
                mangled_tail(cname), cname,
                e->loc.filename ? e->loc.filename : "?",
                e->loc.line);
            return;
        }
        idx = (idx + 1) & (uint32_t)(cl->capacity - 1);
    }
}

static void collect_c_names(CNameClaims *cl, Decl **decls, int count) {
    for (int i = 0; i < count; i++) {
        Decl *d = decls[i];
        switch (d->kind) {
        case DECL_LET:
            /* The entry point is emitted as fc_main, outside the user
             * namespace, and has no codegen_name; a module member spelled
             * `main` is an ordinary function and does have one. A let with no
             * init never reaches C. */
            claim_c_name(cl, d->let.init ? d->let.codegen_name : NULL, d->loc, false);
            break;
        case DECL_STRUCT:
            /* An extern type is emitted under the C tag its header declares,
             * verbatim. The parser already bans a tag under the reserved
             * `fc__` root (extern_c_name_in_reserved_root), so claiming it
             * here is defense in depth. */
            claim_c_name(cl, d->struc.is_extern ? d->struc.c_name : d->struc.name,
                         d->loc, d->struc.is_extern);
            break;
        case DECL_UNION:
            claim_c_name(cl, d->unio.name, d->loc, false);
            break;
        case DECL_ENUM:
            claim_c_name(cl, d->enu.name, d->loc, false);
            break;
        case DECL_EXTERN:
            /* An extern names a C symbol verbatim, and `__` is legal there (it
             * is exactly where the implementation-reserved namespace lives).
             * The parser's `fc__` root ban is the primary defense — before it,
             * `extern fc__m__counter as c` and module member `m.counter`
             * silently referred to one object — so this claim, like the extern
             * type's, is defense in depth. */
            claim_c_name(cl, d->ext.name, d->loc, true);
            break;
        case DECL_MODULE:
            collect_c_names(cl, d->module.decls, d->module.decl_count);
            break;
        default:
            break;
        }
    }
}

static void check_c_name_collisions(Program *prog) {
    /* Only judge an otherwise-clean program. This is a backstop against
     * *silent* wrong output, and a program that already has errors never
     * reaches codegen — while a redefinition (the same file listed twice, a
     * name declared twice in one scope) necessarily also collides, so running
     * anyway would only add a second diagnosis of a reported mistake. */
    if (diag_error_count() > 0) return;
    CNameClaims cl = { NULL, 0, 0 };
    collect_c_names(&cl, prog->decls, prog->decl_count);
    free(cl.slots);
}

void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern,
                   FileImportScopes *file_scopes, bool require_main) {
    /* Phase 0: Validate that file-level imports come before other declarations.
     * Namespace declarations are exempt (they must come first). */
    {
        const char *cur_file = NULL;
        bool seen_non_import = false;
        for (int i = 0; i < prog->decl_count; i++) {
            Decl *d = prog->decls[i];
            if (d->loc.filename != cur_file) {
                cur_file = d->loc.filename;
                seen_non_import = false;
            }
            if (d->kind == DECL_NAMESPACE) continue;
            if (d->kind == DECL_IMPORT) {
                if (seen_non_import) {
                    diag_error(d->loc,
                        "imports must appear at the top of the file, before other declarations");
                }
            } else {
                seen_non_import = true;
            }
        }
    }

    /* Phase 0.5: Reject non-uniform recursive type definitions (audit item 14).
     * Runs before any name mangling so self-references still carry source names. */
    check_recursion_in_decls(prog->decls, prog->decl_count);

    /* Phase 0.6: Reject user types/modules named after a built-in type (§7.8).
     * Also before mangling — the check reads source-spelling declaration names. */
    check_builtin_type_name_decls(prog->decls, prog->decl_count);

    /* Phase 1: Register modules.
     * Track current namespace as we iterate — DECL_NAMESPACE resets it. */
    const char *current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];

        if (d->kind == DECL_NAMESPACE) {
            if (d->ns.name && strcmp(d->ns.name, "global") == 0) {
                diag_error(d->loc, "'global::' is the implicit default namespace and cannot be declared explicitly");
            }
            current_ns = d->ns.name;
            continue;
        }

        if (d->kind != DECL_MODULE) continue;

        const char *mod_name = d->module.name;
        const char *ns_prefix = d->module.ns_prefix ? d->module.ns_prefix : current_ns;
        d->module.ns_prefix = ns_prefix;

        /* Build mangling prefix: fc__[namespace__]module (see mangle_root) */
        const char *mangle_prefix =
            make_mangled(intern, mangle_root(intern, ns_prefix), mod_name);

        /* Build display prefix for qualified names: [namespace::]module */
        const char *display_prefix;
        if (ns_prefix) {
            int needed = snprintf(NULL, 0, "%s::%s", ns_prefix, mod_name) + 1;
            char *buf = malloc((size_t)needed);
            snprintf(buf, (size_t)needed, "%s::%s", ns_prefix, mod_name);
            display_prefix = intern_cstr(intern, buf);
            free(buf);
        } else {
            display_prefix = mod_name;
        }

        /* Check for duplicate module name within the same namespace */
        Symbol *existing = symtab_lookup_module(symtab, mod_name, ns_prefix);
        if (existing) {
            /* Type-associated module: allow struct/union/enum + module with same name */
            if (existing->kind == DECL_STRUCT || existing->kind == DECL_UNION ||
                existing->kind == DECL_ENUM) {
                /* Register the module alongside the type.
                 * The type is already in the symtab. We need a separate symbol
                 * for the module. Use a special mangled key for the symtab,
                 * but the user accesses it by the original name (context-dependent). */
                /* Store as a secondary entry with kind DECL_MODULE */
                symtab_add(symtab, mod_name, DECL_MODULE, d);
                Symbol *mod_sym = symtab_lookup(symtab, mod_name);
                /* symtab_lookup returns the first match (the struct/union).
                 * We need the new one — find it at the end. */
                mod_sym = &symtab->symbols[symtab->count - 1];
                mod_sym->is_private = d->is_private;
                mod_sym->ns_prefix = ns_prefix;
                SymbolTable *members = malloc(sizeof(SymbolTable));
                symtab_init(members);
                mod_sym->members = members;
                register_module_members(d, mangle_prefix, display_prefix, members, intern, symtab, ns_prefix);
                continue;
            }
            if (existing->kind == DECL_MODULE) {
                diag_error(d->loc,
                    d->module.is_error_group
                        ? "redefinition of error group '%s'"
                        : "redefinition of module '%s'",
                    mod_name);
                continue;
            }
        }

        /* Register module in global symtab */
        symtab_add(symtab, mod_name, DECL_MODULE, d);
        Symbol *mod_sym = &symtab->symbols[symtab->count - 1];
        mod_sym->is_private = d->is_private;
        mod_sym->ns_prefix = ns_prefix;

        /* Create sub-symbol table for module members */
        SymbolTable *members = malloc(sizeof(SymbolTable));
        symtab_init(members);
        mod_sym->members = members;

        /* Walk child decls, compute mangled names, register in members */
        register_module_members(d, mangle_prefix, display_prefix, members, intern, symtab, ns_prefix);
    }

    /* Phase 2: Register top-level (non-module) decls.
     *
     * Restrictions:
     * - Top-level 'let' bindings are entry-point-file-only (the file with
     *   'let main'), and thus implicitly global::-only since 'let main'
     *   must live in global::.
     * - Top-level struct/union are permitted in any file under any namespace
     *   (following the same rules as top-level modules). They are mangled
     *   with their namespace prefix to avoid C identifier collisions across
     *   namespaces. */

    /* Pre-scan: find the entry-point file (the one containing let main). */
    const char *entry_file = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_LET && strcmp(d->let.name, "main") == 0) {
            entry_file = d->loc.filename;
            break;
        }
    }
    if (!entry_file && require_main) {
        diag_error((SrcLoc){0}, "no entry point: program must contain 'let main'");
    }
    /* The entry-point-file restriction on top-level 'let' only has meaning when
     * an entry-point file exists. In library mode (require_main == false, no
     * main), there is no entry file to anchor the rule, so top-level 'let' is
     * permitted anywhere — skip the check rather than flag every binding. */
    current_ns = NULL;
    for (int i = 0; entry_file && i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) { current_ns = d->ns.name; continue; }
        if (current_ns) continue; /* non-global namespace — checked below */
        /* Only top-level 'let' is restricted to the entry-point file. */
        if (d->kind == DECL_LET) {
            if (d->loc.filename != entry_file) {
                diag_error(d->loc,
                    "top-level let '%s' not allowed here — "
                    "top-level 'let' bindings are only allowed in the "
                    "entry-point file (the file containing 'let main')",
                    d->let.name);
            }
        }
    }

    current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            current_ns = d->ns.name;
            continue;
        }
        /* Non-global namespaces may not contain top-level 'let' bindings. */
        if (current_ns && d->kind == DECL_LET) {
            diag_error(d->loc,
                "top-level let not allowed in namespace '%s::' — "
                "wrap it in a module or move it to a global:: file",
                current_ns);
            continue;
        }
        switch (d->kind) {
        case DECL_LET: {
            if (symtab_lookup(symtab, d->let.name)) {
                diag_error(d->loc, "redefinition of '%s'", d->let.name);
            } else {
                symtab_add(symtab, d->let.name, DECL_LET, d);
            }
            /* Mangle non-main top-level names — both functions and file-level
             * globals — into the reserved `fc__` namespace, so neither collides
             * with a C keyword or libc symbol once emitted at C file scope
             * (e.g. a global named `log`). File-level globals remain
             * distinguished from module members by is_module_member (false
             * here), not by the presence of codegen_name. */
            if (strcmp(d->let.name, "main") != 0 && !d->let.codegen_name &&
                d->let.init) {
                d->let.codegen_name = make_mangled(intern, "fc", d->let.name);
            }
            /* Detect generic functions */
            Symbol *let_sym = symtab_lookup(symtab, d->let.name);
            if (let_sym) detect_generic_func(intern->arena, d, let_sym);
            break;
        }
        case DECL_STRUCT: {
            if (d->struc.is_extern) {
                diag_error(d->loc, "extern %s must be inside a module with a 'from' clause",
                    d->struc.is_c_union ? "union" : "struct");
                break;
            }
            if (current_ns) {
                /* Namespace-scoped top-level struct: check for duplicates
                 * within the same namespace only; register with ns_prefix set
                 * and mangle the C name with the namespace prefix. */
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                    DECL_STRUCT, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                        DECL_UNION, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                        DECL_ENUM, current_ns);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->struc.name);
                    break;
                }
                const char *src_name = d->struc.name;
                const char *mangled = make_mangled(intern, mangle_root(intern, current_ns), src_name);
                d->struc.name = mangled;
                symtab_add(symtab, src_name, DECL_STRUCT, d);
                Symbol *sym = &symtab->symbols[symtab->count - 1];
                sym->ns_prefix = current_ns;
                Type *st = arena_alloc(intern->arena, sizeof(Type));
                st->kind = TYPE_STRUCT;
                st->struc.name = mangled;
                {
                    int needed = snprintf(NULL, 0, "%s::%s", current_ns, src_name) + 1;
                    char *buf = malloc((size_t)needed);
                    snprintf(buf, (size_t)needed, "%s::%s", current_ns, src_name);
                    st->struc.qualified_name = intern_cstr(intern, buf);
                    free(buf);
                }
                st->struc.c_name = d->struc.c_name;
                st->struc.is_c_union = d->struc.is_c_union;
                st->struc.fields = d->struc.fields;
                st->struc.field_count = d->struc.field_count;
                st->struc.type_args = NULL;
                st->struc.type_arg_count = 0;
                sym->type = st;
                /* resolved_sym deferred to set_type_resolved_syms (stable addr). */
                detect_generic_struct(intern->arena, d, sym);
                /* Also register under the mangled name so canonicalized type
                 * stubs resolve in the global symtab directly. The mangled
                 * alias carries ns_prefix=NULL (matching module-scoped types'
                 * global-symtab entries): the mangled name is self-identifying
                 * and should be findable from any namespace. */
                symtab_add(symtab, mangled, DECL_STRUCT, d);
                Symbol *sym2 = &symtab->symbols[symtab->count - 1];
                sym2->type = st;
                detect_generic_struct(intern->arena, d, sym2);
            } else {
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                    DECL_STRUCT, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                        DECL_UNION, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->struc.name,
                        DECL_ENUM, NULL);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->struc.name);
                } else {
                    register_struct_sym(symtab, intern, d);
                }
            }
            break;
        }
        case DECL_UNION: {
            if (current_ns) {
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                    DECL_UNION, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                        DECL_STRUCT, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                        DECL_ENUM, current_ns);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->unio.name);
                    break;
                }
                const char *src_name = d->unio.name;
                const char *mangled = make_mangled(intern, mangle_root(intern, current_ns), src_name);
                d->unio.name = mangled;
                symtab_add(symtab, src_name, DECL_UNION, d);
                Symbol *sym = &symtab->symbols[symtab->count - 1];
                sym->ns_prefix = current_ns;
                Type *ut = arena_alloc(intern->arena, sizeof(Type));
                ut->kind = TYPE_UNION;
                ut->unio.name = mangled;
                {
                    int needed = snprintf(NULL, 0, "%s::%s", current_ns, src_name) + 1;
                    char *buf = malloc((size_t)needed);
                    snprintf(buf, (size_t)needed, "%s::%s", current_ns, src_name);
                    ut->unio.qualified_name = intern_cstr(intern, buf);
                    free(buf);
                }
                ut->unio.variants = d->unio.variants;
                ut->unio.variant_count = d->unio.variant_count;
                ut->unio.type_args = NULL;
                ut->unio.type_arg_count = 0;
                sym->type = ut;
                /* resolved_sym deferred to set_type_resolved_syms (stable addr). */
                detect_generic_union(intern->arena, d, sym);
                /* Also register under the mangled name (see struct case). */
                symtab_add(symtab, mangled, DECL_UNION, d);
                Symbol *sym2 = &symtab->symbols[symtab->count - 1];
                sym2->type = ut;
                detect_generic_union(intern->arena, d, sym2);
            } else {
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                    DECL_UNION, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                        DECL_STRUCT, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->unio.name,
                        DECL_ENUM, NULL);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->unio.name);
                } else {
                    register_union_sym(symtab, intern, d);
                }
            }
            break;
        }
        case DECL_ENUM: {
            resolve_enum_values(d);
            if (current_ns) {
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                    DECL_ENUM, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                        DECL_STRUCT, current_ns);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                        DECL_UNION, current_ns);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->enu.name);
                    break;
                }
                const char *src_name = d->enu.name;
                const char *mangled = make_mangled(intern, mangle_root(intern, current_ns), src_name);
                d->enu.name = mangled;
                symtab_add(symtab, src_name, DECL_ENUM, d);
                Symbol *sym = &symtab->symbols[symtab->count - 1];
                sym->ns_prefix = current_ns;
                Type *et = arena_alloc(intern->arena, sizeof(Type));
                et->kind = TYPE_ENUM;
                et->enu.name = mangled;
                {
                    int needed = snprintf(NULL, 0, "%s::%s", current_ns, src_name) + 1;
                    char *buf = malloc((size_t)needed);
                    snprintf(buf, (size_t)needed, "%s::%s", current_ns, src_name);
                    et->enu.qualified_name = intern_cstr(intern, buf);
                    free(buf);
                }
                et->enu.repr = d->enu.repr;
                et->enu.variants = d->enu.variants;
                et->enu.variant_count = d->enu.variant_count;
                sym->type = et;
                /* resolved_sym deferred to set_type_resolved_syms (stable addr). */
                /* Also register under the mangled name (see struct case). */
                symtab_add(symtab, mangled, DECL_ENUM, d);
                Symbol *sym2 = &symtab->symbols[symtab->count - 1];
                sym2->type = et;
            } else {
                Symbol *existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                    DECL_ENUM, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                        DECL_STRUCT, NULL);
                if (!existing)
                    existing = symtab_lookup_kind_ns(symtab, d->enu.name,
                        DECL_UNION, NULL);
                if (existing) {
                    diag_error(d->loc, "redefinition of '%s'", d->enu.name);
                } else {
                    register_enum_sym(symtab, intern, d);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    /* Phase 3: Process imports.
     * Imports are lexically scoped: module-level imports are scoped to the
     * module (visible to children), file-level member imports go into per-file
     * tables. Whole-module imports (import MODULE [as ALIAS]) still go to the
     * global symtab for dotted-name resolution. */

    /* Phase 3a: Module-level imports — process recursively (including nested submodules) */
    resolve_module_imports(symtab, intern);
    /* Also process nested submodules within each top-level module */
    for (int i = 0; i < symtab->count; i++) {
        Symbol *ms = &symtab->symbols[i];
        if (ms->kind != DECL_MODULE || !ms->members) continue;
        resolve_nested_module_imports(ms->members, symtab, intern);
    }

    /* Phase 3b: File-level imports */
    current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];

        if (d->kind == DECL_NAMESPACE) {
            current_ns = d->ns.name;
            continue;
        }

        if (d->kind != DECL_IMPORT) continue;

        const char *mod_name = d->import.from_module;
        const char *from_ns = d->import.from_namespace;

        /* All imports go to the file's ImportTable */
        ImportTable *file_tbl = get_file_imports(file_scopes, d->loc.filename);

        if (!mod_name && !from_ns) {
            /* import MODULE [as ALIAS] — bare whole-module imports are not supported.
             * Same-namespace modules are already visible by name; cross-namespace
             * modules require 'import MODULE from ns::'. */
            diag_error(d->loc,
                "bare 'import %s' is not supported; use 'import %s from ns::' for cross-namespace or access members directly",
                d->import.name, d->import.name);
            continue;
        }

        if (from_ns && !mod_name) {
            if (d->import.is_wildcard) {
                diag_error(d->loc, "cannot wildcard-import from a namespace; import * works on modules only");
                continue;
            }
            if (strcmp(from_ns, "global") == 0) {
                diag_error(d->loc, "cannot import from 'global::'; global-namespace modules are already visible by name");
                continue;
            }
            /* import NAME from namespace:: — cross-namespace import of a
             * top-level symbol (module, struct, or union). Companion pairs
             * (same-named struct/union + module) import both entries. */
            const char *name = d->import.name;
            const char *import_name = d->import.alias ? d->import.alias : name;
            Symbol *mod = symtab_lookup_module(symtab, name, from_ns);
            Symbol *type_sym = symtab_lookup_kind_ns(symtab, name, DECL_STRUCT, from_ns);
            if (!type_sym)
                type_sym = symtab_lookup_kind_ns(symtab, name, DECL_UNION, from_ns);
            if (!type_sym)
                type_sym = symtab_lookup_kind_ns(symtab, name, DECL_ENUM, from_ns);
            if (!mod && !type_sym) {
                diag_error(d->loc, "unknown symbol '%s' in namespace '%s'", name, from_ns);
                continue;
            }
            /* §7.8: a cross-namespace whole-symbol import brings in a module
             * and/or a type — both type/module kinds, so an `as` alias onto a
             * built-in type name is unreachable. */
            if (d->import.alias && is_builtin_type_name(import_name)) {
                diag_error(d->loc,
                    "import alias '%s' is a built-in type name and cannot name a type or module",
                    import_name);
            }
            if (mod) import_table_add_module(file_tbl, import_name, mod, symtab);
            if (type_sym) {
                import_table_add(file_tbl, import_name, name, type_sym->kind,
                                 symtab, type_sym);
            }
            continue;
        }

        /* Member import (import X from M / import * from M) */
        process_member_import(d, file_tbl, symtab, intern, current_ns);
    }

    /* Phase 4: Detect circular references between modules.
     * Scan module member expressions for EXPR_FIELD on other modules,
     * build a dependency graph, and detect cycles with DFS. */
    int mod_count = 0;
    for (int i = 0; i < symtab->count; i++)
        if (symtab->symbols[i].kind == DECL_MODULE) mod_count++;

    if (mod_count >= 2) {
        /* Collect module decl pointers */
        Decl **mods = malloc(sizeof(Decl*) * (size_t)mod_count);
        int mi = 0;
        for (int i = 0; i < symtab->count; i++)
            if (symtab->symbols[i].kind == DECL_MODULE)
                mods[mi++] = symtab->symbols[i].decl;

        /* Build adjacency: deps[i*mod_count + j] = true if mod i references mod j */
        bool *deps = calloc((size_t)(mod_count * mod_count), sizeof(bool));

        for (int i = 0; i < mod_count; i++) {
            /* Scan all expressions in module i for IDENT references to other modules */
            for (int d = 0; d < mods[i]->module.decl_count; d++) {
                Decl *child = mods[i]->module.decls[d];
                if (child->kind != DECL_LET || !child->let.init) continue;
                /* Walk expression tree looking for EXPR_IDENT matching other module names */
                Expr **stack = NULL;
                int sp = 0, stack_cap = 0;
                DA_APPEND(stack, sp, stack_cap, child->let.init);
                while (sp > 0) {
                    Expr *ex = stack[--sp];
                    if (!ex) continue;
                    if (ex->kind == EXPR_IDENT) {
                        /* Check if this ident refers to another module */
                        for (int j = 0; j < mod_count; j++) {
                            if (j == i) continue;
                            if (ex->ident.name == mods[j]->module.name) {
                                deps[i * mod_count + j] = true;
                            }
                        }
                    }
                    /* Push children to stack */
                    #define PUSH(e) DA_APPEND(stack, sp, stack_cap, (e))
                    switch (ex->kind) {
                    case EXPR_BINARY: PUSH(ex->binary.left); PUSH(ex->binary.right); break;
                    case EXPR_UNARY_PREFIX: PUSH(ex->unary_prefix.operand); break;
                    case EXPR_UNARY_POSTFIX: PUSH(ex->unary_postfix.operand); break;
                    case EXPR_CALL:
                        PUSH(ex->call.func);
                        for (int a = 0; a < ex->call.arg_count; a++) PUSH(ex->call.args[a]);
                        break;
                    case EXPR_FIELD: case EXPR_DEREF_FIELD: PUSH(ex->field.object); break;
                    case EXPR_INDEX: PUSH(ex->index.object); PUSH(ex->index.index); break;
                    case EXPR_IF:
                        PUSH(ex->if_expr.cond);
                        PUSH(ex->if_expr.then_body);
                        if (ex->if_expr.else_body) { PUSH(ex->if_expr.else_body); }
                        break;
                    case EXPR_BLOCK:
                        for (int s = 0; s < ex->block.count; s++) { PUSH(ex->block.stmts[s]); }
                        break;
                    case EXPR_FUNC:
                        for (int s = 0; s < ex->func.body_count; s++) { PUSH(ex->func.body[s]); }
                        break;
                    case EXPR_LET: PUSH(ex->let_expr.let_init); break;
                    case EXPR_ASSIGN: PUSH(ex->assign.target); PUSH(ex->assign.value); break;
                    case EXPR_RETURN:
                        if (ex->return_expr.value) { PUSH(ex->return_expr.value); }
                        break;
                    case EXPR_BREAK:
                        if (ex->break_expr.value) { PUSH(ex->break_expr.value); }
                        break;
                    case EXPR_LOOP:
                        for (int s = 0; s < ex->loop_expr.body_count; s++) { PUSH(ex->loop_expr.body[s]); }
                        break;
                    case EXPR_FOR:
                        PUSH(ex->for_expr.iter);
                        for (int s = 0; s < ex->for_expr.body_count; s++) { PUSH(ex->for_expr.body[s]); }
                        break;
                    case EXPR_MATCH:
                        PUSH(ex->match_expr.subject);
                        for (int a = 0; a < ex->match_expr.arm_count; a++) {
                            for (int s = 0; s < ex->match_expr.arms[a].body_count; s++) {
                                PUSH(ex->match_expr.arms[a].body[s]);
                            }
                        }
                        break;
                    case EXPR_CAST: PUSH(ex->cast.operand); break;
                    case EXPR_BITCAST: PUSH(ex->bitcast_expr.operand); break;
                    case EXPR_ENUM_OF: PUSH(ex->enum_of_expr.operand); break;
                    case EXPR_SOME: PUSH(ex->some_expr.value); break;
                    case EXPR_OK: PUSH(ex->ok_expr.value); break;
                    case EXPR_ERR: PUSH(ex->err_expr.code); break;
                    case EXPR_ERROR_NAME: PUSH(ex->error_name_expr.code); break;
                    case EXPR_STRUCT_LIT:
                        for (int f = 0; f < ex->struct_lit.field_count; f++) { PUSH(ex->struct_lit.fields[f].value); }
                        break;
                    case EXPR_SLICE:
                        PUSH(ex->slice.object);
                        if (ex->slice.lo) { PUSH(ex->slice.lo); }
                        if (ex->slice.hi) { PUSH(ex->slice.hi); }
                        break;
                    case EXPR_ALLOC:
                        if (ex->alloc_expr.size_expr) { PUSH(ex->alloc_expr.size_expr); }
                        if (ex->alloc_expr.init_expr) { PUSH(ex->alloc_expr.init_expr); }
                        break;
                    case EXPR_FREE: PUSH(ex->free_expr.operand); break;
                    case EXPR_ATOMIC_LOAD: PUSH(ex->atomic_load.ptr); break;
                    case EXPR_ATOMIC_STORE:
                        PUSH(ex->atomic_store.ptr);
                        PUSH(ex->atomic_store.value);
                        break;
                    case EXPR_ASSERT:
                        PUSH(ex->assert_expr.condition);
                        if (ex->assert_expr.message) { PUSH(ex->assert_expr.message); }
                        break;
                    case EXPR_DEFER: PUSH(ex->defer_expr.value); break;
                    case EXPR_IGNORE: PUSH(ex->ignore_expr.value); break;
                    default: break;
                    }
                    #undef PUSH
                }
                free(stack);
            }
        }

        /* Also add edges from import statements: if module i imports from module j,
         * record a dependency.  This catches cycles through imports that the
         * expression-based scan above misses (import names are not module names). */
        for (int i = 0; i < mod_count; i++) {
            for (int d = 0; d < mods[i]->module.decl_count; d++) {
                Decl *child = mods[i]->module.decls[d];
                if (child->kind != DECL_IMPORT) continue;
                const char *from = child->import.from_module;
                if (!from) continue;
                for (int j = 0; j < mod_count; j++) {
                    if (j == i) continue;
                    if (from == mods[j]->module.name)
                        deps[i * mod_count + j] = true;
                }
            }
        }

        /* DFS cycle detection */
        int *color = calloc((size_t)mod_count, sizeof(int)); /* 0=white, 1=gray, 2=black */
        int *dfs_stack = malloc((size_t)mod_count * sizeof(int));
        bool found_cycle = false;
        for (int start = 0; start < mod_count && !found_cycle; start++) {
            if (color[start] != 0) continue;
            int dsp = 0;
            dfs_stack[dsp++] = start;
            color[start] = 1;
            while (dsp > 0 && !found_cycle) {
                int u = dfs_stack[dsp - 1];
                bool pushed = false;
                for (int v = 0; v < mod_count; v++) {
                    if (!deps[u * mod_count + v]) continue;
                    if (color[v] == 1) {
                        diag_error(mods[u]->loc,
                            "circular reference between modules '%s' and '%s'",
                            mods[u]->module.name, mods[v]->module.name);
                        found_cycle = true;
                        break;
                    }
                    if (color[v] == 0) {
                        color[v] = 1;
                        dfs_stack[dsp++] = v;
                        pushed = true;
                        break;
                    }
                }
                if (!pushed && !found_cycle) {
                    color[u] = 2;
                    dsp--;
                }
            }
        }

        free(dfs_stack);
        free(mods);
        free(deps);
        free(color);
    }

    /* Final phase: set parent pointers on all nested modules, and bind top-level
     * struct/union types to their (now address-stable) defining Symbols. Must run
     * after all symtab mutations are complete so Symbol pointers are stable. */
    set_type_resolved_syms(symtab);
    set_module_parents(symtab, NULL);

    /* Infer const-vs-type kinds for generic parameters (whole-symtab fixpoint;
     * see infer_param_kinds above). Runs after all symbols and their
     * type_params/param_kinds arrays exist. */
    infer_param_kinds(symtab);

    /* Assign deterministic codes to declared error constants (error groups).
     * Whole-program by construction: runs on the merged Program. */
    assign_error_codes(prog, intern);

    /* Last: every declaration name is final, so the emitted C names can be
     * checked pairwise-distinct (see check_c_name_collisions above). */
    check_c_name_collisions(prog);
}
