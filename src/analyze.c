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

/* ---- lex cache ----
 *
 * Feed sources (stdlib + sibling/lsp.rsp files) are stable across most edits, so
 * their token arrays are cached per path and reused, skipping the dominant lexing
 * cost. Re-parsing cached tokens is identical to re-lexing: the parser only reads
 * tokens, and tokenization never interns (it just records `start`/`length` slices
 * of the source), so the cache carries no interner dependency. */

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

/* Feed source via the lex cache: reuse cached tokens on a content+flags match,
 * else lex and (re)populate the slot. Re-parses into r->arena like lex_parse_one,
 * but the tokens are owned by the cache (NOT appended to r->token_arrays, so
 * analysis_free never frees them). */
static Program *parse_feed_cached(AnalysisResult *r, LexCache *cache,
                                  const AnalysisSource *src, const char *fn,
                                  const Flag *flags, int flag_count, uint64_t flags_sig) {
    LexCacheEntry *e = lexcache_slot(cache, src->filename);

    bool hit = false;
    if (e && e->flags_sig == flags_sig && e->len == src->len) {
        if (e->last_src == src->text) hit = true;               /* stable-buffer fast path */
        else if (e->hash == fnv64(src->text, (size_t)src->len) &&
                 memcmp(e->text, src->text, (size_t)src->len) == 0) hit = true;
    }

    Token *tokens;
    int tc;
    if (hit) {
        e->last_src = src->text;
        tokens = e->tokens;
        tc = e->token_count;
    } else {
        /* MISS: lex with the same abort slots as lex_parse_one so a feed layout
         * error still longjmps cleanly (the dup below runs only after success, so
         * the abort path has nothing extra to clean up). */
        diag_set_filename(fn);
        r->lex_raw = NULL;
        r->lex_layout = NULL;
        Lexer lexer = {0};
        lexer_init(&lexer, src->text, &r->intern, flags, flag_count);
        lexer.abort_slot_raw = &r->lex_raw;
        lexer.abort_slot_layout = &r->lex_layout;
        tokens = lexer_tokenize(&lexer, &tc);
        r->lex_raw = NULL;
        r->lex_layout = NULL;

        /* Own a stable copy of the text and rebase token `start` pointers into it,
         * so the cached tokens (and the AST literals parsed from them later) stay
         * valid across analyses, independent of the caller's per-analysis buffer.
         * The in-range guard leaves any synthetic/NULL-start token untouched (the
         * parser never dereferences those). */
        char *owned = malloc((size_t)src->len + 1);
        memcpy(owned, src->text, (size_t)src->len);
        owned[src->len] = '\0';
        for (int j = 0; j < tc; j++) {
            const char *s = tokens[j].start;
            if (s >= src->text && s <= src->text + src->len)
                tokens[j].start = owned + (s - src->text);
        }

        if (!e) {
            LexCacheEntry blank = {0};
            DA_APPEND(cache->entries, cache->count, cache->cap, blank);
            e = &cache->entries[cache->count - 1];
            size_t pn = strlen(src->filename);
            e->path = malloc(pn + 1);
            memcpy(e->path, src->filename, pn + 1);
        } else {
            /* Replace: free the superseded copy + tokens. The old `text` may still
             * be referenced by a retained last_good's feed-literal pointers, but
             * those bytes are never read by any LSP query (see the query handlers
             * in lsp.c) — identical to the disk_bufs already freed post-analyze.
             * The old tokens are referenced by nothing (the AST holds no token
             * pointers), so freeing them is unconditionally safe. */
            free(e->text);
            free(e->tokens);
        }
        e->flags_sig = flags_sig;
        e->hash = fnv64(owned, (size_t)src->len);
        e->len = src->len;
        e->last_src = src->text;
        e->text = owned;
        e->tokens = tokens;
        e->token_count = tc;
    }

    Parser parser = {0};
    parser_init(&parser, tokens, tc, &r->arena, &r->intern);
    parser.filename = fn;
    Program *prog = parse_program(&parser);
    free(parser.pending_decls);
    return prog;
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
                        const Flag *flags, int flag_count, LexCache *cache) {
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

        /* The primary edited buffer is always lexed fresh (it changes every
         * keystroke); only the feed sources are cached. */
        programs[0] = lex_parse_one(r, r->source, r->filename, flags, flag_count);
        uint64_t flags_sig = cache ? flags_signature(flags, flag_count) : 0;
        for (int i = 0; i < extra_count; i++) {
            const char *fn = arena_strdup(&r->arena, extra[i].filename,
                                          (int)strlen(extra[i].filename));
            programs[1 + i] = cache
                ? parse_feed_cached(r, cache, &extra[i], fn, flags, flag_count, flags_sig)
                : lex_parse_one(r, extra[i].text, fn, flags, flag_count);
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
        /* Run pass2 even when the parser or pass1 reported recoverable errors, so
           type-aware queries (hover, definition, lenses) stay live on the parts of
           the file that are still well formed — instead of blanking the whole file
           because of one bad line or a duplicate name in a merged sibling. pass2
           already accumulates with diag_error and poisons subtrees with TYPE_ERROR,
           so it tolerates a partial program; a parse error leaves EXPR_ERROR/poison
           nodes that pass2 types silently. (The lexer's longjmp backstop still aborts
           the whole analysis on an unrecoverable layout error — the else branch.) */
        pass2_check(r->program, &r->symtab, &r->intern, &r->mono, &r->file_scopes,
                    &r->arena);
        pass2_ran = true;
    } else {
        r->aborted = true;
        /* Free the lexer's in-progress arrays the abort left behind. The
         * already-returned per-source arrays are tracked in token_arrays. */
        if (r->lex_raw)    { free(r->lex_raw);    r->lex_raw = NULL; }
        if (r->lex_layout) { free(r->lex_layout); r->lex_layout = NULL; }
    }

    /* Safety net against silent blanking. pass2 now runs past recoverable parse/pass1
     * errors (see above), so this only triggers when the analysis HARD-aborted — i.e.
     * the lexer hit an unrecoverable layout error (tab, unterminated string/comment,
     * inconsistent indentation) and longjmp'd, leaving pass2_ran false and r->aborted
     * true. In that case every node's resolved type is NULL, so hover / definition /
     * CodeLens go empty; publish_diagnostics filters to the open file, so an abort in a
     * *merged* sibling shows the open document NOTHING. Surface one file-level diagnostic
     * naming the first offending file so the failure is visible, not a dead editor. */
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
