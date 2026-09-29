#pragma once

#include "lexer.h"

/* Populate built-in os/arch/env flags for the host that built the fcc binary.
 *
 * The values come from #ifdef checks against the C compiler's predefined
 * macros, so they are fixed when fcc is built (as zig, rust, and gcc do for
 * their host triple). Values:
 *
 *   os   = linux | macos | windows | freebsd
 *   arch = x86_64 | aarch64 | arm | riscv64 | wasm32
 *   env  = gnu  (the only documented value; native MSVC is unsupported)
 *
 * An axis that cannot be determined is left unset. `--flag name=value`
 * overrides any axis, and `--no-auto-detect` suppresses these defaults.
 *
 * Appends to the given flag array with DA_APPEND. Names and values are string
 * literals, not heap-allocated.
 */
void platform_detect_flags(Flag **flags, int *count, int *cap);

/* Same #ifdef-derived values exposed individually, for `fcc --version` and
 * other introspection. NULL when an axis is not determinable on the host. */
const char *platform_get_os(void);
const char *platform_get_arch(void);
const char *platform_get_env(void);

/* Canonical absolute path for `path` (malloc'd; caller frees), or NULL on
 * failure. POSIX uses realpath() (resolves symlinks, `..`, and relative
 * spellings). Windows uses GetFullPathNameA() lexical resolution, then turns
 * `\` into `/`: mingw-w64 / UCRT64 hides realpath() under -std=c11 and would
 * truncate its pointer return to int. The result uses forward slashes on both
 * platforms, so callers can keep their `/`-based path handling. */
char *platform_realpath(const char *path);
