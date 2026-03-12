#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "pass1.h"
#include "pass2.h"
#include "codegen.h"
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
        fprintf(stderr, "usage: fc <input.fc> [-o output.c]\n");
        return 1;
    }

    const char *input_path = NULL;
    const char *output_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            fprintf(stderr, "fc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input_path) {
        fprintf(stderr, "fc: no input file\n");
        return 1;
    }

    diag_set_filename(input_path);

    /* Read source */
    char *source = read_file(input_path);

    /* Initialize memory */
    Arena arena;
    arena_init(&arena);

    InternTable intern_table;
    intern_init(&intern_table, &arena);

    /* Lex */
    Lexer lexer;
    lexer_init(&lexer, source, &intern_table);
    int token_count;
    Token *tokens = lexer_tokenize(&lexer, &token_count);

    /* Parse */
    Parser parser;
    parser_init(&parser, tokens, token_count, &arena, &intern_table);
    Program *prog = parse_program(&parser);

    /* Pass 1: collect declarations */
    SymbolTable symtab;
    symtab_init(&symtab);
    pass1_collect(prog, &symtab);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Pass 2: type check */
    pass2_check(prog, &symtab);

    if (diag_error_count() > 0) {
        fprintf(stderr, "%d error(s)\n", diag_error_count());
        return 1;
    }

    /* Code generation */
    if (!output_path) {
        output_path = change_extension(input_path, ".c");
    }

    FILE *out = fopen(output_path, "w");
    if (!out) {
        diag_fatal_simple("cannot open output '%s'", output_path);
    }

    codegen_emit(prog, out);
    fclose(out);

    /* Cleanup */
    free(tokens);
    free(symtab.symbols);
    free(source);
    arena_free(&arena);

    return 0;
}
