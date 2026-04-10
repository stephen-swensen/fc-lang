#ifndef FC_PLATFORM_H
#define FC_PLATFORM_H

#include "lexer.h"

/* Auto-detect host platform by probing the C compiler's predefined macros.
 *
 * Runs `<cc> -dM -E -x c /dev/null` and parses the `#define` lines to set
 * built-in flags in the canonical taxonomy:
 *
 *   os  = linux | macos | windows | freebsd
 *   arch = x86_64 | aarch64 | arm | riscv64 | wasm32
 *   env = gnu | msvc
 *
 * Any axis whose value cannot be determined is left unset rather than guessed.
 * Appends entries to the given flag array; caller must ensure the array can
 * grow via DA_APPEND. Detected names/values are strdup-allocated and owned by
 * the flag array (do not appear in argv).
 *
 * cc: the C compiler to probe (e.g., "cc", "gcc", "clang"). Must be non-NULL.
 */
void platform_detect_flags(Flag **flags, int *count, int *cap, const char *cc);

#endif
