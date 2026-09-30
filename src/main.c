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

static void print_usage(FILE *f) {
    fputs("usage: fcc [options] <input.fc>... [-o <output.c>]\n"
          "       fcc --lsp\n"
          "\n"
          "options:\n"
          "  -o <file>              write the C to <file> (default: the first input\n"
          "                         with a .c extension)\n"
          "  @<file>                read more arguments from a response file\n"
          "  --flag <name[=value]>  set a conditional-compilation flag\n"
          "  --no-auto-detect       don't set the host's os/arch/env flags\n"
          "  --backtraces           print an FC backtrace when the program aborts\n"
          "  --len-repr <16|32|64>  stored width of slice lengths (default 64)\n"
          "  --lsp                  run as a language server on stdin/stdout\n"
          "  -V, --version          print version and build information\n"
          "  -h, --help             print this help\n", f);
}

static char *change_extension(const char *path, const char *new_ext) {
    int len = (int)strlen(path);
    int dot = len;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '.') { dot = i; break; }
        if (path[i] == '/') break;
    }
    return str_sprintf("%.*s%s", dot, path, new_ext);
}

int main(int argc, char **argv) {
    /* --help and --version answer before anything else, wherever they appear. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
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
        print_usage(stderr);
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
    char **sources = xmalloc(sizeof(char*) * (size_t)input_count);
    Token **all_tokens = xmalloc(sizeof(Token*) * (size_t)input_count);

    int *token_counts = xmalloc(sizeof(int) * (size_t)input_count);

    /* Lex every file first: the expression-position `<` scans need the
     * whole-program set of generic declaration names (any file may call a
     * generic declared in any other), collected from the token streams before
     * parsing begins. */
    for (int i = 0; i < input_count; i++) {
        diag_set_filename(input_paths[i]);
        sources[i] = read_file(input_paths[i], NULL);
        if (!sources[i]) diag_fatal_simple("cannot open '%s'", input_paths[i]);

        Lexer lexer = {0};
        lexer_init(&lexer, sources[i], &intern_table, flags, flag_count);
        all_tokens[i] = lexer_tokenize(&lexer, &token_counts[i]);
    }

    Program *prog = parse_files(all_tokens, token_counts, input_paths, input_count,
                                  &arena, &intern_table);
    free(token_counts);

    /* Every syntax error has been reported; the error nodes standing in for
     * them must not reach the later passes. */
    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
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

    /* Pass 2: type check. It runs even when pass1 reported errors, so name
       errors and type errors are reported together; it tolerates pass1's partial
       symbol tables by typing what depends on them as TYPE_ERROR. */
    MonoTable mono = {0};
    pass2_check(prog, &symtab, &intern_table, &mono, &file_scopes, &arena);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Discover transitive monomorphized instances (generic-calling-generic) */
    mono_discover_transitive(&mono, &arena, &intern_table, &symtab);

    /* Finalize: resolve every instance's type names to their C names. (Codegen
     * orders the type definitions; see emit_types.) */
    mono_finalize_types(&mono, &arena, &intern_table, &symtab);

    /* Monomorphization reports its own errors (an infinite instantiation
     * family, a failed static_assert or const evaluation in an instance);
     * don't emit C from a broken instance set. */
    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Code generation */
    if (!output_path) {
        output_path = change_extension(input_paths[0], ".c");
    }

    /* Opened for update: codegen writes the body, reads it back to choose the
     * preamble, then rewrites the file from its start. */
    FILE *out = fopen(output_path, "w+");
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

    /* Error-code map, written beside the .c on every build. Declared error
     * codes are not stable across builds, so the map is the record of what
     * each code means in the build that shipped. One line per declared error,
     * sorted by code, so maps from two builds diff cleanly. Reserved-range
     * passthrough codes (errno / Win32) are not listed. A build that declares
     * no errors removes any stale map. Check-only builds (output to the null
     * device) write no map. */
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
    free(sources);
    free(all_tokens);
    args_compile_free(&ca);      /* frees inputs + flags array + output */
    args_expand_free(&expanded); /* frees the token strings flags borrowed */
    front_end_free(&symtab, &intern_table, &mono, &file_scopes, &arena);

    return 0;
}
