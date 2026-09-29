#pragma once
#include "ast.h"
#include "common.h"

/* Parse one file's tokens into a Program allocated in `arena`. Syntax errors
 * are reported and parsing carries on, so the Program may hold error nodes
 * whenever diag_error_count() > 0. `generic_names` is the whole program's set
 * from parser_collect_generic_names: an expression-position `name<` is read as
 * a generic instantiation only when `name` is in it. */
Program *parse_file(Token *tokens, int count, const char *filename,
                    const char **generic_names, int generic_name_count,
                    Arena *arena, InternTable *intern);


/* Pre-parse pass: scan a file's token stream for declarations that can be
 * generic and append their interned names to `names`. That is a `struct` or
 * `union` whose body mentions a type variable, a `let` whose lambda header
 * (explicit `<...>` prefix or parameter list) mentions one, and every
 * `import ... as` alias (the target's genericness is not visible at token
 * level). Run it over every file before parsing any, and pass the result to
 * parse_file. It reads the same evidence pass1 uses to decide genericness, so
 * no generic declaration is missed; an extra name only means a `<` after it is
 * tried as a type-argument list before falling back to a comparison. */
void parser_collect_generic_names(Token *tokens, int count, InternTable *intern,
                                  const char ***names, int *n, int *cap);
