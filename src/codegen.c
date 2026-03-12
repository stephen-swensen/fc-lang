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
    case TYPE_POINTER:
        emit_type(t->pointer.pointee, out);
        fprintf(out, "*");
        break;
    case TYPE_SLICE:
        fprintf(out, "fc_slice_");
        emit_type(t->slice.elem, out);
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

static void emit_expr(Expr *e, FILE *out);

static void emit_block_stmts(Expr **stmts, int count, FILE *out, bool as_return) {
    for (int i = 0; i < count; i++) {
        Expr *s = stmts[i];
        bool is_last = (i == count - 1);

        emit_indent(out);

        if (s->kind == EXPR_LET) {
            emit_type(s->let_expr.let_type, out);
            fprintf(out, " %s = ", s->let_expr.let_name);
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
            emit_expr(s->assign.target, out);
            fprintf(out, " = ");
            emit_expr(s->assign.value, out);
            fprintf(out, ";\n");
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

    case EXPR_FLOAT_LIT:
        if (e->float_lit.lit_type->kind == TYPE_FLOAT32)
            fprintf(out, "%gf", e->float_lit.value);
        else
            fprintf(out, "%g", e->float_lit.value);
        break;

    case EXPR_BOOL_LIT:
        fprintf(out, "%s", e->bool_lit.value ? "true" : "false");
        break;

    case EXPR_STRING_LIT:
        fprintf(out, "((fc_str){(uint8_t*)\"%.*s\", %d})",
            e->string_lit.length, e->string_lit.value, e->string_lit.length);
        break;

    case EXPR_CSTRING_LIT:
        fprintf(out, "\"%.*s\"", e->cstring_lit.length, e->cstring_lit.value);
        break;

    case EXPR_IDENT:
        fprintf(out, "%s", e->ident.name);
        break;

    case EXPR_BINARY: {
        const char *op_str;
        switch (e->binary.op) {
        case TOK_PLUS:     op_str = "+";  break;
        case TOK_MINUS:    op_str = "-";  break;
        case TOK_STAR:     op_str = "*";  break;
        case TOK_SLASH:    op_str = "/";  break;
        case TOK_PERCENT:  op_str = "%%"; break;
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
        /* Simple branches → ternary; block branches → statement expression */
        if (e->if_expr.else_body && e->type && e->type->kind != TYPE_VOID) {
            /* Expression if/then/else → ternary */
            fprintf(out, "(");
            emit_expr(e->if_expr.cond, out);
            fprintf(out, " ? ");
            emit_expr(e->if_expr.then_body, out);
            fprintf(out, " : ");
            emit_expr(e->if_expr.else_body, out);
            fprintf(out, ")");
        } else {
            /* Statement if — use statement expression for expression context */
            fprintf(out, "({");
            indent_level++;
            fprintf(out, "\n");
            int tid = temp_counter++;
            if (e->type && e->type->kind != TYPE_VOID && e->if_expr.else_body) {
                emit_indent(out);
                emit_type(e->type, out);
                fprintf(out, " _if%d;\n", tid);
            }
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
        /* Last stmt is the value */
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
        emit_expr(e->index.object, out);
        fprintf(out, "[");
        emit_expr(e->index.index, out);
        fprintf(out, "]");
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

    case EXPR_MATCH: {
        /* Emit as statement expression with if-else chain */
        fprintf(out, "({\n");
        indent_level++;

        /* Emit subject into a temp variable */
        int subj_id = temp_counter++;
        emit_indent(out);
        emit_type(e->match_expr.subject->type, out);
        fprintf(out, " _subj%d = ", subj_id);
        emit_expr(e->match_expr.subject, out);
        fprintf(out, ";\n");

        /* Emit result variable */
        int res_id = temp_counter++;
        emit_indent(out);
        emit_type(e->type, out);
        fprintf(out, " _match%d;\n", res_id);

        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            Pattern *pat = arm->pattern;
            emit_indent(out);

            if (i > 0) fprintf(out, "else ");

            bool needs_condition = true;

            switch (pat->kind) {
            case PAT_WILDCARD:
            case PAT_BINDING:
                /* Always matches — this is the else branch */
                needs_condition = false;
                fprintf(out, "{\n");
                break;
            case PAT_INT_LIT:
                fprintf(out, "if (_subj%d == %" PRId64 ") {\n", subj_id, pat->int_lit.value);
                break;
            case PAT_BOOL_LIT:
                fprintf(out, "if (_subj%d == %s) {\n", subj_id,
                    pat->bool_lit.value ? "true" : "false");
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
                        break;
                    }
                }
            }

            /* Emit arm body */
            if (arm->body_count == 1) {
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

            (void)needs_condition;
        }

        emit_indent(out);
        fprintf(out, "_match%d;\n", res_id);

        indent_level--;
        emit_indent(out);
        fprintf(out, "})");
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

static void emit_struct_typedef(Decl *d, FILE *out) {
    fprintf(out, "typedef struct {");
    for (int i = 0; i < d->struc.field_count; i++) {
        fprintf(out, " ");
        emit_type(d->struc.fields[i].type, out);
        fprintf(out, " %s;", d->struc.fields[i].name);
    }
    fprintf(out, " } %s;\n", d->struc.name);
}

static void emit_union_typedef(Decl *d, FILE *out) {
    const char *name = d->unio.name;

    /* Emit tag enum */
    fprintf(out, "typedef enum {");
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, " %s_tag_%s", name, d->unio.variants[i].name);
    }
    fprintf(out, " } %s_tag;\n", name);

    /* Emit tagged union struct */
    fprintf(out, "typedef struct { %s_tag tag; union {", name);
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (d->unio.variants[i].payload) {
            fprintf(out, " ");
            emit_type(d->unio.variants[i].payload, out);
            fprintf(out, " %s;", d->unio.variants[i].name);
        }
    }
    fprintf(out, " }; } %s;\n", name);
}

void codegen_emit(Program *prog, FILE *out) {
    /* Preamble */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <inttypes.h>\n");
    fprintf(out, "\n");

    /* Emit struct and union type definitions */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_STRUCT) {
            emit_struct_typedef(d, out);
        } else if (d->kind == DECL_UNION) {
            emit_union_typedef(d, out);
        }
    }
    fprintf(out, "\n");

    /* Check if we have a main function */
    bool has_main = false;
    bool has_non_func = false;
    for (int i = 0; i < prog->decl_count; i++) {
        if (is_func_decl(prog->decls[i])) {
            if (strcmp(prog->decls[i]->let.name, "main") == 0) has_main = true;
        } else {
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
