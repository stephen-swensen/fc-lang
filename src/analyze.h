#pragma once
#include "common.h"
#include "ast.h"
#include "pass1.h"
#include "monomorph.h"
#include "diag.h"
#include "lexer.h"   /* Flag */

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

/* ---- Lex cache (session-scoped, owned by the LSP server) ----
 *
 * Caches the lexer's token array per FEED file so unchanged feed sources (the
 * stdlib + sibling/lsp.rsp files) are not re-lexed on every keystroke; analyze()
 * re-parses from the cached tokens (the parser only reads tokens, and the lexer
 * never interns, so a re-parse is identical to a re-lex). The primary edited
 * buffer is always lexed fresh. A NULL cache passed to analyze() disables this
 * (the CLI-equivalent path, byte-for-byte today's behavior).
 *
 * One slot per feed path bounds memory to the unit's file count; a content or
 * flag change replaces the slot. The owned `text` keeps the cached tokens' (and
 * the parsed AST's literal) `start` pointers valid across analyses. */
typedef struct LexCacheEntry {
    char       *path;        /* slot key: the feed source's filename (owned) */
    uint64_t    flags_sig;   /* identity of the conditional-compile flag set */
    uint64_t    hash;        /* FNV-1a 64 of the source bytes */
    int         len;         /* source length (with hash, validates content) */
    const char *last_src;    /* last incoming text ptr that matched (compared only,
                              * never dereferenced; a fast path for stable buffers) */
    char       *text;        /* owned NUL-terminated copy; tokens point into it */
    Token      *tokens;      /* owned token array (NOT in any result's token_arrays) */
    int         token_count;
} LexCacheEntry;

typedef struct LexCache {
    LexCacheEntry *entries;  /* one slot per distinct feed path; linear scan */
    int count, cap;
} LexCache;

/* Free every cached entry (path/text/tokens) and the entry array. */
void lexcache_free(LexCache *c);

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
    bool             typed;        /* pass2 ran: the well-formed parts of the AST carry
                                    * their inferred types. pass2 now runs even past
                                    * recoverable parse/pass1 errors, so this is true
                                    * for ordinary mid-typing states (a broken line
                                    * leaves EXPR_ERROR/poison nodes the queries skip).
                                    * It is False only when the analysis HARD-aborted
                                    * (a lexer longjmp — see `aborted`), where the AST
                                    * has no type info; the server then falls back to
                                    * the last good analysis for type-aware queries. */

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
 * merging `extra` sources (may be NULL/0). `flags`/`flag_count` are the
 * conditional-compilation flags (caller-owned; borrowed for the call) — the
 * caller supplies host auto-detect and/or lsp.rsp `--flag`s. `cache` (may be
 * NULL) is a session-scoped lex cache for the feed sources; NULL re-lexes every
 * source (the CLI-equivalent path). Always returns a result (never NULL); check
 * `diags`/`aborted`. */
AnalysisResult *analyze(const char *source, int source_len, const char *filename,
                        const AnalysisSource *extra, int extra_count,
                        const Flag *flags, int flag_count, LexCache *cache);

void analysis_free(AnalysisResult *r);
