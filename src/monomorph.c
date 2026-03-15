#include "monomorph.h"
#include "types.h"
#include <stdio.h>
#include <string.h>

const char *mono_register(MonoTable *t, Arena *a, InternTable *intern_tbl,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count) {
    /* Build the base name for mangling */
    const char *base = name;
    if (ns_prefix) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s_%s", ns_prefix, name);
        base = intern_cstr(intern_tbl, buf);
    }
    const char *mangled = mangle_generic_name(a, intern_tbl, base, type_args, count);

    /* Dedup: check if already registered */
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].mangled_name == mangled)
            return mangled;
    }

    /* Copy type_args into arena */
    Type **args_copy = arena_alloc(a, sizeof(Type*) * (size_t)count);
    memcpy(args_copy, type_args, sizeof(Type*) * (size_t)count);

    /* Copy type_params into arena */
    const char **params_copy = NULL;
    if (tp_count > 0) {
        params_copy = arena_alloc(a, sizeof(const char*) * (size_t)tp_count);
        memcpy(params_copy, type_params, sizeof(const char*) * (size_t)tp_count);
    }

    MonoInstance inst = {
        .generic_name = name,
        .mangled_name = mangled,
        .ns_prefix = ns_prefix,
        .type_args = args_copy,
        .type_arg_count = count,
        .template_decl = tmpl,
        .decl_kind = kind,
        .concrete_type = NULL,
        .type_param_names = params_copy,
        .type_param_count = tp_count,
    };
    DA_APPEND(t->entries, t->count, t->capacity, inst);
    return mangled;
}

MonoInstance *mono_find(MonoTable *t, const char *mangled_name) {
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].mangled_name == mangled_name)
            return &t->entries[i];
    }
    return NULL;
}
