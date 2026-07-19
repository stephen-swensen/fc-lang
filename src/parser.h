#pragma once
#include "ast.h"
#include "common.h"

typedef struct Parser {
    Token *tokens;
    int token_count;
    int pos;
    Arena *arena;
    InternTable *intern;
    const char *filename;   /* source filename for SrcLoc */
    Decl **pending_decls;   /* extra decls from multi-symbol imports */
    int pending_count;
    int pending_cap;
    bool allow_fixed_array; /* true when parsing struct/extern struct field types */
    bool in_const_expr;     /* true inside a const-expression slot (a generic <...> argument or a
                               fixed-array size), including the parenthesized escape hatch that
                               re-enters the general expression grammar. Const expressions evaluate
                               in the i64 domain, so an unsuffixed integer literal is i64 there
                               rather than the i32 expression default. */
    bool block_arm_arrow;   /* true while parsing a match-arm `when` guard: stop expr at top-level `->`; cleared inside bracketed sub-expressions so pointer-field `p->x` still works when parenthesized */
    int expr_start_pos;     /* token index at start of current parse_expr (for postfix ! text capture) */
    int expr_start_errs;    /* diag_error_count() at start of current parse_expr; a delta means error
                               recovery ran, so token pointers may span buffers and text capture must be
                               skipped (the text only feeds codegen, which never runs with errors) */
    bool half_gt;           /* a '>>' (TOK_GTGT) token has had its first '>' consumed as a type-argument closer; the parser is parked on it awaiting the second */
    int half_gt_pos;        /* token index of that '>>' (valid iff half_gt) */
    /* Whole-program set of names that may head a generic instantiation
     * (`name<...>` in expression position), collected from every file's tokens
     * before parsing (parser_collect_generic_names). The expression-position
     * `<` scans claim a type-argument reading only for these names, so a
     * comparison like `f(a < 2, b > (c))` keeps its comparison reading when
     * the `<`'s left operand is not a generic declaration. Interned pointers.
     * The gate applies only when generic_gate is set (the CLI and LSP set it
     * after running the pre-pass); harnesses that skip the pre-pass keep the
     * historical claim-always behavior. */
    const char **generic_names;
    int generic_name_count;
    bool generic_gate;
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern);
Program *parse_program(Parser *p);

/* Pre-parse pass: scan a file's token stream for declarations that can be
 * generic — `struct`/`union` whose body mentions a type variable, `let` whose
 * lambda header (explicit `<...>` prefix or parameter list) mentions one, and
 * `import ... as` aliases (conservatively, since the target's genericness
 * isn't resolvable at token level) — and append their (interned) names to the
 * growing names array. Run over every file of the program before parsing any,
 * then share the result via Parser.generic_names. Over-claiming only re-widens
 * the gate toward the historical behavior; the set is deliberately built from
 * the same lexical evidence pass1's generic detection uses, so a declared
 * generic is never missed. */
void parser_collect_generic_names(Token *tokens, int count, InternTable *intern,
                                  const char ***names, int *n, int *cap);
