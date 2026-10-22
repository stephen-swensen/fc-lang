#pragma once

#include "lexer.h"   /* Flag */
#include <stdbool.h>

/* ---- Response-file argument expansion ----
 *
 * fcc accepts gcc-style response files: a `@file` token on the command line is
 * replaced in place by the whitespace-separated tokens read from `file`
 * (newlines count as whitespace; a token in double quotes may hold spaces, with
 * no escapes). `#` and `//` begin line comments. Response
 * files may reference other response files (`@nested`), expanded recursively;
 * a cycle (or excessive nesting) is an error.
 *
 * Tokens that come from a response file are treated differently from tokens
 * typed on the command line (which the shell already processed):
 *   - relative paths resolve against the response file's own directory, not the
 *     cwd, so a project file is relocatable;
 *   - positional (input-file) tokens undergo shell-style glob expansion
 *     (`*`, `?`, `[...]`), since no shell ran over them.
 * Both happen in args_parse rather than during expansion, so they apply only
 * to input paths (the -o value is rebased too), never to an option value like
 * the `debug` in `--flag debug`.
 *
 * The same machinery backs `fcc @file` on the CLI and the LSP's discovery of
 * `lsp.rsp` (the LSP synthesizes a single `@<path>` token).
 */

/* A flat token stream after @file splicing. tokens[i] is owned (malloc'd);
 * dirs[i] is the owned directory of the response file token i came from, or
 * NULL when token i was typed directly on the command line. The two arrays are
 * kept index-aligned. */
typedef struct {
    char **tokens;
    char **dirs;
    int    count, cap;
} ExpandedArgs;

/* Splice every @file in argv (recursively) into a flat token stream. Each
 * element of argv is treated as typed-directly unless it begins with '@'.
 * Returns true on success; on failure returns false, leaves *out empty, and
 * sets *err to a malloc'd message (caller frees). */
bool args_expand(int argc, char **argv, ExpandedArgs *out, char **err);
void args_expand_free(ExpandedArgs *e);

/* The interpreted compiler invocation. */
typedef struct {
    const char **inputs;     /* owned: rebased + glob-expanded input paths   */
    int          input_count, input_cap;
    char        *output;     /* owned: -o value (rebased); NULL if unset     */
    Flag        *flags;      /* owned array; name/value borrow ExpandedArgs   */
    int          flag_count, flag_cap;
    bool         backtraces;
    bool         native;     /* --native: an x86-64 executable instead of C */
    bool         compile_only; /* -c: with --native, an object file instead */
    char       **link_inputs; /* owned: object, archive and library files
                                 (rebased) and -lNAME, in command-line order */
    int          link_count, link_cap;
    char       **lib_dirs;   /* owned: -L directories (rebased), in order    */
    int          lib_dir_count, lib_dir_cap;
    char       **include_dirs; /* owned: -I directories (rebased), in order  */
    int          include_dir_count, include_dir_cap;
    int          len_repr;   /* stored slice-len width: 16, 32 or 64 */
    char        *error;      /* owned message if parsing failed, else NULL   */
} CompileArgs;

/* Interpret an expanded token stream: applies host platform auto-detect
 * (unless --no-auto-detect), --flag overrides (replace by name), -o, and turns
 * positionals into inputs with response-file glob/rebase. A positional ending
 * in .o, .a or .so (or .so.N) is a link input, as are -l libraries; both, and
 * -L, are for a --native executable only. -I directories are for --native,
 * with or without -c. Flag name/value
 * strings borrow from `e`, so `e` must outlive the returned flags. Returns true on
 * success; on failure returns false with out->error set (and out otherwise
 * safe to pass to args_compile_free). */
bool args_parse(const ExpandedArgs *e, CompileArgs *out);
void args_compile_free(CompileArgs *c);
