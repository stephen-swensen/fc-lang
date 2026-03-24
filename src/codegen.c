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

/* Forward declaration for TypeSet (defined later) */
typedef struct TypeSet TypeSet;
static TypeSet *g_eq_set = NULL;

static int indent_level = 0;
static int temp_counter = 0;

/* Does the option inner type use null-sentinel optimization (bare pointer, NULL = none)? */
static bool is_null_sentinel(Type *opt_type) {
    if (!opt_type || opt_type->kind != TYPE_OPTION) return false;
    Type *inner = opt_type->option.inner;
    return inner && (inner->kind == TYPE_POINTER ||
                     inner->kind == TYPE_ANY_PTR);
}

static void emit_indent(FILE *out) {
    for (int i = 0; i < indent_level; i++) fprintf(out, "    ");
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

/* Resolve a concrete type + property name to a C constant string.
 * Used for type-variable property access ('a.min etc.) during monomorphized emission. */
static const char *resolve_type_prop_codegen(Type *t, const char *prop) {
    TypeKind k = t->kind;
    bool is_int = type_is_integer(t);
    bool is_float = type_is_float(t);
    if (!is_int && !is_float) return NULL;

    if (strcmp(prop, "bits") == 0) {
        switch (k) {
        case TYPE_INT8: case TYPE_UINT8:   return "8";
        case TYPE_INT16: case TYPE_UINT16: return "16";
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: return "32";
        case TYPE_INT64: case TYPE_UINT64: case TYPE_FLOAT64: return "64";
        case TYPE_ISIZE: case TYPE_USIZE: return "((int32_t)(sizeof(ptrdiff_t)*8))";
        default: return NULL;
        }
    }
    if (is_int) {
        if (strcmp(prop, "min") == 0) {
            switch (k) {
            case TYPE_INT8:   return "INT8_MIN";
            case TYPE_INT16:  return "INT16_MIN";
            case TYPE_INT32:  return "INT32_MIN";
            case TYPE_INT64:  return "INT64_MIN";
            case TYPE_UINT8:  return "((uint8_t)0)";
            case TYPE_UINT16: return "((uint16_t)0)";
            case TYPE_UINT32: return "((uint32_t)0)";
            case TYPE_UINT64: return "((uint64_t)0)";
            case TYPE_ISIZE:  return "PTRDIFF_MIN";
            case TYPE_USIZE:  return "((size_t)0)";
            default: return NULL;
            }
        }
        if (strcmp(prop, "max") == 0) {
            switch (k) {
            case TYPE_INT8:   return "INT8_MAX";
            case TYPE_INT16:  return "INT16_MAX";
            case TYPE_INT32:  return "INT32_MAX";
            case TYPE_INT64:  return "INT64_MAX";
            case TYPE_UINT8:  return "UINT8_MAX";
            case TYPE_UINT16: return "UINT16_MAX";
            case TYPE_UINT32: return "UINT32_MAX";
            case TYPE_UINT64: return "UINT64_MAX";
            case TYPE_ISIZE:  return "PTRDIFF_MAX";
            case TYPE_USIZE:  return "SIZE_MAX";
            default: return NULL;
            }
        }
        return NULL;
    }
    /* Float properties */
    bool is_f32 = (k == TYPE_FLOAT32);
    if (strcmp(prop, "min") == 0) return is_f32 ? "FLT_MIN" : "DBL_MIN";
    if (strcmp(prop, "max") == 0) return is_f32 ? "FLT_MAX" : "DBL_MAX";
    if (strcmp(prop, "epsilon") == 0) return is_f32 ? "FLT_EPSILON" : "DBL_EPSILON";
    if (strcmp(prop, "nan") == 0) return is_f32 ? "((float)NAN)" : "((double)NAN)";
    if (strcmp(prop, "inf") == 0) return is_f32 ? "((float)INFINITY)" : "((double)INFINITY)";
    if (strcmp(prop, "neg_inf") == 0) return is_f32 ? "((float)(-INFINITY))" : "((double)(-INFINITY))";
    return NULL;
}

/* Emit the identifier portion of a function type typedef name */
static void emit_fn_type_suffix(Type *t, FILE *out);

static void emit_type_ident(Type *t, FILE *out);

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
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_VOID:    fprintf(out, "void");      break;
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_POINTER:
        if (t->is_const) fprintf(out, "const ");
        emit_type(t->pointer.pointee, out);
        fprintf(out, "*");
        break;
    case TYPE_SLICE:
        if (is_str_type(t)) {
            fprintf(out, "fc_str");
        } else if (is_str32_type(t)) {
            fprintf(out, "fc_str32");
        } else {
            fprintf(out, "fc_slice_");
            emit_type_ident(t->slice.elem, out);
        }
        break;
    case TYPE_OPTION:
        if (t->option.inner && (t->option.inner->kind == TYPE_POINTER ||
            t->option.inner->kind == TYPE_ANY_PTR)) {
            /* Pointer-like? → plain pointer (null = none) */
            emit_type(t->option.inner, out);
        } else {
            fprintf(out, "fc_option_");
            if (t->option.inner) emit_type_ident(t->option.inner, out);
            else fprintf(out, "void");
        }
        break;
    case TYPE_STRUCT:
        if (g_subst && type_contains_type_var(t)) {
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
            return;
        }
        if (t->struc.c_name) {
            fprintf(out, "struct %s", t->struc.c_name);
        } else {
            fprintf(out, "%s", t->struc.name);
        }
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
        if (t->is_const) fprintf(out, "const ");
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
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_ANY_PTR: fprintf(out, "void_ptr");  break;
    case TYPE_STRUCT:
        if (g_subst && type_contains_type_var(t))
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
        else if (t->struc.c_name)
            fprintf(out, "%s", t->struc.c_name);
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
        if (is_str_type(t)) {
            fprintf(out, "fc_str");
        } else if (is_str32_type(t)) {
            fprintf(out, "fc_str32");
        } else {
            fprintf(out, "fc_slice_");
            emit_type_ident(t->slice.elem, out);
        }
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
static void emit_eq_func_name(Type *t, FILE *out);

/* Emit a function call argument for an extern call, inserting casts at the
 * C boundary: cstr (uint8*) → const char*, cstr* (uint8**) → char**,
 * and any** (void**) → void* (C's void* converts to any T** implicitly,
 * but void** does not). */
static void emit_extern_arg(Expr *e, Type *param_type, FILE *out) {
    if (param_type && param_type->kind == TYPE_FUNC) {
        /* Function at C extern boundary — emit C-compatible trampoline name */
        if (e->kind == EXPR_IDENT && !e->ident.is_local) {
            const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
            fprintf(out, "_ctramp_%s", name);
            return;
        }
        if (e->kind == EXPR_FUNC && e->func.capture_count == 0 && e->func.lifted_name) {
            fprintf(out, "_ctramp_%s", e->func.lifted_name);
            return;
        }
        /* Capturing lambda at C boundary — fall through, will produce type error */
    }
    if (param_type && is_cstr_type(param_type)) {
        if (param_type->is_const)
            fprintf(out, "(const char*)");
        else
            fprintf(out, "(char*)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               is_cstr_type(param_type->pointer.pointee)) {
        /* uint8** → char** at C boundary (e.g. strtol's char** out-param) */
        fprintf(out, "(char**)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               param_type->pointer.pointee->kind == TYPE_ANY_PTR) {
        /* any** (void**) → void* at C boundary — void* converts to any T**
         * implicitly in C, but void** does not (e.g. sqlite3_open's sqlite3**) */
        fprintf(out, "(void*)");
    } else if (!param_type && e->type) {
        /* Variadic arg — apply C default argument promotions and boundary casts */
        Type *at = e->type;
        if (is_cstr_type(at)) {
            fprintf(out, "(const char*)");
        } else if (at->kind == TYPE_FLOAT32) {
            fprintf(out, "(double)");
        } else if (at->kind == TYPE_INT8 || at->kind == TYPE_INT16 ||
                   at->kind == TYPE_UINT8 || at->kind == TYPE_UINT16) {
            fprintf(out, "(int)");
        }
    }
    emit_expr(e, out);
}

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
        bool is_ptr = is_null_sentinel(type);
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
        if (is_null_sentinel(type))
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
    case PAT_STRING_LIT:
        if (*has_cond) fprintf(out, " && "); else { fprintf(out, "if ("); *has_cond = true; }
        fprintf(out, "fc_eq_fc_str(%s, (fc_str){(uint8_t*)\"%.*s\", %d})",
            expr, pat->string_lit.length, pat->string_lit.value, pat->string_lit.length);
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
            if (is_null_sentinel(type))
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
            emit_indent(out);
            fprintf(out, "(void)%s;\n", vname);
        } else if (s->kind == EXPR_LET_DESTRUCT) {
            /* Emit: struct_type _ds_N = rhs; then recursively emit field bindings */
            emit_type(s->let_destruct.init_type, out);
            fprintf(out, " %s = ", s->let_destruct.tmp_name);
            emit_expr(s->let_destruct.init, out);
            fprintf(out, ";\n");
            emit_pat_bindings(s->let_destruct.pattern, s->let_destruct.tmp_name, s->let_destruct.init_type, out);
            emit_indent(out);
            fprintf(out, "(void)%s;\n", s->let_destruct.tmp_name);
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

/* Helper: C unsigned type name for a signed integer type */
static const char *unsigned_counterpart(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  return "uint8_t";
    case TYPE_INT16: return "uint16_t";
    case TYPE_INT32: return "uint32_t";
    case TYPE_INT64: return "uint64_t";
    case TYPE_ISIZE: return "size_t";
    default: return NULL;
    }
}

/* Helper: shift mask for an integer type's bit width.
 * Returns -1 for platform-dependent types (isize/usize) — caller must handle. */
static int shift_mask_for(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return 7;
    case TYPE_INT16: case TYPE_UINT16: return 15;
    case TYPE_INT32: case TYPE_UINT32: return 31;
    case TYPE_INT64: case TYPE_UINT64: return 63;
    case TYPE_ISIZE: case TYPE_USIZE:  return -1;
    default: return 0;
    }
}

/* Parse width and precision from an InterpSegment format spec string.
 * Sets *width to the explicit width (0 if absent).
 * Sets *precision to the explicit precision (-1 if absent, 0 for ".0"). */
static void parse_format_width_prec(const char *text,
                                     int *width, int *precision) {
    const char *fs = text;
    *width = 0;
    *precision = -1;
    /* Skip flags */
    while (*fs == '-' || *fs == '+' || *fs == '0' ||
           *fs == '#' || *fs == ' ') fs++;
    /* Width */
    while (*fs >= '0' && *fs <= '9') {
        *width = *width * 10 + (*fs - '0');
        fs++;
    }
    /* Precision */
    if (*fs == '.') {
        fs++;
        *precision = 0;
        while (*fs >= '0' && *fs <= '9') {
            *precision = *precision * 10 + (*fs - '0');
            fs++;
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
        else if (e->int_lit.lit_type->kind == TYPE_ISIZE)
            fprintf(out, "((ptrdiff_t)%" PRId64 "LL)", e->int_lit.value);
        else if (e->int_lit.lit_type->kind == TYPE_USIZE)
            fprintf(out, "((size_t)%" PRId64 "ULL)", e->int_lit.value);
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

    case EXPR_STRING_LIT: {
        /* Compute actual byte length after C escape processing */
        int actual_len = 0;
        const char *s = e->string_lit.value;
        int slen = e->string_lit.length;
        for (int j = 0; j < slen; j++) {
            if (s[j] == '%' && j + 1 < slen && s[j+1] == '%') j++;
            else if (s[j] == '\\' && j + 1 < slen) {
                if (s[j+1] == 'x' && j + 3 < slen) j += 3;
                else j++;
            }
            actual_len++;
        }
        fprintf(out, "((fc_str){(uint8_t*)\"%.*s\", %d})",
            e->string_lit.length, e->string_lit.value, actual_len);
        break;
    }

    case EXPR_CSTRING_LIT:
        fprintf(out, "(uint8_t*)\"%.*s\"", e->cstring_lit.length, e->cstring_lit.value);
        break;

    case EXPR_IDENT:
        /* Built-in globals: stdin, stdout, stderr */
        if (e->type && e->type->kind == TYPE_ANY_PTR) {
            const char *n = e->ident.name;
            if (strcmp(n, "stdin") == 0) { fprintf(out, "(void*)stdin"); break; }
            if (strcmp(n, "stdout") == 0) { fprintf(out, "(void*)stdout"); break; }
            if (strcmp(n, "stderr") == 0) { fprintf(out, "(void*)stderr"); break; }
        }
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
        /* Structural equality on complex types */
        if ((e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) && e->binary.left->type) {
            Type *cmp_type = e->binary.left->type;
            /* Resolve type vars through g_subst if active */
            if (g_subst && cmp_type->kind == TYPE_TYPE_VAR) {
                for (int i = 0; i < g_subst->count; i++) {
                    if (g_subst->var_names[i] == cmp_type->type_var.name) {
                        cmp_type = g_subst->concrete[i];
                        break;
                    }
                }
            }
            if (type_needs_eq_func(cmp_type)) {
                if (e->binary.op == TOK_BANGEQ) fprintf(out, "(!");
                emit_eq_func_name(cmp_type, out);
                fprintf(out, "(");
                emit_expr(e->binary.left, out);
                fprintf(out, ", ");
                emit_expr(e->binary.right, out);
                fprintf(out, ")");
                if (e->binary.op == TOK_BANGEQ) fprintf(out, ")");
                break;
            }
        }
        int op = e->binary.op;
        Type *rt = e->type;

        /* Signed overflow wrapping: (int32_t)((uint32_t)a + (uint32_t)b) */
        if ((op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR) &&
            rt && type_is_signed(rt)) {
            const char *ut = unsigned_counterpart(rt);
            fprintf(out, "(");
            emit_type(rt, out);
            fprintf(out, ")(((%s)", ut);
            emit_expr(e->binary.left, out);
            fprintf(out, ") %s ((%s)", op == TOK_PLUS ? "+" : op == TOK_MINUS ? "-" : "*", ut);
            emit_expr(e->binary.right, out);
            fprintf(out, "))");
            break;
        }

        /* Shift masking: a << (b & 31) for 32-bit, etc. */
        if ((op == TOK_LTLT || op == TOK_GTGT) && rt && type_is_integer(rt)) {
            int mask = shift_mask_for(rt);
            /* For platform-dependent types, emit a computed mask */
            const char *mask_expr = NULL;
            char mask_buf[64];
            if (mask < 0) {
                snprintf(mask_buf, sizeof(mask_buf), "((int)(sizeof(size_t)*8)-1)");
                mask_expr = mask_buf;
            }
            if (op == TOK_LTLT && type_is_signed(rt)) {
                /* Signed left shift: also wrap through unsigned */
                const char *ut = unsigned_counterpart(rt);
                fprintf(out, "(");
                emit_type(rt, out);
                fprintf(out, ")(((%s)", ut);
                emit_expr(e->binary.left, out);
                fprintf(out, ") << (");
                emit_expr(e->binary.right, out);
                if (mask_expr) fprintf(out, " & %s))", mask_expr);
                else fprintf(out, " & %d))", mask);
            } else {
                fprintf(out, "(");
                emit_expr(e->binary.left, out);
                fprintf(out, " %s (", op == TOK_LTLT ? "<<" : ">>");
                emit_expr(e->binary.right, out);
                if (mask_expr) fprintf(out, " & %s))", mask_expr);
                else fprintf(out, " & %d))", mask);
            }
            break;
        }

        /* Integer division/modulo by zero check */
        if ((op == TOK_SLASH || op == TOK_PERCENT) && rt && type_is_integer(rt)) {
            int tid = temp_counter++;
            fprintf(out, "({ ");
            emit_type(rt, out);
            fprintf(out, " _dv%d = ", tid);
            emit_expr(e->binary.right, out);
            fprintf(out, "; if (_dv%d == 0) abort(); (", tid);
            emit_expr(e->binary.left, out);
            fprintf(out, ") %s _dv%d; })", op == TOK_SLASH ? "/" : "%", tid);
            break;
        }

        const char *op_str;
        switch (op) {
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
        /* Signed negation wrapping: (int32_t)(-(uint32_t)x) */
        if (e->unary_prefix.op == TOK_MINUS && e->type && type_is_signed(e->type)) {
            const char *ut = unsigned_counterpart(e->type);
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, ")(-((%s)", ut);
            emit_expr(e->unary_prefix.operand, out);
            fprintf(out, "))");
            break;
        }
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
            if (is_null_sentinel(opt_type)) {
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

        /* Get callee function type for coercion */
        Type *call_ft = e->call.func->type;

        if (e->call.is_indirect) {
            /* Indirect call through fat pointer */
            int tid = temp_counter++;
            fprintf(out, "({ ");
            emit_type(call_ft, out);
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

            if (fn_name && !e->call.is_extern_call) {
                fprintf(out, "%s(", fn_name);
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    emit_expr(e->call.args[i], out);
                }
                if (e->call.arg_count > 0) fprintf(out, ", ");
                fprintf(out, "NULL)");
            } else if (fn_name) {
                /* Extern call: cast cstr-aliased params to const char* for C headers,
                 * and cast cstr/cstr? return values from char* back to uint8_t* */
                Type *ret_type = (call_ft && call_ft->kind == TYPE_FUNC)
                    ? call_ft->func.return_type : NULL;
                bool ret_is_cstr = ret_type && is_cstr_type(ret_type);
                bool ret_is_cstr_opt = ret_type && ret_type->kind == TYPE_OPTION &&
                    is_cstr_type(ret_type->option.inner);
                bool ret_is_cstr_ptr = ret_type && ret_type->kind == TYPE_POINTER &&
                    is_cstr_type(ret_type->pointer.pointee);
                if (ret_is_cstr || ret_is_cstr_opt)
                    fprintf(out, "(uint8_t*)");
                else if (ret_is_cstr_ptr)
                    fprintf(out, "(uint8_t**)");
                fprintf(out, "%s(", fn_name);
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    Type *pt = (call_ft && call_ft->kind == TYPE_FUNC && i < call_ft->func.param_count)
                        ? call_ft->func.param_types[i] : NULL;
                    emit_extern_arg(e->call.args[i], pt, out);
                }
                fprintf(out, ")");
            } else {
                /* Fallback: emit normally */
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
        /* str -> cstr: stack copy with null terminator */
        if (e->cast.operand->type && is_str_type(e->cast.operand->type) &&
            is_cstr_type(e->cast.target)) {
            int tid = temp_counter++;
            fprintf(out, "({ fc_str _sc%d = ", tid);
            emit_expr(e->cast.operand, out);
            fprintf(out, "; uint8_t *_cb%d = (uint8_t*)__builtin_alloca((size_t)(_sc%d.len + 1))", tid, tid);
            fprintf(out, "; memcpy(_cb%d, _sc%d.ptr, (size_t)_sc%d.len)", tid, tid, tid);
            fprintf(out, "; _cb%d[_sc%d.len] = '\\0'; (uint8_t*)_cb%d; })", tid, tid, tid);
        /* cstr -> str: wrap pointer with strlen-computed length */
        } else if (e->cast.operand->type && is_cstr_type(e->cast.operand->type) &&
                   is_str_type(e->cast.target)) {
            int tid = temp_counter++;
            bool src_const = e->cast.operand->type->is_const;
            fprintf(out, "({ %suint8_t *_cp%d = ", src_const ? "const " : "", tid);
            emit_expr(e->cast.operand, out);
            fprintf(out, "; (fc_str){ .ptr = %s_cp%d, .len = (int64_t)strlen((const char*)_cp%d) }; })",
                    src_const ? "(uint8_t*)" : "", tid, tid);
        } else {
            fprintf(out, "((");
            emit_type(e->cast.target, out);
            fprintf(out, ")");
            emit_expr(e->cast.operand, out);
            fprintf(out, ")");
        }
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
        /* Type variable property access: 'a.min → resolve via g_subst */
        if (e->field.object->kind == EXPR_TYPE_VAR_REF && g_subst) {
            const char *tv_name = e->field.object->type_var_ref.name;
            Type *concrete = NULL;
            for (int i = 0; i < g_subst->count; i++) {
                if (g_subst->var_names[i] == tv_name) {
                    concrete = g_subst->concrete[i];
                    break;
                }
            }
            if (concrete) {
                const char *cstr = resolve_type_prop_codegen(concrete, e->field.name);
                if (cstr) {
                    fprintf(out, "%s", cstr);
                    break;
                }
                diag_error(e->loc, "type '%s' has no property '%s'",
                    type_name(concrete), e->field.name);
                fprintf(out, "0 /* error */");
                break;
            }
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
        if (obj_type && obj_type->kind == TYPE_SLICE) {
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
        if (is_null_sentinel(e->type)) {
            /* T*?/any*?/cstr? → plain pointer, some(x) = x */
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

    case EXPR_ARRAY_LIT: {
        /* Stack array literal → alloca + slice struct.
         * Uses __builtin_alloca for function-frame lifetime — the backing memory
         * survives until the function returns, unlike a block-scope VLA which
         * the optimizer can reclaim after the statement expression closes.
         * Per spec, N is always a compile-time literal. */
        int tid = temp_counter++;
        fprintf(out, "({ ");
        emit_type(e->array_lit.elem_type, out);
        fprintf(out, " *_arr%d = (", tid);
        emit_type(e->array_lit.elem_type, out);
        fprintf(out, "*)__builtin_alloca((size_t)(");
        emit_expr(e->array_lit.size_expr, out);
        fprintf(out, ") * sizeof(");
        emit_type(e->array_lit.elem_type, out);
        fprintf(out, ")); ");
        if (e->array_lit.elem_count == 0) {
            fprintf(out, "memset(_arr%d, 0, (size_t)(", tid);
            emit_expr(e->array_lit.size_expr, out);
            fprintf(out, ") * sizeof(");
            emit_type(e->array_lit.elem_type, out);
            fprintf(out, ")); ");
        } else {
            for (int i = 0; i < e->array_lit.elem_count; i++) {
                fprintf(out, "_arr%d[%d] = ", tid, i);
                emit_expr(e->array_lit.elems[i], out);
                fprintf(out, "; ");
            }
        }
        fprintf(out, "(");
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
        }
        if (e->type && e->type->kind == TYPE_STRUCT && e->type->struc.c_name) {
            fprintf(out, "(struct %s){ ", e->type->struc.c_name);
        } else {
            fprintf(out, "(%s){ ", sname);
        }
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
            Type *elem_type = iter_type->slice.elem;
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
        case TYPE_ISIZE: case TYPE_USIZE:
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
            fprintf(out, "NULL");
            break;
        case TYPE_OPTION:
            if (is_null_sentinel(t))
                fprintf(out, "NULL");
            else {
                fprintf(out, "(");
                emit_type(t, out);
                fprintf(out, "){ .has_value = false }");
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
        if (ot && ot->kind == TYPE_SLICE) {
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
        } else if (e->alloc_expr.init_expr->type &&
                   e->alloc_expr.init_expr->type->kind == TYPE_SLICE) {
            /* alloc(slice_expr) → T[]? (deep-copy slice data to heap) */
            int tid = temp_counter++;
            Type *slice_type = e->alloc_expr.init_expr->type;
            Type *elem_type = slice_type->slice.elem;
            fprintf(out, "({ ");
            emit_type(slice_type, out);
            fprintf(out, " _as%d = ", tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; ");
            emit_type(elem_type, out);
            fprintf(out, " *_ap%d = (", tid);
            emit_type(elem_type, out);
            fprintf(out, "*)malloc((size_t)_as%d.len * sizeof(", tid);
            emit_type(elem_type, out);
            fprintf(out, ")); ");
            fprintf(out, "_ap%d ? (memcpy(_ap%d, _as%d.ptr, (size_t)_as%d.len * sizeof(",
                tid, tid, tid, tid);
            emit_type(elem_type, out);
            fprintf(out, ")), (");
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = _ap%d, .len = _as%d.len }, .has_value = true }) : (",
                tid, tid);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
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
            if (obj_type && obj_type->kind == TYPE_SLICE) {
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

    case EXPR_INTERP_STRING: {
        /* Compute buffer size and build format string */
        int tid = temp_counter++;
        int seg_count = e->interp_string.segment_count;
        InterpSegment *segs = e->interp_string.segments;

        /* Always use alloca — VLAs in statement expressions have block scope,
         * but alloca memory survives until the function returns */
        (void)seg_count; /* may be used only in loops below */

        fprintf(out, "({ ");

        /* Pre-evaluate str arguments and compute their lengths */
        int str_arg_idx = 0;
        for (int i = 0; i < seg_count; i++) {
            if (!segs[i].is_literal && segs[i].conversion == 's' &&
                segs[i].expr->type && is_str_type(segs[i].expr->type)) {
                fprintf(out, "fc_str _sa%d_%d = ", tid, str_arg_idx);
                emit_expr(segs[i].expr, out);
                fprintf(out, "; ");
                str_arg_idx++;
            }
        }

        /* Compute buffer size expression */
        fprintf(out, "int64_t _flen%d = ", tid);
        bool first_term = true;
        str_arg_idx = 0;
        for (int i = 0; i < seg_count; i++) {
            if (segs[i].is_literal) {
                /* Count actual bytes after escape processing */
                int actual_len = 0;
                const char *s = segs[i].text;
                int slen = segs[i].text_length;
                for (int j = 0; j < slen; j++) {
                    if (s[j] == '%' && j + 1 < slen && s[j+1] == '%') j++; /* %% -> 1 byte */
                    else if (s[j] == '\\' && j + 1 < slen) {
                        if (s[j+1] == 'x' && j + 3 < slen) j += 3; /* \xNN -> 1 byte */
                        else j++; /* \n etc -> 1 byte */
                    }
                    actual_len++;
                }
                if (actual_len > 0) {
                    if (!first_term) fprintf(out, " + ");
                    fprintf(out, "%d", actual_len);
                    first_term = false;
                }
            } else {
                int bound = 0;
                char conv = segs[i].conversion;

                int explicit_width = 0, explicit_prec = -1;
                parse_format_width_prec(segs[i].text,
                                        &explicit_width, &explicit_prec);

                bool is_str_arg = (conv == 's' && segs[i].expr->type &&
                                   is_str_type(segs[i].expr->type));
                bool is_cstr_arg = (conv == 's' && segs[i].expr->type &&
                                    is_cstr_type(segs[i].expr->type));

                if (is_str_arg) {
                    if (!first_term) fprintf(out, " + ");
                    if (explicit_prec >= 0) {
                        /* Precision bounds the output — compile-time constant */
                        int str_bound = explicit_prec;
                        if (explicit_width > str_bound) str_bound = explicit_width;
                        fprintf(out, "%d", str_bound);
                    } else if (explicit_width > 0) {
                        fprintf(out, "(%d > _sa%d_%d.len ? %d : _sa%d_%d.len)",
                            explicit_width, tid, str_arg_idx, explicit_width, tid, str_arg_idx);
                    } else {
                        fprintf(out, "_sa%d_%d.len", tid, str_arg_idx);
                    }
                    first_term = false;
                    str_arg_idx++;
                } else if (is_cstr_arg) {
                    if (!first_term) fprintf(out, " + ");
                    if (explicit_prec >= 0) {
                        /* Precision bounds the output — compile-time constant */
                        int str_bound = explicit_prec;
                        if (explicit_width > str_bound) str_bound = explicit_width;
                        fprintf(out, "%d", str_bound);
                    } else {
                        /* cstr: need strlen at runtime */
                        fprintf(out, "(int64_t)strlen((const char*)");
                        emit_expr(segs[i].expr, out);
                        fprintf(out, ")");
                    }
                    first_term = false;
                } else if (conv == 'T') {
                    /* %T: compile-time type name literal */
                    const char *tname = type_name(segs[i].expr->type);
                    bound = (int)strlen(tname);
                    if (!first_term) fprintf(out, " + ");
                    fprintf(out, "%d", bound);
                    first_term = false;
                } else {
                    /* Compute compile-time bound based on type and conversion */
                    Type *t = segs[i].expr->type;
                    switch (conv) {
                    case 'd': case 'i': case 'u':
                        switch (t ? t->kind : 0) {
                        case TYPE_INT8: bound = 4; break;
                        case TYPE_UINT8: bound = 3; break;
                        case TYPE_INT16: bound = 6; break;
                        case TYPE_UINT16: bound = 5; break;
                        case TYPE_INT32: bound = 11; break;
                        case TYPE_UINT32: bound = 10; break;
                        case TYPE_INT64: bound = 20; break;
                        case TYPE_UINT64: bound = 20; break;
                        default: bound = 20; break;
                        }
                        break;
                    case 'x': case 'X':
                        switch (t ? t->kind : 0) {
                        case TYPE_INT8: case TYPE_UINT8: bound = 2; break;
                        case TYPE_INT16: case TYPE_UINT16: bound = 4; break;
                        case TYPE_INT32: case TYPE_UINT32: bound = 8; break;
                        case TYPE_INT64: case TYPE_UINT64: bound = 16; break;
                        default: bound = 16; break;
                        }
                        break;
                    case 'o':
                        switch (t ? t->kind : 0) {
                        case TYPE_INT8: case TYPE_UINT8: bound = 3; break;
                        case TYPE_INT16: case TYPE_UINT16: bound = 6; break;
                        case TYPE_INT32: case TYPE_UINT32: bound = 11; break;
                        case TYPE_INT64: case TYPE_UINT64: bound = 22; break;
                        default: bound = 22; break;
                        }
                        break;
                    case 'f':
                        bound = explicit_width > 0 ? explicit_width : 24;
                        break;
                    case 'e': case 'E': case 'g': case 'G':
                        bound = (t && t->kind == TYPE_FLOAT32) ? 15 : 24;
                        break;
                    case 'c':
                        bound = 1;
                        break;
                    case 'p':
                        bound = 18;
                        break;
                    default:
                        bound = 24;
                        break;
                    }

                    /* Account for flags: + or space adds 1, # adds 2 for 0x */
                    const char *flags = segs[i].text;
                    while (*flags == '-' || *flags == '+' || *flags == '0' || *flags == '#' || *flags == ' ') {
                        if (*flags == '+' || *flags == ' ') bound++;
                        if (*flags == '#') bound += 2;
                        flags++;
                    }

                    if (explicit_width > bound) bound = explicit_width;

                    if (!first_term) fprintf(out, " + ");
                    fprintf(out, "%d", bound);
                    first_term = false;
                }
            }
        }
        if (first_term) fprintf(out, "0");
        fprintf(out, "; ");

        /* Allocate buffer with alloca (survives until function return) */
        fprintf(out, "uint8_t *_fbuf%d = (uint8_t*)__builtin_alloca((size_t)(_flen%d + 1)); ", tid, tid);

        /* Build snprintf call */
        fprintf(out, "int _fw%d = snprintf((char*)_fbuf%d, (size_t)(_flen%d + 1), \"",
            tid, tid, tid);

        /* Emit format string */
        str_arg_idx = 0;
        for (int i = 0; i < seg_count; i++) {
            if (segs[i].is_literal) {
                /* Emit literal text, escaping % to %% for snprintf */
                const char *s = segs[i].text;
                int slen = segs[i].text_length;
                for (int j = 0; j < slen; j++) {
                    if (s[j] == '%') {
                        fprintf(out, "%%%%");
                        if (j + 1 < slen && s[j+1] == '%') j++; /* skip second % of %% */
                    } else if (s[j] == '"') {
                        fprintf(out, "\\\"");
                    } else {
                        fputc(s[j], out);
                    }
                }
            } else if (segs[i].conversion == 'T') {
                /* Emit type name as literal text in the format string */
                const char *tname = type_name(segs[i].expr->type);
                fputs(tname, out);
            } else {
                bool is_str_arg = (segs[i].conversion == 's' &&
                                   segs[i].expr->type && is_str_type(segs[i].expr->type));
                if (is_str_arg) {
                    /* Rewrite %s to %.*s for str arguments.
                     * Strip any .precision from the spec since .*
                     * replaces it (precision passed as argument). */
                    fprintf(out, "%%");
                    const char *sp = segs[i].text;
                    int splen = segs[i].text_length;
                    int j = 0;
                    /* Copy flags */
                    while (j < splen - 1 && (sp[j] == '-' || sp[j] == '+' ||
                           sp[j] == '0' || sp[j] == '#' || sp[j] == ' ')) {
                        fputc(sp[j], out);
                        j++;
                    }
                    /* Copy width */
                    while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') {
                        fputc(sp[j], out);
                        j++;
                    }
                    /* Skip .precision if present (replaced by .*) */
                    if (j < splen - 1 && sp[j] == '.') {
                        j++;
                        while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') j++;
                    }
                    fprintf(out, ".*s");
                    str_arg_idx++;
                } else {
                    /* Emit %<spec> with appropriate length modifier for 64-bit types */
                    Type *t = segs[i].expr->type;
                    bool is_64bit = t && (t->kind == TYPE_INT64 || t->kind == TYPE_UINT64 || t->kind == TYPE_ISIZE || t->kind == TYPE_USIZE);
                    const char *sp = segs[i].text;
                    int splen = segs[i].text_length;
                    fprintf(out, "%%");
                    if (is_64bit) {
                        /* Insert "ll" before the conversion character */
                        for (int j = 0; j < splen - 1; j++) fputc(sp[j], out);
                        fprintf(out, "ll%c", sp[splen - 1]);
                    } else {
                        fwrite(sp, 1, (size_t)splen, out);
                    }
                }
            }
        }
        fprintf(out, "\"");

        /* Emit arguments */
        str_arg_idx = 0;
        for (int i = 0; i < seg_count; i++) {
            if (segs[i].is_literal) continue;
            if (segs[i].conversion == 'T') continue; /* type name is literal, no arg */
            fprintf(out, ", ");
            bool is_str_arg = (segs[i].conversion == 's' &&
                               segs[i].expr->type && is_str_type(segs[i].expr->type));
            if (is_str_arg) {
                /* %.*s needs (int)precision, ptr */
                int prec_w = 0, prec_p = -1;
                parse_format_width_prec(segs[i].text, &prec_w, &prec_p);
                if (prec_p >= 0) {
                    /* Clamp to min(precision, len) to avoid reading past str data */
                    fprintf(out, "((int)_sa%d_%d.len < %d ? (int)_sa%d_%d.len : %d), _sa%d_%d.ptr",
                        tid, str_arg_idx, prec_p, tid, str_arg_idx, prec_p, tid, str_arg_idx);
                } else {
                    fprintf(out, "(int)_sa%d_%d.len, _sa%d_%d.ptr",
                        tid, str_arg_idx, tid, str_arg_idx);
                }
                str_arg_idx++;
            } else {
                /* Cast arguments to match the printf format specifier */
                Type *t = segs[i].expr->type;
                char conv = segs[i].conversion;
                bool is_64bit = t && (t->kind == TYPE_INT64 || t->kind == TYPE_UINT64 || t->kind == TYPE_ISIZE || t->kind == TYPE_USIZE);
                if (t && type_is_integer(t)) {
                    if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                        fprintf(out, is_64bit ? "(unsigned long long)" : "(unsigned int)");
                    } else {
                        fprintf(out, is_64bit ? "(long long)" : "(int)");
                    }
                } else if (t && t->kind == TYPE_FLOAT32) {
                    fprintf(out, "(double)");
                } else if (t && is_cstr_type(t)) {
                    /* cstr (uint8*) check before generic pointer — needs (const char*) for %s */
                    if (conv == 'p')
                        fprintf(out, "(void*)");
                    else
                        fprintf(out, "(const char*)");
                } else if (t && (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR)) {
                    fprintf(out, "(void*)");
                }
                emit_expr(segs[i].expr, out);
            }
        }
        fprintf(out, "); ");

        /* Use int64 format types for snprintf format string */
        /* For int64/uint64, we need to use format macros - let's fix the format string */
        /* Actually, we've already cast all args above, so %d, %u, %x etc will work with int/unsigned/long long */

        /* Construct result fc_str */
        fprintf(out, "(fc_str){ .ptr = _fbuf%d, .len = _fw%d >= 0 && _fw%d <= _flen%d ? _fw%d : _flen%d }; })",
            tid, tid, tid, tid, tid, tid);
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
        /* Emit the FC main body as fc_main(str[] args) */
        fprintf(out, "int32_t fc_main(");
        emit_type(fn->func.params[0].type, out);
        fprintf(out, " %s) {\n", fn->func.params[0].name);
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

    /* Emit C main wrapper that converts argc/argv to str[] */
    if (is_main) {
        fprintf(out, "int main(int argc, char **argv) {\n");
        fprintf(out, "    fc_str *_args = (fc_str*)__builtin_alloca((size_t)argc * sizeof(fc_str));\n");
        fprintf(out, "    for (int _i = 0; _i < argc; _i++) {\n");
        fprintf(out, "        _args[_i].ptr = (uint8_t*)argv[_i];\n");
        fprintf(out, "        _args[_i].len = (int64_t)strlen(argv[_i]);\n");
        fprintf(out, "    }\n");
        fprintf(out, "    return fc_main((");
        emit_type(fn->func.params[0].type, out);
        fprintf(out, "){ .ptr = _args, .len = (int64_t)argc });\n");
        fprintf(out, "}\n\n");
    }
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

struct TypeSet {
    Type **types;
    int count;
    int cap;
};

static bool typeset_contains(TypeSet *ts, Type *t) {
    for (int i = 0; i < ts->count; i++) {
        if (type_eq_ignore_const(ts->types[i], t)) return true;
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
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;  /* still has unresolved type vars */
    if (t->kind == TYPE_SLICE) {
        collect_types_in_type(t->slice.elem, slices, options, fns);
        typeset_add(slices, t);
    } else if (t->kind == TYPE_OPTION) {
        /* Recurse into inner type FIRST so dependencies are emitted before this type */
        collect_types_in_type(t->option.inner, slices, options, fns);
        /* Only non-pointer options need typedefs */
        if (!t->option.inner || t->option.inner->kind != TYPE_POINTER) {
            typeset_add(options, t);
        }
    } else if (t->kind == TYPE_FUNC) {
        /* Recurse into param/return types FIRST so dependencies are emitted before this type */
        for (int i = 0; i < t->func.param_count; i++)
            collect_types_in_type(t->func.param_types[i], slices, options, fns);
        collect_types_in_type(t->func.return_type, slices, options, fns);
        typeset_add(fns, t);
    }
}

/* Resolve struct/union stub types (name only, 0 fields) to full definitions */
static Type *resolve_struct_stub(Type *t) {
    if (!g_symtab) return t;
    /* Parser creates all user-defined type references as TYPE_STRUCT stubs
       (field_count=0), even for unions. Resolve via symbol table or mono table. */
    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0) {
        Symbol *sym = symtab_lookup(g_symtab, t->struc.name);
        if (sym && sym->type) {
            if ((sym->type->kind == TYPE_STRUCT && sym->type->struc.field_count > 0) ||
                (sym->type->kind == TYPE_UNION && sym->type->unio.variant_count > 0))
                return sym->type;
        }
        if (g_mono) {
            for (int i = 0; i < g_mono->count; i++) {
                if (g_mono->entries[i].mangled_name == t->struc.name &&
                    g_mono->entries[i].concrete_type)
                    return g_mono->entries[i].concrete_type;
            }
        }
    }
    if (t->kind == TYPE_UNION && t->unio.variant_count == 0) {
        Symbol *sym = symtab_lookup(g_symtab, t->unio.name);
        if (sym && sym->type) {
            if ((sym->type->kind == TYPE_UNION && sym->type->unio.variant_count > 0) ||
                (sym->type->kind == TYPE_STRUCT && sym->type->struc.field_count > 0))
                return sym->type;
        }
        if (g_mono) {
            for (int i = 0; i < g_mono->count; i++) {
                if (g_mono->entries[i].mangled_name == t->unio.name &&
                    g_mono->entries[i].concrete_type)
                    return g_mono->entries[i].concrete_type;
            }
        }
    }
    return t;
}

/* Recursively collect types that need generated eq functions */
static void collect_eq_types(Type *t, TypeSet *eqs) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;
    if (!type_needs_eq_func(t)) return;
    t = resolve_struct_stub(t);
    /* Strip const for eq function collection — constness doesn't affect equality */
    if (t->is_const) {
        Type *nc = arena_alloc(g_arena, sizeof(Type));
        *nc = *t;
        nc->is_const = false;
        t = nc;
    }
    if (typeset_contains(eqs, t)) return;
    typeset_add(eqs, t);
    switch (t->kind) {
    case TYPE_STRUCT:
        for (int i = 0; i < t->struc.field_count; i++)
            collect_eq_types(t->struc.fields[i].type, eqs);
        break;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload)
                collect_eq_types(t->unio.variants[i].payload, eqs);
        break;
    case TYPE_SLICE:
        collect_eq_types(t->slice.elem, eqs);
        break;
    case TYPE_OPTION:
        collect_eq_types(t->option.inner, eqs);
        break;
    default:
        break;
    }
}

/* Walk patterns looking for PAT_STRING_LIT to register str eq */
static void collect_eq_from_pattern(Pattern *pat, TypeSet *eqs) {
    if (!pat) return;
    switch (pat->kind) {
    case PAT_STRING_LIT:
        collect_eq_types(type_str(), eqs);
        break;
    case PAT_SOME:
        if (pat->some_pat.inner) collect_eq_from_pattern(pat->some_pat.inner, eqs);
        break;
    case PAT_VARIANT:
        if (pat->variant.payload) collect_eq_from_pattern(pat->variant.payload, eqs);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            collect_eq_from_pattern(pat->struc.fields[i].pattern, eqs);
        break;
    default:
        break;
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!e) return;
    collect_types_in_type(e->type, slices, options, fns);

    switch (e->kind) {
    case EXPR_BINARY:
        collect_types_expr(e->binary.left, slices, options, fns);
        collect_types_expr(e->binary.right, slices, options, fns);
        if (g_eq_set && (e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ)) {
            Type *cmp_type = e->binary.left->type;
            if (cmp_type) collect_eq_types(cmp_type, g_eq_set);
        }
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
            if (g_eq_set)
                collect_eq_from_pattern(e->match_expr.arms[i].pattern, g_eq_set);
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
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (!e->interp_string.segments[i].is_literal)
                collect_types_expr(e->interp_string.segments[i].expr, slices, options, fns);
        }
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

/* ---- Trampoline set ---- */
/* Tracks FC functions that need C-compatible trampolines for extern call boundaries.
 * A trampoline wraps an FC function (which has an extra void* _ctx param) into a
 * plain C function pointer compatible with the extern's expected signature. */

typedef struct {
    const char *name;   /* C function name (codegen_name or lifted_name) */
    Type *type;         /* TYPE_FUNC — the function's type */
} TrampolineEntry;

typedef struct {
    TrampolineEntry *entries;
    int count;
    int cap;
} TrampolineSet;

static bool trampolineset_contains(TrampolineSet *ts, const char *name) {
    for (int i = 0; i < ts->count; i++) {
        if (strcmp(ts->entries[i].name, name) == 0) return true;
    }
    return false;
}

static void trampolineset_add(TrampolineSet *ts, const char *name, Type *type) {
    if (!trampolineset_contains(ts, name)) {
        TrampolineEntry entry = { name, type };
        DA_APPEND(ts->entries, ts->count, ts->cap, entry);
    }
}

static void collect_trampolines_expr(Expr *e, TrampolineSet *ts);

static void collect_trampolines_expr(Expr *e, TrampolineSet *ts) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_CALL:
        /* Check if this is an extern call with function-type arguments */
        if (e->call.is_extern_call) {
            Type *call_ft = e->call.func->type;
            for (int i = 0; i < e->call.arg_count; i++) {
                Type *pt = (call_ft && call_ft->kind == TYPE_FUNC && i < call_ft->func.param_count)
                    ? call_ft->func.param_types[i] : NULL;
                if (!pt || pt->kind != TYPE_FUNC) continue;
                Expr *arg = e->call.args[i];
                if (arg->kind == EXPR_IDENT && !arg->ident.is_local && arg->type &&
                    arg->type->kind == TYPE_FUNC) {
                    /* Top-level function passed at extern boundary */
                    const char *fname = arg->ident.codegen_name
                        ? arg->ident.codegen_name : arg->ident.name;
                    trampolineset_add(ts, fname, arg->type);
                } else if (arg->kind == EXPR_FUNC && arg->func.capture_count == 0 &&
                           arg->func.lifted_name && arg->type &&
                           arg->type->kind == TYPE_FUNC) {
                    /* Non-capturing lambda passed at extern boundary */
                    trampolineset_add(ts, arg->func.lifted_name, arg->type);
                }
            }
        }
        collect_trampolines_expr(e->call.func, ts);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_trampolines_expr(e->call.args[i], ts);
        break;
    case EXPR_BINARY:
        collect_trampolines_expr(e->binary.left, ts);
        collect_trampolines_expr(e->binary.right, ts);
        break;
    case EXPR_UNARY_PREFIX:
        collect_trampolines_expr(e->unary_prefix.operand, ts);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_trampolines_expr(e->unary_postfix.operand, ts);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_trampolines_expr(e->field.object, ts);
        break;
    case EXPR_INDEX:
        collect_trampolines_expr(e->index.object, ts);
        collect_trampolines_expr(e->index.index, ts);
        break;
    case EXPR_SLICE:
        collect_trampolines_expr(e->slice.object, ts);
        if (e->slice.lo) collect_trampolines_expr(e->slice.lo, ts);
        if (e->slice.hi) collect_trampolines_expr(e->slice.hi, ts);
        break;
    case EXPR_IF:
        collect_trampolines_expr(e->if_expr.cond, ts);
        collect_trampolines_expr(e->if_expr.then_body, ts);
        if (e->if_expr.else_body) collect_trampolines_expr(e->if_expr.else_body, ts);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_trampolines_expr(e->block.stmts[i], ts);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_trampolines_expr(e->func.body[i], ts);
        break;
    case EXPR_LET:
        collect_trampolines_expr(e->let_expr.let_init, ts);
        break;
    case EXPR_LET_DESTRUCT:
        collect_trampolines_expr(e->let_destruct.init, ts);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_trampolines_expr(e->return_expr.value, ts);
        break;
    case EXPR_ASSIGN:
        collect_trampolines_expr(e->assign.target, ts);
        collect_trampolines_expr(e->assign.value, ts);
        break;
    case EXPR_CAST:
        collect_trampolines_expr(e->cast.operand, ts);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_trampolines_expr(e->struct_lit.fields[i].value, ts);
        break;
    case EXPR_SOME:
        collect_trampolines_expr(e->some_expr.value, ts);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_trampolines_expr(e->array_lit.elems[i], ts);
        break;
    case EXPR_MATCH:
        collect_trampolines_expr(e->match_expr.subject, ts);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_trampolines_expr(e->match_expr.arms[i].body[j], ts);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_trampolines_expr(e->loop_expr.body[i], ts);
        break;
    case EXPR_FOR:
        collect_trampolines_expr(e->for_expr.iter, ts);
        if (e->for_expr.range_end) collect_trampolines_expr(e->for_expr.range_end, ts);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_trampolines_expr(e->for_expr.body[i], ts);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_trampolines_expr(e->break_expr.value, ts);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_trampolines_expr(e->alloc_expr.size_expr, ts);
        if (e->alloc_expr.init_expr) collect_trampolines_expr(e->alloc_expr.init_expr, ts);
        break;
    case EXPR_FREE:
        collect_trampolines_expr(e->free_expr.operand, ts);
        break;
    default:
        break;
    }
}

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

/* ---- Eq function generation ---- */

static void emit_eq_func_name(Type *t, FILE *out) {
    /* Strip const — equality semantics don't depend on constness */
    Type tmp;
    if (t->is_const) { tmp = *t; tmp.is_const = false; t = &tmp; }
    fprintf(out, "fc_eq_");
    emit_type_ident(t, out);
}

/* Returns true if the function type references any struct/union/option-of-struct/union
   by value in params or return — such typedefs must be emitted after struct defs. */
static bool fn_type_uses_struct_option(Type *f) {
    Type *rt = f->func.return_type;
    if (rt) {
        if (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) return true;
        if (rt->kind == TYPE_OPTION && rt->option.inner &&
            (rt->option.inner->kind == TYPE_STRUCT || rt->option.inner->kind == TYPE_UNION))
            return true;
    }
    for (int i = 0; i < f->func.param_count; i++) {
        Type *pt = f->func.param_types[i];
        if (pt->kind == TYPE_STRUCT || pt->kind == TYPE_UNION) return true;
        if (pt->kind == TYPE_OPTION && pt->option.inner &&
            (pt->option.inner->kind == TYPE_STRUCT || pt->option.inner->kind == TYPE_UNION))
            return true;
    }
    return false;
}

static void emit_eq_forward(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b);\n");
}

/* Emit the comparison expression for two values of type t.
   a_prefix/b_prefix are "a"/"b" (or "a.field"/"b.field"). */
static void emit_value_eq(Type *t, const char *a_expr, const char *b_expr, FILE *out) {
    if (type_needs_eq_func(t)) {
        emit_eq_func_name(t, out);
        fprintf(out, "(%s, %s)", a_expr, b_expr);
    } else {
        fprintf(out, "%s == %s", a_expr, b_expr);
    }
}

static void emit_eq_func(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b) {\n");

    t = resolve_struct_stub(t);
    switch (t->kind) {
    case TYPE_STRUCT: {
        int fc = t->struc.field_count;
        if (fc == 0) {
            fprintf(out, "    (void)a; (void)b;\n    return true;\n");
        } else {
            fprintf(out, "    return ");
            for (int i = 0; i < fc; i++) {
                if (i > 0) fprintf(out, " && ");
                char a_buf[256], b_buf[256];
                snprintf(a_buf, sizeof(a_buf), "a.%s", t->struc.fields[i].name);
                snprintf(b_buf, sizeof(b_buf), "b.%s", t->struc.fields[i].name);
                emit_value_eq(t->struc.fields[i].type, a_buf, b_buf, out);
            }
            fprintf(out, ";\n");
        }
        break;
    }
    case TYPE_UNION: {
        /* Check if any variant has a payload */
        bool has_payload = false;
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload) { has_payload = true; break; }

        if (!has_payload) {
            /* Tag-only union — just compare tags */
            fprintf(out, "    return a.tag == b.tag;\n");
        } else {
            fprintf(out, "    if (a.tag != b.tag) return false;\n");
            fprintf(out, "    switch (a.tag) {\n");
            const char *uname = t->unio.name;
            for (int i = 0; i < t->unio.variant_count; i++) {
                fprintf(out, "    case %s_tag_%s: ", uname, t->unio.variants[i].name);
                if (t->unio.variants[i].payload) {
                    char a_buf[256], b_buf[256];
                    snprintf(a_buf, sizeof(a_buf), "a.%s", t->unio.variants[i].name);
                    snprintf(b_buf, sizeof(b_buf), "b.%s", t->unio.variants[i].name);
                    fprintf(out, "return ");
                    emit_value_eq(t->unio.variants[i].payload, a_buf, b_buf, out);
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "return true;\n");
                }
            }
            fprintf(out, "    }\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_SLICE: {
        Type *elem = t->slice.elem;
        fprintf(out, "    if (a.len != b.len) return false;\n");
        fprintf(out, "    if (a.len == 0) return true;\n");
        /* Use memcmp for non-float primitives that don't need eq funcs */
        if (!type_needs_eq_func(elem) && !type_is_float(elem)) {
            fprintf(out, "    return memcmp(a.ptr, b.ptr, (size_t)a.len * sizeof(");
            emit_type(elem, out);
            fprintf(out, ")) == 0;\n");
        } else {
            fprintf(out, "    for (int64_t _i = 0; _i < a.len; _i++)\n");
            fprintf(out, "        if (!");
            if (type_needs_eq_func(elem)) {
                emit_eq_func_name(elem, out);
                fprintf(out, "(a.ptr[_i], b.ptr[_i])");
            } else {
                /* float — use C == */
                fprintf(out, "(a.ptr[_i] == b.ptr[_i])");
            }
            fprintf(out, ") return false;\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_OPTION:
        fprintf(out, "    if (a.has_value != b.has_value) return false;\n");
        fprintf(out, "    if (!a.has_value) return true;\n");
        fprintf(out, "    return ");
        emit_value_eq(t->option.inner, "a.value", "b.value", out);
        fprintf(out, ";\n");
        break;
    case TYPE_FUNC:
        fprintf(out, "    return a.fn_ptr == b.fn_ptr && a.ctx == b.ctx;\n");
        break;
    default:
        fprintf(out, "    return false; /* unsupported type */\n");
        break;
    }

    fprintf(out, "}\n");
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

/* Recursively collect unique from_lib strings from module declarations */
static void collect_from_libs(Decl *d, const char **seen, int *count, int cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.from_lib) {
        for (int i = 0; i < *count; i++)
            if (strcmp(seen[i], d->module.from_lib) == 0) return;
        if (*count < cap)
            seen[(*count)++] = d->module.from_lib;
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_from_libs(d->module.decls[i], seen, count, cap);
}

/* ---- Define collection ---- */
/* Tracks #define macros from module `define` annotations */
typedef struct {
    const char *macro;
    const char *value;
} CDefine;

static void collect_defines(Decl *d, CDefine *defs, int *count, int cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.define_macro) {
        for (int i = 0; i < *count; i++) {
            if (strcmp(defs[i].macro, d->module.define_macro) == 0) {
                if (strcmp(defs[i].value, d->module.define_value) != 0) {
                    diag_error(d->loc,
                        "conflicting define for '%s': '%s' vs '%s'",
                        d->module.define_macro, defs[i].value, d->module.define_value);
                }
                return;
            }
        }
        if (*count < cap) {
            defs[*count].macro = d->module.define_macro;
            defs[*count].value = d->module.define_value;
            (*count)++;
        }
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_defines(d->module.decls[i], defs, count, cap);
}

/* ---- Feature detection ---- */
/* Lightweight AST scan to determine which feature-gated headers are needed */
static bool g_needs_stdio;
static bool g_needs_math;
static bool g_needs_float;

static void detect_features_expr(Expr *e) {
    if (!e) return;
    if (g_needs_stdio && g_needs_math && g_needs_float) return; /* all found */

    switch (e->kind) {
    case EXPR_INTERP_STRING:
        g_needs_stdio = true;
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (e->interp_string.segments[i].expr)
                detect_features_expr(e->interp_string.segments[i].expr);
        }
        return;

    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        /* Direct float type properties resolved by pass2 */
        if (e->field.codegen_name) {
            const char *cn = e->field.codegen_name;
            if (strstr(cn, "NAN") || strstr(cn, "INFINITY"))
                g_needs_math = true;
            if (strstr(cn, "FLT_") || strstr(cn, "DBL_"))
                g_needs_float = true;
        }
        /* Type variable property access — conservatively check property name */
        if (e->field.object && e->field.object->kind == EXPR_TYPE_VAR_REF) {
            const char *prop = e->field.name;
            if (strcmp(prop, "nan") == 0 || strcmp(prop, "inf") == 0 ||
                strcmp(prop, "neg_inf") == 0)
                g_needs_math = true;
            if (strcmp(prop, "min") == 0 || strcmp(prop, "max") == 0 ||
                strcmp(prop, "epsilon") == 0)
                g_needs_float = true;
        }
        detect_features_expr(e->field.object);
        return;

    case EXPR_BINARY:
        detect_features_expr(e->binary.left);
        detect_features_expr(e->binary.right);
        return;
    case EXPR_UNARY_PREFIX:
    case EXPR_UNARY_POSTFIX:
        detect_features_expr(e->unary_prefix.operand);
        return;
    case EXPR_CALL:
        detect_features_expr(e->call.func);
        for (int i = 0; i < e->call.arg_count; i++)
            detect_features_expr(e->call.args[i]);
        return;
    case EXPR_CAST:
        detect_features_expr(e->cast.operand);
        return;
    case EXPR_IF:
        detect_features_expr(e->if_expr.cond);
        detect_features_expr(e->if_expr.then_body);
        detect_features_expr(e->if_expr.else_body);
        return;
    case EXPR_MATCH:
        detect_features_expr(e->match_expr.subject);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                detect_features_expr(e->match_expr.arms[i].body[j]);
        }
        return;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            detect_features_expr(e->block.stmts[i]);
        return;
    case EXPR_LET:
        detect_features_expr(e->let_expr.let_init);
        return;
    case EXPR_LET_DESTRUCT:
        detect_features_expr(e->let_destruct.init);
        return;
    case EXPR_ASSIGN:
        detect_features_expr(e->assign.target);
        detect_features_expr(e->assign.value);
        return;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            detect_features_expr(e->loop_expr.body[i]);
        return;
    case EXPR_FOR:
        detect_features_expr(e->for_expr.iter);
        if (e->for_expr.range_end) detect_features_expr(e->for_expr.range_end);
        for (int i = 0; i < e->for_expr.body_count; i++)
            detect_features_expr(e->for_expr.body[i]);
        return;
    case EXPR_RETURN:
        detect_features_expr(e->return_expr.value);
        return;
    case EXPR_BREAK:
        detect_features_expr(e->break_expr.value);
        return;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            detect_features_expr(e->func.body[i]);
        return;
    case EXPR_SOME:
        detect_features_expr(e->some_expr.value);
        return;
    case EXPR_ALLOC:
        detect_features_expr(e->alloc_expr.init_expr);
        detect_features_expr(e->alloc_expr.size_expr);
        return;
    case EXPR_FREE:
        detect_features_expr(e->free_expr.operand);
        return;
    case EXPR_INDEX:
        detect_features_expr(e->index.object);
        detect_features_expr(e->index.index);
        return;
    case EXPR_SLICE:
        detect_features_expr(e->slice.object);
        detect_features_expr(e->slice.lo);
        detect_features_expr(e->slice.hi);
        return;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            detect_features_expr(e->struct_lit.fields[i].value);
        return;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            detect_features_expr(e->array_lit.elems[i]);
        return;
    case EXPR_IDENT:
        /* Built-in globals stdin/stdout/stderr require stdio.h */
        if (e->ident.name &&
            (strcmp(e->ident.name, "stdin") == 0 ||
             strcmp(e->ident.name, "stdout") == 0 ||
             strcmp(e->ident.name, "stderr") == 0))
            g_needs_stdio = true;
        return;
    case EXPR_SIZEOF:
    case EXPR_DEFAULT:
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_CONTINUE:
    case EXPR_TYPE_VAR_REF:
        return;
    }
}

static void detect_features_decl(Decl *d) {
    if (!d) return;
    switch (d->kind) {
    case DECL_LET: {
        Expr *fn = d->let.init;
        if (fn && fn->kind == EXPR_FUNC) {
            for (int i = 0; i < fn->func.body_count; i++)
                detect_features_expr(fn->func.body[i]);
        } else if (fn) {
            detect_features_expr(fn);
        }
        break;
    }
    case DECL_MODULE:
        for (int i = 0; i < d->module.decl_count; i++)
            detect_features_decl(d->module.decls[i]);
        break;
    default:
        break;
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

    /* Collect from_libs and defines from extern module declarations */
    const char *from_libs[64];
    int from_lib_count = 0;
    CDefine defines[64];
    int define_count = 0;
    for (int i = 0; i < prog->decl_count; i++) {
        collect_from_libs(prog->decls[i], from_libs, &from_lib_count, 64);
        collect_defines(prog->decls[i], defines, &define_count, 64);
    }

    /* Flatten module decls into a single array (needed for feature detection) */
    Decl **all_decls;
    int all_count;
    collect_all_decls(prog, &all_decls, &all_count);

    /* Detect which feature-gated headers are needed */
    g_needs_stdio = false;
    g_needs_math = false;
    g_needs_float = false;
    for (int i = 0; i < all_count; i++)
        detect_features_decl(all_decls[i]);
    /* Also scan monomorphized template bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        if (mono->entries[mi].decl_kind == DECL_LET && mono->entries[mi].template_decl)
            detect_features_decl(mono->entries[mi].template_decl);
    }

    /* Preamble — emit defines before all includes */
    for (int i = 0; i < define_count; i++)
        fprintf(out, "#define %s %s\n", defines[i].macro, defines[i].value);

    /* Core headers — always emitted (FC's platform contract) */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");

    /* Feature-gated headers — emitted only when needed */
    if (g_needs_stdio) fprintf(out, "#include <stdio.h>\n");
    if (g_needs_math)  fprintf(out, "#include <math.h>\n");
    if (g_needs_float) fprintf(out, "#include <float.h>\n");

    /* Emit #include for each unique from_lib in extern modules */
    for (int i = 0; i < from_lib_count; i++) {
        /* Skip headers already emitted in core or feature-gated preamble */
        if (strcmp(from_libs[i], "stdint.h") == 0 ||
            strcmp(from_libs[i], "stddef.h") == 0 ||
            strcmp(from_libs[i], "stdbool.h") == 0 ||
            strcmp(from_libs[i], "stdlib.h") == 0 ||
            strcmp(from_libs[i], "string.h") == 0)
            continue;
        if (g_needs_stdio && strcmp(from_libs[i], "stdio.h") == 0) continue;
        if (g_needs_math  && strcmp(from_libs[i], "math.h") == 0) continue;
        if (g_needs_float && strcmp(from_libs[i], "float.h") == 0) continue;
        fprintf(out, "#include <%s>\n", from_libs[i]);
    }
    fprintf(out, "\n");

    /* Always emit fc_str and fc_str32 (aliases for uint8/uint32 slices) */
    fprintf(out, "typedef struct { uint8_t* ptr; int64_t len; } fc_str;\n");
    fprintf(out, "typedef struct { uint32_t* ptr; int64_t len; } fc_str32;\n");

    /* Collect all slice, option, function, and eq types used in the program */
    TypeSet slices = {0};
    TypeSet options = {0};
    TypeSet fns = {0};
    TypeSet eqs = {0};
    g_eq_set = &eqs;

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
    g_eq_set = NULL;

    /* Emit forward declarations for all structs and unions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT && !d->struc.is_extern) emit_struct_forward(d, out);
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
        if (is_str_type(s) || is_str32_type(s)) continue; /* str/str32 already emitted */
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

    /* Phase 1: function typedefs that only use primitive/pointer/slice types.
       These are safe before struct defs (e.g. structs with callback fields).
       Typedefs referencing struct/union/option-of-struct are deferred to phase 2. */
    for (int i = 0; i < fns.count; i++) {
        Type *f = fns.types[i];
        if (fn_type_uses_struct_option(f)) continue;
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

    /* Emit full struct and union definitions (skip generics), interleaved with
       option typedefs that wrap them. */
    bool *opt_emitted = calloc(options.count, sizeof(bool));
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        const char *def_name = NULL;
        if (d->kind == DECL_STRUCT) {
            if (!d->struc.is_extern) emit_struct_def(d, out);
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

    /* Phase 2: deferred function typedefs that reference struct/union/option-of-struct.
       All struct defs and option-of-struct typedefs are now emitted above. */
    for (int i = 0; i < fns.count; i++) {
        Type *f = fns.types[i];
        if (!fn_type_uses_struct_option(f)) continue;
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

    /* Emit eq function forward declarations and definitions */
    for (int i = 0; i < eqs.count; i++)
        emit_eq_forward(eqs.types[i], out);
    for (int i = 0; i < eqs.count; i++)
        emit_eq_func(eqs.types[i], out);

    free(slices.types);
    free(options.types);
    free(fns.types);
    free(eqs.types);

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

    /* Emit forward declarations for functions (with void* _ctx), skip generics.
     * main is emitted as fc_main with its str[] param (no _ctx). */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_func_decl(d) && strcmp(d->let.name, "main") == 0 && !is_generic_decl(d)) {
            Expr *fn = d->let.init;
            fprintf(out, "int32_t fc_main(");
            emit_type(fn->func.params[0].type, out);
            fprintf(out, " %s);\n", fn->func.params[0].name);
            continue;
        }
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

    /* Collect trampolines: FC functions passed at extern call boundaries.
     * Walk all non-generic bodies and all monomorphized bodies. */
    TrampolineSet trampolines = {0};
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init &&
            !is_generic_decl(all_decls[i])) {
            collect_trampolines_expr(all_decls[i]->let.init, &trampolines);
        }
    }
    /* Also walk monomorphized function bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        for (int j = 0; j < fn->func.body_count; j++)
            collect_trampolines_expr(fn->func.body[j], &trampolines);
        g_subst = NULL;
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
    /* Emit forward declarations for C-boundary trampolines */
    for (int i = 0; i < trampolines.count; i++) {
        TrampolineEntry *te = &trampolines.entries[i];
        Type *ft = te->type;
        fprintf(out, "static ");
        emit_type(ft->func.return_type, out);
        fprintf(out, " _ctramp_%s(", te->name);
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(ft->func.param_types[j], out);
            fprintf(out, " _p%d", j);
        }
        if (ft->func.param_count == 0) fprintf(out, "void");
        fprintf(out, ");\n");
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

    /* Emit C-boundary trampoline definitions */
    for (int i = 0; i < trampolines.count; i++) {
        TrampolineEntry *te = &trampolines.entries[i];
        Type *ft = te->type;
        fprintf(out, "static ");
        emit_type(ft->func.return_type, out);
        fprintf(out, " _ctramp_%s(", te->name);
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(ft->func.param_types[j], out);
            fprintf(out, " _p%d", j);
        }
        if (ft->func.param_count == 0) fprintf(out, "void");
        fprintf(out, ") {\n");
        if (ft->func.return_type->kind == TYPE_VOID) {
            fprintf(out, "    %s(", te->name);
        } else {
            fprintf(out, "    return %s(", te->name);
        }
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            fprintf(out, "_p%d", j);
        }
        if (ft->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "NULL);\n");
        fprintf(out, "}\n");
    }
    free(trampolines.entries);

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
