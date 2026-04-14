# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **OR-patterns with bindings (v2)**: or-pattern alternatives are currently binding-free — `| some(x) | none -> x` is rejected. Lifting this requires the same-bindings-at-same-types rule that Rust/OCaml enforce, and a different codegen strategy than the current `(a || b)` predicate (e.g. per-alternative if-branches that set bindings and `goto` a shared arm body). Out of scope until someone asks.

- **Investigate `!` boolean-not ambiguity in nested if/else chains**: while writing the wolf-fc push-wall code, `else if !pushwall_tiles[idx] then false` inside a deeply-nested if/else chain appeared to parse incorrectly — the condition seemed to not trigger as expected. Rewriting as `let is_pw = pushwall_tiles[idx]; if !is_pw then false` worked. Unclear whether this was a real parser issue (e.g. `!` being mis-parsed as postfix option-unwrap on the slice index expression, or precedence vs `else if`) or just a nesting/indentation mistake on my end. Worth writing a minimal repro test to confirm. If real, either fix the parse or document the precedence clearly.

- **Qualified cross-namespace types are not mangled in struct field declarations**: A qualified type reference like `m.t` (where `t` is a struct declared in some other namespace's `module m`) resolves correctly in *expression contexts* — initializers such as `default(m.t*?)`, function parameter types, and inferred `let` bindings — and codegen emits the correctly mangled C type. But in a **struct field annotation**, codegen emits the FC-level identifier verbatim into the C struct, producing `struct wrapper { m.t* f; ... }` which fails to compile with `error: expected specifier-qualifier-list before 'm'`.
  
  Importing the inner name (`import t from other::m`) does not fix it: the struct then emits `t* f`, which fails with `error: unknown type name 't'` because the C file has no typedef for the unqualified `t`. The import brings the FC-level name into scope for the type-checker, but the codegen path for struct fields doesn't consult the same resolution table that function-parameter and expression sites do.
  
  Workarounds that work today:
  1. Don't put the cross-namespace value in a struct field. Keep it as a file-level `let mut x = default(m.t*?)` (works because the initializer is an expression context) and reference it alongside the struct.
  2. Use `any*` in the struct field and cast at every access site.
  
  Neither is ideal — both lose the natural ownership layout. The fix is presumably to route struct-field type names through the same mangling pipeline as function-parameter types.
  
  Minimal repro sketch: a two-file program where file A declares `namespace a::` with `module a = struct foo = x: int32`, and file B declares `struct wrapper = f: a.foo`. The generated C for `wrapper` will contain `a.foo f` rather than the mangled `a__a__foo f`.

- **Explicit bool → int32 cast**: `(int32) some_bool` is currently rejected ("invalid cast from bool to int32"). Users have to write `if b then 1 else 0`, which is verbose and repetitive when formatting booleans for output (e.g. `%d{if g->has_gold_key then 1 else 0}`). Consider either: (a) allowing explicit `(int32) bool_val` → 0/1, or (b) adding a `%s{bool}` format specifier that prints `"true"`/`"false"`, or (c) both. Option (a) is probably the least surprising — it mirrors C, and the cast is already explicit so there's no implicit-widening risk.

- **Windows/MSYS2 test failures (investigate)**: 37 of 987 tests fail on MSYS2 UCRT64 (gcc). Failures fall into several categories:
  - **Abort/signal handling** (core lang): `expressions/assert_fail`, `assert_message_fail`, `div_by_zero`, `div_by_zero_u32`, `mod_by_zero`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `structs/fixed_array_overflow_err` — likely different exit codes from `abort()` on Windows vs Linux (Windows doesn't use POSIX signals).
  - **IO/stdio** (POSIX-dependent): all `io/*` and `stdlib/io_*` tests — likely need `--flag windows` for `io.mkdir`, and possibly other POSIX API differences (`unistd.h` access checks, file path handling).
  - **Networking** (POSIX-dependent): all `stdlib/net_*` tests — networking stdlib uses POSIX sockets which aren't available on MinGW/UCRT64 without Winsock adaptation.
  - **Full listing** (950 passed, 37 failed in 938.445s):
    `expressions/assert_fail`, `expressions/assert_message_fail`, `expressions/div_by_zero`, `expressions/div_by_zero_u32`, `expressions/mod_by_zero`, `io/eprint`, `io/io_can_read`, `io/io_can_write`, `io/io_exists`, `io/io_open_close`, `io/io_open_fail`, `io/io_read`, `io/io_read_char`, `io/io_write_file`, `io/io_write_interp`, `io/io_write_stderr`, `io/io_write_stdout`, `io/print_binding`, `io/print_interp`, `io/print_static`, `options/unwrap_none`, `slices/slice_bounds`, `slices/subslice_bounds_abort`, `stdlib/io_eof`, `stdlib/io_mkdir`, `stdlib/io_remove_rename`, `stdlib/io_seek_tell`, `stdlib/net_byte_order`, `stdlib/net_constants`, `stdlib/net_make_addr`, `stdlib/net_resolve`, `stdlib/net_resolve_connect`, `stdlib/net_tcp_loopback`, `stdlib/net_tcp_socket`, `stdlib/net_udp_loopback`, `stdlib/net_udp_socket`, `structs/fixed_array_overflow_err`.


