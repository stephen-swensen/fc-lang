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

#endif
