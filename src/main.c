#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include "codegen.h"
#include "monomorph.h"
#include "diag.h"
#include "platform.h"
#include "version.h"
#include "lsp.h"
#include "args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        diag_fatal_simple("cannot open '%s'", path);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        diag_fatal_simple("out of memory reading '%s'", path);
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

/* A build whose C output goes to the null device is a check-only build:
 * there is no artifact for the .errcodes map to describe (and deriving a
 * sibling path from /dev/null would try to create /dev/null.errcodes). */
static bool output_is_null_device(const char *path) {
#if defined(_WIN32)
    return _stricmp(path, "NUL") == 0 || _stricmp(path, "NUL:") == 0;
#else
    return strcmp(path, "/dev/null") == 0;
#endif
}

static char *change_extension(const char *path, const char *new_ext) {
    int len = (int)strlen(path);
    int dot = len;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '.') { dot = i; break; }
        if (path[i] == '/') break;
    }
    int ext_len = (int)strlen(new_ext);
    char *result = malloc((size_t)(dot + ext_len + 1));
    memcpy(result, path, (size_t)dot);
    memcpy(result + dot, new_ext, (size_t)ext_len);
    result[dot + ext_len] = '\0';
    return result;
}

int main(int argc, char **argv) {
    /* Scan for --version / -V before any other processing so it short-
     * circuits cleanly and works regardless of other arg ordering. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            print_version();
            return 0;
        }
    }

    /* Language-server mode: speak LSP over stdio and never touch the normal
     * file-compilation path. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lsp") == 0) {
            return lsp_main();
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "usage: fcc <input.fc> [input2.fc ...] [-o output.c]\n"
                "       [@response.rsp] [--flag <name[=value]>] [--no-auto-detect] [--backtraces]\n"
                "       [--len-repr <16|32|64>]\n");
        return 1;
    }

    /* Expand gcc-style @response files into a flat token stream, then interpret
     * it (host auto-detect, --flag overrides, -o, inputs with file-relative
     * glob/rebase). The same args module backs the LSP's lsp.rsp discovery. */
    char *args_err = NULL;
    ExpandedArgs expanded;
    if (!args_expand(argc, argv, &expanded, &args_err)) {
        fprintf(stderr, "fcc: %s\n", args_err);
        free(args_err);
        return 1;
    }
    CompileArgs ca;
    if (!args_parse(&expanded, &ca)) {
        fprintf(stderr, "fcc: %s\n", ca.error);
        args_compile_free(&ca);
        args_expand_free(&expanded);
        return 1;
    }

    const char **input_paths = ca.inputs;
    int input_count = ca.input_count;
    const char *output_path = ca.output;
    Flag *flags = ca.flags;
    int flag_count = ca.flag_count;
    bool backtraces = ca.backtraces;
    g_len_repr = ca.len_repr;

    /* Initialize memory */
    Arena arena;
    arena_init(&arena);

    InternTable intern_table;
    intern_init(&intern_table, &arena);

    /* Lex, parse each input file and collect Programs */
    Program **programs = malloc(sizeof(Program*) * (size_t)input_count);
    char **sources = malloc(sizeof(char*) * (size_t)input_count);
    Token **all_tokens = malloc(sizeof(Token*) * (size_t)input_count);

    int *token_counts = malloc(sizeof(int) * (size_t)input_count);

    /* Lex every file first: the expression-position `<` scans need the
     * whole-program set of generic declaration names (any file may call a
     * generic declared in any other), collected from the token streams before
     * parsing begins. */
    for (int i = 0; i < input_count; i++) {
        diag_set_filename(input_paths[i]);
        sources[i] = read_file(input_paths[i]);

        Lexer lexer = {0};
        lexer_init(&lexer, sources[i], &intern_table, flags, flag_count);
        all_tokens[i] = lexer_tokenize(&lexer, &token_counts[i]);
    }

    const char **generic_names = NULL;
    int gn_count = 0, gn_cap = 0;
    for (int i = 0; i < input_count; i++)
        parser_collect_generic_names(all_tokens[i], token_counts[i], &intern_table,
                                     &generic_names, &gn_count, &gn_cap);

    for (int i = 0; i < input_count; i++) {
        diag_set_filename(input_paths[i]);
        Parser parser = {0};
        parser_init(&parser, all_tokens[i], token_counts[i], &arena, &intern_table);
        parser.filename = input_paths[i];
        parser.generic_names = generic_names;
        parser.generic_name_count = gn_count;
        parser.generic_gate = true;
        programs[i] = parse_program(&parser);
    }
    free(token_counts);
    free(generic_names);

    /* The parser now recovers from syntax errors (producing error nodes) instead of
       aborting on the first one, so all files are parsed and every syntax error is
       reported in one run. But error nodes must never reach pass1/codegen — gate here
       before the merge. (Item 2 relaxes the pass1 gate below; this parse gate stays.) */
    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Merge all programs into one.
     * Insert DECL_NAMESPACE(NULL) sentinel at the start of each file's decls
     * if the file doesn't begin with a DECL_NAMESPACE, so that the namespace
     * resets to global:: between files during pass1 iteration. */
    Program *prog;
    if (input_count == 1) {
        prog = programs[0];
    } else {
        int total_decls = 0;
        for (int i = 0; i < input_count; i++)
            total_decls += programs[i]->decl_count + 1; /* +1 for possible sentinel */

        prog = arena_alloc(&arena, sizeof(Program));
        prog->decls = arena_alloc(&arena, sizeof(Decl*) * (size_t)total_decls);
        prog->decl_count = 0;
        for (int i = 0; i < input_count; i++) {
            /* Check if this file starts with a DECL_NAMESPACE */
            bool has_ns = (programs[i]->decl_count > 0 &&
                           programs[i]->decls[0]->kind == DECL_NAMESPACE);
            if (!has_ns) {
                /* Inject a namespace-reset sentinel (global::) */
                Decl *sentinel = arena_alloc(&arena, sizeof(Decl));
                sentinel->kind = DECL_NAMESPACE;
                sentinel->loc = (SrcLoc){0};
                sentinel->is_private = false;
                sentinel->ns.name = NULL;
                prog->decls[prog->decl_count++] = sentinel;
            }
            for (int j = 0; j < programs[i]->decl_count; j++) {
                prog->decls[prog->decl_count++] = programs[i]->decls[j];
            }
        }
    }

    /* Set filename for diagnostics during later passes */
    if (input_count == 1) {
        diag_set_filename(input_paths[0]);
    } else {
        diag_set_filename("<merged>");
    }

    /* Pass 1: collect declarations */
    SymbolTable symtab = {0};
    symtab_init(&symtab);
    FileImportScopes file_scopes = { .scopes = NULL, .count = 0, .capacity = 0 };
    pass1_collect(prog, &symtab, &intern_table, &file_scopes, /*require_main=*/true);

    /* Pass 2: type check. Run it even when pass1 reported recoverable errors, so name
       errors and type errors surface together in one run. The parse gate above already
       returned on any syntax error, so the AST here has no error nodes — only pass1's
       symbol-table partiality to tolerate, which pass2 handles via its TYPE_ERROR poison.
       The post-pass2 gate below still fails the build (and keeps mono/codegen gated). */
    MonoTable mono = {0};
    pass2_check(prog, &symtab, &intern_table, &mono, &file_scopes, &arena);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Discover transitive monomorphized instances (generic-calling-generic) */
    mono_discover_transitive(&mono, &arena, &intern_table, &symtab);

    /* Finalize: sort monomorphized types for correct C emission order */
    mono_finalize_types(&mono, &arena, &intern_table, &symtab);

    /* Monomorphization can detect an infinite generic instantiation (a generic
     * that instantiates itself with an ever-growing type argument); bail before
     * emitting C built from a truncated/dangling instance family. */
    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Code generation */
    if (!output_path) {
        output_path = change_extension(input_paths[0], ".c");
    }

    FILE *out = fopen(output_path, "w");
    if (!out) {
        diag_fatal_simple("cannot open output '%s'", output_path);
    }

    CodegenOptions cg_opts = { .backtraces = backtraces };
    codegen_emit(prog, out, &mono, &arena, &intern_table, &symtab, &cg_opts);
    fclose(out);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        remove(output_path);
        /* Keep the error-code map in step with the .c it describes. */
        if (!output_is_null_device(output_path)) {
            char *stale_map = change_extension(output_path, ".errcodes");
            remove(stale_map);
            free(stale_map);
        }
        return 1;
    }

    /* Error-code map: a per-build artifact like the .c itself (the "strip the
     * binary, keep the map" channel — declared error codes are deliberately
     * not build-stable, so the map is the durable record; emitting it
     * unconditionally means it always exists for the build you shipped). One
     * line per declared error, sorted by code — deterministic, so CI can diff
     * the map across builds to see exactly which codes shifted. Reserved-range
     * passthrough codes (errno / Win32) belong to the platform's own
     * documentation and are not listed. A build that declares no errors
     * removes any stale map so it can never lie about the current build.
     * Check-only builds (output to the null device) skip the channel. */
    if (!output_is_null_device(output_path)) {
        char *map_path = change_extension(output_path, ".errcodes");
        if (error_code_count() > 0) {
            FILE *mf = fopen(map_path, "w");
            if (!mf) {
                diag_fatal_simple("cannot open error-code map '%s'", map_path);
            }
            for (int i = 0; i < error_code_count(); i++) {
                ErrorCodeInfo info = error_code_info(i);
                fprintf(mf, "%d\t%s\t%s:%d\n", FC_ERROR_CODE_BASE + i, info.qualified,
                        info.loc.filename ? info.loc.filename : "<unknown>",
                        info.loc.line);
            }
            fclose(mf);
        } else {
            remove(map_path);
        }
        free(map_path);
    }

    /* Cleanup */
    for (int i = 0; i < input_count; i++) {
        free(all_tokens[i]);
        free(sources[i]);
    }
    free(programs);
    free(sources);
    free(all_tokens);
    args_compile_free(&ca);      /* frees inputs + flags array + output */
    args_expand_free(&expanded); /* frees the token strings flags borrowed */
    symtab_free_nested(&symtab);  /* module member/import tables (malloc'd by pass1) */
    free(symtab.symbols);
    for (int i = 0; i < file_scopes.count; i++)
        free(file_scopes.scopes[i].imports.entries);
    free(file_scopes.scopes);
    free(mono.entries);
    free(intern_table.entries);  /* hash array is malloc'd; strings live in arena */
    arena_free(&arena);

    return 0;
}
