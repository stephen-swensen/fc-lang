#include "pass2.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

/* ---- Scope for local bindings ---- */

typedef struct {
    const char *name;
    const char *codegen_name;   /* unique C name for shadowing */
    Type *type;
    bool is_mut;
} LocalBinding;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    LocalBinding *locals;
    int local_count;
    int local_cap;
};

static Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    return s;
}

static int local_id_counter = 0;

static void scope_add(Scope *s, const char *name, const char *codegen_name, Type *type, bool is_mut) {
    LocalBinding b = { name, codegen_name, type, is_mut };
    DA_APPEND(s->locals, s->local_count, s->local_cap, b);
}

static Type *scope_lookup(Scope *s, const char *name, const char **out_codegen_name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = sc->local_count - 1; i >= 0; i--) {
            if (sc->locals[i].name == name) {
                if (out_codegen_name) *out_codegen_name = sc->locals[i].codegen_name;
                return sc->locals[i].type;
            }
        }
    }
    if (out_codegen_name) *out_codegen_name = NULL;
    return NULL;
}

/* ---- Type checking ---- */

typedef struct {
    SymbolTable *symtab;
    Scope *scope;
    Arena *arena;
    Type **loop_break_type;  /* non-NULL when inside a loop; points to break value type */
    bool in_for;             /* true when inside a for loop (break value forbidden) */
    SymbolTable *module_symtab;  /* non-NULL when checking inside a module */
    const char *current_ns;      /* current namespace for namespace isolation */
    Type *recursive_ret;         /* non-NULL placeholder when resolving a recursive function */
} CheckCtx;

static Type *check_expr(CheckCtx *ctx, Expr *e);
static void check_decl_let(CheckCtx *ctx, Decl *d);

static Type *check_block(CheckCtx *ctx, Expr **stmts, int count) {
    if (count == 0) return type_void();
    Type *last = type_void();
    for (int i = 0; i < count; i++) {
        last = check_expr(ctx, stmts[i]);
    }
    return last;
}

/* Resolve a named type stub (TYPE_STRUCT with no fields) to the actual type from symtab */
static Type *resolve_type(CheckCtx *ctx, Type *t) {
    if (!t) return t;
    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0 && t->struc.fields == NULL && t->struc.name) {
        /* Check module symtab first (for within-module type references) */
        if (ctx->module_symtab) {
            Symbol *sym = symtab_lookup(ctx->module_symtab, t->struc.name);
            if (sym && sym->type) return sym->type;
        }
        Symbol *sym = symtab_lookup(ctx->symtab, t->struc.name);
        if (sym && sym->type) return sym->type;
    }
    return t;
}

/* Wrap an expression in an implicit widening cast */
static Expr *wrap_widen(Arena *a, Expr *e, Type *target) {
    Expr *cast = arena_alloc(a, sizeof(Expr));
    cast->kind = EXPR_CAST;
    cast->loc = e->loc;
    cast->type = target;
    cast->cast.target = target;
    cast->cast.operand = e;
    return cast;
}

static Type *check_match(CheckCtx *ctx, Expr *e);

/* Recursively check a struct destructuring pattern, adding bindings to scope */
static void check_destruct_pattern(CheckCtx *ctx, Pattern *pat, Type *struct_type, bool is_mut, SrcLoc loc) {
    if (pat->kind != PAT_STRUCT)
        diag_fatal(pat->loc, "expected struct destructuring pattern");
    if (struct_type->kind != TYPE_STRUCT)
        diag_fatal(loc, "cannot destructure non-struct type %s", type_name(struct_type));

    for (int i = 0; i < pat->struc.field_count; i++) {
        const char *fname = pat->struc.fields[i].name;
        Pattern *inner = pat->struc.fields[i].pattern;

        /* Find the field type */
        Type *field_type = NULL;
        for (int j = 0; j < struct_type->struc.field_count; j++) {
            if (struct_type->struc.fields[j].name == fname) {
                field_type = struct_type->struc.fields[j].type;
                break;
            }
        }
        if (!field_type)
            diag_fatal(loc, "struct '%s' has no field '%s'", struct_type->struc.name, fname);

        field_type = resolve_type(ctx, field_type);
        pat->struc.fields[i].resolved_type = field_type;

        if (inner->kind == PAT_BINDING) {
            const char *orig_name = inner->binding.name;
            int id = local_id_counter++;
            char buf[128];
            snprintf(buf, sizeof(buf), "_l_%s_%d", orig_name, id);
            char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(cg, buf, strlen(buf) + 1);
            inner->binding.name = cg;  /* overwrite with codegen name */
            scope_add(ctx->scope, orig_name, cg, field_type, is_mut);
        } else if (inner->kind == PAT_STRUCT) {
            check_destruct_pattern(ctx, inner, field_type, is_mut, loc);
        } else {
            diag_fatal(inner->loc, "unsupported pattern in let destructuring");
        }
    }
}

static Type *check_expr(CheckCtx *ctx, Expr *e) {
    switch (e->kind) {
    case EXPR_INT_LIT:
        e->type = e->int_lit.lit_type;
        return e->type;

    case EXPR_FLOAT_LIT:
        e->type = e->float_lit.lit_type;
        return e->type;

    case EXPR_BOOL_LIT:
        e->type = type_bool();
        return e->type;

    case EXPR_CHAR_LIT:
        e->type = type_char();
        return e->type;

    case EXPR_STRING_LIT:
        e->type = type_str();
        return e->type;

    case EXPR_CSTRING_LIT:
        e->type = type_cstr();
        return e->type;

    case EXPR_IDENT: {
        /* Check local scope first */
        const char *cg_name = NULL;
        Type *t = scope_lookup(ctx->scope, e->ident.name, &cg_name);
        if (t) {
            e->ident.codegen_name = cg_name;
            e->type = t;
            return t;
        }
        /* Check module symtab (for within-module sibling references) */
        if (ctx->module_symtab) {
            Symbol *msym = symtab_lookup(ctx->module_symtab, e->ident.name);
            if (msym) {
                if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) {
                    e->type = msym->type;
                    return e->type;
                }
                /* Use the mangled codegen_name */
                if (msym->decl && msym->decl->kind == DECL_LET && msym->decl->let.codegen_name) {
                    e->ident.codegen_name = msym->decl->let.codegen_name;
                }
                if (!msym->type) diag_fatal(e->loc, "use of '%s' before its type is resolved", e->ident.name);
                e->type = msym->type;
                return e->type;
            }
        }
        /* Check global symbol table */
        Symbol *sym = symtab_lookup(ctx->symtab, e->ident.name);
        if (!sym) diag_fatal(e->loc, "undefined name '%s'", e->ident.name);
        /* For struct/union type names used in expressions (e.g., variant construction),
         * return the type itself */
        if (sym->kind == DECL_STRUCT || sym->kind == DECL_UNION) {
            e->type = sym->type;
            return e->type;
        }
        /* Module names resolve to a sentinel — handled in EXPR_FIELD.
         * Use namespace-aware lookup: prefer same-namespace entry. */
        if (sym->kind == DECL_MODULE) {
            Symbol *ns_mod = symtab_lookup_module(ctx->symtab, e->ident.name, ctx->current_ns);
            if (!ns_mod && ctx->current_ns) {
                ns_mod = symtab_lookup_module(ctx->symtab, e->ident.name, NULL);
            }
            if (!ns_mod) {
                diag_fatal(e->loc, "module '%s' is in a different namespace; use 'import' to access it",
                    e->ident.name);
            }
            e->type = type_void();  /* placeholder; real type determined by EXPR_FIELD */
            return e->type;
        }
        if (!sym->type) diag_fatal(e->loc, "use of '%s' before its type is resolved", e->ident.name);
        /* Propagate codegen_name from imported/module symbols */
        if (sym->decl && sym->decl->kind == DECL_LET && sym->decl->let.codegen_name) {
            e->ident.codegen_name = sym->decl->let.codegen_name;
        }
        e->type = sym->type;
        return e->type;
    }

    case EXPR_BINARY: {
        Type *lt = check_expr(ctx, e->binary.left);
        Type *rt = check_expr(ctx, e->binary.right);
        TokenKind op = e->binary.op;

        if (op == TOK_PLUS || op == TOK_MINUS) {
            /* Allow pointer arithmetic: ptr + int, ptr - int */
            if (lt->kind == TYPE_POINTER && type_is_integer(rt)) {
                e->type = lt;
                return e->type;
            }
            if (!type_is_numeric(lt) || !type_is_numeric(rt))
                diag_fatal(e->loc, "arithmetic requires numeric operands, got %s and %s",
                    type_name(lt), type_name(rt));
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common)
                    diag_fatal(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        if (op == TOK_STAR || op == TOK_SLASH || op == TOK_PERCENT) {
            if (!type_is_numeric(lt) || !type_is_numeric(rt))
                diag_fatal(e->loc, "arithmetic requires numeric operands, got %s and %s",
                    type_name(lt), type_name(rt));
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common)
                    diag_fatal(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        if (op == TOK_EQEQ || op == TOK_BANGEQ || op == TOK_LT ||
            op == TOK_GT || op == TOK_LTEQ || op == TOK_GTEQ) {
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common)
                    diag_fatal(e->loc, "comparison type mismatch: %s vs %s", type_name(lt), type_name(rt));
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
            }
            e->type = type_bool();
            return e->type;
        }

        if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
            if (!type_eq(lt, type_bool()) || !type_eq(rt, type_bool()))
                diag_fatal(e->loc, "logical operator requires bool operands");
            e->type = type_bool();
            return e->type;
        }

        if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET) {
            if (!type_is_integer(lt) || !type_is_integer(rt))
                diag_fatal(e->loc, "bitwise operator requires integer operands");
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common)
                    diag_fatal(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        if (op == TOK_LTLT || op == TOK_GTGT) {
            if (!type_is_integer(lt) || !type_is_integer(rt))
                diag_fatal(e->loc, "shift requires integer operands");
            e->type = lt;
            return e->type;
        }

        diag_fatal(e->loc, "unsupported binary operator");
    }

    case EXPR_UNARY_PREFIX: {
        Type *ot = check_expr(ctx, e->unary_prefix.operand);
        TokenKind op = e->unary_prefix.op;
        if (op == TOK_MINUS) {
            if (!type_is_numeric(ot))
                diag_fatal(e->loc, "unary minus requires numeric operand, got %s", type_name(ot));
            e->type = ot;
        } else if (op == TOK_BANG) {
            if (!type_eq(ot, type_bool()))
                diag_fatal(e->loc, "unary ! requires bool operand, got %s", type_name(ot));
            e->type = type_bool();
        } else if (op == TOK_TILDE) {
            if (!type_is_integer(ot))
                diag_fatal(e->loc, "bitwise not requires integer operand");
            e->type = ot;
        } else if (op == TOK_AMP) {
            /* Address-of: result is pointer */
            e->type = type_pointer(ctx->arena, ot);
        } else if (op == TOK_STAR) {
            /* Dereference: operand must be pointer */
            if (ot->kind != TYPE_POINTER)
                diag_fatal(e->loc, "dereference requires pointer operand, got %s", type_name(ot));
            e->type = ot->pointer.pointee;
        } else {
            diag_fatal(e->loc, "unsupported unary operator");
        }
        return e->type;
    }

    case EXPR_UNARY_POSTFIX: {
        Type *ot = check_expr(ctx, e->unary_postfix.operand);
        if (e->unary_postfix.op == TOK_BANG) {
            /* Option unwrap: T? -> T */
            if (ot->kind != TYPE_OPTION)
                diag_fatal(e->loc, "unwrap (!) requires option type, got %s", type_name(ot));
            e->type = ot->option.inner;
            return e->type;
        }
        diag_fatal(e->loc, "unsupported postfix operator");
    }

    case EXPR_FUNC: {
        /* Create function type from params */
        int pc = e->func.param_count;
        Type **ptypes = NULL;
        if (pc > 0) {
            ptypes = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)pc);
        }

        /* Create inner scope for function body */
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        for (int i = 0; i < pc; i++) {
            ptypes[i] = resolve_type(ctx, e->func.params[i].type);
            e->func.params[i].type = ptypes[i];
            scope_add(inner, e->func.params[i].name, e->func.params[i].name, ptypes[i], false);
        }

        /* Type-check body in inner scope */
        Scope *saved = ctx->scope;
        ctx->scope = inner;
        Type *ret = check_block(ctx, e->func.body, e->func.body_count);
        ctx->scope = saved;

        /* Build function type */
        Type *ft = arena_alloc(ctx->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_types = ptypes;
        ft->func.param_count = pc;
        ft->func.return_type = ret;
        e->type = ft;
        return e->type;
    }

    case EXPR_CALL: {
        Type *ft = check_expr(ctx, e->call.func);

        /* Check if this is a union variant constructor: union_name.variant(payload) */
        if (ft->kind == TYPE_UNION && e->call.func->kind == EXPR_FIELD) {
            Type *union_type = ft;
            const char *variant_name = e->call.func->field.name;
            /* Find the variant */
            for (int v = 0; v < union_type->unio.variant_count; v++) {
                if (union_type->unio.variants[v].name == variant_name) {
                    Type *payload_type = resolve_type(ctx, union_type->unio.variants[v].payload);
                    if (!payload_type)
                        diag_fatal(e->loc, "variant '%s' takes no payload", variant_name);
                    if (e->call.arg_count != 1)
                        diag_fatal(e->loc, "variant constructor takes exactly 1 argument");
                    Type *arg_type = check_expr(ctx, e->call.args[0]);
                    if (!type_eq(arg_type, payload_type))
                        diag_fatal(e->call.args[0]->loc,
                            "variant '%s': expected %s, got %s",
                            variant_name, type_name(payload_type), type_name(arg_type));
                    e->type = union_type;
                    return e->type;
                }
            }
            diag_fatal(e->loc, "union '%s' has no variant '%s'",
                union_type->unio.name, variant_name);
        }

        if (ft->kind != TYPE_FUNC)
            diag_fatal(e->loc, "cannot call non-function type %s", type_name(ft));
        if (e->call.arg_count != ft->func.param_count)
            diag_fatal(e->loc, "expected %d arguments, got %d",
                ft->func.param_count, e->call.arg_count);
        for (int i = 0; i < e->call.arg_count; i++) {
            Type *at = check_expr(ctx, e->call.args[i]);
            if (!type_eq(at, ft->func.param_types[i])) {
                if (type_can_widen(at, ft->func.param_types[i])) {
                    e->call.args[i] = wrap_widen(ctx->arena, e->call.args[i], ft->func.param_types[i]);
                } else {
                    diag_fatal(e->call.args[i]->loc, "argument %d: expected %s, got %s",
                        i + 1, type_name(ft->func.param_types[i]), type_name(at));
                }
            }
        }
        e->type = ft->func.return_type;
        return e->type;
    }

    case EXPR_IF: {
        Type *ct = check_expr(ctx, e->if_expr.cond);
        if (!type_eq(ct, type_bool()))
            diag_fatal(e->loc, "if condition must be bool, got %s", type_name(ct));
        Type *tt = check_expr(ctx, e->if_expr.then_body);
        /* If resolving a recursive function, fill in the return type from the
         * base case (then-branch) before checking the recursive branch (else). */
        if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_VOID &&
            tt->kind != TYPE_VOID) {
            *ctx->recursive_ret = *tt;
        }
        if (e->if_expr.else_body) {
            Type *et = check_expr(ctx, e->if_expr.else_body);
            if (!type_eq(tt, et)) {
                diag_fatal(e->loc, "if branches have different types: %s vs %s",
                    type_name(tt), type_name(et));
            }
            e->type = tt;
        } else {
            /* No else → void */
            e->type = type_void();
        }
        return e->type;
    }

    case EXPR_BLOCK: {
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;
        e->type = check_block(ctx, e->block.stmts, e->block.count);
        ctx->scope = saved;
        return e->type;
    }

    case EXPR_LET: {
        Type *t = check_expr(ctx, e->let_expr.let_init);
        if (t->kind == TYPE_VOID)
            diag_fatal(e->loc, "cannot bind void expression to '%s'", e->let_expr.let_name);
        e->let_expr.let_type = t;
        int id = local_id_counter++;
        char buf[128];
        snprintf(buf, sizeof(buf), "_l_%s_%d", e->let_expr.let_name, id);
        char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
        memcpy(cg, buf, strlen(buf) + 1);
        e->let_expr.codegen_name = cg;
        scope_add(ctx->scope, e->let_expr.let_name, cg, t, e->let_expr.let_is_mut);
        e->type = type_void();
        return e->type;
    }

    case EXPR_LET_DESTRUCT: {
        Type *t = check_expr(ctx, e->let_destruct.init);
        if (t->kind != TYPE_STRUCT)
            diag_fatal(e->loc, "cannot destructure non-struct type %s", type_name(t));
        e->let_destruct.init_type = t;

        /* Generate temp name for the RHS struct value */
        int tmp_id = local_id_counter++;
        char tmp_buf[128];
        snprintf(tmp_buf, sizeof(tmp_buf), "_ds_%d", tmp_id);
        char *tmp_name = arena_alloc(ctx->arena, strlen(tmp_buf) + 1);
        memcpy(tmp_name, tmp_buf, strlen(tmp_buf) + 1);
        e->let_destruct.tmp_name = tmp_name;

        check_destruct_pattern(ctx, e->let_destruct.pattern, t, e->let_destruct.is_mut, e->loc);

        e->type = type_void();
        return e->type;
    }

    case EXPR_RETURN: {
        if (e->return_expr.value) {
            check_expr(ctx, e->return_expr.value);
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ASSIGN: {
        Type *lt = check_expr(ctx, e->assign.target);
        Type *vt = check_expr(ctx, e->assign.value);
        if (!type_eq(lt, vt)) {
            if (type_can_widen(vt, lt)) {
                e->assign.value = wrap_widen(ctx->arena, e->assign.value, lt);
            } else {
                diag_fatal(e->loc, "assignment type mismatch: %s vs %s", type_name(lt), type_name(vt));
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_CAST: {
        Type *from = check_expr(ctx, e->cast.operand);
        Type *to = e->cast.target;
        bool from_num = type_is_numeric(from);
        bool to_num = type_is_numeric(to);
        bool from_ptr = (from->kind == TYPE_POINTER || from->kind == TYPE_ANY_PTR || from->kind == TYPE_CSTR);
        bool to_ptr = (to->kind == TYPE_POINTER || to->kind == TYPE_ANY_PTR || to->kind == TYPE_CSTR);
        bool from_int = type_is_integer(from);
        bool to_int = type_is_integer(to);
        /* Allowed: numeric <-> numeric, pointer <-> pointer, pointer <-> integer */
        if (!((from_num && to_num) || (from_ptr && to_ptr) ||
              (from_ptr && to_int) || (from_int && to_ptr)))
            diag_fatal(e->loc, "invalid cast from %s to %s", type_name(from), type_name(to));
        e->type = to;
        return e->type;
    }

    case EXPR_STRUCT_LIT: {
        /* Look up the struct type */
        Symbol *sym = symtab_lookup(ctx->symtab, e->struct_lit.type_name);
        if (!sym) diag_fatal(e->loc, "unknown type '%s'", e->struct_lit.type_name);
        if (sym->kind != DECL_STRUCT || !sym->type || sym->type->kind != TYPE_STRUCT)
            diag_fatal(e->loc, "'%s' is not a struct type", e->struct_lit.type_name);
        Type *st = sym->type;

        /* Type-check each field init */
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            FieldInit *fi = &e->struct_lit.fields[i];
            /* Find field in struct */
            bool found = false;
            for (int j = 0; j < st->struc.field_count; j++) {
                if (st->struc.fields[j].name == fi->name) {
                    Type *ft = check_expr(ctx, fi->value);
                    Type *expected = resolve_type(ctx, st->struc.fields[j].type);
                    if (!type_eq(ft, expected))
                        diag_fatal(fi->value->loc, "field '%s': expected %s, got %s",
                            fi->name, type_name(expected), type_name(ft));
                    found = true;
                    break;
                }
            }
            if (!found)
                diag_fatal(e->loc, "struct '%s' has no field '%s'",
                    e->struct_lit.type_name, fi->name);
        }
        e->type = st;
        return e->type;
    }

    case EXPR_FIELD: {
        Type *obj_type = check_expr(ctx, e->field.object);

        /* Try to resolve the object as a module reference (handles nested chains).
         * For EXPR_IDENT "math", find module "math".
         * For EXPR_FIELD "geometry.shapes", recursively find submodule "shapes" in "geometry".
         * Also handles type-associated modules (same name as struct/union). */
        Symbol *mod_sym = NULL;
        if (e->field.object->kind == EXPR_IDENT) {
            const char *name = e->field.object->ident.name;
            /* Namespace-aware module lookup: try current namespace first, then global */
            mod_sym = symtab_lookup_module(ctx->symtab, name, ctx->current_ns);
            if (!mod_sym && ctx->current_ns)
                mod_sym = symtab_lookup_module(ctx->symtab, name, NULL);
            /* Also check module symtab for within-module references */
            if (!mod_sym && ctx->module_symtab)
                mod_sym = symtab_lookup_kind(ctx->module_symtab, name, DECL_MODULE);
        } else if (e->field.object->kind == EXPR_FIELD && e->field.object->type &&
                   e->field.object->type->kind == TYPE_VOID) {
            /* Object is a field expression that resolved to void (submodule sentinel).
             * We stored a module_ref tag to help find it. Walk the chain. */
            /* Use the codegen_name we set as a breadcrumb to the submodule */
        }

        /* If object's EXPR_FIELD resolved to a module (for nested chains like a.b.member),
         * try to find the module by walking the EXPR_FIELD chain. */
        if (!mod_sym && e->field.object->kind == EXPR_FIELD) {
            /* Walk up the nested EXPR_FIELD chain to find the deepest module. */
            /* We need to re-resolve: the object is already type-checked and set to void.
             * Walk the chain from root to find the module. */
            Expr *chain[32];
            int depth = 0;
            Expr *cur = e->field.object;
            while (cur->kind == EXPR_FIELD && depth < 31) {
                chain[depth++] = cur;
                cur = cur->field.object;
            }
            if (cur->kind == EXPR_IDENT) {
                Symbol *root = symtab_lookup_module(ctx->symtab, cur->ident.name, ctx->current_ns);
                if (!root && ctx->current_ns)
                    root = symtab_lookup_module(ctx->symtab, cur->ident.name, NULL);
                if (!root && ctx->module_symtab)
                    root = symtab_lookup_kind(ctx->module_symtab, cur->ident.name, DECL_MODULE);
                if (root && root->members) {
                    Symbol *walk = root;
                    for (int k = depth - 1; k >= 0; k--) {
                        Symbol *next = symtab_lookup_kind(walk->members, chain[k]->field.name, DECL_MODULE);
                        if (!next) { walk = NULL; break; }
                        walk = next;
                    }
                    if (walk) mod_sym = walk;
                }
            }
        }

        if (mod_sym && mod_sym->members) {
            Symbol *member = symtab_lookup(mod_sym->members, e->field.name);
            if (!member)
                diag_fatal(e->loc, "module '%s' has no member '%s'",
                    mod_sym->name, e->field.name);
            if (member->is_private)
                diag_fatal(e->loc, "cannot access private member '%s' of module '%s'",
                    e->field.name, mod_sym->name);
            /* Submodule access: return void sentinel for further chaining */
            if (member->kind == DECL_MODULE) {
                e->type = type_void();
                return e->type;
            }
            /* Struct/union type member */
            if (member->kind == DECL_STRUCT || member->kind == DECL_UNION) {
                e->type = member->type;
                return e->type;
            }
            /* Let member: set codegen_name */
            if (member->decl && member->decl->kind == DECL_LET) {
                e->field.codegen_name = member->decl->let.codegen_name;
            }
            if (!member->type && member->decl && member->decl->kind == DECL_LET) {
                /* On-demand type-check: target module member hasn't been processed yet.
                 * This is safe because pass1 phase 4 already detected circular deps. */
                SymbolTable *saved_mod = ctx->module_symtab;
                Scope *saved_scope = ctx->scope;
                ctx->module_symtab = mod_sym->members;
                ctx->scope = scope_new(ctx->arena, NULL);
                check_decl_let(ctx, member->decl);
                ctx->scope = saved_scope;
                ctx->module_symtab = saved_mod;
            }
            if (!member->type)
                diag_fatal(e->loc, "use of '%s.%s' before its type is resolved",
                    mod_sym->name, e->field.name);
            e->type = member->type;
            return e->type;
        }

        /* If the object is an IDENT referencing a union type, this is variant construction */
        if (e->field.object->kind == EXPR_IDENT) {
            Symbol *sym = symtab_lookup(ctx->symtab, e->field.object->ident.name);
            if (sym && sym->kind == DECL_UNION && sym->type && sym->type->kind == TYPE_UNION) {
                e->type = sym->type;
                return e->type;
            }
        }

        /* If the object resolved to a union type (e.g., module.UnionType.Variant) */
        if (obj_type->kind == TYPE_UNION) {
            e->type = obj_type;
            return e->type;
        }

        obj_type = resolve_type(ctx, obj_type);

        /* Slice .len and .ptr fields */
        if (obj_type->kind == TYPE_SLICE || obj_type->kind == TYPE_STR) {
            if (strcmp(e->field.name, "len") == 0) {
                e->type = type_int64();
                return e->type;
            }
            if (strcmp(e->field.name, "ptr") == 0) {
                Type *elem = (obj_type->kind == TYPE_STR) ? type_uint8() : obj_type->slice.elem;
                e->type = type_pointer(ctx->arena, elem);
                return e->type;
            }
        }

        /* Normal struct field access */
        if (obj_type->kind != TYPE_STRUCT)
            diag_fatal(e->loc, "field access on non-struct type %s", type_name(obj_type));
        for (int i = 0; i < obj_type->struc.field_count; i++) {
            if (obj_type->struc.fields[i].name == e->field.name) {
                e->type = resolve_type(ctx, obj_type->struc.fields[i].type);
                return e->type;
            }
        }
        diag_fatal(e->loc, "struct '%s' has no field '%s'",
            obj_type->struc.name, e->field.name);
    }

    case EXPR_DEREF_FIELD: {
        Type *obj_type = check_expr(ctx, e->field.object);
        if (obj_type->kind != TYPE_POINTER)
            diag_fatal(e->loc, "-> requires pointer type, got %s", type_name(obj_type));
        Type *pointee = resolve_type(ctx, obj_type->pointer.pointee);
        if (pointee->kind != TYPE_STRUCT)
            diag_fatal(e->loc, "-> requires pointer to struct, got pointer to %s", type_name(pointee));
        for (int i = 0; i < pointee->struc.field_count; i++) {
            if (pointee->struc.fields[i].name == e->field.name) {
                e->type = resolve_type(ctx, pointee->struc.fields[i].type);
                return e->type;
            }
        }
        diag_fatal(e->loc, "struct '%s' has no field '%s'",
            pointee->struc.name, e->field.name);
    }

    case EXPR_INDEX: {
        Type *obj_type = check_expr(ctx, e->index.object);
        Type *idx_type = check_expr(ctx, e->index.index);
        if (!type_is_integer(idx_type))
            diag_fatal(e->loc, "index must be integer, got %s", type_name(idx_type));

        if (obj_type->kind == TYPE_SLICE) {
            e->type = obj_type->slice.elem;
            return e->type;
        }
        if (obj_type->kind == TYPE_STR) {
            e->type = type_uint8();
            return e->type;
        }
        if (obj_type->kind == TYPE_POINTER) {
            e->type = obj_type->pointer.pointee;
            return e->type;
        }
        diag_fatal(e->loc, "indexing requires slice or pointer, got %s", type_name(obj_type));
    }

    case EXPR_SLICE: {
        Type *obj_type = check_expr(ctx, e->slice.object);
        if (e->slice.lo) {
            Type *lo_type = check_expr(ctx, e->slice.lo);
            if (!type_is_integer(lo_type))
                diag_fatal(e->loc, "slice index must be integer");
        }
        if (e->slice.hi) {
            Type *hi_type = check_expr(ctx, e->slice.hi);
            if (!type_is_integer(hi_type))
                diag_fatal(e->loc, "slice index must be integer");
        }
        if (obj_type->kind != TYPE_SLICE && obj_type->kind != TYPE_STR)
            diag_fatal(e->loc, "subslice requires slice type, got %s", type_name(obj_type));
        e->type = obj_type;
        return e->type;
    }

    case EXPR_ARRAY_LIT: {
        /* Array literal: type[size] { elems... } → creates a slice */
        /* The size expression must be an integer */
        Type *size_type = check_expr(ctx, e->array_lit.size_expr);
        if (!type_is_integer(size_type))
            diag_fatal(e->loc, "array size must be integer, got %s", type_name(size_type));
        /* Type-check elements */
        Type *elem_type = resolve_type(ctx, e->array_lit.elem_type);
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            Type *et = check_expr(ctx, e->array_lit.elems[i]);
            if (!type_eq(et, elem_type))
                diag_fatal(e->array_lit.elems[i]->loc,
                    "array element type mismatch: expected %s, got %s",
                    type_name(elem_type), type_name(et));
        }
        e->type = type_slice(ctx->arena, elem_type);
        return e->type;
    }

    case EXPR_SOME: {
        Type *inner = check_expr(ctx, e->some_expr.value);
        e->type = type_option(ctx->arena, inner);
        return e->type;
    }

    case EXPR_NONE: {
        if (e->none_expr.target) {
            /* T.none form: resolve any type stubs */
            e->type = resolve_type(ctx, e->none_expr.target);
            if (e->type->kind == TYPE_OPTION)
                e->type->option.inner = resolve_type(ctx, e->type->option.inner);
            return e->type;
        }
        diag_fatal(e->loc, "bare 'none' is not allowed in expressions; use T.none (e.g. int32.none)");
    }

    case EXPR_LOOP: {
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;

        /* Set up break type tracking */
        Type *break_type = NULL;
        Type **saved_break = ctx->loop_break_type;
        bool saved_in_for = ctx->in_for;
        ctx->loop_break_type = &break_type;
        ctx->in_for = false;

        check_block(ctx, e->loop_expr.body, e->loop_expr.body_count);

        ctx->scope = saved;
        ctx->loop_break_type = saved_break;
        ctx->in_for = saved_in_for;

        /* Loop type comes from break values; void if no break-with-value */
        e->type = break_type ? break_type : type_void();
        return e->type;
    }

    case EXPR_FOR: {
        /* Type check the iterator/range */
        Type *iter_type = check_expr(ctx, e->for_expr.iter);

        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;

        if (e->for_expr.range_end) {
            /* Range iteration: for i in lo..hi */
            Type *end_type = check_expr(ctx, e->for_expr.range_end);
            if (!type_is_integer(iter_type) || !type_is_integer(end_type))
                diag_fatal(e->loc, "range bounds must be integer types");
            /* Use the wider type; if both same, use that */
            Type *var_type = iter_type;
            if (!type_eq(iter_type, end_type)) {
                /* Allow int32..int64 widening */
                if (iter_type->kind < end_type->kind) var_type = end_type;
                else var_type = iter_type;
            }
            scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, var_type, false);
        } else {
            /* Collection iteration: for x in slice */
            if (iter_type->kind == TYPE_SLICE) {
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, iter_type->slice.elem, false);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var, e->for_expr.index_var, type_int64(), false);
                }
            } else if (iter_type->kind == TYPE_STR) {
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_uint8(), false);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var, e->for_expr.index_var, type_int64(), false);
                }
            } else {
                diag_fatal(e->loc, "for-in requires slice or range, got %s", type_name(iter_type));
            }
        }

        /* Save/set loop context for break checking */
        Type **saved_break = ctx->loop_break_type;
        bool saved_in_for = ctx->in_for;
        Type *break_type = NULL;
        ctx->loop_break_type = &break_type;
        ctx->in_for = true;

        check_block(ctx, e->for_expr.body, e->for_expr.body_count);

        ctx->scope = saved;
        ctx->loop_break_type = saved_break;
        ctx->in_for = saved_in_for;

        e->type = type_void();
        return e->type;
    }

    case EXPR_BREAK: {
        if (!ctx->loop_break_type)
            diag_fatal(e->loc, "break outside of loop");
        if (e->break_expr.value) {
            if (ctx->in_for)
                diag_fatal(e->loc, "break with value is not allowed in for loops");
            Type *vt = check_expr(ctx, e->break_expr.value);
            if (*ctx->loop_break_type == NULL) {
                *ctx->loop_break_type = vt;
            } else if (!type_eq(*ctx->loop_break_type, vt)) {
                diag_fatal(e->loc, "break type mismatch: expected %s, got %s",
                    type_name(*ctx->loop_break_type), type_name(vt));
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_CONTINUE: {
        if (!ctx->loop_break_type)
            diag_fatal(e->loc, "continue outside of loop");
        e->type = type_void();
        return e->type;
    }

    case EXPR_MATCH:
        return check_match(ctx, e);

    case EXPR_SIZEOF: {
        Type *ty = resolve_type(ctx, e->sizeof_expr.target);
        e->sizeof_expr.target = ty;
        e->type = type_int64();
        return e->type;
    }

    case EXPR_DEFAULT: {
        Type *ty = resolve_type(ctx, e->default_expr.target);
        e->default_expr.target = ty;
        e->type = ty;
        return e->type;
    }

    case EXPR_FREE: {
        Type *ot = check_expr(ctx, e->free_expr.operand);
        if (ot->kind != TYPE_POINTER && ot->kind != TYPE_SLICE &&
            ot->kind != TYPE_STR && ot->kind != TYPE_ANY_PTR && ot->kind != TYPE_CSTR)
            diag_fatal(e->loc, "free requires pointer or slice, got %s", type_name(ot));
        e->type = type_void();
        return e->type;
    }

    case EXPR_ALLOC: {
        if (e->alloc_expr.alloc_type) {
            Type *ty = resolve_type(ctx, e->alloc_expr.alloc_type);
            e->alloc_expr.alloc_type = ty;
            if (e->alloc_expr.size_expr) {
                /* alloc(T[N]) → T[]? */
                Type *st = check_expr(ctx, e->alloc_expr.size_expr);
                if (!type_is_integer(st))
                    diag_fatal(e->loc, "alloc array size must be integer, got %s", type_name(st));
                e->type = type_option(ctx->arena, type_slice(ctx->arena, ty));
            } else {
                /* alloc(T) → T*? */
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
            }
        } else {
            /* alloc(expr) → T*? where T is the type of expr */
            Type *t = check_expr(ctx, e->alloc_expr.init_expr);
            e->type = type_option(ctx->arena, type_pointer(ctx->arena, t));
        }
        return e->type;
    }

    default:
        diag_fatal(e->loc, "unsupported expression kind in type checker (kind=%d)", e->kind);
    }
}

/* Recursively check a struct pattern in a match arm, resolving types and adding bindings */
static void check_match_struct_pattern(CheckCtx *ctx, Pattern *pat, Type *struct_type) {
    for (int fi = 0; fi < pat->struc.field_count; fi++) {
        const char *fname = pat->struc.fields[fi].name;
        Pattern *inner = pat->struc.fields[fi].pattern;
        Type *field_type = NULL;
        for (int fj = 0; fj < struct_type->struc.field_count; fj++) {
            if (struct_type->struc.fields[fj].name == fname) {
                field_type = resolve_type(ctx, struct_type->struc.fields[fj].type);
                break;
            }
        }
        if (!field_type)
            diag_fatal(pat->loc, "struct '%s' has no field '%s'", struct_type->struc.name, fname);
        pat->struc.fields[fi].resolved_type = field_type;
        switch (inner->kind) {
        case PAT_BINDING:
            /* Check if binding name is a no-payload union variant */
            if (field_type->kind == TYPE_UNION) {
                bool is_variant = false;
                for (int v = 0; v < field_type->unio.variant_count; v++) {
                    if (field_type->unio.variants[v].name == inner->binding.name &&
                        field_type->unio.variants[v].payload == NULL) {
                        inner->kind = PAT_VARIANT;
                        inner->variant.variant = inner->binding.name;
                        inner->variant.payload = NULL;
                        is_variant = true;
                        break;
                    }
                }
                if (!is_variant)
                    scope_add(ctx->scope, inner->binding.name, inner->binding.name, field_type, false);
            } else {
                scope_add(ctx->scope, inner->binding.name, inner->binding.name, field_type, false);
            }
            break;
        case PAT_WILDCARD:
            break;
        case PAT_STRUCT:
            if (field_type->kind != TYPE_STRUCT)
                diag_fatal(inner->loc, "struct pattern on non-struct field '%s' (type %s)", fname, type_name(field_type));
            check_match_struct_pattern(ctx, inner, field_type);
            break;
        case PAT_INT_LIT:
            if (!type_is_integer(field_type))
                diag_fatal(inner->loc, "integer pattern on non-integer field '%s'", fname);
            break;
        case PAT_BOOL_LIT:
            if (!type_eq(field_type, type_bool()))
                diag_fatal(inner->loc, "bool pattern on non-bool field '%s'", fname);
            break;
        case PAT_CHAR_LIT:
            if (!type_eq(field_type, type_char()))
                diag_fatal(inner->loc, "char pattern on non-char field '%s'", fname);
            break;
        case PAT_SOME:
            if (field_type->kind != TYPE_OPTION)
                diag_fatal(inner->loc, "some pattern on non-option field '%s' (type %s)", fname, type_name(field_type));
            if (inner->some_pat.inner && inner->some_pat.inner->kind == PAT_BINDING) {
                Type *inner_type = field_type->option.inner;
                scope_add(ctx->scope, inner->some_pat.inner->binding.name,
                    inner->some_pat.inner->binding.name, inner_type, false);
            }
            break;
        case PAT_NONE:
            if (field_type->kind != TYPE_OPTION)
                diag_fatal(inner->loc, "none pattern on non-option field '%s' (type %s)", fname, type_name(field_type));
            break;
        case PAT_VARIANT: {
            if (field_type->kind != TYPE_UNION)
                diag_fatal(inner->loc, "variant pattern on non-union field '%s' (type %s)", fname, type_name(field_type));
            bool found = false;
            for (int v = 0; v < field_type->unio.variant_count; v++) {
                if (field_type->unio.variants[v].name == inner->variant.variant) {
                    found = true;
                    if (inner->variant.payload && field_type->unio.variants[v].payload) {
                        Type *payload_type = resolve_type(ctx, field_type->unio.variants[v].payload);
                        if (inner->variant.payload->kind == PAT_BINDING) {
                            scope_add(ctx->scope, inner->variant.payload->binding.name,
                                inner->variant.payload->binding.name, payload_type, false);
                        }
                    }
                    break;
                }
            }
            if (!found)
                diag_fatal(inner->loc, "union '%s' has no variant '%s'",
                    field_type->unio.name, inner->variant.variant);
            break;
        }
        default:
            diag_fatal(inner->loc, "unsupported pattern in struct field '%s'", fname);
            break;
        }
    }
}

static Type *check_match(CheckCtx *ctx, Expr *e) {
    Type *subj_type = check_expr(ctx, e->match_expr.subject);
    subj_type = resolve_type(ctx, subj_type);
    /* Update the subject's type to the resolved type so codegen can access it */
    e->match_expr.subject->type = subj_type;

    if (e->match_expr.arm_count == 0)
        diag_fatal(e->loc, "match expression has no arms");

    Type *result_type = NULL;

    for (int i = 0; i < e->match_expr.arm_count; i++) {
        MatchArm *arm = &e->match_expr.arms[i];
        Pattern *pat = arm->pattern;

        /* Create a new scope for pattern bindings */
        Scope *arm_scope = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = arm_scope;

        /* Check pattern and introduce bindings */
        switch (pat->kind) {
        case PAT_WILDCARD:
            /* Matches anything */
            break;
        case PAT_BINDING:
            /* Check if binding name is actually a no-payload union variant */
            if (subj_type->kind == TYPE_UNION) {
                bool is_variant = false;
                for (int v = 0; v < subj_type->unio.variant_count; v++) {
                    if (subj_type->unio.variants[v].name == pat->binding.name &&
                        subj_type->unio.variants[v].payload == NULL) {
                        /* Rewrite as variant pattern */
                        pat->kind = PAT_VARIANT;
                        pat->variant.variant = pat->binding.name;
                        pat->variant.payload = NULL;
                        is_variant = true;
                        break;
                    }
                }
                if (is_variant) break;
            }
            /* Bind the subject value to a name */
            scope_add(ctx->scope, pat->binding.name, pat->binding.name, subj_type, false);
            break;
        case PAT_INT_LIT:
            if (!type_is_integer(subj_type))
                diag_fatal(pat->loc, "integer pattern on non-integer type %s", type_name(subj_type));
            break;
        case PAT_BOOL_LIT:
            if (!type_eq(subj_type, type_bool()))
                diag_fatal(pat->loc, "bool pattern on non-bool type %s", type_name(subj_type));
            break;
        case PAT_CHAR_LIT:
            if (!type_eq(subj_type, type_char()))
                diag_fatal(pat->loc, "char pattern on non-char type %s", type_name(subj_type));
            break;
        case PAT_SOME: {
            if (subj_type->kind != TYPE_OPTION)
                diag_fatal(pat->loc, "some pattern on non-option type %s", type_name(subj_type));
            Type *inner_type = subj_type->option.inner;
            if (pat->some_pat.inner->kind == PAT_BINDING) {
                scope_add(ctx->scope, pat->some_pat.inner->binding.name, pat->some_pat.inner->binding.name, inner_type, false);
            }
            break;
        }
        case PAT_NONE:
            if (subj_type->kind != TYPE_OPTION)
                diag_fatal(pat->loc, "none pattern on non-option type %s", type_name(subj_type));
            break;
        case PAT_STRUCT: {
            if (subj_type->kind != TYPE_STRUCT)
                diag_fatal(pat->loc, "struct pattern on non-struct type %s", type_name(subj_type));
            check_match_struct_pattern(ctx, pat, subj_type);
            break;
        }
        case PAT_VARIANT: {
            if (subj_type->kind != TYPE_UNION)
                diag_fatal(pat->loc, "variant pattern on non-union type %s", type_name(subj_type));
            /* Find variant */
            bool found = false;
            for (int v = 0; v < subj_type->unio.variant_count; v++) {
                if (subj_type->unio.variants[v].name == pat->variant.variant) {
                    found = true;
                    /* If variant has payload and pattern has binding, add binding */
                    if (pat->variant.payload && subj_type->unio.variants[v].payload) {
                        Type *payload_type = resolve_type(ctx, subj_type->unio.variants[v].payload);
                        if (pat->variant.payload->kind == PAT_BINDING) {
                            scope_add(ctx->scope, pat->variant.payload->binding.name,
                                pat->variant.payload->binding.name, payload_type, false);
                        }
                    }
                    break;
                }
            }
            if (!found)
                diag_fatal(pat->loc, "union '%s' has no variant '%s'",
                    subj_type->unio.name, pat->variant.variant);
            break;
        }
        default:
            diag_fatal(pat->loc, "unsupported pattern kind in match");
        }

        /* Type-check arm body */
        Type *arm_type = check_block(ctx, arm->body, arm->body_count);
        ctx->scope = saved;

        if (!result_type) {
            result_type = arm_type;
            /* Fill recursive return type from first concrete arm (base case) */
            if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_VOID &&
                arm_type->kind != TYPE_VOID) {
                *ctx->recursive_ret = *arm_type;
            }
        } else if (!type_eq(result_type, arm_type)) {
            diag_fatal(arm->loc, "match arms have different types: %s vs %s",
                type_name(result_type), type_name(arm_type));
        }
    }

    e->type = result_type;
    return e->type;
}

static void check_decl_let(CheckCtx *ctx, Decl *d) {
    /* For function declarations, pre-register a partial function type
     * so the body can make recursive calls. */
    const char *lookup_name = d->let.name;
    Symbol *sym = NULL;
    if (ctx->module_symtab) {
        sym = symtab_lookup(ctx->module_symtab, lookup_name);
    }
    if (!sym) {
        sym = symtab_lookup(ctx->symtab, lookup_name);
    }

    Type *recursive_ret = NULL;
    if (d->let.init && d->let.init->kind == EXPR_FUNC && sym && !sym->type) {
        /* Build a partial function type with params known, return type placeholder.
         * Allocate the return type as a mutable cell; after body checking we
         * overwrite it in-place so all references (including recursive call sites)
         * see the resolved return type. */
        Expr *fn = d->let.init;
        int pc = fn->func.param_count;
        Type **ptypes = NULL;
        if (pc > 0)
            ptypes = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)pc);
        for (int i = 0; i < pc; i++)
            ptypes[i] = resolve_type(ctx, fn->func.params[i].type);

        recursive_ret = malloc(sizeof(Type));
        memset(recursive_ret, 0, sizeof(Type));
        recursive_ret->kind = TYPE_VOID;  /* placeholder */

        Type *ft = arena_alloc(ctx->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_types = ptypes;
        ft->func.param_count = pc;
        ft->func.return_type = recursive_ret;
        sym->type = ft;
    }

    Type *saved_recursive_ret = ctx->recursive_ret;
    ctx->recursive_ret = recursive_ret;
    Type *t = check_expr(ctx, d->let.init);
    ctx->recursive_ret = saved_recursive_ret;

    /* If we pre-registered a recursive function type, patch the return type */
    if (recursive_ret) {
        Type *actual_ret = t->kind == TYPE_FUNC ? t->func.return_type : t;
        *recursive_ret = *actual_ret;
    }

    d->let.resolved_type = t;
    if (sym) sym->type = t;
    /* Add to scope so later decls can reference it */
    const char *cg_name = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    scope_add(ctx->scope, d->let.name, cg_name, t, d->let.is_mut);
}

void pass2_check(Program *prog, SymbolTable *symtab) {
    Arena arena;
    arena_init(&arena);

    CheckCtx ctx = {
        .symtab = symtab,
        .scope = scope_new(&arena, NULL),
        .arena = &arena,
        .loop_break_type = NULL,
        .in_for = false,
        .module_symtab = NULL,
        .current_ns = NULL,
        .recursive_ret = NULL,
    };

    /* First pass: type-check all module member decls (including nested submodules) */
    const char *ns_tracker = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            ns_tracker = d->ns.name;
            continue;
        }
        if (d->kind != DECL_MODULE) continue;
        Symbol *mod_sym = symtab_lookup_module(symtab, d->module.name,
            d->module.ns_prefix ? d->module.ns_prefix : ns_tracker);
        if (!mod_sym || !mod_sym->members) continue;
        ctx.current_ns = mod_sym->ns_prefix;
        SymbolTable *saved_mod = ctx.module_symtab;
        ctx.module_symtab = mod_sym->members;
        /* Type-check all let decls in this module, recursing into submodules */
        for (int j = 0; j < d->module.decl_count; j++) {
            Decl *child = d->module.decls[j];
            if (child->kind == DECL_LET) {
                check_decl_let(&ctx, child);
            } else if (child->kind == DECL_MODULE) {
                /* Nested submodule: type-check with submodule's symtab */
                Symbol *sub_sym = symtab_lookup_kind(mod_sym->members, child->module.name, DECL_MODULE);
                if (sub_sym && sub_sym->members) {
                    SymbolTable *saved_inner = ctx.module_symtab;
                    ctx.module_symtab = sub_sym->members;
                    for (int k = 0; k < child->module.decl_count; k++) {
                        if (child->module.decls[k]->kind == DECL_LET) {
                            check_decl_let(&ctx, child->module.decls[k]);
                        }
                    }
                    ctx.module_symtab = saved_inner;
                }
            }
        }
        ctx.module_symtab = saved_mod;
    }

    /* Update imported symbols' types from resolved module members */
    for (int i = 0; i < symtab->count; i++) {
        Symbol *gsym = &symtab->symbols[i];
        /* Skip non-imported symbols (those with no decl or that aren't DECL_LET from a module) */
        if (!gsym->decl) continue;
        if (gsym->kind == DECL_LET && !gsym->type && gsym->decl->let.codegen_name) {
            /* This is likely an imported let that needs its type resolved */
            /* Find it in any module's member table */
            for (int j = 0; j < symtab->count; j++) {
                Symbol *mod = &symtab->symbols[j];
                if (mod->kind != DECL_MODULE || !mod->members) continue;
                for (int k = 0; k < mod->members->count; k++) {
                    Symbol *msym = &mod->members->symbols[k];
                    if (msym->decl == gsym->decl && msym->type) {
                        gsym->type = msym->type;
                        goto next_sym;
                    }
                }
            }
            next_sym:;
        }
    }

    /* Second pass: type-check top-level (non-module) decls */
    ctx.current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            ctx.current_ns = d->ns.name;
            continue;
        }
        if (d->kind == DECL_LET) {
            check_decl_let(&ctx, d);
        }
    }

    /* Don't free arena — types are referenced from AST */
}
