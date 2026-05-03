# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Standard library — fleshing out

Part 9 of the spec is intentionally a stub; the stdlib itself still has gaps identified in the 2026-04-20 audit. Candidate work, roughly ordered by impact:

- **Test coverage for `std::sys`** — currently zero tests. `env`, `time`, `sleep`, `get_pid`, `temp_dir`, `home_dir`, `exit` all need coverage, especially across POSIX/MinGW.
- **Fill in `std::io` test gaps** — `read_all`, `read_char`, `exists`, `can_read`, `can_write` are currently untested.
- **Formatted output beyond interpolation** — `sprintf`-style number→string into a caller-supplied buffer; formatted writers on a `FILE*` handle. Interpolation is `alloca`-backed and doesn't cover every case.
- **Error type for I/O / syscalls** — today `io`/`sys`/`net` surface failure as `bool` or `option`, losing `errno`. A small `io_error` union (or a thread-local `last_errno`) would make diagnostics actionable.
- **Bit utilities** — `popcount`, `leading_zeros`, `trailing_zeros`, `rotl`/`rotr`, `byteswap`. Cheap to add, commonly needed.
- **Encoding helpers** — `base64`, `hex`, URL encoding.
