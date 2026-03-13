#include "pass2.h"
#include "diag.h"
#include <string.h>

/* ---- Scope for local bindings ---- */

typedef struct {
    const char *name;
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

static void scope_add(Scope *s, const char *name, Type *type, bool is_mut) {
    LocalBinding b = { name, type, is_mut };
    DA_APPEND(s->locals, s->local_count, s->local_cap, b);
}

static Type *scope_lookup(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = 0; i < sc->local_count; i++) {
            if (sc->locals[i].name == name) return sc->locals[i].type;
        }
    }
    return NULL;
}

/* ---- Type checking ---- */

typedef struct {
    SymbolTable *symtab;
    Scope *scope;
    Arena *arena;
    Type **loop_break_type;  /* non-NULL when inside a loop; points to break value type */
    bool in_for;             /* true when inside a for loop (break value forbidden) */
} CheckCtx;

static Type *check_expr(CheckCtx *ctx, Expr *e);

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
        Type *t = scope_lookup(ctx->scope, e->ident.name);
        if (t) {
            e->type = t;
            return t;
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
        if (!sym->type) diag_fatal(e->loc, "use of '%s' before its type is resolved", e->ident.name);
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
            ptypes[i] = e->func.params[i].type;
            scope_add(inner, e->func.params[i].name, ptypes[i], false);
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
        e->let_expr.let_type = t;
        scope_add(ctx->scope, e->let_expr.let_name, t, e->let_expr.let_is_mut);
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
        check_expr(ctx, e->cast.operand);
        e->type = e->cast.target;
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

        /* If the object is an IDENT referencing a union type, this is variant construction */
        if (e->field.object->kind == EXPR_IDENT) {
            Symbol *sym = symtab_lookup(ctx->symtab, e->field.object->ident.name);
            if (sym && sym->kind == DECL_UNION && sym->type && sym->type->kind == TYPE_UNION) {
                e->type = sym->type;
                return e->type;
            }
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
            scope_add(ctx->scope, e->for_expr.var, var_type, false);
        } else {
            /* Collection iteration: for x in slice */
            if (iter_type->kind == TYPE_SLICE) {
                scope_add(ctx->scope, e->for_expr.var, iter_type->slice.elem, false);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var, type_int64(), false);
                }
            } else if (iter_type->kind == TYPE_STR) {
                scope_add(ctx->scope, e->for_expr.var, type_uint8(), false);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var, type_int64(), false);
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

    default:
        diag_fatal(e->loc, "unsupported expression kind in type checker (kind=%d)", e->kind);
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
            /* Bind the subject value to a name */
            scope_add(ctx->scope, pat->binding.name, subj_type, false);
            break;
        case PAT_INT_LIT:
            if (!type_is_integer(subj_type))
                diag_fatal(pat->loc, "integer pattern on non-integer type %s", type_name(subj_type));
            break;
        case PAT_BOOL_LIT:
            if (!type_eq(subj_type, type_bool()))
                diag_fatal(pat->loc, "bool pattern on non-bool type %s", type_name(subj_type));
            break;
        case PAT_SOME: {
            if (subj_type->kind != TYPE_OPTION)
                diag_fatal(pat->loc, "some pattern on non-option type %s", type_name(subj_type));
            Type *inner_type = subj_type->option.inner;
            if (pat->some_pat.inner->kind == PAT_BINDING) {
                scope_add(ctx->scope, pat->some_pat.inner->binding.name, inner_type, false);
            }
            break;
        }
        case PAT_NONE:
            if (subj_type->kind != TYPE_OPTION)
                diag_fatal(pat->loc, "none pattern on non-option type %s", type_name(subj_type));
            break;
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
                                payload_type, false);
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
        } else if (!type_eq(result_type, arm_type)) {
            diag_fatal(arm->loc, "match arms have different types: %s vs %s",
                type_name(result_type), type_name(arm_type));
        }
    }

    e->type = result_type;
    return e->type;
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
    };

    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        switch (d->kind) {
        case DECL_LET: {
            Type *t = check_expr(&ctx, d->let.init);
            d->let.resolved_type = t;
            Symbol *sym = symtab_lookup(symtab, d->let.name);
            if (sym) sym->type = t;
            /* Add to global scope so later decls can reference it */
            scope_add(ctx.scope, d->let.name, t, d->let.is_mut);
            break;
        }
        default:
            break;
        }
    }

    /* Don't free arena — types are referenced from AST */
}
