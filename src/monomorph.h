#pragma once
#include "ast.h"
#include "common.h"

typedef struct {
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
} MonoInstance;

typedef struct {
    MonoInstance *entries;
    int count;
    int capacity;
} MonoTable;

/* Register an instantiation. Returns the mangled name. Deduplicates by mangled name. */
const char *mono_register(MonoTable *t, Arena *a, InternTable *intern,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count);

/* Find an instance by mangled name */
MonoInstance *mono_find(MonoTable *t, const char *mangled_name);
