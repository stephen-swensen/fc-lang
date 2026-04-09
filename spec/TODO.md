# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Packed structs and bit-level layout control**: Support `packed struct` for exact bit layout without padding, and potentially bit fields. Currently FC emits C structs with C's default layout rules, offering no control over padding or alignment beyond what the C compiler provides. Packed structs would enable memory-mapped I/O, binary protocol parsing, and compact serialization formats.

- **Platform flags and cross-platform support model**: The current `--flag windows` approach conflates OS with toolchain/environment. MSYS2 UCRT64 is "Windows" but with GCC/MinGW and partial POSIX compatibility, which is fundamentally different from native MSVC. Research into how Zig and Rust handle this:

  **Both use a structured target taxonomy** decomposed into typed axes, not flat string flags:
  - **OS**: `linux`, `windows`, `macos`, `freebsd`, `freestanding` (for embedded/bare-metal)
  - **Arch**: `x86_64`, `aarch64`, `arm`, `riscv64`
  - **ABI/env**: `gnu`, `musl`, `msvc`, `eabi` — this is the axis that distinguishes MSYS2/MinGW (`gnu`) from native MSVC (`msvc`)

  **Both auto-detect from the host by default**; the user only specifies a target explicitly for cross-compilation. Since FC delegates to a C compiler, auto-detection could sniff the C compiler's predefined macros (`cc -dM -E`).

  **Both keep user-defined flags separate** from the built-in platform taxonomy (Rust: `--cfg` / Cargo features; Zig: build options).

  **Design options for FC**:
  1. **Value-carrying flags** — e.g., `#if os == "windows" && env == "gnu"`. More expressive, mirrors Zig/Rust, but requires extending `#if` to support equality comparisons on string-valued flags.
  2. **Auto-detected boolean flags** — compiler sniffs the C compiler and sets booleans (`windows`, `linux`, `x86_64`, `gnu`, `msvc`, `freestanding`, etc.). `#if` stays as-is with presence checks: `#if windows && gnu`. Simpler, mirrors what C does with `_WIN32`/`__linux__`/`__MINGW64__`. Less structured but probably sufficient in practice.
  3. Keep `--flag` for user-defined flags on top of whichever approach.

  **Open questions**: which approach fits FC's pragmatic style better; what the embedded/freestanding story looks like (runtime assumptions like `abort()`/`malloc` may not exist); confirmed support matrix (platform × toolchain × flag); spec documentation (Part 6, conditional compilation section) with stdlib portability guidance.

- **Windows/MSYS2 test failures (investigate)**: 37 of 987 tests fail on MSYS2 UCRT64 (gcc). Failures fall into several categories:
  - **Abort/signal handling** (core lang): `expressions/assert_fail`, `assert_message_fail`, `div_by_zero`, `div_by_zero_u32`, `mod_by_zero`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `structs/fixed_array_overflow_err` — likely different exit codes from `abort()` on Windows vs Linux (Windows doesn't use POSIX signals).
  - **IO/stdio** (POSIX-dependent): all `io/*` and `stdlib/io_*` tests — likely need `--flag windows` for `io.mkdir`, and possibly other POSIX API differences (`unistd.h` access checks, file path handling).
  - **Networking** (POSIX-dependent): all `stdlib/net_*` tests — networking stdlib uses POSIX sockets which aren't available on MinGW/UCRT64 without Winsock adaptation.
  - **Full listing** (950 passed, 37 failed in 938.445s):
    `expressions/assert_fail`, `expressions/assert_message_fail`, `expressions/div_by_zero`, `expressions/div_by_zero_u32`, `expressions/mod_by_zero`, `io/eprint`, `io/io_can_read`, `io/io_can_write`, `io/io_exists`, `io/io_open_close`, `io/io_open_fail`, `io/io_read`, `io/io_read_char`, `io/io_write_file`, `io/io_write_interp`, `io/io_write_stderr`, `io/io_write_stdout`, `io/print_binding`, `io/print_interp`, `io/print_static`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `stdlib/io_eof`, `stdlib/io_mkdir`, `stdlib/io_remove_rename`, `stdlib/io_seek_tell`, `stdlib/net_byte_order`, `stdlib/net_constants`, `stdlib/net_make_addr`, `stdlib/net_resolve`, `stdlib/net_resolve_connect`, `stdlib/net_tcp_loopback`, `stdlib/net_tcp_socket`, `stdlib/net_udp_loopback`, `stdlib/net_udp_socket`, `structs/fixed_array_overflow_err`.


- **SIMD / vector types (evaluate)**: Investigate first-class vector types for SIMD operations (e.g., `float32x4`, `int32x8`). Would require language-level support — the types (`__m128`, `__m256`) aren't C structs and can't be represented through extern, and wrapping intrinsics in function calls defeats inlining. The natural fit is emitting GCC/Clang `__attribute__((vector_size(N)))` typedefs, which get arithmetic operators for free in C. **Use cases**: image/audio processing, physics simulations, batch data transforms, cryptography, neural net inference — anywhere you're doing the same math on many values at once. **Counterpoint**: GCC/Clang already auto-vectorize many loops, so FC programs may get SIMD "for free" through the C compiler without any language changes. Explicit vector types only matter when auto-vectorization fails or you need guaranteed vectorization. Low priority unless FC targets performance-critical numeric workloads.


