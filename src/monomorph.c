#include "monomorph.h"
#include "types.h"
#include "diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monomorphization termination guards.
 *
 * A generic may instantiate itself with any arguments, and the instances it
 * makes are the ones those arguments reach. Some families never close: one
 * that grows a type argument (`f(some(x))` forces f<'a> -> f<'a?> ->
 * f<'a??> -> ..., and a type that wraps its own parameter in a reference to
 * itself, `node<node<'a>>`, forces node<i32> -> node<node<i32>> -> ...), and
 * one whose const argument never comes round again (f<'n + 1>). Without a
 * guard, discovery (mono_discover_transitive) would never end, or, if a name
 * collision happened to stop it, would emit a truncated C type.
 *
 * Every instance records how many steps of discovery separate it from the
 * nearest instance code names directly (MonoInstance.depth), and one past
 * MONO_MAX_CHAIN_DEPTH is rejected (reject_deep_chain): a chain that has not
 * come round to instances already made by then is taken never to. A growing
 * type argument also nests one constructor deeper each step, so the cap on
 * the structural depth of an instance's type arguments reports it sooner,
 * as what it is. The instance-count cap (MONO_MAX_INSTANCES) stops a family
 * that grows in breadth instead: each instance of `f` naming both
 * f<'n * 2> and f<'n * 2 + 1> doubles the instances at every step, so the
 * count runs out long before the depth does. All three limits are far above
 * what real programs reach (generic nesting is a handful of levels deep, a
 * recursion that halves its argument is at most 64, and programs have
 * thousands of instances, not hundreds of thousands). */
#define MONO_MAX_INSTANTIATION_DEPTH 128

static void mono_drain_const_eval_error(void);

/* The name a message gives the template `tmpl` of an instance registered as
 * `name`: its source name, which pass1 replaced with the C name in a struct
 * or union decl (but keeps in a let). */
static const char *template_display_name(Decl *tmpl, const char *name) {
    if (!tmpl) return name;
    if (tmpl->kind == DECL_LET && tmpl->let.name) return tmpl->let.name;
    if (tmpl->kind == DECL_STRUCT) return mangled_source_name(tmpl->struc.name);
    if (tmpl->kind == DECL_UNION) return mangled_source_name(tmpl->unio.name);
    return name;
}

/* An instance one step past MONO_MAX_CHAIN_DEPTH: `tmpl` at `type_args`,
 * named while discovery walks the instance `t->walking` names. Reports it with
 * the first two instances of `tmpl` on its chain, the first step of the
 * recursion. Returns true when it reported. */
static bool reject_deep_chain(MonoTable *t, Decl *tmpl, const char *name,
                              Type **type_args, int count, int depth) {
    if (depth <= MONO_MAX_CHAIN_DEPTH) return false;
    /* The chain from the instance code names, outermost first. */
    int *chain = xmalloc(sizeof(int) * (size_t)(depth));
    int n = 0;
    for (int k = t->walking - 1; k >= 0 && n < depth; k = t->entries[k].named_by - 1)
        chain[n++] = k;
    int first = -1, second = -1;
    for (int j = n - 1; j >= 0 && second < 0; j--) {
        if (t->entries[chain[j]].template_decl != tmpl) continue;
        if (first < 0) first = chain[j];
        else second = chain[j];
    }
    free(chain);
    const char *disp = template_display_name(tmpl, name);
    char *at = type_inst_display(disp, type_args, count);
    char *from = first >= 0
        ? type_inst_display(disp, t->entries[first].type_args, t->entries[first].type_arg_count)
        : NULL;
    char *to = second >= 0
        ? type_inst_display(disp, t->entries[second].type_args, t->entries[second].type_arg_count)
        : NULL;
    mono_report_deep_chain(tmpl ? tmpl->loc : (SrcLoc){0}, disp, from, to, at);
    free(at);
    free(from);
    free(to);
    return true;
}

void mono_report_deep_chain(SrcLoc loc, const char *name, const char *from,
                            const char *to, const char *at) {
    char *step = from && to ? str_sprintf("%s leads to %s, and ", from, to) : str_sprintf("%s", "");
    diag_error(loc,
        "'%s' instantiates itself more than %d levels deep: %sthe chain is still "
        "making new instances at %s. A generic may instantiate itself with changed "
        "arguments only if they come round again; an 'if' does not end the chain, "
        "since FC evaluates no branches at compile time",
        name, MONO_MAX_CHAIN_DEPTH, step, at);
    free(step);
}

void mono_report_too_many(SrcLoc loc, const char *name) {
    diag_error(loc,
        "instantiating '%s' takes the program past %d generic instances, the most "
        "it may have. A generic that names more than one new instance of itself in "
        "each instance (f<'n * 2> and f<'n * 2 + 1> inside f) doubles the instances "
        "at every step, so they never run out",
        name, MONO_MAX_INSTANCES);
}

/* The slot mono_find probes first for `name`. */
static unsigned mono_slot(const MonoTable *t, const char *name) {
    uint64_t h = (uint64_t)(uintptr_t)name * 0x9e3779b97f4a7c15u;
    return (unsigned)(h >> 32) & (unsigned)(t->index_cap - 1);
}

/* Put entry `i` in the index, after any slot left by a dropped entry, and
 * rebuild the index from the live entries when it is half full. */
static void mono_index_add(MonoTable *t, int i) {
    if ((t->index_used + 1) * 2 > t->index_cap) {
        int cap = t->index_cap ? t->index_cap : 64;
        while ((t->count + 1) * 2 > cap) cap *= 2;
        free(t->index);
        t->index = xmalloc(sizeof(int) * (size_t)cap);
        memset(t->index, -1, sizeof(int) * (size_t)cap);
        t->index_cap = cap;
        t->index_used = 0;
        for (int k = 0; k < t->count; k++) {
            if (k == i) continue;
            unsigned s = mono_slot(t, t->entries[k].mangled_name);
            while (t->index[s] >= 0) s = (s + 1) & (unsigned)(cap - 1);
            t->index[s] = k;
            t->index_used++;
        }
    }
    unsigned s = mono_slot(t, t->entries[i].mangled_name);
    while (t->index[s] >= 0 && t->index[s] < t->count && t->index[s] != i)
        s = (s + 1) & (unsigned)(t->index_cap - 1);
    if (t->index[s] < 0) t->index_used++;
    t->index[s] = i;
}

const char *mono_register(MonoTable *t, Arena *a, InternTable *intern_tbl,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count) {
    /* Once an error has been reported (such as an infinite instantiation), stop
     * growing the table so discovery ends (it walks to t->count) and the
     * diagnostic below is reported only once. */
    if (diag_error_count() > 0) return name;

    /* Build the base name for mangling */
    const char *base = name;
    if (ns_prefix) {
        /* Sized to fit: a clipped prefix would mangle two differently-named
         * generics onto one C symbol. */
        base = intern_sprintf(intern_tbl, "%s__%s", ns_prefix, name);
    }
    const char *mangled = mangle_generic_name(intern_tbl, base, type_args, count);

    /* Dedup: check if already registered */
    if (mono_find(t, mangled)) return mangled;

    /* void has no value representation, so an instance binding a type
     * parameter to it would emit `void x;`. pass2 rejects the written and
     * inferred spellings at their use sites (with a source location); this
     * catches any other path to instantiation, so no such instance reaches
     * codegen. */
    for (int i = 0; i < count; i++) {
        if (type_args[i] && type_args[i]->kind == TYPE_VOID) {
            diag_error(tmpl ? tmpl->loc : (SrcLoc){0},
                "cannot instantiate '%s' with void: void is not a value type",
                name);
            return mangled;
        }
    }

    int depth = t->walking ? t->entries[t->walking - 1].depth + 1 : 0;
    if (reject_deep_chain(t, tmpl, name, type_args, count, depth)) return mangled;

    /* Termination guard: a new instance whose type arguments nest deeper than any
     * finite program would means a generic is instantiating itself with a growing
     * type argument (infinite monomorphization). Report once and stop. */
    int arg_depth = 0;
    for (int i = 0; i < count; i++) {
        int d = type_arg_depth(type_args[i]);
        if (d > arg_depth) arg_depth = d;
    }
    if (arg_depth > MONO_MAX_INSTANTIATION_DEPTH) {
        /* Prefer the source-level name over the mangled C name for the message. */
        diag_error(tmpl ? tmpl->loc : (SrcLoc){0},
            "infinite generic instantiation of '%s': its type arguments nest more "
            "than %d deep. A generic that instantiates itself with a growing type "
            "argument (f(some(x)) inside f makes f<'a?>, then f<'a?" "?>, and so on) needs "
            "infinitely many instances",
            template_display_name(tmpl, name), MONO_MAX_INSTANTIATION_DEPTH);
        return mangled;
    }
    if (t->count >= MONO_MAX_INSTANCES) {
        mono_report_too_many(tmpl ? tmpl->loc : (SrcLoc){0}, template_display_name(tmpl, name));
        return mangled;
    }

    /* static_assert enforcement. Every new instance passes through here once,
     * however it was reached (explicit annotation, call site, transitive
     * discovery, nested field type). Conditions were shape-validated and
     * normalized in pass2, so the context-free evaluator suffices here. */
    {
        StaticAssert *sas = NULL;
        int san = 0;
        if (tmpl) {
            if (tmpl->kind == DECL_STRUCT) {
                sas = tmpl->struc.static_asserts; san = tmpl->struc.static_assert_count;
            } else if (tmpl->kind == DECL_UNION) {
                sas = tmpl->unio.static_asserts; san = tmpl->unio.static_assert_count;
            } else if (tmpl->kind == DECL_LET) {
                sas = tmpl->let.static_asserts; san = tmpl->let.static_assert_count;
            }
        }
        if (san > 0) {
            /* Instance descriptor for messages, e.g. "uwide<100>", built from
             * the owner name captured at parse time (decl names get mangled).
             * Spelled only on the two failure paths below. */
            const char *disp = sas[0].owner ? sas[0].owner : name;

            int nbind = tp_count < count ? tp_count : count;
            for (int i = 0; i < san; i++) {
                if (sas[i].judged) continue;   /* concrete: judged in pass2 */
                Type wrapper = {0};
                wrapper.kind = TYPE_CONST_EXPR;
                wrapper.const_expr.expr = sas[i].cond;
                int64_t v;
                bool evaluated = sas[i].typed
                    ? const_eval_typed(sas[i].cond, type_params, type_args, nbind, &v)
                    : const_type_eval(&wrapper, type_params, type_args, nbind, &v);
                if (evaluated) {
                    if (v == 0) {
                        char *inst = type_inst_display(disp, type_args, count);
                        diag_error(sas[i].loc,
                            "static assertion failed in instantiation of '%s': %s",
                            inst, sas[i].msg);
                        free(inst);
                        return mangled;   /* rejected: never registered */
                    }
                } else {
                    SrcLoc eloc = {0};
                    const char *emsg = const_eval_take_error(&eloc);
                    char *inst = type_inst_display(disp, type_args, count);
                    diag_error((emsg && eloc.filename) ? eloc : sas[i].loc,
                        "%s (in static_assert of '%s')",
                        emsg ? emsg : "could not evaluate static_assert condition",
                        inst);
                    free(inst);
                    return mangled;
                }
            }
        }
    }

    Type **args_copy = arena_dup(a, type_args, count, sizeof(Type*));
    const char **params_copy = arena_dup(a, type_params, tp_count, sizeof(const char*));

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
        .named_by = t->walking,
        .depth = depth,
    };
    DA_APPEND(t->entries, t->count, t->capacity, inst);
    mono_index_add(t, t->count - 1);
    return mangled;
}

MonoInstance *mono_find(MonoTable *t, const char *mangled_name) {
    if (t->index_cap == 0) return NULL;
    for (unsigned s = mono_slot(t, mangled_name); t->index[s] >= 0;
         s = (s + 1) & (unsigned)(t->index_cap - 1)) {
        int i = t->index[s];
        if (i < t->count && t->entries[i].mangled_name == mangled_name)
            return &t->entries[i];
    }
    return NULL;
}

void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: mono_resolve_type_names(t, a, intern, type->pointer.pointee); return;
    case TYPE_SLICE:   mono_resolve_type_names(t, a, intern, type->slice.elem); return;
    case TYPE_OPTION:  mono_resolve_type_names(t, a, intern, type->option.inner); return;
    case TYPE_RESULT:  mono_resolve_type_names(t, a, intern, type->result.inner); return;
    case TYPE_FIXED_ARRAY: mono_resolve_type_names(t, a, intern, type->fixed_array.elem); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            mono_resolve_type_names(t, a, intern, type->func.param_types[i]);
        mono_resolve_type_names(t, a, intern, type->func.return_type);
        return;
    case TYPE_STRUCT:
        /* Tuples carry their instantiation in fields (type_args is empty), so the
         * generic "base + type_args" rename doesn't apply; re-derive the canonical
         * name from the resolved element types instead. Recurse fields first so
         * nested tuples/structs are named before this one. */
        if (type->struc.is_tuple) {
            for (int i = 0; i < type->struc.field_count; i++)
                mono_resolve_type_names(t, a, intern, type->struc.fields[i].type);
            if (!type_contains_type_var(type)) {
                const char *cn = tuple_canonical_name(intern,
                    type->struc.fields, type->struc.field_count);
                type->struc.name = cn;
                type->struc.qualified_name = cn;
            }
            return;
        }
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            /* The instance name is a pure function of the type (mangle_type_name:
             * canonical base from resolved_sym plus recursion over the
             * arguments). The mono_find guard leaves a node that already carries
             * a registered instance name untouched. */
            if (!mono_find(t, type->struc.name)) {
                char *m = mangle_type_name(type);
                type->struc.name = intern_cstr(intern, m);
                free(m);
            }
        }
        for (int i = 0; i < type->struc.field_count; i++)
            mono_resolve_type_names(t, a, intern, type->struc.fields[i].type);
        return;
    case TYPE_UNION:
        if (type->unio.type_arg_count > 0 && !type_contains_type_var(type)) {
            /* Pure spelling; see the struct arm. */
            if (!mono_find(t, type->unio.name)) {
                char *m = mangle_type_name(type);
                type->unio.name = intern_cstr(intern, m);
                free(m);
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            mono_resolve_type_names(t, a, intern, type->unio.variants[i].payload);
        return;
    case TYPE_STUB:
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (!type->stub.base_name && !mono_find(t, type->stub.name)) {
                type->stub.base_name = type->stub.name;
                type->stub.name = mangle_generic_name(intern,
                    type->stub.name, type->stub.type_args, type->stub.type_arg_count);
            }
        }
        return;
    CASE_TYPE_PRIMITIVES:
    case TYPE_ENUM:
    case TYPE_ANY_PTR:
    case TYPE_TYPE_VAR:
    case TYPE_CONST_INT:
    case TYPE_CONST_EXPR:
    case TYPE_ERROR:
    case TYPE_NEVER:
    case TYPE_UNRESOLVED:
    case TYPE_COUNT:
        return;
    }
}

/* Substitute type vars using a binding map, returning concrete types */
static Type **substitute_type_args(Arena *a, Type **type_args, int type_arg_count,
                                    const char **var_names, Type **concrete, int var_count) {
    Type **result = xmalloc(sizeof(Type*) * (size_t)type_arg_count);
    for (int i = 0; i < type_arg_count; i++)
        result[i] = type_substitute(a, type_args[i], var_names, concrete, var_count);
    return result;
}

static void discover_nested_types(Type *type, MonoTable *t, Arena *a,
                                  InternTable *intern, SymbolTable *symtab);

/* The substitution a generic function body is walked under, and the tables its
 * instances are registered in. */
typedef struct {
    MonoTable *t;
    Arena *a;
    InternTable *intern;
    SymbolTable *symtab;
    const char **var_names;
    Type **concrete;
    int var_count;
} DiscoverCtx;

/* Substitute the type vars of an expression's type operand (e.g. the target of
 * sizeof/alignof/default/alloc, an array/slice-literal element type, or a variant
 * constructor's union result type) and register any generic struct/union instances
 * it transitively names. Such instances are otherwise never registered when they
 * appear only inside a generic body: pass2 sees the still-abstract template type,
 * and the expression walk below recurses into sub-expressions but not into the
 * types they carry. */
static void discover_in_type(Type *ty, DiscoverCtx *c) {
    if (!ty) return;
    Type *ct = type_substitute(c->a, ty, c->var_names, c->concrete, c->var_count);
    if (type_contains_type_var(ct)) return;
    /* discover_nested_types rewrites struct/union/stub names in place; isolate a
     * private deep copy so it can't corrupt the live AST type or a template. */
    ct = type_deep_copy(c->a, ct);
    discover_nested_types(ct, c->t, c->a, c->intern, c->symtab);
}

/* A call whose callee is generic and whose type arguments mention the
 * enclosing body's type variables: register the instance this substitution
 * makes of it. */
static void discover_call(Expr *e, DiscoverCtx *c) {
    if (e->call.mangled_name || e->call.type_arg_count == 0) return;
    Type **concrete_args = substitute_type_args(c->a, e->call.type_args,
        e->call.type_arg_count, c->var_names, c->concrete, c->var_count);
    bool all_concrete = true;
    for (int i = 0; i < e->call.type_arg_count; i++) {
        if (type_contains_type_var(concrete_args[i])) {
            all_concrete = false;
            break;
        }
    }
    if (all_concrete) {
        /* An argument type may itself name a generic instance: inside
         * `bx2<'a>`, the call `bx(bx(v))` binds the outer `bx` to
         * `box<'a>`, which substitutes to `box<i32>`. Registering the
         * callee does not register that instance, and nothing else
         * reaches it (pass2 only saw the abstract `box<'a>`), so the
         * emitted C would name a struct it never defined. Register it
         * here; the callee's mangled name spells the instance from its
         * structure (mangle_type_name), so no renaming is needed. */
        for (int i = 0; i < e->call.type_arg_count; i++)
            discover_nested_types(concrete_args[i], c->t, c->a, c->intern, c->symtab);
        Symbol *callee_sym = e->call.resolved_callee;
        if (callee_sym) {
            const char *base_name = (callee_sym->decl && callee_sym->decl->kind == DECL_LET
                                     && callee_sym->decl->let.codegen_name)
                                    ? callee_sym->decl->let.codegen_name : callee_sym->name;
            mono_register(c->t, c->a, c->intern, base_name, NULL,
                concrete_args, e->call.type_arg_count,
                callee_sym->decl, DECL_LET,
                callee_sym->type_params, callee_sym->type_param_count);
        }
    }
    free(concrete_args);
}

/* Report the first fixed array in `t` whose size is invalid, a member type of
 * the instance `inst` of `tmpl`; false after reporting. Descends through
 * wrappers and tuples but not into other named types: a generic one is
 * checked as it is made, and a plain one was checked at its declaration. */
static bool check_member_sizes(Type *t, Decl *tmpl, const char *inst) {
    if (!t) return true;
    switch (t->kind) {
    case TYPE_FIXED_ARRAY: {
        int64_t sz;
        char *msg = type_fixed_array_size(t, &sz)
            ? static_length_error("fixed array size", sz, true) : NULL;
        if (msg) {
            diag_error(tmpl ? tmpl->loc : (SrcLoc){0}, "%s (in '%s', in instance '%s')",
                       msg, type_name(t), inst);
            free(msg);
            return false;
        }
        return check_member_sizes(t->fixed_array.elem, tmpl, inst);
    }
    case TYPE_POINTER: return check_member_sizes(t->pointer.pointee, tmpl, inst);
    case TYPE_SLICE:   return check_member_sizes(t->slice.elem, tmpl, inst);
    case TYPE_OPTION:  return check_member_sizes(t->option.inner, tmpl, inst);
    case TYPE_RESULT:  return check_member_sizes(t->result.inner, tmpl, inst);
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            if (!check_member_sizes(t->func.param_types[i], tmpl, inst)) return false;
        return check_member_sizes(t->func.return_type, tmpl, inst);
    case TYPE_STRUCT:
        if (t->struc.is_tuple)
            for (int i = 0; i < t->struc.field_count; i++)
                if (!check_member_sizes(t->struc.fields[i].type, tmpl, inst)) return false;
        return true;
    CASE_TYPE_PRIMITIVES:
    case TYPE_UNION:
    case TYPE_STUB:
    case TYPE_ENUM:
    case TYPE_ANY_PTR:
    case TYPE_TYPE_VAR:
    case TYPE_CONST_INT:
    case TYPE_CONST_EXPR:
    case TYPE_ERROR:
    case TYPE_NEVER:
    case TYPE_UNRESOLVED:
    case TYPE_COUNT:
        break;
    }
    return true;
}

/* Completeness backstop for the sizes in an instance monomorphization makes:
 * every fixed array its fields or payloads hold must have a valid size.
 * pass2 checks the instances checked code names, and those their fields name
 * a few steps on (`d: cbox<'n - 3>` in s<3>); a family can run further
 * (`next: s<'n * 2>*` reaches s<64> from s<1>), and this checks every
 * instance however far its chain runs. */
static void check_instance_sizes(Type *ct, Decl *tmpl, const char *disp,
                                 Type **args, int n) {
    char *inst = type_inst_display(disp, args, n);
    if (ct->kind == TYPE_STRUCT) {
        for (int i = 0; i < ct->struc.field_count; i++)
            if (!check_member_sizes(ct->struc.fields[i].type, tmpl, inst)) break;
    } else if (ct->kind == TYPE_UNION) {
        for (int i = 0; i < ct->unio.variant_count; i++)
            if (!check_member_sizes(ct->unio.variants[i].payload, tmpl, inst)) break;
    }
    free(inst);
}

/* Register the instance of generic struct or union `sym` at `args` (its C name
 * built from `base`) and give a new entry its concrete type: the template with
 * the arguments substituted, named for the instance, with nested instance
 * names resolved. The substituted type is deep-copied first, because
 * type_substitute shares unchanged subtrees with the template and the renaming
 * is in place. */
static void mono_instantiate(MonoTable *t, Arena *a, InternTable *intern,
                             Symbol *sym, const char *base, Type **args, int n) {
    const char *mangled = mono_register(t, a, intern, base, NULL, args, n, sym->decl,
                                        sym->kind, sym->type_params, sym->type_param_count);
    MonoInstance *mi = mono_find(t, mangled);
    if (!mi || mi->concrete_type) return;
    int ntp = sym->type_param_count < n ? sym->type_param_count : n;
    Type *ct = type_deep_copy(a, type_substitute(a, sym->type, sym->type_params, args, ntp));
    if (ct->kind == TYPE_STRUCT) ct->struc.name = mangled;
    else if (ct->kind == TYPE_UNION) ct->unio.name = mangled;
    mono_resolve_type_names(t, a, intern, ct);
    mi->concrete_type = ct;
    check_instance_sizes(ct, sym->decl, template_display_name(sym->decl, base), args, n);
}

/* A tuple literal whose element types mention type variables: register the
 * concrete tuple this substitution produces. Concrete tuples were already
 * registered in pass2. */
static void discover_tuple_lit(Expr *e, DiscoverCtx *c) {
    if (!e->type || e->type->kind != TYPE_STRUCT || !e->type->struc.is_tuple ||
        !type_contains_type_var(e->type))
        return;
    Type *ct = type_substitute(c->a, e->type, c->var_names, c->concrete, c->var_count);
    if (type_contains_type_var(ct)) return;
    ct = type_deep_copy(c->a, ct);  /* isolate before in-place name canonicalization */
    mono_resolve_type_names(c->t, c->a, c->intern, ct);  /* sets ct->struc.name canonically */
    Type *noargs[1] = {0};
    const char *mangled = mono_register(c->t, c->a, c->intern, ct->struc.name, NULL,
        noargs, 0, NULL, DECL_STRUCT, NULL, 0);
    MonoInstance *mi = mono_find(c->t, mangled);
    if (mi && !mi->concrete_type)
        mi->concrete_type = ct;
}

/* Walk a generic function body under one substitution and register every
 * instance it makes: generic calls, generic struct and tuple literals, and
 * generic types named by type operands. Children are registered first. */
static void discover_in_expr(Expr *e, void *ctx) {
    DiscoverCtx *c = ctx;
    expr_for_each_child(e, discover_in_expr, ctx);
    switch (e->kind) {
    case EXPR_CALL:       discover_call(e, c); break;
    /* A generic struct literal makes the instance its own type names: the
     * type's arguments substituted, not the body's variables in order. */
    case EXPR_STRUCT_LIT:
        if (e->type && type_contains_type_var(e->type)) discover_in_type(e->type, c);
        break;
    case EXPR_TUPLE_LIT:  discover_tuple_lit(e, c); break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        /* Generic-union variant construction (maybe<'a>.just(x) / maybe<'a>.nothing):
         * the result type is the union instance, otherwise unregistered when the
         * construction appears only inside a generic body. */
        if (e->field.is_variant_constructor) discover_in_type(e->type, c);
        break;
    /* The element type may be a generic instance (box<'a>[] { ... }) used only
     * inside a generic body; register it even when no element constructs it. */
    case EXPR_ARRAY_LIT:  discover_in_type(e->array_lit.elem_type, c); break;
    case EXPR_SLICE_LIT:  discover_in_type(e->slice_lit.elem_type, c); break;
    case EXPR_CAST:       discover_in_type(e->cast.target, c); break;
    case EXPR_BITCAST:    discover_in_type(e->bitcast_expr.target, c); break;
    case EXPR_ERR:        discover_in_type(e->err_expr.target, c); break;
    case EXPR_ALLOC:      discover_in_type(e->alloc_expr.alloc_type, c); break;
    case EXPR_SIZEOF:     discover_in_type(e->sizeof_expr.target, c); break;
    case EXPR_ALIGNOF:    discover_in_type(e->alignof_expr.target, c); break;
    case EXPR_DEFAULT:    discover_in_type(e->default_expr.target, c); break;
    default: break;
    }
}

/* A generic struct or union reached as a type: when it is a concrete instance
 * not yet registered, register the instances its arguments name (reachable only
 * here: the field walk descends the definition, not the arguments), then its
 * own, under the name every reference gives it (instance_base_name). An
 * instance's fields or payloads are walked when discovery reaches it in turn,
 * one step further from the code that names it; any other aggregate's (a plain
 * struct, or a template's still holding type variables) are walked here. A
 * node straight from resolve_type (a sizeof, default or alloc operand) may
 * carry no resolved_sym; the name lookup that finds its template records it on
 * the node, so the name comes from the template's canonical name here too. */
static void discover_aggregate(Type *type, MonoTable *t, Arena *a,
                               InternTable *intern, SymbolTable *symtab) {
    bool is_struct = type->kind == TYPE_STRUCT;
    Type **args = is_struct ? type->struc.type_args : type->unio.type_args;
    int nargs = is_struct ? type->struc.type_arg_count : type->unio.type_arg_count;
    const char *name = is_struct ? type->struc.name : type->unio.name;
    MonoInstance *self = NULL;
    if (nargs > 0 && !type_contains_type_var(type) && !(self = mono_find(t, name))) {
        Symbol **rsym = is_struct ? &type->struc.resolved_sym : &type->unio.resolved_sym;
        if (!*rsym && symtab)
            *rsym = symtab_lookup_kind(symtab, name, is_struct ? DECL_STRUCT : DECL_UNION);
        for (int i = 0; i < nargs; i++)
            discover_nested_types(args[i], t, a, intern, symtab);
        const char *base = instance_base_name(type);
        const char *mangled = mangle_generic_name(intern, base, args, nargs);
        Symbol *sym = *rsym;
        if (!(self = mono_find(t, mangled)) &&
            sym && sym->is_generic && sym->decl && sym->type) {
            mono_instantiate(t, a, intern, sym, base, args, nargs);
            self = mono_find(t, mangled);
        }
    }
    if (self) return;
    if (is_struct)
        for (int i = 0; i < type->struc.field_count; i++)
            discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
    else
        for (int i = 0; i < type->unio.variant_count; i++)
            discover_nested_types(type->unio.variants[i].payload, t, a, intern, symtab);
}

/* Recursively walk a type tree and register any concrete generic struct/union
 * references that don't have a MonoInstance yet. This handles structs referenced
 * only as field types (never directly constructed via struct literals). */
static void discover_nested_types(Type *type, MonoTable *t, Arena *a,
                                   InternTable *intern, SymbolTable *symtab) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: discover_nested_types(type->pointer.pointee, t, a, intern, symtab); return;
    case TYPE_SLICE:   discover_nested_types(type->slice.elem, t, a, intern, symtab); return;
    case TYPE_OPTION:  discover_nested_types(type->option.inner, t, a, intern, symtab); return;
    case TYPE_RESULT:  discover_nested_types(type->result.inner, t, a, intern, symtab); return;
    case TYPE_FIXED_ARRAY: discover_nested_types(type->fixed_array.elem, t, a, intern, symtab); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            discover_nested_types(type->func.param_types[i], t, a, intern, symtab);
        discover_nested_types(type->func.return_type, t, a, intern, symtab);
        return;
    case TYPE_STRUCT:
        /* Tuple appearing only as a field type of another mono entry: register it
         * so its typedef/eq/default emit. type_args is empty, so it takes no
         * part in discover_aggregate. Recurse fields first to name nested
         * elements. */
        if (type->struc.is_tuple) {
            for (int i = 0; i < type->struc.field_count; i++)
                discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
            if (!type_contains_type_var(type)) {
                const char *cn = tuple_canonical_name(intern,
                    type->struc.fields, type->struc.field_count);
                type->struc.name = cn;
                type->struc.qualified_name = cn;
                if (!mono_find(t, cn)) {
                    Type *noargs[1] = {0};
                    mono_register(t, a, intern, cn, NULL, noargs, 0, NULL, DECL_STRUCT, NULL, 0);
                    MonoInstance *mi = mono_find(t, cn);
                    if (mi && !mi->concrete_type)
                        mi->concrete_type = type_deep_copy(a, type);
                }
            }
            return;
        }
        discover_aggregate(type, t, a, intern, symtab);
        return;
    case TYPE_UNION:
        discover_aggregate(type, t, a, intern, symtab);
        return;
    case TYPE_STUB:
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (mono_find(t, type->stub.name)) return;
            const char *base_name = instance_base_name(type);
            const char *mangled = mangle_generic_name(intern,
                base_name, type->stub.type_args, type->stub.type_arg_count);
            if (!mono_find(t, mangled) && symtab) {
                Symbol *sym = symtab_lookup_kind(symtab, base_name, DECL_STRUCT);
                if (!sym) sym = symtab_lookup_kind(symtab, base_name, DECL_UNION);
                if (sym && sym->is_generic && sym->decl && sym->type)
                    mono_instantiate(t, a, intern, sym, base_name,
                                     type->stub.type_args, type->stub.type_arg_count);
            }
        }
        return;
    CASE_TYPE_PRIMITIVES:
    case TYPE_ENUM:
    case TYPE_ANY_PTR:
    case TYPE_TYPE_VAR:
    case TYPE_CONST_INT:
    case TYPE_CONST_EXPR:
    case TYPE_ERROR:
    case TYPE_NEVER:
    case TYPE_UNRESOLVED:
    case TYPE_COUNT:
        return;
    }
}

/* Completeness backstop: walk a finalized concrete type and, if it references a
 * generic struct/union instance that was never registered (so codegen would
 * emit a dangling C typedef name), report an infinite-instantiation error once.
 * A missing instance is the sign of a truncated infinite family that the
 * limits above did not report; this reports it however discovery stopped.
 * Recurses through wrapper constructors, but not into a referenced instance's
 * own fields (those are checked when its own table entry is visited), so this
 * terminates. */
static void check_dangling_instance(MonoTable *t, Type *ty, Decl *site, bool *reported) {
    if (!ty || *reported) return;
    /* A type that still contains a type variable is template residue, not a
     * concrete instance codegen emits, so it is never dangling. */
    if (type_contains_type_var(ty)) return;
    switch (ty->kind) {
    /* Recurse only through wrapper constructors and function signatures, the
     * shapes that carry an emitted field type. Not into a generic instance's
     * type arguments: those are mangling inputs, not emitted member types, and
     * mono_resolve_type_names leaves them at their base name. Every emitted
     * instance is reached as some entry's field anyway. */
    case TYPE_POINTER:     check_dangling_instance(t, ty->pointer.pointee, site, reported); return;
    case TYPE_SLICE:       check_dangling_instance(t, ty->slice.elem, site, reported); return;
    case TYPE_OPTION:      check_dangling_instance(t, ty->option.inner, site, reported); return;
    case TYPE_RESULT:      check_dangling_instance(t, ty->result.inner, site, reported); return;
    case TYPE_FIXED_ARRAY: check_dangling_instance(t, ty->fixed_array.elem, site, reported); return;
    case TYPE_FUNC:
        for (int i = 0; i < ty->func.param_count; i++)
            check_dangling_instance(t, ty->func.param_types[i], site, reported);
        check_dangling_instance(t, ty->func.return_type, site, reported);
        return;
    case TYPE_STRUCT:
        if (ty->struc.type_arg_count > 0 && !mono_find(t, ty->struc.name)) goto dangling;
        return;
    case TYPE_UNION:
        if (ty->unio.type_arg_count > 0 && !mono_find(t, ty->unio.name)) goto dangling;
        return;
    case TYPE_STUB:
        /* A stub with args should have been resolved+registered by now; if not,
         * it is the same dangling-reference condition. */
        if (ty->stub.type_arg_count > 0 && !mono_find(t, ty->stub.name)) goto dangling;
        return;
    CASE_TYPE_PRIMITIVES:
    case TYPE_ENUM:
    case TYPE_ANY_PTR:
    case TYPE_TYPE_VAR:
    case TYPE_CONST_INT:
    case TYPE_CONST_EXPR:
    case TYPE_ERROR:
    case TYPE_NEVER:
    case TYPE_UNRESOLVED:
    case TYPE_COUNT:
        return;
    }
dangling:
    *reported = true;
    diag_error(site ? site->loc : (SrcLoc){0},
        "infinite generic instantiation: a generic type refers, through its fields, "
        "to an unbounded family of instances, and one of them was never made");
}

void mono_finalize_types(MonoTable *t, Arena *a, InternTable *intern) {
    mono_finalize_types_from(t, 0, a, intern);
}

void mono_finalize_types_from(MonoTable *t, int first, Arena *a, InternTable *intern) {
    if (t->count == first) return;

    /* Resolve all type names in concrete_types: convert canonical struct names
     * (e.g., "fc__m__entry") to instance C identifiers (e.g.,
     * "fc__m__entry__3_i323_i32"). Done after discovery so all instances are
     * registered and mono_find can prevent double-mangling. */
    for (int i = first; i < t->count; i++) {
        MonoInstance *inst = &t->entries[i];
        if (inst->concrete_type)
            mono_resolve_type_names(t, a, intern, inst->concrete_type);
    }

    /* Completeness backstop: reject any concrete type that references an
     * unregistered generic instance (a truncated infinite family; see
     * check_dangling_instance), so an incomplete table is never handed to
     * codegen. After an error, registration has stopped (mono_register), so
     * missing instances are expected and that error already stops codegen. */
    bool dangling_reported = diag_error_count() > 0;
    for (int i = first; i < t->count && !dangling_reported; i++) {
        MonoInstance *inst = &t->entries[i];
        if (!inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        if (ct->kind == TYPE_STRUCT) {
            for (int f = 0; f < ct->struc.field_count && !dangling_reported; f++)
                check_dangling_instance(t, ct->struc.fields[f].type, inst->template_decl, &dangling_reported);
        } else if (ct->kind == TYPE_UNION) {
            for (int v = 0; v < ct->unio.variant_count && !dangling_reported; v++)
                check_dangling_instance(t, ct->unio.variants[v].payload, inst->template_decl, &dangling_reported);
        }
    }
}

/* Backstop for const-generic evaluation failures stashed by this phase's
 * type_substitute calls (e.g. division by zero in the size expression of a
 * transitively discovered instance). Sites that report their own diagnostic
 * drain the slot themselves. Anything still stashed would otherwise be lost:
 * fcc would succeed while the emitted C names an unresolved instance. Report
 * it with its real cause and location. */
static void mono_drain_const_eval_error(void) {
    SrcLoc eloc = {0};
    const char *emsg = const_eval_take_error(&eloc);
    if (emsg)
        diag_error(eloc, "%s (in a const-generic instantiation)", emsg);
}

void mono_discover_transitive(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab) {
    mono_discover_transitive_from(t, 0, a, intern, symtab);
}

/* What instance `i` names: the instances its body makes, for a function, or
 * its fields and payloads, for a struct, union or tuple. */
static void discover_from_instance(MonoTable *t, int i, Arena *a, InternTable *intern,
                                   SymbolTable *symtab) {
    /* Snapshot what the walk needs: it can mono_register -> DA_APPEND ->
     * realloc t->entries, which would dangle a held &t->entries[i]. These are
     * arena/AST pointers and a plain int, so they stay valid across that
     * growth. */
    MonoInstance *inst = &t->entries[i];
    Type *ct = inst->concrete_type;
    t->walking = i + 1;
    if (inst->decl_kind == DECL_LET) {
        Decl *tmpl = inst->template_decl;
        if (tmpl && tmpl->let.init && tmpl->let.init->kind == EXPR_FUNC) {
            Expr *fn = tmpl->let.init;
            DiscoverCtx c = { t, a, intern, symtab, inst->type_param_names,
                              inst->type_args, inst->type_param_count };
            for (int j = 0; j < fn->func.body_count; j++)
                discover_in_expr(fn->func.body[j], &c);
        }
    } else if (ct && ct->kind == TYPE_STRUCT) {
        for (int f = 0; f < ct->struc.field_count; f++)
            discover_nested_types(ct->struc.fields[f].type, t, a, intern, symtab);
    } else if (ct && ct->kind == TYPE_UNION) {
        for (int v = 0; v < ct->unio.variant_count; v++)
            discover_nested_types(ct->unio.variants[v].payload, t, a, intern, symtab);
    }
    t->walking = 0;
}

/* Instances are walked in the order they were registered, and each one an
 * instance names is registered one step further from code than it: the walk
 * is breadth-first from every instance pass2 registered at once, so an
 * instance's depth is its distance from the nearest of them, through bodies
 * and fields alike. */
void mono_discover_transitive_from(MonoTable *t, int first, Arena *a, InternTable *intern,
                                   SymbolTable *symtab) {
    /* Give every concrete_type pass2 stored a private deep copy before the
     * in-place name canonicalization (discover_nested_types and, in
     * mono_finalize_types, mono_resolve_type_names both rewrite
     * struct/union/stub names in place). Several pass2 sites store as
     * concrete_type the expression type they inferred, a shallow copy that
     * shares field/variant subtrees with a live pass2 type or a generic
     * template, and renaming those shared nodes would corrupt them. Instances
     * discovery creates deep-copy at their build sites. */
    for (int i = first; i < t->count; i++)
        if (t->entries[i].concrete_type)
            t->entries[i].concrete_type = type_deep_copy(a, t->entries[i].concrete_type);
    for (int i = first; i < t->count; i++)
        discover_from_instance(t, i, a, intern, symtab);
    mono_drain_const_eval_error();
}
