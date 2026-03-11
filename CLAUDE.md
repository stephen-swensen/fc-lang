# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

This is an early-stage compiler project for **FC** (version 0.5 draft), a systems programming language that transpiles to C11. Currently, the repository contains only the language specification (`spec/fc-spec.html`). No compiler code exists yet.

## Git Workflow

- **Never auto-commit** — only commit when explicitly asked
- When asked to commit, look at `git diff` and `git log` to write an accurate message reflecting what changed since the last commit
- Default branch is `main`; experimental work may happen in other branches — always check the current branch before acting
- Working changes may be discarded and branches may be abandoned — follow the user's lead
- No `Co-Authored-By` attribution in commit messages

## Language Overview

FC is a C-targeting language with these core design constraints:
- **Target**: C11 (using `int8_t`/etc. from `<stdint.h>`, `_Static_assert`, anonymous unions)
- **Memory model**: Manual, no GC, no borrow checker — follows C's philosophy
- **Syntax**: Indentation-based (offside rule, spaces only — tabs are a compile error)
- **Type inference**: Directional (bottom-up, inside-out), never global unification
- **Generics**: Monomorphized at compile time, zero runtime cost

## Key Language Design Decisions

### Type System
- No explicit type annotations on bindings — always inferred from RHS
- Function parameter types are always required (they anchor inference)
- No function overloading — names always resolve to exactly one function
- Implicit widening only where lossless (e.g., `int32` → `int64`, NOT int → float)
- Integer-to-float always requires an explicit cast
- No `null` in the language — option types (`T?`) replace nullable values

### Compilation Model
- **Two-pass**: First pass collects top-level names/types/layouts; second pass type-checks expressions
- Top-level declarations can reference each other regardless of order
- Local bindings resolve left-to-right within function bodies

### Generated C Patterns
- Signed integer arithmetic uses cast-through-unsigned to define overflow: `(int32_t)((uint32_t)a + (uint32_t)b)`
- Shift amounts are masked to avoid C UB: `a << (b & 31)` for 32-bit types
- Bounds checks emit `abort()` before slice accesses
- Option unwrap (`x!`) emits a tag check before value read
- Struct/union equality emits generated comparison functions

### Types and Literals
- Default integer: `int32`; default float: `float64`
- Suffixed literals: `42i8`, `42u64`, `3.14f32`
- String types: `str` = `uint8[]` (fat pointer), `cstr` = `uint8*` (null-terminated, C interop), `str32` = `uint32[]`
- `any*` = opaque pointer (`void*`), cannot be dereferenced

### Control Flow
- `if`, `match`, `loop` are expressions
- `return`, `break`, `continue` are void-typed expressions (enable early-return in expression positions)
- `loop` produces a value via `break value`; `for` is always void
- `match` is exhaustive; wildcard `_` satisfies exhaustiveness

### Bindings
- `let`: immutable binding, not addressable, capturable in closures (by copy)
- `let mut`: mutable binding, addressable (`&x` → `T*`), not capturable in closures
- Both `let` and `let mut` allow field/element mutation — `let` controls reassignability, not content mutation
- Shadowing is allowed (any combination of `let`/`let mut`)

### Operators
- No compound assignment (`+=`, `-=`, etc.) — only plain `=`
- Bitwise operators bind tighter than comparison (fixes C's `x & mask == 0` wart)
- `!` is postfix option-unwrap AND prefix boolean-not (context-dependent)
- `->` means: pointer field access, function type arrow, OR match arm separator (context-dependent)

## Spec Reference

The full specification lives in `spec/fc-spec.html`. It is a self-contained HTML file with embedded markdown rendered by `marked.js`. Open it in a browser to read it. Sections are organized as:
- Part 1 — Foundations (types, literals, operators, let/mut, inference)
- Part 2 — Control flow (if, match, loop, for)
- Additional parts cover structs, unions, generics, modules, closures, memory, and C interop
