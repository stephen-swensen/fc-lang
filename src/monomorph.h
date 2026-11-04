#pragma once
#include "ast.h"
#include "common.h"
#include "pass1.h"

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
     * code names (pass2). The chain of these is how mono_register finds a
     * generic instantiating itself. */
    int named_by;
} MonoInstance;

typedef struct MonoTable {
    MonoInstance *entries;
    int count;
    int capacity;
    /* While discovery walks an instance's body or fields, one more than its
     * index (so a zeroed table means none): the named_by of what it finds. */
    int walking;
} MonoTable;

/* Register an instantiation. Returns the mangled name. Deduplicates by mangled name. */
const char *mono_register(MonoTable *t, Arena *a, InternTable *intern,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count);

/* Find an instance by mangled name */
MonoInstance *mono_find(MonoTable *t, const char *mangled_name);

/* Recursively resolve generic struct/union stubs with concrete type_args into
 * mangled names in a type tree. Uses resolved_sym on Type (set by pass1/pass2)
 * to get canonical names without symtab re-lookup. */
void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type);

/* Finalize monomorphized types: give every instance a private concrete type,
 * discover the instances their fields and payloads name, resolve every type
 * name to its C name, and reject a truncated infinite instance family. The
 * order definitions are emitted in is codegen's (emit_types). */
void mono_finalize_types(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab);

/* The same for the instances from index `first` on, which are new since a
 * finalized table held `first` of them: an interactive session finalizes each
 * statement's instances as it compiles the statement. */
void mono_finalize_types_from(MonoTable *t, int first, Arena *a, InternTable *intern,
                              SymbolTable *symtab);

/* Discover all transitive monomorphized instances by walking template function
 * bodies. Generic calls inside generic functions aren't resolved until the outer
 * function is instantiated; this fixpoint loop finds them all. Call after pass2. */
void mono_discover_transitive(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab);

/* The same, walking only the instances from index `first` on (and those they
 * discover): the instances before it were walked already. */
void mono_discover_transitive_from(MonoTable *t, int first, Arena *a, InternTable *intern,
                                   SymbolTable *symtab);
