# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Packed structs and bit-level layout control**: Support `packed struct` for exact bit layout without padding, and potentially bit fields. Currently FC emits C structs with C's default layout rules, offering no control over padding or alignment beyond what the C compiler provides. Packed structs would enable memory-mapped I/O, binary protocol parsing, and compact serialization formats.

- **Windows/MSYS2 test failures (investigate)**: 37 of 987 tests fail on MSYS2 UCRT64 (gcc). Failures fall into several categories:
  - **Abort/signal handling** (core lang): `expressions/assert_fail`, `assert_message_fail`, `div_by_zero`, `div_by_zero_u32`, `mod_by_zero`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `structs/fixed_array_overflow_err` — likely different exit codes from `abort()` on Windows vs Linux (Windows doesn't use POSIX signals).
  - **IO/stdio** (POSIX-dependent): all `io/*` and `stdlib/io_*` tests — likely need `--flag windows` for `io.mkdir`, and possibly other POSIX API differences (`unistd.h` access checks, file path handling).
  - **Networking** (POSIX-dependent): all `stdlib/net_*` tests — networking stdlib uses POSIX sockets which aren't available on MinGW/UCRT64 without Winsock adaptation.
  - **Full listing** (950 passed, 37 failed in 938.445s):
    `expressions/assert_fail`, `expressions/assert_message_fail`, `expressions/div_by_zero`, `expressions/div_by_zero_u32`, `expressions/mod_by_zero`, `io/eprint`, `io/io_can_read`, `io/io_can_write`, `io/io_exists`, `io/io_open_close`, `io/io_open_fail`, `io/io_read`, `io/io_read_char`, `io/io_write_file`, `io/io_write_interp`, `io/io_write_stderr`, `io/io_write_stdout`, `io/print_binding`, `io/print_interp`, `io/print_static`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `stdlib/io_eof`, `stdlib/io_mkdir`, `stdlib/io_remove_rename`, `stdlib/io_seek_tell`, `stdlib/net_byte_order`, `stdlib/net_constants`, `stdlib/net_make_addr`, `stdlib/net_resolve`, `stdlib/net_resolve_connect`, `stdlib/net_tcp_loopback`, `stdlib/net_tcp_socket`, `stdlib/net_udp_loopback`, `stdlib/net_udp_socket`, `structs/fixed_array_overflow_err`.


- **SIMD / vector types (evaluate)**: Investigate first-class vector types for SIMD operations (e.g., `float32x4`, `int32x8`). Would require language-level support — the types (`__m128`, `__m256`) aren't C structs and can't be represented through extern, and wrapping intrinsics in function calls defeats inlining. The natural fit is emitting GCC/Clang `__attribute__((vector_size(N)))` typedefs, which get arithmetic operators for free in C. **Use cases**: image/audio processing, physics simulations, batch data transforms, cryptography, neural net inference — anywhere you're doing the same math on many values at once. **Counterpoint**: GCC/Clang already auto-vectorize many loops, so FC programs may get SIMD "for free" through the C compiler without any language changes. Explicit vector types only matter when auto-vectorization fails or you need guaranteed vectorization. Low priority unless FC targets performance-critical numeric workloads.


