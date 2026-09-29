#include "ast.h"

/* Test one child, returning from the enclosing function on a hit. */
#define CHILD(x) do {                                 \
        Expr *child_ = (x);                           \
        if (child_ && pred(child_, ctx)) return true; \
    } while (0)

#define CHILDREN(arr, n) do {                         \
        for (int i_ = 0; i_ < (n); i_++)              \
            CHILD((arr)[i_]);                         \
    } while (0)

bool expr_any_child(Expr *e, ExprPredFn pred, void *ctx) {
    switch (e->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT:
    case EXPR_IDENT:
    case EXPR_CONTINUE:
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
    case EXPR_TYPE_VAR_REF:
    case EXPR_ERROR:
        break;
    case EXPR_BINARY:
        CHILD(e->binary.left);
        CHILD(e->binary.right);
        break;
    case EXPR_UNARY_PREFIX:
        CHILD(e->unary_prefix.operand);
        break;
    case EXPR_UNARY_POSTFIX:
        CHILD(e->unary_postfix.operand);
        break;
    case EXPR_CALL:
        CHILD(e->call.func);
        CHILDREN(e->call.args, e->call.arg_count);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        CHILD(e->field.object);
        break;
    case EXPR_INDEX:
        CHILD(e->index.object);
        CHILD(e->index.index);
        break;
    case EXPR_SLICE:
        CHILD(e->slice.object);
        CHILD(e->slice.lo);
        CHILD(e->slice.hi);
        break;
    case EXPR_CAST:
        CHILD(e->cast.operand);
        break;
    case EXPR_GUARD:
        CHILD(e->guard.body);
        break;
    case EXPR_IF:
        CHILD(e->if_expr.cond);
        CHILD(e->if_expr.then_body);
        CHILD(e->if_expr.else_body);
        break;
    case EXPR_MATCH:
        CHILD(e->match_expr.subject);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            CHILD(arm->guard);
            CHILDREN(arm->body, arm->body_count);
        }
        break;
    case EXPR_LOOP:
        CHILDREN(e->loop_expr.body, e->loop_expr.body_count);
        break;
    case EXPR_FOR:
        CHILD(e->for_expr.iter);
        CHILD(e->for_expr.range_end);
        CHILDREN(e->for_expr.body, e->for_expr.body_count);
        break;
    case EXPR_BREAK:
        CHILD(e->break_expr.value);
        break;
    case EXPR_RETURN:
        CHILD(e->return_expr.value);
        break;
    case EXPR_BLOCK:
        CHILDREN(e->block.stmts, e->block.count);
        break;
    case EXPR_FUNC:
        CHILDREN(e->func.body, e->func.body_count);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            CHILD(e->struct_lit.fields[i].value);
        break;
    case EXPR_TUPLE_LIT:
        CHILDREN(e->tuple_lit.elems, e->tuple_lit.elem_count);
        break;
    case EXPR_ARRAY_LIT:
        CHILD(e->array_lit.size_expr);
        CHILDREN(e->array_lit.elems, e->array_lit.elem_count);
        break;
    case EXPR_SLICE_LIT:
        CHILD(e->slice_lit.ptr_expr);
        CHILD(e->slice_lit.len_expr);
        break;
    case EXPR_ALLOC:
        CHILD(e->alloc_expr.size_expr);
        CHILD(e->alloc_expr.init_expr);
        break;
    case EXPR_FREE:
        CHILD(e->free_expr.operand);
        break;
    case EXPR_BITCAST:
        CHILD(e->bitcast_expr.operand);
        break;
    case EXPR_ENUM_OF:
        CHILD(e->enum_of_expr.operand);
        break;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            CHILD(e->interp_string.segments[i].expr);
        break;
    case EXPR_ASSIGN:
        CHILD(e->assign.target);
        CHILD(e->assign.value);
        break;
    case EXPR_SOME:
        CHILD(e->some_expr.value);
        break;
    case EXPR_OK:
        CHILD(e->ok_expr.value);
        break;
    case EXPR_ERR:
        CHILD(e->err_expr.code);
        break;
    case EXPR_ERROR_NAME:
        CHILD(e->error_name_expr.code);
        break;
    case EXPR_LET:
        CHILD(e->let_expr.let_init);
        break;
    case EXPR_LET_DESTRUCT:
        CHILD(e->let_destruct.init);
        break;
    case EXPR_ASSERT:
        CHILD(e->assert_expr.condition);
        CHILD(e->assert_expr.message);
        break;
    case EXPR_STATIC_ASSERT:
        CHILD(e->static_assert_expr.condition);
        break;
    case EXPR_DEFER:
        CHILD(e->defer_expr.value);
        break;
    case EXPR_IGNORE:
        CHILD(e->ignore_expr.value);
        break;
    case EXPR_ATOMIC_LOAD:
        CHILD(e->atomic_load.ptr);
        break;
    case EXPR_ATOMIC_STORE:
        CHILD(e->atomic_store.ptr);
        CHILD(e->atomic_store.value);
        break;
    }
    return false;
}

#undef CHILD
#undef CHILDREN

typedef struct {
    ExprVisitFn fn;
    void *ctx;
} ForEach;

static bool visit_and_continue(Expr *child, void *for_each) {
    ForEach *f = for_each;
    f->fn(child, f->ctx);
    return false;
}

void expr_for_each_child(Expr *e, ExprVisitFn fn, void *ctx) {
    ForEach f = { fn, ctx };
    expr_any_child(e, visit_and_continue, &f);
}

static void visit_pattern(Pattern *child, PatternVisitFn fn, void *ctx) {
    if (child) fn(child, ctx);
}

void pattern_for_each_child(Pattern *p, PatternVisitFn fn, void *ctx) {
    switch (p->kind) {
    case PAT_WILDCARD:
    case PAT_BINDING:
    case PAT_INT_LIT:
    case PAT_CHAR_LIT:
    case PAT_BOOL_LIT:
    case PAT_STRING_LIT:
    case PAT_NONE:
    case PAT_CONST_PATH:
    case PAT_ERROR:
        break;
    case PAT_SOME:
    case PAT_OK:
    case PAT_ERR:
        visit_pattern(p->some_pat.inner, fn, ctx);
        break;
    case PAT_VARIANT:
        visit_pattern(p->variant.payload, fn, ctx);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < p->struc.field_count; i++)
            visit_pattern(p->struc.fields[i].pattern, fn, ctx);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < p->tuple_pat.pattern_count; i++)
            visit_pattern(p->tuple_pat.patterns[i], fn, ctx);
        break;
    case PAT_OR:
        for (int i = 0; i < p->or_pat.alt_count; i++)
            visit_pattern(p->or_pat.alts[i], fn, ctx);
        break;
    }
}
