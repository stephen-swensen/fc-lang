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

/* ---- lexing ---- */

/* Lex one source. The lexer's in-progress arrays are exposed through r so that
 * a lex error, which longjmps out of the lexer, leaves them for analyze() to
 * free rather than leaking them (see lexer.h). */
static Token *lex_source(AnalysisResult *r, const char *text, const char *filename,
                         const Flag *flags, int flag_count, int *out_count) {
    diag_set_filename(filename);
    r->lex_input = NULL;
    r->lex_output = NULL;
    Lexer lexer = {0};
    lexer_init(&lexer, text, &r->intern, flags, flag_count);
    lexer.abort_slot_input = &r->lex_input;
    lexer.abort_slot_output = &r->lex_output;
    Token *tokens = lexer_tokenize(&lexer, out_count);
    r->lex_input = NULL;
    r->lex_output = NULL;
    return tokens;
}

/* Lex a source into a token array the analysis owns. */
static Token *lex_one(AnalysisResult *r, const char *text, const char *filename,
                      const Flag *flags, int flag_count, int *out_count) {
    Token *tokens = lex_source(r, text, filename, flags, flag_count, out_count);
    DA_APPEND(r->token_arrays, r->token_array_count, r->token_array_cap, tokens);
    return tokens;
}

/* ---- lex cache ----
 *
 * Feed sources (stdlib and sibling/lsp.rsp files) rarely change between edits, so
 * their token arrays are cached per path and reused. Re-parsing cached tokens
 * gives the same result as re-lexing: the parser only reads tokens, and
 * tokenization never interns (it records `start`/`length` slices of the source),
 * so the cache has no interner dependency. */

static uint64_t fnv64(const void *data, size_t n) {
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ULL;       /* FNV-1a 64 offset basis */
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* Identity of the conditional-compile flag set. A feed file lexed under one set
 * of flags is invalid under another (filter_conditionals strips #if-like spans by
 * flag), so a flag change must miss and re-lex. */
static uint64_t flags_signature(const Flag *flags, int flag_count) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < flag_count; i++) {
        h = fnv64(flags[i].name, (size_t)flags[i].name_len) ^ (h * 1099511628211ULL);
        if (flags[i].value)
            h = fnv64(flags[i].value, strlen(flags[i].value)) ^ (h * 1099511628211ULL);
        h *= 1099511628211ULL;   /* separator so {ab,c} != {a,bc} */
    }
    return h;
}

static LexCacheEntry *lexcache_slot(LexCache *c, const char *path) {
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->entries[i].path, path) == 0) return &c->entries[i];
    return NULL;
}

/* Lex a feed source through the cache: reuse cached tokens on a content+flags
 * match, else lex and (re)populate the slot. The cache owns the tokens; they are
 * not appended to r->token_arrays, so analysis_free never frees them. */
static Token *lex_feed_cached(AnalysisResult *r, LexCache *cache,
                              const AnalysisSource *src, const char *fn,
                              const Flag *flags, int flag_count,
                              uint64_t flags_sig, int *out_count) {
    LexCacheEntry *e = lexcache_slot(cache, src->filename);

    /* A slot hits only when the content matches (hash as a fast reject, memcmp
     * to confirm). Pointer identity is not a usable shortcut: an open sibling's
     * buffer is freed and re-malloc'd on every edit, and the allocator often
     * returns the just-freed address for a new same-length buffer holding
     * different text (`double` edited to `triple`), which would serve stale
     * tokens. */
    bool hit = e && e->flags_sig == flags_sig && e->len == src->len &&
               e->hash == fnv64(src->text, (size_t)src->len) &&
               memcmp(e->text, src->text, (size_t)src->len) == 0;

    Token *tokens;
    int tc;
    if (hit) {
        tokens = e->tokens;
        tc = e->token_count;
    } else {
        tokens = lex_source(r, src->text, fn, flags, flag_count, &tc);

        /* Own a stable copy of the text and rebase token `start` pointers into it,
         * so the cached tokens (and the AST literals parsed from them later) stay
         * valid across analyses, independent of the caller's per-analysis buffer.
         * The in-range guard leaves any synthetic/NULL-start token untouched (the
         * parser never dereferences those). */
        char *owned = str_ndup(src->text, (size_t)src->len);
        for (int j = 0; j < tc; j++) {
            const char *s = tokens[j].start;
            if (s >= src->text && s <= src->text + src->len)
                tokens[j].start = owned + (s - src->text);
        }

        if (!e) {
            LexCacheEntry blank = {0};
            DA_APPEND(cache->entries, cache->count, cache->cap, blank);
            e = &cache->entries[cache->count - 1];
            e->path = str_dup(src->filename);
        } else {
            /* Replace: free the superseded text and tokens. A retained last_good
             * AST may still point into the old `text` for feed literals, but no
             * LSP query reads those bytes (the same holds for the disk_bufs lsp.c
             * frees after each analysis). Nothing references the old tokens; the
             * AST holds no token pointers. */
            free(e->text);
            free(e->tokens);
        }
        e->flags_sig = flags_sig;
        e->hash = fnv64(owned, (size_t)src->len);
        e->len = src->len;
        e->text = owned;
        e->tokens = tokens;
        e->token_count = tc;
    }

    *out_count = tc;
    return tokens;
}

void lexcache_free(LexCache *c) {
    for (int i = 0; i < c->count; i++) {
        free(c->entries[i].path);
        free(c->entries[i].text);
        free(c->entries[i].tokens);
    }
    free(c->entries);
    c->entries = NULL;
    c->count = c->cap = 0;
}

/* ---- public entry ---- */

AnalysisResult *analyze(const char *source, int source_len, const char *filename,
                        const AnalysisSource *extra, int extra_count,
                        const Flag *flags, int flag_count, int len_repr,
                        LexCache *cache) {
    int saved_len_repr = g_len_repr;
    g_len_repr = len_repr;
    AnalysisResult *r = xcalloc(1, sizeof *r);
    arena_init(&r->arena);
    intern_init(&r->intern, &r->arena);
    symtab_init(&r->symtab);

    r->source = str_ndup(source, (size_t)source_len);
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
        Token **toks = arena_alloc(&r->arena, sizeof(Token *) * (size_t)nsrc);
        int *tcs = arena_alloc(&r->arena, sizeof(int) * (size_t)nsrc);
        const char **fns = arena_alloc(&r->arena, sizeof(const char *) * (size_t)nsrc);

        /* Lex every source before parsing any: the expression-position `<`
         * scans need the whole-unit set of generic declaration names (see
         * parser_collect_generic_names). The primary edited buffer is always
         * lexed fresh (it changes every keystroke); only the feed sources are
         * cached. */
        fns[0] = r->filename;
        toks[0] = lex_one(r, r->source, r->filename, flags, flag_count, &tcs[0]);
        uint64_t flags_sig = cache ? flags_signature(flags, flag_count) : 0;
        for (int i = 0; i < extra_count; i++) {
            const char *fn = arena_strdup(&r->arena, extra[i].filename,
                                          (int)strlen(extra[i].filename));
            fns[1 + i] = fn;
            toks[1 + i] = cache
                ? lex_feed_cached(r, cache, &extra[i], fn, flags, flag_count,
                                  flags_sig, &tcs[1 + i])
                : lex_one(r, extra[i].text, fn, flags, flag_count, &tcs[1 + i]);
        }

        r->program = parse_files(toks, tcs, fns, nsrc, &r->arena, &r->intern);

        /* After the merge, diagnostics with no per-node filename default to the
         * primary document (stdlib is presumed clean and is filtered out). */
        diag_set_filename(r->filename);

        /* Server mode tolerates library code with no entry point (e.g. editing
         * a stdlib module), so the missing-`main` diagnostic is suppressed. */
        pass1_collect(r->program, &r->symtab, &r->intern, &r->file_scopes,
                      /*require_main=*/false);
        /* Run pass2 even when the parser or pass1 reported errors, so type-aware
           queries (hover, definition, lenses) keep working on the well-formed
           parts of the file rather than going blank because of one bad line or a
           duplicate name in a merged sibling. pass2 poisons what it cannot type
           with TYPE_ERROR and types parse-error nodes silently. A fatal error
           (a lex error, or an internal error) still aborts the whole analysis
           (the else branch). */
        pass2_check(r->program, &r->symtab, &r->intern, &r->mono, &r->file_scopes,
                    &r->arena);
        pass2_ran = true;
    } else {
        r->aborted = true;
        /* Free the lexer's in-progress arrays the abort left behind. The
         * already-returned per-source arrays are tracked in token_arrays. */
        if (r->lex_input)  { free(r->lex_input);  r->lex_input = NULL; }
        if (r->lex_output) { free(r->lex_output); r->lex_output = NULL; }
    }

    /* pass2 did not run only when a fatal error aborted the analysis: almost
     * always a lexical error (a tab, an unterminated string or comment,
     * inconsistent indentation, an #if error), or an internal compiler error.
     * Every node's type is then NULL, so the server answers hover, definition
     * and CodeLens from the last analysis that type-checked, if any, and if the
     * abort was in a merged sibling nothing on the open document says why. Add
     * one file-level diagnostic on the open document naming the offending file.
     * (Project-wide publishing shows the sibling's own error on the sibling.) */
    if (!pass2_ran) {
        bool open_has_diag = false;
        const char *other = NULL;
        for (int i = 0; i < r->diag_count; i++) {
            const char *fn = r->diags[i].loc.filename;
            /* A NULL filename means the open document, which then already has a
             * diagnostic explaining the missing overlays. */
            if (!fn || strcmp(fn, r->filename) == 0) { open_has_diag = true; break; }
            if (!other) other = fn;
        }
        if (!open_has_diag) {
            const char *base = other;
            if (base) {
                const char *slash = strrchr(base, '/');
                if (slash) base = slash + 1;
            }
            const char *msg = base
                ? arena_sprintf(&r->arena,
                         "analysis incomplete: an error in an included file (%s) "
                         "halted type checking; until it is resolved, hover, "
                         "definition, and lenses show the last version that "
                         "type-checked", base)
                : "analysis incomplete: type checking did not run; hover, "
                  "definition, and lenses show the last version that type-checked";
            Diagnostic d;
            d.loc = (SrcLoc){ .filename = r->filename, .line = 1, .col = 1 };
            d.message = arena_strdup(&r->arena, msg, (int)strlen(msg));
            DA_APPEND(r->diags, r->diag_count, r->diag_cap, d);
        }
    }

    r->typed = pass2_ran;

    diag_set_abort_jmp(NULL);
    diag_set_sink(NULL, NULL);
    g_len_repr = saved_len_repr;
    return r;
}

/* Reclaim everything an analysis allocated: the token arrays, diagnostics and
 * source copy it owns, then the front end's tables and arena
 * (front_end_free). */
void analysis_free(AnalysisResult *r) {
    if (!r) return;
    for (int i = 0; i < r->token_array_count; i++) free(r->token_arrays[i]);
    free(r->token_arrays);
    free(r->lex_input);
    free(r->lex_output);

    free(r->diags);
    free(r->source);
    front_end_free(&r->symtab, &r->intern, &r->mono, &r->file_scopes, &r->arena);
    free(r);
}
