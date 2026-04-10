/* popen/pclose and strdup are POSIX extensions; request them under -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rule for detecting OS and arch axes from a single compiler macro.
 * First match within an axis wins. */
typedef struct {
    const char *macro;
    const char *axis;
    const char *value;
} DetectRule;

static const DetectRule OS_ARCH_RULES[] = {
    /* OS */
    {"__linux__",    "os",   "linux"},
    {"__APPLE__",    "os",   "macos"},
    {"__FreeBSD__",  "os",   "freebsd"},
    {"_WIN32",       "os",   "windows"},

    /* Arch */
    {"__x86_64__",   "arch", "x86_64"},
    {"_M_X64",       "arch", "x86_64"},
    {"__aarch64__",  "arch", "aarch64"},
    {"_M_ARM64",     "arch", "aarch64"},
    {"__arm__",      "arch", "arm"},
    {"_M_ARM",       "arch", "arm"},
    {"__riscv",      "arch", "riscv64"},
    {"__wasm32__",   "arch", "wasm32"},
};

static const int OS_ARCH_RULE_COUNT =
    (int)(sizeof(OS_ARCH_RULES) / sizeof(OS_ARCH_RULES[0]));

static bool has_axis(const Flag *flags, int count, const char *axis) {
    int axis_len = (int)strlen(axis);
    for (int i = 0; i < count; i++) {
        if (flags[i].name_len == axis_len &&
            memcmp(flags[i].name, axis, (size_t)axis_len) == 0) {
            return true;
        }
    }
    return false;
}

static const char *axis_value(const Flag *flags, int count, const char *axis) {
    int axis_len = (int)strlen(axis);
    for (int i = 0; i < count; i++) {
        if (flags[i].name_len == axis_len &&
            memcmp(flags[i].name, axis, (size_t)axis_len) == 0) {
            return flags[i].value;
        }
    }
    return NULL;
}

static void add_detected(Flag **flags, int *count, int *cap,
                          const char *axis, const char *value) {
    if (has_axis(*flags, *count, axis)) return;
    Flag f = {0};
    f.name = strdup(axis);
    f.name_len = (int)strlen(axis);
    f.value = strdup(value);
    DA_APPEND(*flags, *count, *cap, f);
}

/* Returns true if NAME appears in the seen-macros set. */
static bool seen_has(char **seen, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(seen[i], name) == 0) return true;
    }
    return false;
}

/* Scan a line of `cc -dM -E` output and return the macro name as a
 * newly-allocated null-terminated string, or NULL if the line doesn't
 * match `#define NAME ...`. Caller frees. */
static char *parse_macro_name(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "#define", 7) != 0) return NULL;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '(' && *p != '\n') p++;
    int len = (int)(p - start);
    if (len == 0) return NULL;
    char *name = malloc((size_t)len + 1);
    memcpy(name, start, (size_t)len);
    name[len] = '\0';
    return name;
}

void platform_detect_flags(Flag **flags, int *count, int *cap, const char *cc) {
    char cmd[1024];
    int n = snprintf(cmd, sizeof cmd,
                     "%s -dM -E -x c /dev/null 2>/dev/null", cc);
    if (n < 0 || n >= (int)sizeof cmd) return;

    FILE *p = popen(cmd, "r");
    if (!p) return;

    /* Collect every macro the compiler defined into a flat set. */
    char **seen = NULL;
    int seen_count = 0, seen_cap = 0;

    char line[4096];
    while (fgets(line, sizeof line, p)) {
        char *name = parse_macro_name(line);
        if (!name) continue;
        DA_APPEND(seen, seen_count, seen_cap, name);
    }
    pclose(p);

    /* Pass 1: set os and arch from the first-matching rule per axis. */
    for (int r = 0; r < OS_ARCH_RULE_COUNT; r++) {
        const DetectRule *rule = &OS_ARCH_RULES[r];
        if (seen_has(seen, seen_count, rule->macro)) {
            add_detected(flags, count, cap, rule->axis, rule->value);
        }
    }

    /* Pass 2: env axis is meaningful only for linux (gnu/musl) and windows
     * (gnu/msvc). For macos and freebsd the env is implied by the OS and we
     * leave it unset. Musl detection requires header probing we don't do
     * yet; glibc is assumed on linux. */
    const char *os = axis_value(*flags, *count, "os");
    if (os) {
        if (strcmp(os, "windows") == 0) {
            if (seen_has(seen, seen_count, "__MINGW64__") ||
                seen_has(seen, seen_count, "__MINGW32__")) {
                add_detected(flags, count, cap, "env", "gnu");
            } else if (seen_has(seen, seen_count, "_MSC_VER")) {
                add_detected(flags, count, cap, "env", "msvc");
            }
        } else if (strcmp(os, "linux") == 0) {
            add_detected(flags, count, cap, "env", "gnu");
        }
    }

    for (int s = 0; s < seen_count; s++) free(seen[s]);
    free(seen);
}
