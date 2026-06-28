/* Expose POSIX glob() under -std=c11 on glibc (it sits behind __STRICT_ANSI__
 * otherwise). Must precede every #include. Path canonicalization goes through
 * platform_realpath() rather than realpath() directly, so the mingw-w64 /
 * UCRT64 gap is handled there; the missing <glob.h> on that toolchain is
 * covered by the compatible shim provided further down. */
#define _DEFAULT_SOURCE

#include "args.h"
#include "common.h"     /* DA_APPEND */
#include "platform.h"   /* platform_detect_flags */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <glob.h>
#endif

#define ARGS_MAX_DEPTH 32   /* nesting backstop in case realpath cycle check fails */

/* ---- small helpers ---- */

/* malloc'd formatted message (caller frees). */
static char *msgf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    char *s = malloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

static char *dupn(const char *s, int n) {
    char *r = malloc((size_t)n + 1);
    memcpy(r, s, (size_t)n);
    r[n] = '\0';
    return r;
}

static bool path_is_abs(const char *p) {
    return p[0] == '/';
}

/* Directory portion of `path` (everything before the last '/'), malloc'd.
 * "." when there is no '/', "/" for a root-level path like "/foo". */
static char *path_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return dupn(".", 1);
    if (slash == path) return dupn("/", 1);
    return dupn(path, (int)(slash - path));
}

/* "dir/rel", malloc'd. */
static char *path_join(const char *dir, const char *rel) {
    size_t dn = strlen(dir), rn = strlen(rel);
    char *p = malloc(dn + 1 + rn + 1);
    memcpy(p, dir, dn);
    p[dn] = '/';
    memcpy(p + dn + 1, rel, rn);
    p[dn + 1 + rn] = '\0';
    return p;
}

static char *read_file_or_null(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    size_t rd = fread(buf, 1, (size_t)size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* ---- expansion ---- */

/* Emit a token + provenance dir, keeping the two arrays index-aligned. Both
 * pointers are taken by ownership. */
static void emit(ExpandedArgs *out, char *tok, char *dir) {
    if (out->count >= out->cap) {
        out->cap = out->cap ? out->cap * 2 : 16;
        out->tokens = realloc(out->tokens, (size_t)out->cap * sizeof(char *));
        out->dirs   = realloc(out->dirs,   (size_t)out->cap * sizeof(char *));
    }
    out->tokens[out->count] = tok;
    out->dirs[out->count]   = dir;
    out->count++;
}

/* Stack of canonical paths currently being expanded (cycle detection). */
typedef struct { char **v; int n, cap; } PathStack;

static bool expand_file(ExpandedArgs *out, const char *rsp_path,
                        PathStack *stack, int depth, char **err) {
    if (depth > ARGS_MAX_DEPTH) {
        *err = msgf("response files nested too deeply at '%s'", rsp_path);
        return false;
    }

    /* Cycle detection: a file that is already on the active stack would loop. */
    char *canon = platform_realpath(rsp_path);
    if (!canon) {
        *err = msgf("cannot open response file '%s'", rsp_path);
        return false;
    }
    for (int i = 0; i < stack->n; i++) {
        if (strcmp(stack->v[i], canon) == 0) {
            *err = msgf("recursive response file '%s'", rsp_path);
            free(canon);
            return false;
        }
    }

    char *buf = read_file_or_null(rsp_path);
    if (!buf) {
        *err = msgf("cannot open response file '%s'", rsp_path);
        free(canon);
        return false;
    }

    DA_APPEND(stack->v, stack->n, stack->cap, canon);   /* push (owns canon) */
    char *srcdir = path_dir(rsp_path);

    bool ok = true;
    for (char *p = buf; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '#' || (p[0] == '/' && p[1] == '/')) {   /* comment to EOL */
            while (*p && *p != '\n') p++;
            continue;
        }
        char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        int len = (int)(p - s);

        if (s[0] == '@' && len > 1) {                      /* nested rsp */
            char *child = dupn(s + 1, len - 1);
            char *cpath = path_is_abs(child) ? child : path_join(srcdir, child);
            if (cpath != child) free(child);
            ok = expand_file(out, cpath, stack, depth + 1, err);
            free(cpath);
            if (!ok) break;
        } else {
            emit(out, dupn(s, len), dupn(srcdir, (int)strlen(srcdir)));
        }
    }

    free(srcdir);
    free(buf);
    stack->n--;                                            /* pop */
    free(canon);
    return ok;
}

bool args_expand(int argc, char **argv, ExpandedArgs *out, char **err) {
    memset(out, 0, sizeof *out);
    *err = NULL;

    PathStack stack = {0};
    bool ok = true;
    for (int i = 0; i < argc; i++) {
        const char *t = argv[i];
        if (t[0] == '@' && t[1] != '\0') {                 /* top-level rsp: cwd-relative */
            if (!expand_file(out, t + 1, &stack, 0, err)) { ok = false; break; }
        } else {
            emit(out, dupn(t, (int)strlen(t)), NULL);      /* directly typed: verbatim */
        }
    }

    for (int i = 0; i < stack.n; i++) free(stack.v[i]);    /* empty unless we broke out */
    free(stack.v);

    if (!ok) args_expand_free(out);
    return ok;
}

void args_expand_free(ExpandedArgs *e) {
    for (int i = 0; i < e->count; i++) {
        free(e->tokens[i]);
        free(e->dirs[i]);
    }
    free(e->tokens);
    free(e->dirs);
    memset(e, 0, sizeof *e);
}

/* ---- glob() shim for Windows ---- */

/* mingw-w64 / UCRT64 has no <glob.h> and no POSIX glob(). add_inputs() needs
 * only the narrow slice it uses: GLOB_NOCHECK expansion of a forward-slash
 * input pattern into matching paths, with the pattern passed through verbatim
 * when nothing matches (the bash default). We provide a compatible glob_t /
 * glob() / globfree() over the Win32 directory API so the call site below stays
 * byte-for-byte identical across platforms.
 *
 * Wildcards are matched by our own wc_match (not Win32's quirky DOS matcher,
 * which folds short names and case) for POSIX-consistent semantics: '*' (any
 * run, never crossing '/'), '?' (one char), and '[...]' classes with ranges and
 * leading '!'/'^' negation. Wildcards may appear in any path component. MSYS
 * mount paths ('/c/...') are not understood by the native Win32 API, so they
 * simply won't match and fall through literally — the same limitation the rest
 * of the native binary already has with such paths. */
#if defined(_WIN32)

typedef struct { size_t gl_pathc; char **gl_pathv; } glob_t;
#define GLOB_NOCHECK 0x1

/* fnmatch-style matcher for a single path component (never contains '/'). */
static bool wc_match(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;              /* collapse a run of stars */
            if (!*p) return true;               /* trailing '*' matches the rest */
            for (const char *t = s; ; t++) {
                if (wc_match(p, t)) return true;
                if (!*t) return false;
            }
        } else if (*p == '?') {
            if (!*s) return false;
            p++; s++;
        } else if (*p == '[') {
            const char *q = p + 1;
            bool neg = (*q == '!' || *q == '^');
            if (neg) q++;
            const char *first = q;
            bool matched = false;
            while (*q && (*q != ']' || q == first)) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    unsigned char lo = (unsigned char)q[0], hi = (unsigned char)q[2];
                    unsigned char c = (unsigned char)*s;
                    if (*s && c >= lo && c <= hi) matched = true;
                    q += 3;
                } else {
                    if (*s && *s == *q) matched = true;
                    q++;
                }
            }
            if (*q != ']') {                    /* unterminated class: literal '[' */
                if (*s != '[') return false;
                p++; s++;
            } else {
                if (neg) matched = !matched;
                if (!matched || !*s) return false;
                p = q + 1; s++;
            }
        } else {
            if (*p != *s) return false;
            p++; s++;
        }
    }
    return *s == '\0';
}

typedef struct { char **v; int n, cap; } GlobList;

/* "base/comp", or just "comp" when base is empty (caller frees). */
static char *glob_join(const char *base, const char *comp) {
    return base[0] ? path_join(base, comp) : dupn(comp, (int)strlen(comp));
}

/* Match `comp[i..n)` under directory `base`, appending full paths to `out`.
 * Non-wildcard components are descended literally; a wildcard component
 * enumerates `base` and matches each entry. Only the leaf may match files;
 * intermediate wildcard components descend into directories only. */
static void glob_walk(const char *base, char **comp, int n, int i, GlobList *out) {
    const char *c = comp[i];
    bool leaf = (i == n - 1);

    if (!strpbrk(c, "*?[")) {                   /* literal component */
        char *next = glob_join(base, c);
        if (leaf) {
            if (GetFileAttributesA(next) != INVALID_FILE_ATTRIBUTES)
                DA_APPEND(out->v, out->n, out->cap, next);
            else free(next);
        } else {
            glob_walk(next, comp, n, i + 1, out);
            free(next);
        }
        return;
    }

    char *spec = base[0] ? path_join(base, "*") : dupn("*", 1);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    free(spec);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
            continue;                           /* skip "." and ".." */
        if (!wc_match(c, name)) continue;
        char *next = glob_join(base, name);
        if (leaf) {
            DA_APPEND(out->v, out->n, out->cap, next);
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            glob_walk(next, comp, n, i + 1, out);
            free(next);
        } else {
            free(next);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int glob(const char *pattern, int flags, void *errfunc, glob_t *pg) {
    (void)flags; (void)errfunc;                 /* only GLOB_NOCHECK is modelled */
    pg->gl_pathc = 0;
    pg->gl_pathv = NULL;

    int ncomp = 1;
    for (const char *q = pattern; *q; q++) if (*q == '/') ncomp++;
    char **comp = malloc((size_t)ncomp * sizeof *comp);
    int k = 0;
    const char *start = pattern;
    for (const char *q = pattern; ; q++) {
        if (*q == '/' || !*q) {
            comp[k++] = dupn(start, (int)(q - start));
            start = q + 1;
            if (!*q) break;
        }
    }

    GlobList list = {0};
    glob_walk("", comp, ncomp, 0, &list);

    for (int j = 0; j < ncomp; j++) free(comp[j]);
    free(comp);

    /* GLOB_NOCHECK: nothing matched -> yield the pattern verbatim. */
    if (list.n == 0)
        DA_APPEND(list.v, list.n, list.cap, dupn(pattern, (int)strlen(pattern)));

    pg->gl_pathc = (size_t)list.n;
    pg->gl_pathv = list.v;
    return 0;
}

static void globfree(glob_t *pg) {
    for (size_t i = 0; i < pg->gl_pathc; i++) free(pg->gl_pathv[i]);
    free(pg->gl_pathv);
    pg->gl_pathc = 0;
    pg->gl_pathv = NULL;
}

#endif /* _WIN32 */

/* ---- parsing ---- */

/* Append the inputs named by `tok`. Directly-typed tokens (dir == NULL) were
 * already shell-globbed, so they pass through verbatim. Response-file tokens
 * are rebased relative to their file's dir and glob-expanded; GLOB_NOCHECK
 * makes a pattern matching nothing pass through literally (bash default), so it
 * surfaces later as a clear "cannot open" rather than vanishing. */
static bool add_inputs(CompileArgs *out, const char *tok, const char *dir) {
    if (!dir) {
        DA_APPEND(out->inputs, out->input_count, out->input_cap, dupn(tok, (int)strlen(tok)));
        return true;
    }
    char *pat = path_is_abs(tok) ? dupn(tok, (int)strlen(tok)) : path_join(dir, tok);
    glob_t g;
    int rc = glob(pat, GLOB_NOCHECK, NULL, &g);
    if (rc != 0) {                                          /* NOSPACE / ABORTED */
        out->error = msgf("cannot expand input pattern '%s'", pat);
        free(pat);
        return false;
    }
    for (size_t k = 0; k < g.gl_pathc; k++) {
        const char *m = g.gl_pathv[k];
        DA_APPEND(out->inputs, out->input_count, out->input_cap, dupn(m, (int)strlen(m)));
    }
    globfree(&g);
    free(pat);
    return true;
}

bool args_parse(const ExpandedArgs *e, CompileArgs *out) {
    memset(out, 0, sizeof *out);

    /* Auto-detect host flags unless suppressed (matches the historical CLI
     * default); later --flag entries override by name. */
    bool auto_detect = true;
    for (int i = 1; i < e->count; i++)
        if (strcmp(e->tokens[i], "--no-auto-detect") == 0) auto_detect = false;
    if (auto_detect)
        platform_detect_flags(&out->flags, &out->flag_count, &out->flag_cap);

    for (int i = 1; i < e->count; i++) {
        const char *a = e->tokens[i];

        if (strcmp(a, "-o") == 0 && i + 1 < e->count) {
            i++;
            const char *val = e->tokens[i];
            const char *vdir = e->dirs[i];
            free(out->output);
            out->output = (vdir && !path_is_abs(val)) ? path_join(vdir, val)
                                                       : dupn(val, (int)strlen(val));
        } else if (strcmp(a, "--no-auto-detect") == 0) {
            /* handled above */
        } else if (strcmp(a, "--backtraces") == 0) {
            out->backtraces = true;
        } else if (strcmp(a, "--flag") == 0 && i + 1 < e->count) {
            const char *arg = e->tokens[++i];      /* borrowed: name/value point into e */
            const char *eq = strchr(arg, '=');
            Flag f = {0};
            if (eq) {
                if (eq == arg) { out->error = msgf("--flag requires a name before '='"); return false; }
                if (*(eq + 1) == '\0') { out->error = msgf("--flag '%s' has empty value after '='", arg); return false; }
                f.name = arg;
                f.name_len = (int)(eq - arg);
                f.value = eq + 1;
            } else {
                if (*arg == '\0') { out->error = msgf("--flag requires a name"); return false; }
                f.name = arg;
                f.name_len = (int)strlen(arg);
                f.value = NULL;
            }
            bool replaced = false;
            for (int j = 0; j < out->flag_count; j++) {
                if (out->flags[j].name_len == f.name_len &&
                    memcmp(out->flags[j].name, f.name, (size_t)f.name_len) == 0) {
                    out->flags[j] = f;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) DA_APPEND(out->flags, out->flag_count, out->flag_cap, f);
        } else if (a[0] != '-') {
            if (!add_inputs(out, a, e->dirs[i])) return false;
        } else {
            out->error = msgf("unknown option '%s'", a);
            return false;
        }
    }

    if (out->input_count == 0) {
        out->error = msgf("no input file");
        return false;
    }
    return true;
}

void args_compile_free(CompileArgs *c) {
    for (int i = 0; i < c->input_count; i++) free((void *)c->inputs[i]);
    free(c->inputs);
    free(c->flags);
    free(c->output);
    free(c->error);
    memset(c, 0, sizeof *c);
}
