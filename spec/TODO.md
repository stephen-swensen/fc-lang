# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Platform & Portability

### Platform-conditional stdlib (sys.sleep)

`sys.sleep` uses POSIX `nanosleep` — works on Linux, macOS, and Windows via MinGW/MSYS2, but not with MSVC. For native MSVC support, need a mechanism for platform-conditional code in the stdlib. Options: documented manual flags (`--flag posix` / `--flag win32`) with `#if`/`#else`/`#end`, or extending C interop to check C preprocessor `#ifdef` symbols. `sys.time` is already pure C11 (`timespec_get`). Low priority — MinGW is free/open-source and produces native Windows binaries.

## Standard Library

### io.read_all

Read an entire file into a heap-allocated string. API design TBD: return type (`str?` vs `str` with error), max size limits, error handling strategy.

### std::text — string manipulation utilities

`std::text` exists with parse functions (`parse_int32`, `parse_int64`, `parse_float32`, `parse_float64`). Remaining: split, join, trim, contains, starts_with, etc. Design TBD.

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

