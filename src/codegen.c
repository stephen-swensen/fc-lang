#include "codegen.h"
#include "diag.h"
#include <inttypes.h>
#include <string.h>

static int indent_level = 0;
static int temp_counter = 0;

static void emit_indent(FILE *out) {
    for (int i = 0; i < indent_level; i++) fprintf(out, "    ");
}

static void emit_type(Type *t, FILE *out) {
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
        fprintf(out, "%s", t->struc.name);
        break;
    case TYPE_UNION:
        fprintf(out, "%s", t->unio.name);
        break;
    default:
        fprintf(out, "/* TODO: type %d */", t->kind);
        break;
    }
}

/* Emit a C type name suitable for use in identifiers (slice/option typedef names) */
static void emit_type_ident(Type *t, FILE *out) {
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
    case TYPE_STRUCT:  fprintf(out, "%s", t->struc.name); break;
    case TYPE_UNION:   fprintf(out, "%s", t->unio.name);  break;
    case TYPE_SLICE:
        fprintf(out, "fc_slice_");
        emit_type_ident(t->slice.elem, out);
        break;
    case TYPE_POINTER:
        emit_type_ident(t->pointer.pointee, out);
        fprintf(out, "_ptr");
        break;
    default:           fprintf(out, "unknown");   break;
    }
}

static void emit_expr(Expr *e, FILE *out);

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
        fprintf(out, "%s", e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
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
            const char *variant_name = e->call.func->field.name;
            fprintf(out, "(%s){ .tag = %s_tag_%s, .%s = ",
                union_name, union_name, variant_name, variant_name);
            emit_expr(e->call.args[0], out);
            fprintf(out, " }");
            break;
        }
        emit_expr(e->call.func, out);
        fprintf(out, "(");
        for (int i = 0; i < e->call.arg_count; i++) {
            if (i > 0) fprintf(out, ", ");
            emit_expr(e->call.args[i], out);
        }
        fprintf(out, ")");
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
        /* No-payload variant constructor: color.green → (color){ .tag = color_tag_green } */
        if (e->type && e->type->kind == TYPE_UNION &&
            e->field.object->kind == EXPR_IDENT) {
            const char *union_name = e->type->unio.name;
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
        fprintf(out, "(%s){ ", e->struct_lit.type_name);
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
            emit_indent(out);

            if (i > 0) fprintf(out, "else ");

            switch (pat->kind) {
            case PAT_WILDCARD:
            case PAT_BINDING:
                /* Always matches — this is the else branch */
                fprintf(out, "{\n");
                break;
            case PAT_INT_LIT:
                fprintf(out, "if (_subj%d == %" PRId64 ") {\n", subj_id, pat->int_lit.value);
                break;
            case PAT_BOOL_LIT:
                fprintf(out, "if (_subj%d == %s) {\n", subj_id,
                    pat->bool_lit.value ? "true" : "false");
                break;
            case PAT_SOME:
                if (e->match_expr.subject->type->kind == TYPE_OPTION &&
                    e->match_expr.subject->type->option.inner &&
                    e->match_expr.subject->type->option.inner->kind == TYPE_POINTER) {
                    fprintf(out, "if (_subj%d != NULL) {\n", subj_id);
                } else {
                    fprintf(out, "if (_subj%d.has_value) {\n", subj_id);
                }
                break;
            case PAT_NONE:
                if (e->match_expr.subject->type->kind == TYPE_OPTION &&
                    e->match_expr.subject->type->option.inner &&
                    e->match_expr.subject->type->option.inner->kind == TYPE_POINTER) {
                    fprintf(out, "if (_subj%d == NULL) {\n", subj_id);
                } else {
                    fprintf(out, "if (!_subj%d.has_value) {\n", subj_id);
                }
                break;
            case PAT_VARIANT:
                fprintf(out, "if (_subj%d.tag == %s_tag_%s) {\n", subj_id,
                    e->match_expr.subject->type->unio.name, pat->variant.variant);
                break;
            default:
                fprintf(out, "/* unsupported pattern */ {\n");
                break;
            }

            indent_level++;

            /* Emit bindings */
            if (pat->kind == PAT_BINDING) {
                emit_indent(out);
                emit_type(e->match_expr.subject->type, out);
                fprintf(out, " %s = _subj%d;\n", pat->binding.name, subj_id);
                emit_indent(out);
                fprintf(out, "(void)%s;\n", pat->binding.name);
            } else if (pat->kind == PAT_SOME && pat->some_pat.inner &&
                       pat->some_pat.inner->kind == PAT_BINDING) {
                emit_indent(out);
                Type *inner_type = e->match_expr.subject->type->option.inner;
                emit_type(inner_type, out);
                if (inner_type->kind == TYPE_POINTER) {
                    fprintf(out, " %s = _subj%d;\n",
                        pat->some_pat.inner->binding.name, subj_id);
                } else {
                    fprintf(out, " %s = _subj%d.value;\n",
                        pat->some_pat.inner->binding.name, subj_id);
                }
                emit_indent(out);
                fprintf(out, "(void)%s;\n", pat->some_pat.inner->binding.name);
            } else if (pat->kind == PAT_VARIANT && pat->variant.payload &&
                       pat->variant.payload->kind == PAT_BINDING) {
                /* Find the payload type */
                Type *union_type = e->match_expr.subject->type;
                for (int v = 0; v < union_type->unio.variant_count; v++) {
                    if (union_type->unio.variants[v].name == pat->variant.variant) {
                        emit_indent(out);
                        emit_type(union_type->unio.variants[v].payload, out);
                        fprintf(out, " %s = _subj%d.%s;\n",
                            pat->variant.payload->binding.name,
                            subj_id, pat->variant.variant);
                        emit_indent(out);
                        fprintf(out, "(void)%s;\n", pat->variant.payload->binding.name);
                        break;
                    }
                }
            }

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

    default:
        fprintf(out, "/* TODO: expr kind %d */", e->kind);
        break;
    }
}

/* Check if a top-level decl is a function (its init expr is EXPR_FUNC) */
static bool is_func_decl(Decl *d) {
    return d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC;
}

static void emit_func_decl(Decl *d, FILE *out) {
    Expr *fn = d->let.init;
    Type *ft = d->let.resolved_type;
    bool is_main = strcmp(d->let.name, "main") == 0;

    if (is_main) {
        fprintf(out, "int main(int argc, char **argv) {\n");
        /* TODO: convert argc/argv to FC str[] args */
        fprintf(out, "    (void)argc; (void)argv;\n");
    } else {
        /* Emit return type */
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", d->let.name);
        for (int i = 0; i < fn->func.param_count; i++) {
            if (i > 0) fprintf(out, ", ");
            emit_type(fn->func.params[i].type, out);
            fprintf(out, " %s", fn->func.params[i].name);
        }
        fprintf(out, ") {\n");
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

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options);

static void collect_types_in_type(Type *t, TypeSet *slices, TypeSet *options) {
    if (!t) return;
    if (t->kind == TYPE_SLICE) {
        typeset_add(slices, t);
    } else if (t->kind == TYPE_OPTION) {
        /* Only non-pointer options need typedefs */
        if (!t->option.inner || t->option.inner->kind != TYPE_POINTER) {
            typeset_add(options, t);
        }
        /* Recurse into inner type (e.g., T[]? needs slice typedef too) */
        collect_types_in_type(t->option.inner, slices, options);
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options) {
    if (!e) return;
    collect_types_in_type(e->type, slices, options);

    switch (e->kind) {
    case EXPR_BINARY:
        collect_types_expr(e->binary.left, slices, options);
        collect_types_expr(e->binary.right, slices, options);
        break;
    case EXPR_UNARY_PREFIX:
        collect_types_expr(e->unary_prefix.operand, slices, options);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_types_expr(e->unary_postfix.operand, slices, options);
        break;
    case EXPR_CALL:
        collect_types_expr(e->call.func, slices, options);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_types_expr(e->call.args[i], slices, options);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_types_expr(e->field.object, slices, options);
        break;
    case EXPR_INDEX:
        collect_types_expr(e->index.object, slices, options);
        collect_types_expr(e->index.index, slices, options);
        break;
    case EXPR_SLICE:
        collect_types_expr(e->slice.object, slices, options);
        if (e->slice.lo) collect_types_expr(e->slice.lo, slices, options);
        if (e->slice.hi) collect_types_expr(e->slice.hi, slices, options);
        break;
    case EXPR_IF:
        collect_types_expr(e->if_expr.cond, slices, options);
        collect_types_expr(e->if_expr.then_body, slices, options);
        if (e->if_expr.else_body) collect_types_expr(e->if_expr.else_body, slices, options);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_types_expr(e->block.stmts[i], slices, options);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_types_expr(e->func.body[i], slices, options);
        /* Also check param types */
        for (int i = 0; i < e->func.param_count; i++)
            collect_types_in_type(e->func.params[i].type, slices, options);
        break;
    case EXPR_LET:
        collect_types_in_type(e->let_expr.let_type, slices, options);
        collect_types_expr(e->let_expr.let_init, slices, options);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_types_expr(e->return_expr.value, slices, options);
        break;
    case EXPR_ASSIGN:
        collect_types_expr(e->assign.target, slices, options);
        collect_types_expr(e->assign.value, slices, options);
        break;
    case EXPR_CAST:
        collect_types_in_type(e->cast.target, slices, options);
        collect_types_expr(e->cast.operand, slices, options);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_types_expr(e->struct_lit.fields[i].value, slices, options);
        break;
    case EXPR_SOME:
        collect_types_expr(e->some_expr.value, slices, options);
        break;
    case EXPR_ARRAY_LIT:
        collect_types_in_type(e->array_lit.elem_type, slices, options);
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_types_expr(e->array_lit.elems[i], slices, options);
        break;
    case EXPR_MATCH:
        collect_types_expr(e->match_expr.subject, slices, options);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_types_expr(e->match_expr.arms[i].body[j], slices, options);
        }
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_types_expr(e->loop_expr.body[i], slices, options);
        break;
    case EXPR_FOR:
        collect_types_expr(e->for_expr.iter, slices, options);
        if (e->for_expr.range_end) collect_types_expr(e->for_expr.range_end, slices, options);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_types_expr(e->for_expr.body[i], slices, options);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_types_expr(e->break_expr.value, slices, options);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_types_expr(e->alloc_expr.size_expr, slices, options);
        if (e->alloc_expr.init_expr) collect_types_expr(e->alloc_expr.init_expr, slices, options);
        break;
    case EXPR_FREE:
        collect_types_expr(e->free_expr.operand, slices, options);
        break;
    case EXPR_SIZEOF:
        collect_types_in_type(e->sizeof_expr.target, slices, options);
        break;
    case EXPR_DEFAULT:
        collect_types_in_type(e->default_expr.target, slices, options);
        break;
    default:
        break;
    }
}

void codegen_emit(Program *prog, FILE *out) {
    /* Preamble */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <inttypes.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "\n");

    /* Always emit fc_str (alias for uint8 slice) */
    fprintf(out, "typedef struct { uint8_t* ptr; int64_t len; } fc_str;\n");

    /* Emit forward declarations for all structs and unions */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_STRUCT) emit_struct_forward(d, out);
        else if (d->kind == DECL_UNION) emit_union_forward(d, out);
    }

    /* Emit union tag enums (must come before full definitions) */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_UNION) emit_union_tag_enum(d, out);
    }

    /* Emit full struct and union definitions */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_STRUCT) emit_struct_def(d, out);
        else if (d->kind == DECL_UNION) emit_union_def(d, out);
    }

    /* Collect all slice and option types used in the program */
    TypeSet slices = {0};
    TypeSet options = {0};

    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_LET) {
            collect_types_in_type(d->let.resolved_type, &slices, &options);
            collect_types_expr(d->let.init, &slices, &options);
        }
        /* Check struct field types */
        if (d->kind == DECL_STRUCT) {
            for (int j = 0; j < d->struc.field_count; j++)
                collect_types_in_type(d->struc.fields[j].type, &slices, &options);
        }
        if (d->kind == DECL_UNION) {
            for (int j = 0; j < d->unio.variant_count; j++)
                collect_types_in_type(d->unio.variants[j].payload, &slices, &options);
        }
    }

    /* Emit slice typedefs (skip str which is already emitted) */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        /* str is already defined above */
        if (type_eq(s->slice.elem, type_uint8())) continue;
        fprintf(out, "typedef struct { ");
        emit_type(s->slice.elem, out);
        fprintf(out, "* ptr; int64_t len; } fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, ";\n");
    }

    /* Emit option typedefs (after struct/union defs so inner types are defined) */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        fprintf(out, "typedef struct { ");
        emit_type(o->option.inner, out);
        fprintf(out, " value; bool has_value; } fc_option_");
        emit_type_ident(o->option.inner, out);
        fprintf(out, ";\n");
    }

    free(slices.types);
    free(options.types);

    fprintf(out, "\n");

    /* Check if we have a main function */
    bool has_main = false;
    bool has_non_func = false;
    for (int i = 0; i < prog->decl_count; i++) {
        if (is_func_decl(prog->decls[i])) {
            if (strcmp(prog->decls[i]->let.name, "main") == 0) has_main = true;
        } else if (prog->decls[i]->kind == DECL_LET) {
            has_non_func = true;
        }
    }

    /* Emit forward declarations for functions */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (is_func_decl(d) && strcmp(d->let.name, "main") != 0) {
            Type *ft = d->let.resolved_type;
            emit_type(ft->func.return_type, out);
            fprintf(out, " %s(", d->let.name);
            Expr *fn = d->let.init;
            for (int j = 0; j < fn->func.param_count; j++) {
                if (j > 0) fprintf(out, ", ");
                emit_type(fn->func.params[j].type, out);
                fprintf(out, " %s", fn->func.params[j].name);
            }
            fprintf(out, ");\n");
        }
    }
    fprintf(out, "\n");

    /* Emit function definitions */
    for (int i = 0; i < prog->decl_count; i++) {
        if (is_func_decl(prog->decls[i])) {
            emit_func_decl(prog->decls[i], out);
        }
    }

    /* If no main function, wrap non-func decls in synthetic main */
    if (!has_main && has_non_func) {
        fprintf(out, "int main(void) {\n");
        indent_level = 1;
        for (int i = 0; i < prog->decl_count; i++) {
            Decl *d = prog->decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                emit_indent(out);
                emit_type(d->let.resolved_type, out);
                fprintf(out, " %s = ", d->let.name);
                emit_expr(d->let.init, out);
                fprintf(out, ";\n");
            }
        }
        /* Return last binding's value */
        for (int i = prog->decl_count - 1; i >= 0; i--) {
            Decl *d = prog->decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                if (type_is_integer(d->let.resolved_type) || type_eq(d->let.resolved_type, type_bool())) {
                    emit_indent(out);
                    fprintf(out, "return (int)%s;\n", d->let.name);
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
}
