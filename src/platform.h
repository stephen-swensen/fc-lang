#ifndef FC_PLATFORM_H
#define FC_PLATFORM_H

#include "lexer.h"

/* Populate built-in os/arch/env flags for the host that built the fcc binary.
 *
 * Detection happens at fcc compile time via #ifdef checks against the C
 * compiler's predefined macros — there is no runtime subprocess. The values
 * are baked into fcc itself when it is built, which is fast (~no overhead per
 * invocation) and matches what zig, rust, and gcc do for their host triple.
 *
 * Canonical taxonomy:
 *
 *   os  = linux | macos | windows | freebsd
 *   arch = x86_64 | aarch64 | arm | riscv64 | wasm32
 *   env = gnu  (the only documented value; native MSVC is unsupported)
 *
 * Any axis whose value cannot be determined at fc build time is left unset.
 * Users can override any axis at runtime via `--flag name=value`, and the
 * `--no-auto-detect` CLI flag suppresses these defaults entirely.
 *
 * Appends entries to the given flag array; caller must ensure it can grow
 * via DA_APPEND. Names and values are static string literals owned by the
 * compiled binary, not heap-allocated.
 */
void platform_detect_flags(Flag **flags, int *count, int *cap);

/* Same #ifdef-derived values exposed individually, for `fcc --version` and
 * other introspection. NULL when an axis is not determinable on the host. */
const char *platform_get_os(void);
const char *platform_get_arch(void);
const char *platform_get_env(void);

/* Canonical absolute path for `path` (malloc'd; caller frees), or NULL on
 * failure. POSIX uses realpath() (resolves symlinks, `..`, and relative
 * spellings); Windows uses GetFullPathNameA() lexical resolution with `\` then
 * normalized to `/` — mingw-w64 / UCRT64 hides realpath() under -std=c11 and
 * would truncate its pointer return to int. The result uses forward slashes on
 * both platforms, so callers can keep their `/`-based path handling. */
char *platform_realpath(const char *path);

#endif
