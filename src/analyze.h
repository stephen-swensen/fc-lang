#pragma once
#include "common.h"
#include "ast.h"
#include "pass1.h"
#include "monomorph.h"
#include "diag.h"

/* Reusable, non-fatal front-end pipeline (lexer → parser → pass1 → pass2) over
 * an in-memory source string, for the in-process LSP server (src/lsp.c).
 *
 * Unlike the CLI path in main.c, analyze():
 *   - captures diagnostics as structured records instead of printing to stderr,
 *   - never calls exit(): a lexer/parser diag_fatal is caught via longjmp and
 *     recorded, with `aborted` set,
 *   - stops after pass2 (no mono discovery / codegen — queries only need the
 *     typed AST + symbols),
 *   - keeps the typed AST, symbols and arena alive in the result for later
 *     hover/definition/completion/codeLens queries.
 *
 * The AST is arena-allocated and mutated in place by pass2 (Expr.type,
 * ident.resolved_sym, let_expr.let_type, param types, ...), so the "typed AST"
 * is simply the arena + program + symtab held by the result. */

typedef struct Diagnostic {
    SrcLoc loc;        /* resolved filename (interned/owned), 1-based line/col */
    char  *message;    /* arena-owned, no "file:line:col:" prefix */
} Diagnostic;

/* An extra already-in-memory source merged into the analysis (e.g. a stdlib
 * file so `import ... from std::...` resolves). text must be NUL-terminated and
 * remain valid for the lifetime of the returned result. */
typedef struct AnalysisSource {
    const char *filename;   /* stable for the analysis */
    const char *text;       /* NUL-terminated */
    int         len;
} AnalysisSource;

typedef struct AnalysisResult {
    Arena            arena;        /* owns AST, types, interned names, diag messages */
    InternTable      intern;
    Program         *program;      /* merged program; NULL if aborted before parse */
    SymbolTable      symtab;
    MonoTable        mono;
    FileImportScopes file_scopes;

    Diagnostic      *diags;        /* malloc'd dynamic array */
    int              diag_count, diag_cap;

    bool             aborted;      /* a diag_fatal longjmp'd out of lex/parse */

    char            *source;       /* owned copy of the primary document text */
    int              source_len;
    const char      *filename;     /* arena-owned copy of the primary filename */

    /* malloc'd token arrays (one per merged source) freed in analysis_free */
    Token          **token_arrays;
    int              token_array_count, token_array_cap;

    /* Lexer abort-cleanup slots (see lexer.h): hold a tokenization's in-progress
     * arrays so an abort during lexing can free them. NULL on a clean run. */
    Token           *lex_raw;
    Token           *lex_layout;
} AnalysisResult;

/* Analyze `source` (NUL-terminated copy made internally) under `filename`,
 * merging `extra` sources (may be NULL/0). Always returns a result (never NULL);
 * check `diags`/`aborted`. */
AnalysisResult *analyze(const char *source, int source_len, const char *filename,
                        const AnalysisSource *extra, int extra_count);

void analysis_free(AnalysisResult *r);
