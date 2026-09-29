/* Expose POSIX realpath() under -std=c11 (glibc hides it behind __STRICT_ANSI__
 * otherwise). Must precede every #include. */
#define _DEFAULT_SOURCE

#include "platform.h"
#include "common.h"
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#endif

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

/* Append an axis=value pair if not already set. Both strings must be string
 * literals (or otherwise have program-lifetime storage); we never free them. */
static void add_static(Flag **flags, int *count, int *cap,
                        const char *axis, const char *value) {
    if (has_axis(*flags, *count, axis)) return;
    Flag f = {0};
    f.name = axis;
    f.name_len = (int)strlen(axis);
    f.value = value;
    DA_APPEND(*flags, *count, *cap, f);
}


const char *platform_get_os(void) {
#if defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(_WIN32)
    return "windows";
#else
    return NULL;
#endif
}

const char *platform_get_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv)
    return "riscv64";
#elif defined(__wasm32__)
    return "wasm32";
#else
    return NULL;
#endif
}

const char *platform_get_env(void) {
#if defined(__linux__)
    return "gnu";
#elif defined(_WIN32) && (defined(__MINGW32__) || defined(__MINGW64__))
    return "gnu";
#else
    return NULL;
#endif
}

void platform_detect_flags(Flag **flags, int *count, int *cap) {
    const char *axes[][2] = {
        { "os",   platform_get_os() },
        { "arch", platform_get_arch() },
        /* Env is meaningful only on Linux (gnu; musl is not detected) and
         * Windows (gnu under MinGW; native MSVC is unsupported). On macOS and
         * FreeBSD the OS implies it. */
        { "env",  platform_get_env() },
    };
    for (int i = 0; i < 3; i++)
        if (axes[i][1]) add_static(flags, count, cap, axes[i][0], axes[i][1]);
}

char *platform_realpath(const char *path) {
#if defined(_WIN32)
    /* First call sizes the buffer (return value includes the terminating NUL);
     * second call fills it. GetFullPathNameA resolves `..` and makes the path
     * absolute lexically. It does not require the file to exist, which suits
     * both uses (cycle-detection keys and path-dedup keys). */
    DWORD need = GetFullPathNameA(path, 0, NULL, NULL);
    if (need == 0) return NULL;
    char *buf = malloc(need);
    if (!buf) return NULL;
    DWORD wrote = GetFullPathNameA(path, need, buf, NULL);
    if (wrote == 0 || wrote >= need) { free(buf); return NULL; }
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    return buf;
#else
    return realpath(path, NULL);
#endif
}
