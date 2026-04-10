#include "platform.h"
#include "common.h"
#include <string.h>

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

void platform_detect_flags(Flag **flags, int *count, int *cap) {
    /* OS */
#if defined(__linux__)
    add_static(flags, count, cap, "os", "linux");
#elif defined(__APPLE__)
    add_static(flags, count, cap, "os", "macos");
#elif defined(__FreeBSD__)
    add_static(flags, count, cap, "os", "freebsd");
#elif defined(_WIN32)
    add_static(flags, count, cap, "os", "windows");
#endif

    /* Arch */
#if defined(__x86_64__) || defined(_M_X64)
    add_static(flags, count, cap, "arch", "x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    add_static(flags, count, cap, "arch", "aarch64");
#elif defined(__arm__) || defined(_M_ARM)
    add_static(flags, count, cap, "arch", "arm");
#elif defined(__riscv)
    add_static(flags, count, cap, "arch", "riscv64");
#elif defined(__wasm32__)
    add_static(flags, count, cap, "arch", "wasm32");
#endif

    /* Env. Meaningful only on Linux (gnu/musl, with musl detection deferred)
     * and Windows (gnu under MinGW; native MSVC is unsupported). Implied by
     * the OS on macOS and FreeBSD, so left unset there. */
#if defined(__linux__)
    add_static(flags, count, cap, "env", "gnu");
#elif defined(_WIN32) && (defined(__MINGW32__) || defined(__MINGW64__))
    add_static(flags, count, cap, "env", "gnu");
#endif
}
