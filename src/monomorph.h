#pragma once
#include "ast.h"
#include "common.h"
#include "pass1.h"

/* How many steps of discovery may separate an instance from code that names
 * it directly (MonoInstance.depth): a chain of instantiations still making new
 * instances past this is rejected as one that never comes round (see
 * monomorph.c). The C++ standard's suggested minimum for nested template
 * instantiations, and clang's default limit. pass2 validates instances to the
 * same depth. */
#define MONO_MAX_CHAIN_DEPTH 1024

/* How many instances a program may have: a backstop for a family that grows
 * in breadth rather than depth (two new instances of itself in each), far
 * above what real programs reach. pass2 applies it to the instances it
 * checks. */
#define MONO_MAX_INSTANCES 200000

typedef struct MonoInstance {
    const char *generic_name;    /* original name (interned) */
    const char *mangled_name;    /* C output name (interned) */
    const char *ns_prefix;       /* namespace prefix, NULL for global */
    Type **type_args;            /* concrete type arguments */
    int type_arg_count;
    Decl *template_decl;         /* pointer to original generic declaration */
    DeclKind decl_kind;          /* DECL_LET, DECL_STRUCT, DECL_UNION */
    /* For structs/unions: the substituted concrete Type */
    Type *concrete_type;
    /* For functions: type var names (from template) for building substitution map */
    const char **type_param_names;
    int type_param_count;
    /* The instance whose body or fields first named this one, as one more
     * than its index (always an earlier entry), or 0 for an instance checked
     * code names (pass2). */
    int named_by;
    /* The steps along that chain to an instance checked code names (0 for
     * one itself): how far a recursion has run (MONO_MAX_CHAIN_DEPTH). */
    int depth;
} MonoInstance;

typedef struct MonoTable {
    MonoInstance *entries;
    int count;
    int capacity;
    /* While discovery walks an instance's body or fields, one more than its
     * index (so a zeroed table means none): the named_by of what it finds. */
    int walking;
    /* Open-addressed index from mangled name (interned, so compared by
     * pointer) to entry, for mono_find: index_cap slots (a power of two, 0
     * before the first registration), each an entry index or -1. A slot can
     * outlive its entry when an interactive session drops the instances of
     * an input that failed (count shrinks), so a lookup checks the entry it
     * finds still has the name. */
    int *index;
    int index_cap;
    int index_used;     /* slots ever filled since the last rebuild */
} MonoTable;

/* Register an instantiation. Returns the mangled name. Deduplicates by mangled name. */
const char *mono_register(MonoTable *t, Arena *a, InternTable *intern,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count);

/* Find an instance by mangled name */
MonoInstance *mono_find(MonoTable *t, const char *mangled_name);

/* Report a chain of instantiations still making new instances past
 * MONO_MAX_CHAIN_DEPTH, at `loc`, the declaration of the generic `name`: `at`
 * is its instance past the limit, and `from` and `to` its first two instances
 * on the chain (NULL when it appears only once). pass2 finds the same chains
 * through function bodies and reports them in the same words. */
void mono_report_deep_chain(SrcLoc loc, const char *name, const char *from,
                            const char *to, const char *at);

/* Report that instantiating the generic `name` (declared at `loc`) takes the
 * program past MONO_MAX_INSTANCES instances; shared with pass2 likewise. */
void mono_report_too_many(SrcLoc loc, const char *name);

/* Recursively resolve generic struct/union stubs with concrete type_args into
 * mangled names in a type tree. Uses resolved_sym on Type (set by pass1/pass2)
 * to get canonical names without symtab re-lookup. */
void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type);

/* Finalize monomorphized types, after discovery: resolve every type name to
 * its C name, and reject a truncated infinite instance family. The order
 * definitions are emitted in is codegen's (emit_types). */
void mono_finalize_types(MonoTable *t, Arena *a, InternTable *intern);

/* The same for the instances from index `first` on, which are new since a
 * finalized table held `first` of them: an interactive session finalizes each
 * statement's instances as it compiles the statement. */
void mono_finalize_types_from(MonoTable *t, int first, Arena *a, InternTable *intern);

/* Discover every instance the ones pass2 registered lead to: the instances
 * each function instance's body makes (generic calls inside a generic body
 * aren't resolved until it is instantiated) and those each struct or union
 * instance's fields and payloads name, breadth-first, so each instance's
 * depth is its distance from the nearest one pass2 registered. Also gives
 * every instance a private concrete type. Call after pass2. */
void mono_discover_transitive(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab);

/* The same, walking only the instances from index `first` on (and those they
 * discover): the instances before it were walked already. */
void mono_discover_transitive_from(MonoTable *t, int first, Arena *a, InternTable *intern,
                                   SymbolTable *symtab);
