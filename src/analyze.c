#include "analyze.h"
#include "lexer.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

/* ---- diagnostic sink ---- */

static void collect_sink(SrcLoc loc, const char *msg, void *ud) {
    AnalysisResult *r = ud;
    Diagnostic d;
    d.loc = loc;
    d.message = arena_strdup(&r->arena, msg, (int)strlen(msg));
    DA_APPEND(r->diags, r->diag_count, r->diag_cap, d);
}

/* ---- one source: lex + parse into an arena-owned Program ---- */

static Program *lex_parse_one(AnalysisResult *r, const char *text,
                              const char *filename, Flag *flags, int flag_count) {
    diag_set_filename(filename);

    /* Expose the lexer's in-progress arrays so an abort during lexing frees
     * them rather than leaking (see lexer.h / analyze() abort handler). */
    r->lex_raw = NULL;
    r->lex_layout = NULL;

    Lexer lexer = {0};
    lexer_init(&lexer, text, &r->intern, flags, flag_count);
    lexer.abort_slot_raw = &r->lex_raw;
    lexer.abort_slot_layout = &r->lex_layout;

    int token_count;
    Token *tokens = lexer_tokenize(&lexer, &token_count);

    /* Tokenize succeeded: the pre-filter array was freed internally and the
     * returned array is `tokens`. Clear the in-progress slots and track the
     * returned array for cleanup. */
    r->lex_raw = NULL;
    r->lex_layout = NULL;
    DA_APPEND(r->token_arrays, r->token_array_count, r->token_array_cap, tokens);

    Parser parser = {0};
    parser_init(&parser, tokens, token_count, &r->arena, &r->intern);
    parser.filename = filename;
    Program *prog = parse_program(&parser);
    /* pending_decls is malloc'd scratch the parser never frees (one-shot CLI
     * relies on process exit); the AST itself lives in the arena. */
    free(parser.pending_decls);
    return prog;
}

/* ---- deep free of pass1/pass2's malloc'd symbol metadata ----
 *
 * The compiler is written for a one-shot process: module member tables and
 * import tables are malloc'd and never freed (main.c only frees the top-level
 * symtab.symbols array). A long-running server must reclaim them per analysis.
 * Ownership is a tree — each module Symbol owns exactly one members table and
 * at most one imports table; ImportRefs only *reference* other tables — so a
 * recursive walk frees each exactly once. */
static void symtab_free_nested(SymbolTable *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->imports) {
            free(s->imports->entries);
            free(s->imports);
            s->imports = NULL;
        }
        if (s->members) {
            symtab_free_nested(s->members);
            free(s->members->symbols);
            free(s->members);
            s->members = NULL;
        }
    }
}

/* ---- public entry ---- */

AnalysisResult *analyze(const char *source, int source_len, const char *filename,
                        const AnalysisSource *extra, int extra_count) {
    AnalysisResult *r = calloc(1, sizeof *r);
    arena_init(&r->arena);
    intern_init(&r->intern, &r->arena);
    symtab_init(&r->symtab);

    r->source = malloc((size_t)source_len + 1);
    memcpy(r->source, source, (size_t)source_len);
    r->source[source_len] = '\0';
    r->source_len = source_len;
    r->filename = arena_strdup(&r->arena, filename, (int)strlen(filename));

    /* Conditional-compilation flags for the host (matches the CLI default). */
    Flag *flags = NULL;
    int flag_count = 0, flag_cap = 0;
    platform_detect_flags(&flags, &flag_count, &flag_cap);

    diag_reset_counts();
    diag_set_filename(r->filename);
    diag_set_sink(collect_sink, r);

    jmp_buf env;
    diag_set_abort_jmp(&env);
    if (setjmp(env) == 0) {
        int nsrc = 1 + extra_count;
        Program **programs = arena_alloc(&r->arena, sizeof(Program *) * (size_t)nsrc);

        programs[0] = lex_parse_one(r, r->source, r->filename, flags, flag_count);
        for (int i = 0; i < extra_count; i++) {
            const char *fn = arena_strdup(&r->arena, extra[i].filename,
                                          (int)strlen(extra[i].filename));
            programs[1 + i] = lex_parse_one(r, extra[i].text, fn, flags, flag_count);
        }

        /* Merge programs, mirroring main.c: inject a global-namespace reset
         * sentinel before any file that does not begin with a namespace decl. */
        if (nsrc == 1) {
            r->program = programs[0];
        } else {
            int total = 0;
            for (int i = 0; i < nsrc; i++) total += programs[i]->decl_count + 1;
            Program *prog = arena_alloc(&r->arena, sizeof(Program));
            prog->decls = arena_alloc(&r->arena, sizeof(Decl *) * (size_t)total);
            prog->decl_count = 0;
            for (int i = 0; i < nsrc; i++) {
                bool has_ns = (programs[i]->decl_count > 0 &&
                               programs[i]->decls[0]->kind == DECL_NAMESPACE);
                if (!has_ns) {
                    Decl *sentinel = arena_alloc(&r->arena, sizeof(Decl));
                    sentinel->kind = DECL_NAMESPACE;
                    sentinel->loc = (SrcLoc){0};
                    sentinel->is_private = false;
                    sentinel->ns.name = NULL;
                    prog->decls[prog->decl_count++] = sentinel;
                }
                for (int j = 0; j < programs[i]->decl_count; j++)
                    prog->decls[prog->decl_count++] = programs[i]->decls[j];
            }
            r->program = prog;
        }

        /* After the merge, diagnostics with no per-node filename default to the
         * primary document (stdlib is presumed clean and is filtered out). */
        diag_set_filename(r->filename);

        pass1_collect(r->program, &r->symtab, &r->intern, &r->file_scopes);
        if (diag_error_count() == 0)
            pass2_check(r->program, &r->symtab, &r->intern, &r->mono, &r->file_scopes,
                        &r->arena);
    } else {
        r->aborted = true;
        /* Free the lexer's in-progress arrays the abort left behind. The
         * already-returned per-source arrays are tracked in token_arrays. */
        if (r->lex_raw)    { free(r->lex_raw);    r->lex_raw = NULL; }
        if (r->lex_layout) { free(r->lex_layout); r->lex_layout = NULL; }
    }

    diag_set_abort_jmp(NULL);
    diag_set_sink(NULL, NULL);
    free(flags);
    return r;
}

/* Reclaim everything an analysis allocated. Note: pass1 still malloc's a few
 * *referenced* Type nodes (module-member types, resolved stub types) and the
 * generic type_params arrays; these are aliased from the symtab/AST/imports and
 * are not safe to free piecemeal, so a small bounded amount (~13KB/analysis,
 * dominated by re-processing the merged stdlib) is not reclaimed here. The fix
 * is to arena-allocate them in pass1 — left as a follow-up. */
void analysis_free(AnalysisResult *r) {
    if (!r) return;
    for (int i = 0; i < r->token_array_count; i++) free(r->token_arrays[i]);
    free(r->token_arrays);
    if (r->lex_raw)    free(r->lex_raw);
    if (r->lex_layout) free(r->lex_layout);

    symtab_free_nested(&r->symtab);
    free(r->symtab.symbols);
    for (int i = 0; i < r->file_scopes.count; i++)
        free(r->file_scopes.scopes[i].imports.entries);
    free(r->file_scopes.scopes);
    free(r->mono.entries);

    free(r->diags);
    free(r->source);
    free(r->intern.entries);   /* hash array is malloc'd; strings live in arena */
    arena_free(&r->arena);
    free(r);
}
