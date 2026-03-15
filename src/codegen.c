#include "codegen.h"
#include "monomorph.h"
#include "pass1.h"
#include "diag.h"
#include <inttypes.h>
#include <string.h>

/* Substitution context for monomorphized emission */
typedef struct {
    const char **var_names;
    Type **concrete;
    int count;
} SubstCtx;
static SubstCtx *g_subst = NULL;
static MonoTable *g_mono = NULL;
static Arena *g_arena = NULL;
static InternTable *g_intern = NULL;
static SymbolTable *g_symtab = NULL;

static int indent_level = 0;
static int temp_counter = 0;

static void emit_indent(FILE *out) {
    for (int i = 0; i < indent_level; i++) fprintf(out, "    ");
}

/* Recursively resolve struct/union stubs with concrete type_args into mangled names.
 * Used to fix self-referential fields in monomorphized struct definitions. */
static void resolve_concrete_stubs(Type *t) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_POINTER: resolve_concrete_stubs(t->pointer.pointee); return;
    case TYPE_SLICE:   resolve_concrete_stubs(t->slice.elem); return;
    case TYPE_OPTION:  resolve_concrete_stubs(t->option.inner); return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            resolve_concrete_stubs(t->func.param_types[i]);
        resolve_concrete_stubs(t->func.return_type);
        return;
    case TYPE_STRUCT:
        if (t->struc.type_arg_count > 0 && !type_contains_type_var(t)) {
            /* Only mangle if name isn't already a known mangled entry */
            if (!mono_find(g_mono, t->struc.name)) {
                t->struc.name = mangle_generic_name(g_arena, g_intern,
                    t->struc.name, t->struc.type_args, t->struc.type_arg_count);
            }
            t->struc.type_args = NULL;
            t->struc.type_arg_count = 0;
        }
        for (int i = 0; i < t->struc.field_count; i++)
            resolve_concrete_stubs(t->struc.fields[i].type);
        return;
    case TYPE_UNION:
        if (t->unio.type_arg_count > 0 && !type_contains_type_var(t)) {
            if (!mono_find(g_mono, t->unio.name)) {
                t->unio.name = mangle_generic_name(g_arena, g_intern,
                    t->unio.name, t->unio.type_args, t->unio.type_arg_count);
            }
            t->unio.type_args = NULL;
            t->unio.type_arg_count = 0;
        }
        for (int i = 0; i < t->unio.variant_count; i++)
            resolve_concrete_stubs(t->unio.variants[i].payload);
        return;
    default: return;
    }
}

/* Compute mangled name for a generic struct/union type under g_subst */
static const char *mangle_generic_with_subst(const char *base_name, Type *t) {
    const char **vars = NULL;
    int vc = 0, vcap = 0;
    type_collect_vars(t, &vars, &vc, &vcap);
    Type **concrete_args = arena_alloc(g_arena, sizeof(Type*) * (size_t)vc);
    for (int i = 0; i < vc; i++) {
        /* Look up this type var in g_subst */
        concrete_args[i] = NULL;
        for (int j = 0; j < g_subst->count; j++) {
            if (g_subst->var_names[j] == vars[i]) {
                concrete_args[i] = g_subst->concrete[j];
                break;
            }
        }
        if (!concrete_args[i]) {
            /* Not found — create a type var type */
            concrete_args[i] = type_type_var(g_arena, vars[i]);
        }
    }
    const char *mangled = mangle_generic_name(g_arena, g_intern,
        base_name, concrete_args, vc);
    free(vars);
    return mangled;
}

/* Emit the identifier portion of a function type typedef name */
static void emit_fn_type_suffix(Type *t, FILE *out);

static void emit_type(Type *t, FILE *out) {
    /* Handle type variable substitution during monomorphized emission */
    if (g_subst && t->kind == TYPE_TYPE_VAR) {
        for (int i = 0; i < g_subst->count; i++) {
            if (g_subst->var_names[i] == t->type_var.name) {
                emit_type(g_subst->concrete[i], out);
                return;
            }
        }
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_VOID:    fprintf(out, "void");      break;
    case TYPE_STR:     fprintf(out, "fc_str");    break;
    case TYPE_CSTR:    fprintf(out, "const char*"); break;
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_POINTER:
        emit_type(t->pointer.pointee, out);
        fprintf(out, "*");
        break;
    case TYPE_SLICE:
        fprintf(out, "fc_slice_");
        emit_type(t->slice.elem, out);
        break;
    case TYPE_OPTION:
        if (t->option.inner && t->option.inner->kind == TYPE_POINTER) {
            /* T*? → plain pointer (null = none) */
            emit_type(t->option.inner, out);
        } else {
            fprintf(out, "fc_option_");
            if (t->option.inner) emit_type(t->option.inner, out);
            else fprintf(out, "void");
        }
        break;
    case TYPE_STRUCT:
        if (g_subst && type_contains_type_var(t)) {
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
            return;
        }
        fprintf(out, "%s", t->struc.name);
        break;
    case TYPE_UNION:
        if (g_subst && type_contains_type_var(t)) {
            fprintf(out, "%s", mangle_generic_with_subst(t->unio.name, t));
            return;
        }
        fprintf(out, "%s", t->unio.name);
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_ANY_PTR:
        fprintf(out, "void*");
        break;
    default:
        fprintf(out, "/* TODO: type %d */", t->kind);
        break;
    }
}

/* Emit a C type name suitable for use in identifiers (slice/option typedef names) */
static void emit_type_ident(Type *t, FILE *out) {
    /* Handle type variable substitution */
    if (g_subst && t->kind == TYPE_TYPE_VAR) {
        for (int i = 0; i < g_subst->count; i++) {
            if (g_subst->var_names[i] == t->type_var.name) {
                emit_type_ident(g_subst->concrete[i], out);
                return;
            }
        }
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_STRUCT:
        if (g_subst && type_contains_type_var(t))
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
        else
            fprintf(out, "%s", t->struc.name);
        break;
    case TYPE_UNION:
        if (g_subst && type_contains_type_var(t))
            fprintf(out, "%s", mangle_generic_with_subst(t->unio.name, t));
        else
            fprintf(out, "%s", t->unio.name);
        break;
    case TYPE_SLICE:
        fprintf(out, "fc_slice_");
        emit_type_ident(t->slice.elem, out);
        break;
    case TYPE_POINTER:
        emit_type_ident(t->pointer.pointee, out);
        fprintf(out, "_ptr");
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_OPTION:
        fprintf(out, "fc_option_");
        if (t->option.inner) emit_type_ident(t->option.inner, out);
        else fprintf(out, "void");
        break;
    case TYPE_VOID:
        fprintf(out, "void");
        break;
    default:           fprintf(out, "unknown");   break;
    }
}

/* Emit the identifier suffix for function type typedef names.
   E.g., for (int32, int32) -> bool: "int32_t_int32_t__bool" */
static void emit_fn_type_suffix(Type *t, FILE *out) {
    if (t->func.param_count == 0) {
        fprintf(out, "v");
    } else {
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) fprintf(out, "_");
            emit_type_ident(t->func.param_types[i], out);
        }
    }
    fprintf(out, "__");
    if (t->func.return_type->kind == TYPE_VOID) {
        fprintf(out, "void");
    } else {
        emit_type_ident(t->func.return_type, out);
    }
}

static void emit_expr(Expr *e, FILE *out);

/* Recursively emit condition checks for any pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value.
   has_cond tracks whether "if (" has been emitted. */
static void emit_pat_conditions(Pattern *pat, const char *expr, Type *type, bool *has_cond, FILE *out) {
    switch (pat->kind) {
    case PAT_BINDING:
    case PAT_WILDCARD:
        break;
    case PAT_INT_LIT:
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        fprintf(out, "%s == %" PRId64, expr, pat->int_lit.value);
        break;
    case PAT_BOOL_LIT:
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        fprintf(out, "%s == %s", expr, pat->bool_lit.value ? "true" : "false");
        break;
    case PAT_CHAR_LIT:
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        fprintf(out, "%s == '\\x%02x'", expr, pat->char_lit.value);
        break;
    case PAT_SOME: {
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        bool is_ptr = type->option.inner && type->option.inner->kind == TYPE_POINTER;
        if (is_ptr) fprintf(out, "%s != NULL", expr);
        else fprintf(out, "%s.has_value", expr);
        if (pat->some_pat.inner) {
            char inner_expr[256];
            if (is_ptr) snprintf(inner_expr, sizeof(inner_expr), "%s", expr);
            else snprintf(inner_expr, sizeof(inner_expr), "%s.value", expr);
            emit_pat_conditions(pat->some_pat.inner, inner_expr, type->option.inner, has_cond, out);
        }
        break;
    }
    case PAT_NONE:
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        if (type->option.inner && type->option.inner->kind == TYPE_POINTER)
            fprintf(out, "%s == NULL", expr);
        else
            fprintf(out, "!%s.has_value", expr);
        break;
    case PAT_VARIANT: {
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        const char *uname = type->unio.name;
        if (g_subst && type_contains_type_var(type)) {
            uname = mangle_generic_with_subst(uname, type);
        }
        fprintf(out, "%s.tag == %s_tag_%s", expr, uname, pat->variant.variant);
        if (pat->variant.payload) {
            char payload_expr[256];
            snprintf(payload_expr, sizeof(payload_expr), "%s.%s", expr, pat->variant.variant);
            Type *payload_type = NULL;
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    payload_type = type->unio.variants[v].payload;
                    break;
                }
            }
            if (payload_type)
                emit_pat_conditions(pat->variant.payload, payload_expr, payload_type, has_cond, out);
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.%s", expr, pat->struc.fields[fi].name);
            emit_pat_conditions(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, has_cond, out);
        }
        break;
    default:
        break;
    }
}

/* Recursively emit variable declarations for all bindings in a pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value. */
static void emit_pat_bindings(Pattern *pat, const char *expr, Type *type, FILE *out) {
    switch (pat->kind) {
    case PAT_BINDING:
        emit_indent(out);
        emit_type(type, out);
        fprintf(out, " %s = %s;\n", pat->binding.name, expr);
        emit_indent(out);
        fprintf(out, "(void)%s;\n", pat->binding.name);
        break;
    case PAT_WILDCARD:
    case PAT_INT_LIT:
    case PAT_BOOL_LIT:
    case PAT_CHAR_LIT:
    case PAT_NONE:
    case PAT_STRING_LIT:
        break;
    case PAT_SOME: {
        if (pat->some_pat.inner) {
            Type *inner_type = type->option.inner;
            char inner_expr[256];
            if (inner_type && inner_type->kind == TYPE_POINTER)
                snprintf(inner_expr, sizeof(inner_expr), "%s", expr);
            else
                snprintf(inner_expr, sizeof(inner_expr), "%s.value", expr);
            emit_pat_bindings(pat->some_pat.inner, inner_expr, inner_type, out);
        }
        break;
    }
    case PAT_VARIANT: {
        if (pat->variant.payload) {
            Type *payload_type = NULL;
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    payload_type = type->unio.variants[v].payload;
                    break;
                }
            }
            if (payload_type) {
                char payload_expr[256];
                snprintf(payload_expr, sizeof(payload_expr), "%s.%s", expr, pat->variant.variant);
                emit_pat_bindings(pat->variant.payload, payload_expr, payload_type, out);
            }
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.%s", expr, pat->struc.fields[fi].name);
            emit_pat_bindings(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, out);
        }
        break;
    }
}

static void emit_block_stmts(Expr **stmts, int count, FILE *out, bool as_return) {
    for (int i = 0; i < count; i++) {
        Expr *s = stmts[i];
        bool is_last = (i == count - 1);

        emit_indent(out);

        if (s->kind == EXPR_LET) {
            const char *vname = s->let_expr.codegen_name ? s->let_expr.codegen_name : s->let_expr.let_name;
            emit_type(s->let_expr.let_type, out);
            fprintf(out, " %s = ", vname);
            emit_expr(s->let_expr.let_init, out);
            fprintf(out, ";\n");
            /* A let binding evaluates to void, so if it's the last stmt in a block
             * the variable is unused — suppress the warning. */
            if (is_last) {
                emit_indent(out);
                fprintf(out, "(void)%s;\n", vname);
            }
        } else if (s->kind == EXPR_LET_DESTRUCT) {
            /* Emit: struct_type _ds_N = rhs; then recursively emit field bindings */
            emit_type(s->let_destruct.init_type, out);
            fprintf(out, " %s = ", s->let_destruct.tmp_name);
            emit_expr(s->let_destruct.init, out);
            fprintf(out, ";\n");
            emit_pat_bindings(s->let_destruct.pattern, s->let_destruct.tmp_name, s->let_destruct.init_type, out);
            if (is_last) {
                emit_indent(out);
                fprintf(out, "(void)%s;\n", s->let_destruct.tmp_name);
            }
        } else if (s->kind == EXPR_RETURN) {
            if (s->return_expr.value) {
                fprintf(out, "return ");
                emit_expr(s->return_expr.value, out);
                fprintf(out, ";\n");
            } else {
                fprintf(out, "return;\n");
            }
        } else if (s->kind == EXPR_ASSIGN) {
            emit_expr(s, out);
            fprintf(out, ";\n");
        } else if (s->kind == EXPR_BREAK) {
            if (s->break_expr.value) {
                /* break with value — assign to loop result temp, then break */
                fprintf(out, "_loop_result = ");
                emit_expr(s->break_expr.value, out);
                fprintf(out, "; break;\n");
            } else {
                fprintf(out, "break;\n");
            }
        } else if (s->kind == EXPR_CONTINUE) {
            fprintf(out, "continue;\n");
        } else if (s->kind == EXPR_IF && (!s->type || s->type->kind == TYPE_VOID)) {
            /* void-typed if → emit as C if statement */
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_FOR) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_LOOP && (!s->type || s->type->kind == TYPE_VOID)) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (is_last && as_return && s->type && s->type->kind != TYPE_VOID) {
            fprintf(out, "return ");
            emit_expr(s, out);
            fprintf(out, ";\n");
        } else {
            emit_expr(s, out);
            fprintf(out, ";\n");
        }
    }
}

static void emit_if_stmt(Expr *e, FILE *out) {
    /* Emit if as a C statement (not expression) */
    fprintf(out, "if (");
    emit_expr(e->if_expr.cond, out);
    fprintf(out, ") {\n");
    indent_level++;
    if (e->if_expr.then_body->kind == EXPR_BLOCK) {
        emit_block_stmts(e->if_expr.then_body->block.stmts,
            e->if_expr.then_body->block.count, out, false);
    } else {
        emit_indent(out);
        emit_expr(e->if_expr.then_body, out);
        fprintf(out, ";\n");
    }
    indent_level--;
    emit_indent(out);
    fprintf(out, "}");
    if (e->if_expr.else_body) {
        if (e->if_expr.else_body->kind == EXPR_IF &&
            (!e->if_expr.else_body->type || e->if_expr.else_body->type->kind == TYPE_VOID)) {
            fprintf(out, " else ");
            emit_if_stmt(e->if_expr.else_body, out);
        } else {
            fprintf(out, " else {\n");
            indent_level++;
            if (e->if_expr.else_body->kind == EXPR_BLOCK) {
                emit_block_stmts(e->if_expr.else_body->block.stmts,
                    e->if_expr.else_body->block.count, out, false);
            } else {
                emit_indent(out);
                emit_expr(e->if_expr.else_body, out);
                fprintf(out, ";\n");
            }
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
    }
}

static void emit_expr(Expr *e, FILE *out) {
    switch (e->kind) {
    case EXPR_INT_LIT:
        if (e->int_lit.lit_type->kind == TYPE_INT64)
            fprintf(out, "INT64_C(%" PRId64 ")", e->int_lit.value);
        else if (e->int_lit.lit_type->kind == TYPE_UINT64)
            fprintf(out, "UINT64_C(%" PRId64 ")", e->int_lit.value);
        else
            fprintf(out, "%" PRId64, e->int_lit.value);
        break;

    case EXPR_FLOAT_LIT: {
        /* %g may strip the decimal point (e.g., 0.0 → "0"), which makes
         * "0f" invalid C. Ensure there's always a decimal point. */
        char fbuf[64];
        snprintf(fbuf, sizeof(fbuf), "%g", e->float_lit.value);
        bool has_dot = (strchr(fbuf, '.') || strchr(fbuf, 'e') || strchr(fbuf, 'E'));
        if (e->float_lit.lit_type->kind == TYPE_FLOAT32) {
            if (has_dot) fprintf(out, "%sf", fbuf);
            else fprintf(out, "%s.0f", fbuf);
        } else {
            if (has_dot) fprintf(out, "%s", fbuf);
            else fprintf(out, "%s.0", fbuf);
        }
        break;
    }

    case EXPR_BOOL_LIT:
        fprintf(out, "%s", e->bool_lit.value ? "true" : "false");
        break;

    case EXPR_CHAR_LIT:
        fprintf(out, "'\\x%02x'", e->char_lit.value);
        break;

    case EXPR_STRING_LIT:
        fprintf(out, "((fc_str){(uint8_t*)\"%.*s\", %d})",
            e->string_lit.length, e->string_lit.value, e->string_lit.length);
        break;

    case EXPR_CSTRING_LIT:
        fprintf(out, "\"%.*s\"", e->cstring_lit.length, e->cstring_lit.value);
        break;

    case EXPR_IDENT:
        if (e->type && e->type->kind == TYPE_FUNC &&
            !e->ident.is_local) {
            /* Top-level function used as a value — wrap in fat pointer */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        } else {
            fprintf(out, "%s", e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        }
        break;

    case EXPR_BINARY: {
        const char *op_str;
        switch (e->binary.op) {
        case TOK_PLUS:     op_str = "+";  break;
        case TOK_MINUS:    op_str = "-";  break;
        case TOK_STAR:     op_str = "*";  break;
        case TOK_SLASH:    op_str = "/";  break;
        case TOK_PERCENT:  op_str = "%"; break;
        case TOK_EQEQ:    op_str = "=="; break;
        case TOK_BANGEQ:  op_str = "!="; break;
        case TOK_LT:      op_str = "<";  break;
        case TOK_GT:      op_str = ">";  break;
        case TOK_LTEQ:    op_str = "<="; break;
        case TOK_GTEQ:    op_str = ">="; break;
        case TOK_AMPAMP:  op_str = "&&"; break;
        case TOK_PIPEPIPE: op_str = "||"; break;
        case TOK_AMP:     op_str = "&";  break;
        case TOK_PIPE:    op_str = "|";  break;
        case TOK_CARET:   op_str = "^";  break;
        case TOK_LTLT:    op_str = "<<"; break;
        case TOK_GTGT:    op_str = ">>"; break;
        default: op_str = "?"; break;
        }
        fprintf(out, "(");
        emit_expr(e->binary.left, out);
        fprintf(out, " %s ", op_str);
        emit_expr(e->binary.right, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_UNARY_PREFIX: {
        const char *op_str;
        switch (e->unary_prefix.op) {
        case TOK_MINUS: op_str = "-"; break;
        case TOK_BANG:  op_str = "!"; break;
        case TOK_TILDE: op_str = "~"; break;
        case TOK_AMP:   op_str = "&"; break;
        case TOK_STAR:  op_str = "*"; break;
        default: op_str = "?"; break;
        }
        fprintf(out, "(%s", op_str);
        emit_expr(e->unary_prefix.operand, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_UNARY_POSTFIX: {
        if (e->unary_postfix.op == TOK_BANG) {
            /* Option unwrap: check has_value, abort if none */
            Type *opt_type = e->unary_postfix.operand->type;
            if (opt_type && opt_type->kind == TYPE_OPTION &&
                opt_type->option.inner && opt_type->option.inner->kind == TYPE_POINTER) {
                /* T*? → plain pointer, unwrap = null check */
                fprintf(out, "({ ");
                emit_type(opt_type->option.inner, out);
                int tid = temp_counter++;
                fprintf(out, " _uw%d = ", tid);
                emit_expr(e->unary_postfix.operand, out);
                fprintf(out, "; if (!_uw%d) abort(); _uw%d; })", tid, tid);
            } else {
                /* Non-pointer option: check .has_value */
                fprintf(out, "({ ");
                emit_type(opt_type, out);
                int tid = temp_counter++;
                fprintf(out, " _uw%d = ", tid);
                emit_expr(e->unary_postfix.operand, out);
                fprintf(out, "; if (!_uw%d.has_value) abort(); _uw%d.value; })", tid, tid);
            }
        }
        break;
    }

    case EXPR_CALL: {
        /* Check if this is a variant constructor: type is union and func is EXPR_FIELD */
        if (e->type && e->type->kind == TYPE_UNION &&
            e->call.func->kind == EXPR_FIELD) {
            const char *union_name = e->type->unio.name;
            /* Under substitution, compute mangled name for generic unions */
            if (g_subst && type_contains_type_var(e->type)) {
                union_name = mangle_generic_with_subst(union_name, e->type);
            }
            const char *variant_name = e->call.func->field.name;
            fprintf(out, "(%s){ .tag = %s_tag_%s, .%s = ",
                union_name, union_name, variant_name, variant_name);
            emit_expr(e->call.args[0], out);
            fprintf(out, " }");
            break;
        }

        if (e->call.is_indirect) {
            /* Indirect call through fat pointer */
            int tid = temp_counter++;
            Type *ft = e->call.func->type;
            fprintf(out, "({ ");
            emit_type(ft, out);
            fprintf(out, " _cf%d = ", tid);
            emit_expr(e->call.func, out);
            fprintf(out, "; _cf%d.fn_ptr(", tid);
            for (int i = 0; i < e->call.arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(e->call.args[i], out);
            }
            if (e->call.arg_count > 0) fprintf(out, ", ");
            fprintf(out, "_cf%d.ctx); })", tid);
        } else {
            /* Direct call — emit function name directly (not via emit_expr
               to avoid fat-pointer wrapping) and append NULL for _ctx */
            Expr *callee = e->call.func;
            const char *fn_name = e->call.mangled_name; /* monomorphized name if generic */

            /* Resolve deferred generic call under substitution context */
            if (!fn_name && g_subst && e->call.type_arg_count > 0) {
                Type **concrete_args = malloc(sizeof(Type*) * (size_t)e->call.type_arg_count);
                for (int i = 0; i < e->call.type_arg_count; i++) {
                    concrete_args[i] = type_substitute(g_arena, e->call.type_args[i],
                        g_subst->var_names, g_subst->concrete, g_subst->count);
                }
                /* Find the callee symbol and its base name for mangling */
                const char *base_name = NULL;
                Symbol *callee_sym = NULL;
                if (callee->kind == EXPR_IDENT) {
                    callee_sym = symtab_lookup(g_symtab, callee->ident.name);
                } else if (callee->kind == EXPR_FIELD && callee->field.object->kind == EXPR_IDENT) {
                    Symbol *mod = symtab_lookup_module(g_symtab, callee->field.object->ident.name, NULL);
                    if (!mod) mod = symtab_lookup_kind(g_symtab, callee->field.object->ident.name, DECL_MODULE);
                    if (mod && mod->members)
                        callee_sym = symtab_lookup(mod->members, callee->field.name);
                }
                if (callee_sym) {
                    base_name = (callee_sym->decl && callee_sym->decl->kind == DECL_LET
                                 && callee_sym->decl->let.codegen_name)
                                ? callee_sym->decl->let.codegen_name : callee_sym->name;
                }
                if (base_name && callee_sym) {
                    fn_name = mono_register(g_mono, g_arena, g_intern,
                        base_name, NULL, concrete_args, e->call.type_arg_count,
                        callee_sym->decl, DECL_LET,
                        callee_sym->type_params, callee_sym->type_param_count);
                }
                free(concrete_args);
            }

            if (!fn_name) {
                if (callee->kind == EXPR_IDENT) {
                    fn_name = callee->ident.codegen_name
                        ? callee->ident.codegen_name : callee->ident.name;
                } else if (callee->kind == EXPR_FIELD && callee->field.codegen_name) {
                    fn_name = callee->field.codegen_name;
                }
            }

            if (fn_name) {
                fprintf(out, "%s(", fn_name);
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    emit_expr(e->call.args[i], out);
                }
                if (e->call.arg_count > 0) fprintf(out, ", ");
                fprintf(out, "NULL)");
            } else {
                /* Fallback: emit normally (e.g., extern functions) */
                emit_expr(e->call.func, out);
                fprintf(out, "(");
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    emit_expr(e->call.args[i], out);
                }
                fprintf(out, ")");
            }
        }
        break;
    }

    case EXPR_IF: {
        if (e->type && e->type->kind == TYPE_VOID) {
            /* Void-typed if → C if statement */
            emit_if_stmt(e, out);
        } else if (e->if_expr.else_body && e->type && e->type->kind != TYPE_VOID) {
            /* Expression if/then/else → ternary */
            fprintf(out, "(");
            emit_expr(e->if_expr.cond, out);
            fprintf(out, " ? ");
            emit_expr(e->if_expr.then_body, out);
            fprintf(out, " : ");
            emit_expr(e->if_expr.else_body, out);
            fprintf(out, ")");
        } else {
            /* Fallback: statement expression */
            fprintf(out, "({");
            indent_level++;
            fprintf(out, "\n");
            emit_indent(out);
            fprintf(out, "if (");
            emit_expr(e->if_expr.cond, out);
            fprintf(out, ") {\n");
            indent_level++;
            if (e->if_expr.then_body->kind == EXPR_BLOCK) {
                emit_block_stmts(e->if_expr.then_body->block.stmts,
                    e->if_expr.then_body->block.count, out, false);
            } else {
                emit_indent(out);
                emit_expr(e->if_expr.then_body, out);
                fprintf(out, ";\n");
            }
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
            if (e->if_expr.else_body) {
                fprintf(out, " else {\n");
                indent_level++;
                if (e->if_expr.else_body->kind == EXPR_BLOCK) {
                    emit_block_stmts(e->if_expr.else_body->block.stmts,
                        e->if_expr.else_body->block.count, out, false);
                } else {
                    emit_indent(out);
                    emit_expr(e->if_expr.else_body, out);
                    fprintf(out, ";\n");
                }
                indent_level--;
                emit_indent(out);
                fprintf(out, "}");
            }
            fprintf(out, "\n");
            indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        }
        break;
    }

    case EXPR_BLOCK: {
        /* Statement expression */
        fprintf(out, "({\n");
        indent_level++;
        emit_block_stmts(e->block.stmts, e->block.count, out, false);
        indent_level--;
        emit_indent(out);
        fprintf(out, "})");
        break;
    }

    case EXPR_CAST: {
        fprintf(out, "((");
        emit_type(e->cast.target, out);
        fprintf(out, ")");
        emit_expr(e->cast.operand, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_FIELD: {
        /* Module member access: emit mangled name directly */
        if (e->field.codegen_name) {
            if (e->type && e->type->kind == TYPE_FUNC) {
                /* Module function used as a value — wrap in fat pointer */
                fprintf(out, "(");
                emit_type(e->type, out);
                fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                    e->field.codegen_name);
            } else {
                fprintf(out, "%s", e->field.codegen_name);
            }
            break;
        }
        /* No-payload variant constructor: color.green → (color){ .tag = color_tag_green } */
        if (e->type && e->type->kind == TYPE_UNION) {
            const char *union_name = e->type->unio.name;
            if (g_subst && type_contains_type_var(e->type)) {
                union_name = mangle_generic_with_subst(union_name, e->type);
            }
            fprintf(out, "(%s){ .tag = %s_tag_%s }",
                union_name, union_name, e->field.name);
            break;
        }
        emit_expr(e->field.object, out);
        fprintf(out, ".%s", e->field.name);
        break;
    }

    case EXPR_DEREF_FIELD: {
        emit_expr(e->field.object, out);
        fprintf(out, "->%s", e->field.name);
        break;
    }

    case EXPR_INDEX: {
        /* Bounds check for slices */
        Type *obj_type = e->index.object->type;
        if (obj_type && (obj_type->kind == TYPE_SLICE || obj_type->kind == TYPE_STR)) {
            int tid = temp_counter++;
            fprintf(out, "({ ");
            emit_type(obj_type, out);
            fprintf(out, " _s%d = ", tid);
            emit_expr(e->index.object, out);
            fprintf(out, "; int64_t _i%d = (int64_t)", tid);
            emit_expr(e->index.index, out);
            fprintf(out, "; if (_i%d < 0 || _i%d >= _s%d.len) abort(); _s%d.ptr[_i%d]; })",
                tid, tid, tid, tid, tid);
        } else {
            /* Pointer indexing — no bounds check */
            emit_expr(e->index.object, out);
            fprintf(out, "[");
            emit_expr(e->index.index, out);
            fprintf(out, "]");
        }
        break;
    }

    case EXPR_SLICE: {
        /* Subslice: s[lo..hi] */
        int tid = temp_counter++;
        Type *obj_type = e->slice.object->type;
        fprintf(out, "({ ");
        emit_type(obj_type, out);
        fprintf(out, " _s%d = ", tid);
        emit_expr(e->slice.object, out);
        fprintf(out, "; int64_t _lo%d = ", tid);
        if (e->slice.lo) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.lo, out);
        } else {
            fprintf(out, "0");
        }
        fprintf(out, "; int64_t _hi%d = ", tid);
        if (e->slice.hi) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.hi, out);
        } else {
            fprintf(out, "_s%d.len", tid);
        }
        fprintf(out, "; if (_lo%d < 0 || _hi%d > _s%d.len || _lo%d > _hi%d) abort(); ",
            tid, tid, tid, tid, tid);
        fprintf(out, "(");
        emit_type(obj_type, out);
        fprintf(out, "){ .ptr = _s%d.ptr + _lo%d, .len = _hi%d - _lo%d }; })",
            tid, tid, tid, tid);
        break;
    }

    case EXPR_SOME: {
        Type *opt_type = e->type;
        if (opt_type && opt_type->kind == TYPE_OPTION &&
            opt_type->option.inner && opt_type->option.inner->kind == TYPE_POINTER) {
            /* T*? → plain pointer, some(x) = x */
            emit_expr(e->some_expr.value, out);
        } else {
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .value = ");
            emit_expr(e->some_expr.value, out);
            fprintf(out, ", .has_value = true }");
        }
        break;
    }

    case EXPR_NONE: {
        Type *opt_type = e->type;
        if (opt_type && opt_type->kind == TYPE_OPTION &&
            opt_type->option.inner && opt_type->option.inner->kind == TYPE_POINTER) {
            /* T*? → plain pointer, none = NULL */
            fprintf(out, "NULL");
        } else {
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }");
        }
        break;
    }

    case EXPR_ARRAY_LIT: {
        /* Stack array literal → C array + slice struct */
        int tid = temp_counter++;
        fprintf(out, "({ ");
        emit_type(e->array_lit.elem_type, out);
        fprintf(out, " _arr%d[", tid);
        emit_expr(e->array_lit.size_expr, out);
        fprintf(out, "]");
        if (e->array_lit.elem_count == 0) {
            fprintf(out, " = {0}");
        } else {
            fprintf(out, " = { ");
            for (int i = 0; i < e->array_lit.elem_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(e->array_lit.elems[i], out);
            }
            fprintf(out, " }");
        }
        fprintf(out, "; (");
        emit_type(e->type, out);
        fprintf(out, "){ .ptr = _arr%d, .len = ", tid);
        emit_expr(e->array_lit.size_expr, out);
        fprintf(out, " }; })");
        break;
    }

    case EXPR_STRUCT_LIT: {
        /* Use the resolved type name (handles mangled module types) */
        const char *sname = (e->type && e->type->kind == TYPE_STRUCT) ?
            e->type->struc.name : e->struct_lit.type_name;
        /* Under substitution, compute mangled name for generic structs */
        if (g_subst && e->type && e->type->kind == TYPE_STRUCT &&
            type_contains_type_var(e->type)) {
            sname = mangle_generic_with_subst(sname, e->type);
            /* Register the mono instance for this struct if needed */
            Symbol *struct_sym = symtab_lookup(g_symtab, e->struct_lit.type_name);
            if (struct_sym && struct_sym->is_generic) {
                const char **vars = NULL;
                int vc = 0, vcap = 0;
                type_collect_vars(e->type, &vars, &vc, &vcap);
                Type **concrete_args = arena_alloc(g_arena, sizeof(Type*) * (size_t)vc);
                for (int k = 0; k < vc; k++) {
                    concrete_args[k] = NULL;
                    for (int j = 0; j < g_subst->count; j++) {
                        if (g_subst->var_names[j] == vars[k]) {
                            concrete_args[k] = g_subst->concrete[j];
                            break;
                        }
                    }
                    if (!concrete_args[k]) concrete_args[k] = type_type_var(g_arena, vars[k]);
                }
                mono_register(g_mono, g_arena, g_intern,
                    struct_sym->name, struct_sym->ns_prefix,
                    concrete_args, vc, struct_sym->decl,
                    DECL_STRUCT, struct_sym->type_params, struct_sym->type_param_count);
                /* Set concrete_type on the instance */
                MonoInstance *mi = mono_find(g_mono, sname);
                if (mi && !mi->concrete_type) {
                    Type *ct = type_substitute(g_arena, struct_sym->type,
                        struct_sym->type_params, concrete_args,
                        struct_sym->type_param_count < vc ? struct_sym->type_param_count : vc);
                    if (ct == struct_sym->type) {
                        Type *cp = arena_alloc(g_arena, sizeof(Type));
                        *cp = *ct;
                        ct = cp;
                    }
                    ct->struc.name = sname;
                    resolve_concrete_stubs(ct);
                    mi->concrete_type = ct;
                }
                free(vars);
            }
        }
        fprintf(out, "(%s){ ", sname);
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, ".%s = ", e->struct_lit.fields[i].name);
            emit_expr(e->struct_lit.fields[i].value, out);
        }
        fprintf(out, " }");
        break;
    }

    case EXPR_LOOP: {
        if (e->type && e->type->kind != TYPE_VOID) {
            /* Loop as expression: produces value via break */
            fprintf(out, "({\n");
            indent_level++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " _loop_result;\n");
            emit_indent(out);
            fprintf(out, "while (1) {\n");
            indent_level++;
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false);
            indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            emit_indent(out);
            fprintf(out, "_loop_result;\n");
            indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        } else {
            /* Void loop */
            fprintf(out, "while (1) {\n");
            indent_level++;
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false);
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
        break;
    }

    case EXPR_FOR: {
        if (e->for_expr.range_end) {
            /* Range iteration: for i in lo..hi */
            Type *var_type = e->for_expr.iter->type;
            fprintf(out, "for (");
            emit_type(var_type, out);
            fprintf(out, " %s = ", e->for_expr.var);
            emit_expr(e->for_expr.iter, out);
            fprintf(out, "; %s < ", e->for_expr.var);
            emit_expr(e->for_expr.range_end, out);
            fprintf(out, "; %s++) {\n", e->for_expr.var);
        } else {
            /* Collection iteration */
            int tid = temp_counter++;
            Type *iter_type = e->for_expr.iter->type;

            fprintf(out, "for (int64_t _fi%d = 0; _fi%d < ", tid, tid);
            emit_expr(e->for_expr.iter, out);
            fprintf(out, ".len; _fi%d++) {\n", tid);
            indent_level++;

            /* Element binding */
            emit_indent(out);
            Type *elem_type = (iter_type->kind == TYPE_STR) ? type_uint8() : iter_type->slice.elem;
            emit_type(elem_type, out);
            fprintf(out, " %s = ", e->for_expr.var);
            emit_expr(e->for_expr.iter, out);
            fprintf(out, ".ptr[_fi%d];\n", tid);

            /* Index binding if present */
            if (e->for_expr.index_var) {
                emit_indent(out);
                fprintf(out, "int64_t %s = _fi%d;\n", e->for_expr.index_var, tid);
            }

            /* Body (already indented by indent_level++) */
            emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false);
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
            break;
        }

        /* Body for range iteration */
        indent_level++;
        emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false);
        indent_level--;
        emit_indent(out);
        fprintf(out, "}");
        break;
    }

    case EXPR_MATCH: {
        /* Emit as statement expression with if-else chain */
        bool match_is_void = (e->type && e->type->kind == TYPE_VOID);
        fprintf(out, "({\n");
        indent_level++;

        /* Emit subject into a temp variable */
        int subj_id = temp_counter++;
        emit_indent(out);
        emit_type(e->match_expr.subject->type, out);
        fprintf(out, " _subj%d = ", subj_id);
        emit_expr(e->match_expr.subject, out);
        fprintf(out, ";\n");
        emit_indent(out);
        fprintf(out, "(void)_subj%d;\n", subj_id);

        /* Emit result variable (skip for void matches) */
        int res_id = -1;
        if (!match_is_void) {
            res_id = temp_counter++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " _match%d;\n", res_id);
        }

        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            Pattern *pat = arm->pattern;
            char subj_expr[64];
            snprintf(subj_expr, sizeof(subj_expr), "_subj%d", subj_id);
            emit_indent(out);

            if (i > 0) fprintf(out, "else ");

            /* Emit conditions */
            bool has_cond = false;
            emit_pat_conditions(pat, subj_expr, e->match_expr.subject->type, &has_cond, out);
            if (has_cond) fprintf(out, ") {\n");
            else fprintf(out, "{\n");

            indent_level++;

            /* Emit bindings */
            emit_pat_bindings(pat, subj_expr, e->match_expr.subject->type, out);

            /* Emit arm body */
            if (match_is_void) {
                emit_block_stmts(arm->body, arm->body_count, out, false);
            } else if (arm->body_count == 1) {
                emit_indent(out);
                fprintf(out, "_match%d = ", res_id);
                emit_expr(arm->body[0], out);
                fprintf(out, ";\n");
            } else {
                /* Multiple statements — emit all, last is the value */
                for (int s = 0; s < arm->body_count; s++) {
                    emit_indent(out);
                    if (s == arm->body_count - 1) {
                        fprintf(out, "_match%d = ", res_id);
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                    } else {
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                    }
                }
            }

            indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
        }

        if (!match_is_void) {
            emit_indent(out);
            fprintf(out, "_match%d;\n", res_id);
        }

        indent_level--;
        emit_indent(out);
        fprintf(out, "})");
        break;
    }

    case EXPR_BREAK:
        if (e->break_expr.value) {
            fprintf(out, "_loop_result = ");
            emit_expr(e->break_expr.value, out);
            fprintf(out, "; break");
        } else {
            fprintf(out, "break");
        }
        break;

    case EXPR_CONTINUE:
        fprintf(out, "continue");
        break;

    case EXPR_RETURN:
        if (e->return_expr.value) {
            fprintf(out, "return ");
            emit_expr(e->return_expr.value, out);
        } else {
            fprintf(out, "return");
        }
        break;

    case EXPR_SIZEOF: {
        fprintf(out, "(int64_t)sizeof(");
        emit_type(e->sizeof_expr.target, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_DEFAULT: {
        Type *t = e->default_expr.target;
        switch (t->kind) {
        case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_CHAR:
            fprintf(out, "0");
            break;
        case TYPE_FLOAT32:
            fprintf(out, "0.0f");
            break;
        case TYPE_FLOAT64:
            fprintf(out, "0.0");
            break;
        case TYPE_BOOL:
            fprintf(out, "false");
            break;
        case TYPE_POINTER:
        case TYPE_ANY_PTR:
        case TYPE_CSTR:
            fprintf(out, "NULL");
            break;
        case TYPE_OPTION:
            if (t->option.inner && t->option.inner->kind == TYPE_POINTER)
                fprintf(out, "NULL");
            else {
                fprintf(out, "(");
                emit_type(t, out);
                fprintf(out, "){0}");
            }
            break;
        default:
            /* Structs, unions, slices — compound literal with {0} */
            fprintf(out, "(");
            emit_type(t, out);
            fprintf(out, "){0}");
            break;
        }
        break;
    }

    case EXPR_FREE: {
        Type *ot = e->free_expr.operand->type;
        if (ot && (ot->kind == TYPE_SLICE || ot->kind == TYPE_STR)) {
            fprintf(out, "free((");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ").ptr)");
        } else {
            fprintf(out, "free(");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ")");
        }
        break;
    }

    case EXPR_ALLOC: {
        if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr) {
            /* alloc(T[N]) → T[]? */
            int tid = temp_counter++;
            fprintf(out, "({ int64_t _asz%d = (int64_t)", tid);
            emit_expr(e->alloc_expr.size_expr, out);
            fprintf(out, "; ");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "* _aptr%d = (", tid);
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)calloc((size_t)_asz%d, sizeof(", tid);
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, ")); _aptr%d ? (", tid);
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            /* inner slice type */
            Type *slice_type = e->type->option.inner;
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = _aptr%d, .len = _asz%d }, .has_value = true } : (", tid, tid);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
        } else if (e->alloc_expr.alloc_type) {
            /* alloc(T) → T*? (bare pointer, calloc returns NULL on failure) */
            fprintf(out, "(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)calloc(1, sizeof(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "))");
        } else {
            /* alloc(expr) → T*? with initialization */
            int tid = temp_counter++;
            Type *val_type = e->alloc_expr.init_expr->type;
            fprintf(out, "({ ");
            emit_type(val_type, out);
            fprintf(out, "* _aptr%d = (", tid);
            emit_type(val_type, out);
            fprintf(out, "*)calloc(1, sizeof(");
            emit_type(val_type, out);
            fprintf(out, ")); if (_aptr%d) *_aptr%d = ", tid, tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; _aptr%d; })", tid);
        }
        break;
    }

    case EXPR_ASSIGN: {
        /* Special case: assignment to slice/str element needs bounds check inside */
        Expr *target = e->assign.target;
        if (target->kind == EXPR_INDEX) {
            Type *obj_type = target->index.object->type;
            if (obj_type && (obj_type->kind == TYPE_SLICE || obj_type->kind == TYPE_STR)) {
                int tid = temp_counter++;
                fprintf(out, "({ ");
                emit_type(obj_type, out);
                fprintf(out, " _s%d = ", tid);
                emit_expr(target->index.object, out);
                fprintf(out, "; int64_t _i%d = (int64_t)", tid);
                emit_expr(target->index.index, out);
                fprintf(out, "; if (_i%d < 0 || _i%d >= _s%d.len) abort(); _s%d.ptr[_i%d] = ",
                    tid, tid, tid, tid, tid);
                emit_expr(e->assign.value, out);
                fprintf(out, "; })");
                break;
            }
        }
        emit_expr(e->assign.target, out);
        fprintf(out, " = ");
        emit_expr(e->assign.value, out);
        break;
    }

    case EXPR_LET: {
        const char *vname = e->let_expr.codegen_name ? e->let_expr.codegen_name : e->let_expr.let_name;
        emit_type(e->let_expr.let_type, out);
        fprintf(out, " %s = ", vname);
        emit_expr(e->let_expr.let_init, out);
        break;
    }

    case EXPR_FUNC: {
        /* Lambda in expression position — emit fat pointer */
        if (e->func.capture_count > 0) {
            /* Capturing lambda: use compound literal for context (block-scope lifetime) */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = &(_ctx_%s){ ",
                e->func.lifted_name, e->func.lifted_name);
            for (int i = 0; i < e->func.capture_count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, ".%s = %s",
                    e->func.captures[i].codegen_name,
                    e->func.captures[i].codegen_name);
            }
            fprintf(out, " } }");
        } else {
            /* Non-capturing lambda: NULL context */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->func.lifted_name);
        }
        break;
    }

    default:
        fprintf(out, "/* TODO: expr kind %d */", e->kind);
        break;
    }
}

/* Check if a top-level decl is a function (its init expr is EXPR_FUNC) */
static bool is_func_decl(Decl *d) {
    return d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC;
}

static bool is_generic_decl(Decl *d) {
    if (d->kind == DECL_STRUCT) return d->struc.is_generic;
    if (d->kind == DECL_UNION) return d->unio.is_generic;
    if (d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC) {
        /* Check if any param contains type vars */
        Expr *fn = d->let.init;
        if (fn->func.explicit_type_var_count > 0) return true;
        for (int i = 0; i < fn->func.param_count; i++)
            if (type_contains_type_var(fn->func.params[i].type)) return true;
    }
    return false;
}

static void emit_func_decl(Decl *d, FILE *out) {
    Expr *fn = d->let.init;
    Type *ft = d->let.resolved_type;
    const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    bool is_main = strcmp(d->let.name, "main") == 0;

    if (is_main) {
        fprintf(out, "int main(int argc, char **argv) {\n");
        /* TODO: convert argc/argv to FC str[] args */
        fprintf(out, "    (void)argc; (void)argv;\n");
    } else {
        /* Emit return type */
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", cname);
        for (int i = 0; i < fn->func.param_count; i++) {
            if (i > 0) fprintf(out, ", ");
            emit_type(fn->func.params[i].type, out);
            fprintf(out, " %s", fn->func.params[i].name);
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");
        fprintf(out, "    (void)_ctx;\n");
    }

    indent_level = 1;
    emit_block_stmts(fn->func.body, fn->func.body_count, out, true);
    indent_level = 0;

    /* For main, if body's last expr doesn't return, add return 0 */
    if (is_main) {
        int last_idx = fn->func.body_count - 1;
        if (last_idx >= 0) {
            Expr *last = fn->func.body[last_idx];
            if (last->kind != EXPR_RETURN && last->type &&
                type_is_integer(last->type)) {
                /* Already emitted as return by emit_block_stmts */
            } else if (last->kind == EXPR_RETURN) {
                /* Already has return */
            } else {
                fprintf(out, "    return 0;\n");
            }
        }
    }

    fprintf(out, "}\n\n");
}

static void emit_struct_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->struc.name, d->struc.name);
}

static void emit_struct_def(Decl *d, FILE *out) {
    fprintf(out, "struct %s {", d->struc.name);
    for (int i = 0; i < d->struc.field_count; i++) {
        fprintf(out, " ");
        emit_type(d->struc.fields[i].type, out);
        fprintf(out, " %s;", d->struc.fields[i].name);
    }
    fprintf(out, " };\n");
}

static void emit_union_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->unio.name, d->unio.name);
}

static void emit_union_tag_enum(Decl *d, FILE *out) {
    const char *name = d->unio.name;
    fprintf(out, "typedef enum {");
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, " %s_tag_%s", name, d->unio.variants[i].name);
    }
    fprintf(out, " } %s_tag;\n", name);
}

static void emit_union_def(Decl *d, FILE *out) {
    const char *name = d->unio.name;
    /* Check if any variant has a payload */
    bool has_any_payload = false;
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (d->unio.variants[i].payload) { has_any_payload = true; break; }
    }
    if (has_any_payload) {
        fprintf(out, "struct %s { %s_tag tag; union {", name, name);
        for (int i = 0; i < d->unio.variant_count; i++) {
            if (d->unio.variants[i].payload) {
                fprintf(out, " ");
                emit_type(d->unio.variants[i].payload, out);
                fprintf(out, " %s;", d->unio.variants[i].name);
            }
        }
        fprintf(out, " }; };\n");
    } else {
        /* Tag-only union (enum-like) — no anonymous union needed */
        fprintf(out, "struct %s { %s_tag tag; };\n", name, name);
    }
}

/* ---- Collect used slice/option types for typedef generation ---- */

typedef struct {
    Type **types;
    int count;
    int cap;
} TypeSet;

static bool typeset_contains(TypeSet *ts, Type *t) {
    for (int i = 0; i < ts->count; i++) {
        if (type_eq(ts->types[i], t)) return true;
    }
    return false;
}

static void typeset_add(TypeSet *ts, Type *t) {
    if (!typeset_contains(ts, t)) {
        DA_APPEND(ts->types, ts->count, ts->cap, t);
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options, TypeSet *fns);

static void collect_types_in_type(Type *t, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            resolve_concrete_stubs(t);
    }
    if (type_contains_type_var(t)) return;  /* still has unresolved type vars */
    if (t->kind == TYPE_SLICE) {
        typeset_add(slices, t);
    } else if (t->kind == TYPE_OPTION) {
        /* Only non-pointer options need typedefs */
        if (!t->option.inner || t->option.inner->kind != TYPE_POINTER) {
            typeset_add(options, t);
        }
        /* Recurse into inner type (e.g., T[]? needs slice typedef too) */
        collect_types_in_type(t->option.inner, slices, options, fns);
    } else if (t->kind == TYPE_FUNC) {
        /* Recurse into param/return types FIRST so dependencies are emitted before this type */
        for (int i = 0; i < t->func.param_count; i++)
            collect_types_in_type(t->func.param_types[i], slices, options, fns);
        collect_types_in_type(t->func.return_type, slices, options, fns);
        typeset_add(fns, t);
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!e) return;
    collect_types_in_type(e->type, slices, options, fns);

    switch (e->kind) {
    case EXPR_BINARY:
        collect_types_expr(e->binary.left, slices, options, fns);
        collect_types_expr(e->binary.right, slices, options, fns);
        break;
    case EXPR_UNARY_PREFIX:
        collect_types_expr(e->unary_prefix.operand, slices, options, fns);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_types_expr(e->unary_postfix.operand, slices, options, fns);
        break;
    case EXPR_CALL:
        collect_types_expr(e->call.func, slices, options, fns);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_types_expr(e->call.args[i], slices, options, fns);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_types_expr(e->field.object, slices, options, fns);
        break;
    case EXPR_INDEX:
        collect_types_expr(e->index.object, slices, options, fns);
        collect_types_expr(e->index.index, slices, options, fns);
        break;
    case EXPR_SLICE:
        collect_types_expr(e->slice.object, slices, options, fns);
        if (e->slice.lo) collect_types_expr(e->slice.lo, slices, options, fns);
        if (e->slice.hi) collect_types_expr(e->slice.hi, slices, options, fns);
        break;
    case EXPR_IF:
        collect_types_expr(e->if_expr.cond, slices, options, fns);
        collect_types_expr(e->if_expr.then_body, slices, options, fns);
        if (e->if_expr.else_body) collect_types_expr(e->if_expr.else_body, slices, options, fns);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_types_expr(e->block.stmts[i], slices, options, fns);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_types_expr(e->func.body[i], slices, options, fns);
        /* Also check param types */
        for (int i = 0; i < e->func.param_count; i++)
            collect_types_in_type(e->func.params[i].type, slices, options, fns);
        break;
    case EXPR_LET:
        collect_types_in_type(e->let_expr.let_type, slices, options, fns);
        collect_types_expr(e->let_expr.let_init, slices, options, fns);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_types_expr(e->return_expr.value, slices, options, fns);
        break;
    case EXPR_ASSIGN:
        collect_types_expr(e->assign.target, slices, options, fns);
        collect_types_expr(e->assign.value, slices, options, fns);
        break;
    case EXPR_CAST:
        collect_types_in_type(e->cast.target, slices, options, fns);
        collect_types_expr(e->cast.operand, slices, options, fns);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_types_expr(e->struct_lit.fields[i].value, slices, options, fns);
        break;
    case EXPR_SOME:
        collect_types_expr(e->some_expr.value, slices, options, fns);
        break;
    case EXPR_ARRAY_LIT:
        collect_types_in_type(e->array_lit.elem_type, slices, options, fns);
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_types_expr(e->array_lit.elems[i], slices, options, fns);
        break;
    case EXPR_MATCH:
        collect_types_expr(e->match_expr.subject, slices, options, fns);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_types_expr(e->match_expr.arms[i].body[j], slices, options, fns);
        }
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_types_expr(e->loop_expr.body[i], slices, options, fns);
        break;
    case EXPR_FOR:
        collect_types_expr(e->for_expr.iter, slices, options, fns);
        if (e->for_expr.range_end) collect_types_expr(e->for_expr.range_end, slices, options, fns);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_types_expr(e->for_expr.body[i], slices, options, fns);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_types_expr(e->break_expr.value, slices, options, fns);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_types_expr(e->alloc_expr.size_expr, slices, options, fns);
        if (e->alloc_expr.init_expr) collect_types_expr(e->alloc_expr.init_expr, slices, options, fns);
        break;
    case EXPR_FREE:
        collect_types_expr(e->free_expr.operand, slices, options, fns);
        break;
    case EXPR_SIZEOF:
        collect_types_in_type(e->sizeof_expr.target, slices, options, fns);
        break;
    case EXPR_DEFAULT:
        collect_types_in_type(e->default_expr.target, slices, options, fns);
        break;
    default:
        break;
    }
}

/* ---- Lambda collection ---- */

typedef struct {
    Expr **exprs;
    int count;
    int cap;
} LambdaSet;

static void collect_lambdas_expr(Expr *e, LambdaSet *ls) {
    if (!e) return;
    if (e->kind == EXPR_FUNC && e->func.lifted_name) {
        DA_APPEND(ls->exprs, ls->count, ls->cap, e);
    }
    switch (e->kind) {
    case EXPR_BINARY:
        collect_lambdas_expr(e->binary.left, ls);
        collect_lambdas_expr(e->binary.right, ls);
        break;
    case EXPR_UNARY_PREFIX:
        collect_lambdas_expr(e->unary_prefix.operand, ls);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_lambdas_expr(e->unary_postfix.operand, ls);
        break;
    case EXPR_CALL:
        collect_lambdas_expr(e->call.func, ls);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_lambdas_expr(e->call.args[i], ls);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_lambdas_expr(e->field.object, ls);
        break;
    case EXPR_INDEX:
        collect_lambdas_expr(e->index.object, ls);
        collect_lambdas_expr(e->index.index, ls);
        break;
    case EXPR_SLICE:
        collect_lambdas_expr(e->slice.object, ls);
        if (e->slice.lo) collect_lambdas_expr(e->slice.lo, ls);
        if (e->slice.hi) collect_lambdas_expr(e->slice.hi, ls);
        break;
    case EXPR_IF:
        collect_lambdas_expr(e->if_expr.cond, ls);
        collect_lambdas_expr(e->if_expr.then_body, ls);
        if (e->if_expr.else_body) collect_lambdas_expr(e->if_expr.else_body, ls);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_lambdas_expr(e->block.stmts[i], ls);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_lambdas_expr(e->func.body[i], ls);
        break;
    case EXPR_LET:
        collect_lambdas_expr(e->let_expr.let_init, ls);
        break;
    case EXPR_LET_DESTRUCT:
        collect_lambdas_expr(e->let_destruct.init, ls);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_lambdas_expr(e->return_expr.value, ls);
        break;
    case EXPR_ASSIGN:
        collect_lambdas_expr(e->assign.target, ls);
        collect_lambdas_expr(e->assign.value, ls);
        break;
    case EXPR_CAST:
        collect_lambdas_expr(e->cast.operand, ls);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_lambdas_expr(e->struct_lit.fields[i].value, ls);
        break;
    case EXPR_SOME:
        collect_lambdas_expr(e->some_expr.value, ls);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_lambdas_expr(e->array_lit.elems[i], ls);
        break;
    case EXPR_MATCH:
        collect_lambdas_expr(e->match_expr.subject, ls);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_lambdas_expr(e->match_expr.arms[i].body[j], ls);
        }
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_lambdas_expr(e->loop_expr.body[i], ls);
        break;
    case EXPR_FOR:
        collect_lambdas_expr(e->for_expr.iter, ls);
        if (e->for_expr.range_end) collect_lambdas_expr(e->for_expr.range_end, ls);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_lambdas_expr(e->for_expr.body[i], ls);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_lambdas_expr(e->break_expr.value, ls);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_lambdas_expr(e->alloc_expr.size_expr, ls);
        if (e->alloc_expr.init_expr) collect_lambdas_expr(e->alloc_expr.init_expr, ls);
        break;
    case EXPR_FREE:
        collect_lambdas_expr(e->free_expr.operand, ls);
        break;
    default:
        break;
    }
}

/* Collect all top-level decls plus module child decls into a flat array */
/* Recursively flatten module decls into a single array */
static void flatten_decls(Decl **decls, int decl_count, Decl ***out, int *count, int *cap) {
    for (int i = 0; i < decl_count; i++) {
        Decl *d = decls[i];
        if (d->kind == DECL_MODULE) {
            flatten_decls(d->module.decls, d->module.decl_count, out, count, cap);
        } else {
            DA_APPEND(*out, *count, *cap, d);
        }
    }
}

static void collect_all_decls(Program *prog, Decl ***out_decls, int *out_count) {
    Decl **all = NULL;
    int count = 0, cap = 0;
    flatten_decls(prog->decls, prog->decl_count, &all, &count, &cap);
    *out_decls = all;
    *out_count = count;
}

void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern_tbl, SymbolTable *symtab) {
    g_mono = mono;
    g_arena = arena;
    g_intern = intern_tbl;
    g_symtab = symtab;
    /* Preamble */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <inttypes.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "\n");

    /* Always emit fc_str (alias for uint8 slice) */
    fprintf(out, "typedef struct { uint8_t* ptr; int64_t len; } fc_str;\n");

    /* Flatten module decls into a single array */
    Decl **all_decls;
    int all_count;
    collect_all_decls(prog, &all_decls, &all_count);

    /* Discover transitive monomorphized instances BEFORE emitting anything.
     * Walk each mono function body to trigger deferred generic call resolution,
     * which may register new struct/union/function instances. Use /dev/null
     * to discard output — we only care about side-effects on the mono table. */
    {
        FILE *null_out = fopen("/dev/null", "w");
        int discovered = 0;
        while (discovered < mono->count) {
            int batch_end = mono->count;
            for (int mi = discovered; mi < batch_end; mi++) {
                MonoInstance *inst = &mono->entries[mi];
                if (inst->decl_kind != DECL_LET) continue;
                Decl *tmpl = inst->template_decl;
                if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
                Expr *fn = tmpl->let.init;
                SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
                g_subst = &subst;
                int save_indent = indent_level;
                int save_temp = temp_counter;
                indent_level = 1;
                emit_block_stmts(fn->func.body, fn->func.body_count, null_out, true);
                indent_level = save_indent;
                temp_counter = save_temp;
                g_subst = NULL;
            }
            discovered = batch_end;
        }
        fclose(null_out);
    }

    /* Collect all slice, option, and function types used in the program */
    TypeSet slices = {0};
    TypeSet options = {0};
    TypeSet fns = {0};

    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_LET) {
            collect_types_in_type(d->let.resolved_type, &slices, &options, &fns);
            collect_types_expr(d->let.init, &slices, &options, &fns);
        }
        if (d->kind == DECL_STRUCT) {
            for (int j = 0; j < d->struc.field_count; j++)
                collect_types_in_type(d->struc.fields[j].type, &slices, &options, &fns);
        }
        if (d->kind == DECL_UNION) {
            for (int j = 0; j < d->unio.variant_count; j++)
                collect_types_in_type(d->unio.variants[j].payload, &slices, &options, &fns);
        }
    }
    /* Also collect types from monomorphized instances */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->concrete_type) {
            if (inst->concrete_type->kind == TYPE_STRUCT) {
                for (int f = 0; f < inst->concrete_type->struc.field_count; f++)
                    collect_types_in_type(inst->concrete_type->struc.fields[f].type, &slices, &options, &fns);
            } else if (inst->concrete_type->kind == TYPE_UNION) {
                for (int v = 0; v < inst->concrete_type->unio.variant_count; v++)
                    collect_types_in_type(inst->concrete_type->unio.variants[v].payload, &slices, &options, &fns);
            }
        }
    }

    /* Also collect types from monomorphized function bodies (with substitution) */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        for (int j = 0; j < fn->func.body_count; j++)
            collect_types_expr(fn->func.body[j], &slices, &options, &fns);
        g_subst = NULL;
    }

    /* Emit forward declarations for all structs and unions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT) emit_struct_forward(d, out);
        else if (d->kind == DECL_UNION) emit_union_forward(d, out);
    }
    /* Forward declarations for monomorphized structs/unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind == DECL_STRUCT) {
            fprintf(out, "typedef struct %s %s;\n", inst->mangled_name, inst->mangled_name);
        } else if (inst->decl_kind == DECL_UNION) {
            fprintf(out, "typedef struct %s %s;\n", inst->mangled_name, inst->mangled_name);
        }
    }

    /* Emit union tag enums (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_UNION) emit_union_tag_enum(d, out);
    }
    /* Tag enums for monomorphized unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_UNION || !inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        fprintf(out, "typedef enum {");
        for (int v = 0; v < ct->unio.variant_count; v++) {
            if (v > 0) fprintf(out, ",");
            fprintf(out, " %s_tag_%s", inst->mangled_name, ct->unio.variants[v].name);
        }
        fprintf(out, " } %s_tag;\n", inst->mangled_name);
    }

    /* Emit slice typedefs — before struct defs since structs may contain slices */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        if (type_eq(s->slice.elem, type_uint8())) continue; /* str already emitted */
        /* Forward-declare as named struct so structs can reference it */
        fprintf(out, "typedef struct fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, "_s { ");
        emit_type(s->slice.elem, out);
        fprintf(out, "* ptr; int64_t len; } fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, ";\n");
    }

    /* Emit option typedefs — before struct defs since structs may contain options.
       For options wrapping structs, the struct is forward-declared above but the
       option stores T by value, so emit those after struct defs. */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        if (o->option.inner &&
            (o->option.inner->kind == TYPE_STRUCT || o->option.inner->kind == TYPE_UNION))
            continue; /* defer until after struct/union defs */
        fprintf(out, "typedef struct { ");
        emit_type(o->option.inner, out);
        fprintf(out, " value; bool has_value; } fc_option_");
        emit_type_ident(o->option.inner, out);
        fprintf(out, ";\n");
    }

    /* Emit full struct and union definitions (skip generics), interleaved with
       option typedefs that wrap them. */
    bool *opt_emitted = calloc(options.count, sizeof(bool));
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        const char *def_name = NULL;
        if (d->kind == DECL_STRUCT) {
            emit_struct_def(d, out);
            def_name = d->struc.name;
        } else if (d->kind == DECL_UNION) {
            emit_union_def(d, out);
            def_name = d->unio.name;
        }
        if (!def_name) continue;
        /* Emit any deferred option typedefs whose inner type matches this name */
        for (int j = 0; j < options.count; j++) {
            if (opt_emitted[j]) continue;
            Type *o = options.types[j];
            if (!o->option.inner) continue;
            const char *inner_name = NULL;
            if (o->option.inner->kind == TYPE_STRUCT) inner_name = o->option.inner->struc.name;
            else if (o->option.inner->kind == TYPE_UNION) inner_name = o->option.inner->unio.name;
            if (inner_name && inner_name == def_name) {
                fprintf(out, "typedef struct { ");
                emit_type(o->option.inner, out);
                fprintf(out, " value; bool has_value; } fc_option_");
                emit_type_ident(o->option.inner, out);
                fprintf(out, ";\n");
                opt_emitted[j] = true;
            }
        }
    }
    free(opt_emitted);

    /* Resolve self-referential stubs in monomorphized concrete types.
     * Only resolve nested types (fields/variants), not the top-level type
     * which already has its mangled name set. */
    for (int mi = 0; mi < mono->count; mi++) {
        Type *ct = mono->entries[mi].concrete_type;
        if (!ct) continue;
        if (ct->kind == TYPE_STRUCT) {
            for (int f = 0; f < ct->struc.field_count; f++)
                resolve_concrete_stubs(ct->struc.fields[f].type);
        } else if (ct->kind == TYPE_UNION) {
            for (int v = 0; v < ct->unio.variant_count; v++)
                resolve_concrete_stubs(ct->unio.variants[v].payload);
        }
    }

    /* Emit monomorphized struct/union definitions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (!inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        if (inst->decl_kind == DECL_STRUCT) {
            fprintf(out, "struct %s {", inst->mangled_name);
            for (int f = 0; f < ct->struc.field_count; f++) {
                fprintf(out, " ");
                emit_type(ct->struc.fields[f].type, out);
                fprintf(out, " %s;", ct->struc.fields[f].name);
            }
            fprintf(out, " };\n");
        } else if (inst->decl_kind == DECL_UNION) {
            bool has_payload = false;
            for (int v = 0; v < ct->unio.variant_count; v++)
                if (ct->unio.variants[v].payload) { has_payload = true; break; }
            if (has_payload) {
                fprintf(out, "struct %s { %s_tag tag; union {", inst->mangled_name, inst->mangled_name);
                for (int v = 0; v < ct->unio.variant_count; v++) {
                    if (ct->unio.variants[v].payload) {
                        fprintf(out, " ");
                        emit_type(ct->unio.variants[v].payload, out);
                        fprintf(out, " %s;", ct->unio.variants[v].name);
                    }
                }
                fprintf(out, " }; };\n");
            } else {
                fprintf(out, "struct %s { %s_tag tag; };\n", inst->mangled_name, inst->mangled_name);
            }
        }
    }

    /* Emit function type typedefs */
    for (int i = 0; i < fns.count; i++) {
        Type *f = fns.types[i];
        fprintf(out, "typedef struct { ");
        emit_type(f->func.return_type, out);
        fprintf(out, " (*fn_ptr)(");
        for (int j = 0; j < f->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(f->func.param_types[j], out);
        }
        if (f->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void*); void* ctx; } ");
        emit_type(f, out);
        fprintf(out, ";\n");
    }

    free(slices.types);
    free(options.types);
    free(fns.types);

    fprintf(out, "\n");

    /* Check if we have a main function */
    bool has_main = false;
    bool has_non_func = false;
    for (int i = 0; i < all_count; i++) {
        if (is_func_decl(all_decls[i])) {
            if (strcmp(all_decls[i]->let.name, "main") == 0) has_main = true;
        } else if (all_decls[i]->kind == DECL_LET) {
            has_non_func = true;
        }
    }

    /* Emit forward declarations for functions (with void* _ctx), skip generics */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_func_decl(d) && strcmp(d->let.name, "main") != 0 && !is_generic_decl(d)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            Type *ft = d->let.resolved_type;
            emit_type(ft->func.return_type, out);
            fprintf(out, " %s(", cname);
            Expr *fn = d->let.init;
            for (int j = 0; j < fn->func.param_count; j++) {
                if (j > 0) fprintf(out, ", ");
                emit_type(fn->func.params[j].type, out);
                fprintf(out, " %s", fn->func.params[j].name);
            }
            if (fn->func.param_count > 0) fprintf(out, ", ");
            fprintf(out, "void* _ctx);\n");
        }
    }
    /* Forward declarations for monomorphized functions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        /* Set up substitution context */
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", fn->func.params[j].name);
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
        g_subst = NULL;
    }
    fprintf(out, "\n");

    /* Collect lambdas from all declarations */
    LambdaSet lambdas = {0};
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init) {
            collect_lambdas_expr(all_decls[i]->let.init, &lambdas);
        }
    }

    /* Emit non-function global variable definitions.
     * Must come before lifted lambdas so they can reference globals.
     * Pass2 enforces that initializers are constant expressions, so these
     * are valid at C file scope. Module members (with codegen_name) are
     * always emitted; other top-level variables only when there is a main. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d) &&
            (d->let.codegen_name || has_main)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            emit_type(d->let.resolved_type, out);
            fprintf(out, " %s = ", cname);
            emit_expr(d->let.init, out);
            fprintf(out, ";\n");
        }
    }
    fprintf(out, "\n");

    /* Emit context structs for capturing lambdas */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        if (lam->func.capture_count > 0) {
            fprintf(out, "typedef struct {");
            for (int j = 0; j < lam->func.capture_count; j++) {
                fprintf(out, " ");
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s;", lam->func.captures[j].codegen_name);
            }
            fprintf(out, " } _ctx_%s;\n", lam->func.lifted_name);
        }
    }

    /* Emit forward declarations for lifted lambdas */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        Type *ft = lam->type;
        fprintf(out, "static ");
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", lam->func.lifted_name);
        for (int j = 0; j < lam->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(lam->func.params[j].type, out);
            fprintf(out, " %s", lam->func.params[j].name);
        }
        if (lam->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
    }
    fprintf(out, "\n");

    /* Emit lifted lambda function definitions */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        Type *ft = lam->type;
        fprintf(out, "static ");
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", lam->func.lifted_name);
        for (int j = 0; j < lam->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(lam->func.params[j].type, out);
            fprintf(out, " %s", lam->func.params[j].name);
        }
        if (lam->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");

        indent_level = 1;
        if (lam->func.capture_count > 0) {
            /* Extract captures from context struct */
            emit_indent(out);
            fprintf(out, "_ctx_%s* _c = (_ctx_%s*)_ctx;\n",
                lam->func.lifted_name, lam->func.lifted_name);
            for (int j = 0; j < lam->func.capture_count; j++) {
                emit_indent(out);
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s = _c->%s;\n",
                    lam->func.captures[j].codegen_name,
                    lam->func.captures[j].codegen_name);
            }
        } else {
            emit_indent(out);
            fprintf(out, "(void)_ctx;\n");
        }

        emit_block_stmts(lam->func.body, lam->func.body_count, out, true);
        indent_level = 0;
        fprintf(out, "}\n\n");
    }
    free(lambdas.exprs);

    /* Emit function definitions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        if (is_func_decl(all_decls[i]) && !is_generic_decl(all_decls[i])) {
            emit_func_decl(all_decls[i], out);
        }
    }

    /* Emit forward declarations for all monomorphized functions (including transitive) */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", fn->func.params[j].name);
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
        g_subst = NULL;
    }
    fprintf(out, "\n");

    /* Emit monomorphized function definitions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", fn->func.params[j].name);
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");
        fprintf(out, "    (void)_ctx;\n");
        indent_level = 1;
        emit_block_stmts(fn->func.body, fn->func.body_count, out, true);
        indent_level = 0;
        fprintf(out, "}\n\n");
        g_subst = NULL;
    }

    /* If no main function, wrap non-func decls in synthetic main */
    if (!has_main && has_non_func) {
        fprintf(out, "int main(void) {\n");
        indent_level = 1;
        for (int i = 0; i < all_count; i++) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
                emit_indent(out);
                emit_type(d->let.resolved_type, out);
                fprintf(out, " %s = ", cname);
                emit_expr(d->let.init, out);
                fprintf(out, ";\n");
            }
        }
        /* Return last binding's value */
        for (int i = all_count - 1; i >= 0; i--) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
                if (type_is_integer(d->let.resolved_type) || type_eq(d->let.resolved_type, type_bool())) {
                    emit_indent(out);
                    fprintf(out, "return (int)%s;\n", cname);
                } else {
                    emit_indent(out);
                    fprintf(out, "return 0;\n");
                }
                break;
            }
        }
        indent_level = 0;
        fprintf(out, "}\n");
    }

    free(all_decls);
}
