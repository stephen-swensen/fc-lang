# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **OR-patterns in match (`| a | b -> ...`)**: match arms currently take exactly one pattern, so sharing an arm body across several variants or literals requires duplicating it. Adding OR-patterns would let us write e.g. `| splash | game_over -> reset()` or `| 'A' | 'a' -> ...`. The main work lives in the parser (peel off `|` at the start of an arm into a list of alternatives under a new `PAT_OR` node), pass2 exhaustiveness (flatten to the existing per-variant check), and codegen (emit `if (... || ...)` for the compiled arm condition). Payload-binding alternatives need a binding-compatibility rule — either restrict to no-payload / literal alternatives in v1, or require all branches bind the same names and types.

- **Windows/MSYS2 test failures (investigate)**: 37 of 987 tests fail on MSYS2 UCRT64 (gcc). Failures fall into several categories:
  - **Abort/signal handling** (core lang): `expressions/assert_fail`, `assert_message_fail`, `div_by_zero`, `div_by_zero_u32`, `mod_by_zero`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `structs/fixed_array_overflow_err` — likely different exit codes from `abort()` on Windows vs Linux (Windows doesn't use POSIX signals).
  - **IO/stdio** (POSIX-dependent): all `io/*` and `stdlib/io_*` tests — likely need `--flag windows` for `io.mkdir`, and possibly other POSIX API differences (`unistd.h` access checks, file path handling).
  - **Networking** (POSIX-dependent): all `stdlib/net_*` tests — networking stdlib uses POSIX sockets which aren't available on MinGW/UCRT64 without Winsock adaptation.
  - **Full listing** (950 passed, 37 failed in 938.445s):
    `expressions/assert_fail`, `expressions/assert_message_fail`, `expressions/div_by_zero`, `expressions/div_by_zero_u32`, `expressions/mod_by_zero`, `io/eprint`, `io/io_can_read`, `io/io_can_write`, `io/io_exists`, `io/io_open_close`, `io/io_open_fail`, `io/io_read`, `io/io_read_char`, `io/io_write_file`, `io/io_write_interp`, `io/io_write_stderr`, `io/io_write_stdout`, `io/print_binding`, `io/print_interp`, `io/print_static`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `stdlib/io_eof`, `stdlib/io_mkdir`, `stdlib/io_remove_rename`, `stdlib/io_seek_tell`, `stdlib/net_byte_order`, `stdlib/net_constants`, `stdlib/net_make_addr`, `stdlib/net_resolve`, `stdlib/net_resolve_connect`, `stdlib/net_tcp_loopback`, `stdlib/net_tcp_socket`, `stdlib/net_udp_loopback`, `stdlib/net_udp_socket`, `structs/fixed_array_overflow_err`.


