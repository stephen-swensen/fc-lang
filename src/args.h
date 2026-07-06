#ifndef FC_ARGS_H
#define FC_ARGS_H

#include "lexer.h"   /* Flag */
#include <stdbool.h>

/* ---- Response-file argument expansion ----
 *
 * fcc accepts gcc-style response files: a `@file` token on the command line is
 * replaced in place by the whitespace-separated tokens read from `file`
 * (newlines count as whitespace). `#` and `//` begin line comments. Response
 * files may reference other response files (`@nested`), expanded recursively;
 * a cycle (or excessive nesting) is an error.
 *
 * Two behaviours distinguish tokens that originate *inside* a response file
 * from tokens typed directly on the command line (which the shell already
 * processed):
 *   - relative paths resolve against the response file's own directory, not the
 *     cwd, so a project file is relocatable;
 *   - positional (input-file) tokens undergo shell-style glob expansion
 *     (`*`, `?`, `[...]`), since no shell ran over them.
 * Both apply only to response-file tokens. They are deferred to argument
 * *parsing* (args_parse) so they land only on real input paths, never on an
 * option value like the `debug` in `--flag debug`.
 *
 * The same machinery backs `fcc @file` on the CLI and the LSP's by-convention
 * discovery of `lsp.rsp` (the LSP simply synthesizes a single `@<path>` token).
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
    char        *error;      /* owned message if parsing failed, else NULL   */
} CompileArgs;

/* Interpret an expanded token stream: applies host platform auto-detect
 * (unless --no-auto-detect), --flag overrides (replace by name), -o, and turns
 * positionals into inputs with response-file glob/rebase. Flag name/value
 * strings BORROW `e`, so `e` must outlive the returned flags. Returns true on
 * success; on failure returns false with out->error set (and out otherwise
 * safe to pass to args_compile_free). */
bool args_parse(const ExpandedArgs *e, CompileArgs *out);
void args_compile_free(CompileArgs *c);

#endif
