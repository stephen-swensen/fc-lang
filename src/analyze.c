#include "analyze.h"
#include "lexer.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include <stdio.h>
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
                              const char *filename, const Flag *flags, int flag_count) {
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
                        const AnalysisSource *extra, int extra_count,
                        const Flag *flags, int flag_count) {
    AnalysisResult *r = calloc(1, sizeof *r);
    arena_init(&r->arena);
    intern_init(&r->intern, &r->arena);
    symtab_init(&r->symtab);

    r->source = malloc((size_t)source_len + 1);
    memcpy(r->source, source, (size_t)source_len);
    r->source[source_len] = '\0';
    r->source_len = source_len;
    r->filename = arena_strdup(&r->arena, filename, (int)strlen(filename));

    diag_reset_counts();
    diag_set_filename(r->filename);
    diag_set_sink(collect_sink, r);

    /* Volatile so its value survives the longjmp below (it is read after the
     * setjmp/else path). Stays false unless pass2 actually executed. */
    volatile bool pass2_ran = false;

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

        /* Server mode tolerates library code with no entry point (e.g. editing
         * a stdlib module), so the missing-`main` diagnostic is suppressed. */
        pass1_collect(r->program, &r->symtab, &r->intern, &r->file_scopes,
                      /*require_main=*/false);
        if (diag_error_count() == 0) {
            pass2_check(r->program, &r->symtab, &r->intern, &r->mono, &r->file_scopes,
                        &r->arena);
            pass2_ran = true;
        }
    } else {
        r->aborted = true;
        /* Free the lexer's in-progress arrays the abort left behind. The
         * already-returned per-source arrays are tracked in token_arrays. */
        if (r->lex_raw)    { free(r->lex_raw);    r->lex_raw = NULL; }
        if (r->lex_layout) { free(r->lex_layout); r->lex_layout = NULL; }
    }

    /* Safety net against silent blanking. Type-checking is gated on a clean
     * pass1 (and on a clean parse), so a single error in a *merged* sibling or
     * stdlib-feed file suppresses pass2 for the whole analysis — every node's
     * resolved type is then NULL, so hover / definition / CodeLens all go empty.
     * publish_diagnostics filters to the open file, so when the offending error
     * lives in another file the open document shows NOTHING: no error to explain
     * the silence, no type info. (The canonical trigger is the same module merged
     * twice from two different paths.) Surface one file-level diagnostic on the
     * open document, naming the first offending included file, so the failure is
     * visible and actionable instead of a mysteriously dead editor. */
    if (!pass2_ran) {
        bool open_has_diag = false;
        const char *other = NULL;
        for (int i = 0; i < r->diag_count; i++) {
            const char *fn = r->diags[i].loc.filename;
            /* A NULL filename defaults to the open document (publish_diagnostics
             * shows it), so it already explains the silence — treat as open. */
            if (!fn || strcmp(fn, r->filename) == 0) { open_has_diag = true; break; }
            if (!other) other = fn;
        }
        if (!open_has_diag) {
            const char *base = other;
            if (base) {
                const char *slash = strrchr(base, '/');
                if (slash) base = slash + 1;
            }
            char msg[256];
            if (base)
                snprintf(msg, sizeof msg,
                         "analysis incomplete: an error in an included file (%s) "
                         "halted type checking — hover, definition, and lenses are "
                         "unavailable for this file until it is resolved", base);
            else
                snprintf(msg, sizeof msg,
                         "analysis incomplete: type checking did not run — hover, "
                         "definition, and lenses are unavailable for this file");
            Diagnostic d;
            d.loc = (SrcLoc){ .filename = r->filename, .line = 1, .col = 1 };
            d.message = arena_strdup(&r->arena, msg, (int)strlen(msg));
            DA_APPEND(r->diags, r->diag_count, r->diag_cap, d);
        }
    }

    r->typed = pass2_ran;

    diag_set_abort_jmp(NULL);
    diag_set_sink(NULL, NULL);
    return r;
}

/* Reclaim everything an analysis allocated. The arena (r->arena) holds the AST,
 * interned strings, and — since the leak fix — pass1's referenced Type nodes and
 * generic type_params arrays and pass2's self-recursion placeholder, so freeing
 * the arena reclaims them. The malloc'd side tables (token arrays, the nested
 * symtab/import trees, mono entries, diags, source) are freed explicitly below.
 * A 40-edit ASan session importing std:: reports zero leaks. */
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
