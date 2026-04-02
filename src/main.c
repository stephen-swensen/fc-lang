#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include "codegen.h"
#include "monomorph.h"
#include "diag.h"
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
    if (argc < 2) {
        fprintf(stderr, "usage: fc <input.fc> [input2.fc ...] [-o output.c]\n");
        return 1;
    }

    const char **input_paths = NULL;
    int input_count = 0, input_cap = 0;
    const char *output_path = NULL;
    const char **flags = NULL;
    int flag_count = 0, flag_cap = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--flag") == 0 && i + 1 < argc) {
            DA_APPEND(flags, flag_count, flag_cap, argv[++i]);
        } else if (argv[i][0] != '-') {
            DA_APPEND(input_paths, input_count, input_cap, argv[i]);
        } else {
            fprintf(stderr, "fc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (input_count == 0) {
        fprintf(stderr, "fc: no input file\n");
        return 1;
    }

    /* Initialize memory */
    Arena arena;
    arena_init(&arena);

    InternTable intern_table;
    intern_init(&intern_table, &arena);

    /* Lex, parse each input file and collect Programs */
    Program **programs = malloc(sizeof(Program*) * (size_t)input_count);
    char **sources = malloc(sizeof(char*) * (size_t)input_count);
    Token **all_tokens = malloc(sizeof(Token*) * (size_t)input_count);

    for (int i = 0; i < input_count; i++) {
        diag_set_filename(input_paths[i]);
        sources[i] = read_file(input_paths[i]);

        Lexer lexer;
        lexer_init(&lexer, sources[i], &intern_table, flags, flag_count);
        int token_count;
        all_tokens[i] = lexer_tokenize(&lexer, &token_count);

        Parser parser;
        parser_init(&parser, all_tokens[i], token_count, &arena, &intern_table);
        parser.filename = input_paths[i];
        programs[i] = parse_program(&parser);
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
    SymbolTable symtab;
    symtab_init(&symtab);
    FileImportScopes file_scopes = { .scopes = NULL, .count = 0, .capacity = 0 };
    pass1_collect(prog, &symtab, &intern_table, &file_scopes);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Pass 2: type check */
    MonoTable mono = {0};
    pass2_check(prog, &symtab, &intern_table, &mono, &file_scopes);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Discover transitive monomorphized instances (generic-calling-generic) */
    mono_discover_transitive(&mono, &arena, &intern_table, &symtab);

    /* Finalize: sort monomorphized types for correct C emission order */
    mono_finalize_types(&mono, &arena, &intern_table, &symtab);

    /* Code generation */
    if (!output_path) {
        output_path = change_extension(input_paths[0], ".c");
    }

    FILE *out = fopen(output_path, "w");
    if (!out) {
        diag_fatal_simple("cannot open output '%s'", output_path);
    }

    codegen_emit(prog, out, &mono, &arena, &intern_table, &symtab);
    fclose(out);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        remove(output_path);
        return 1;
    }

    /* Cleanup */
    for (int i = 0; i < input_count; i++) {
        free(all_tokens[i]);
        free(sources[i]);
    }
    free(programs);
    free(sources);
    free(all_tokens);
    free(input_paths);
    free(flags);
    free(symtab.symbols);
    free(mono.entries);
    arena_free(&arena);

    return 0;
}
