# FC Compiler Implementation Plan

## Context

The FC language spec (`spec/fc-spec.html`) is comprehensive enough to begin formal grammar definition and compiler implementation. All expression/statement/declaration forms, operator precedence (12 levels), type syntax, and indentation rules are well-specified. The ~15% gap (indentation edge cases, overloaded token disambiguation) will be resolved during grammar formalization — that's the point of doing it.

---

## Phase 1: Formal BNF Grammar (`grammar.bnf`)

A standalone file using EBNF notation, organized into two sections:

### Lexical Grammar

- **Indentation tokens:** Lexer emits synthetic `INDENT`, `DEDENT`, `NEWLINE` (Python-style column stack)
- **Key algorithm:** Lexer tracks `expecting_block` flag after block-forming tokens (`=`, `->`, `with`, `then`, `else`, `loop`, `for...in`). Only emits `INDENT` when flag is set and column increases; otherwise deeper-indented lines are continuations (no layout token emitted)
- **Conditional compilation** (`#if`/`#else`/`#end`): Handled at lexer level, suppresses tokens for inactive branches
- **Token categories:** layout tokens, literals (int/float/string/cstring/char with suffixes), keywords, type names, operators/punctuation, identifiers, type variables (`'a`)

### Syntactic Grammar

- **Program:** Optional namespace, then top-level items (imports, modules, structs, unions, let decls)
- **Declarations:** `let`/`let mut` (binding = expr), `struct`, `union`, `module`, `import`, `extern`
- **Expressions:** Precedence climbing (Pratt) across 12 levels, plus prefix/postfix operators, control flow expressions (`if`/`match`/`loop`/`for`), lambdas, struct/slice/variant literals, built-in operators (`alloc`/`free`/`sizeof`/`default`/`print` family)
- **Types:** Primitives, pointers (`T*`), slices (`T[]`), options (`T?`), function types (`(A) -> B`), generic instantiation (`name<T>`), type variables (`'a`)
- **Patterns:** Wildcard `_`, binding, integer/char/string/bool literals, negative literals, `some(pat)`, `none`, variant with payload, struct destructure

### Disambiguation Rules

| # | Token | Rule |
|---|-------|------|
| 1 | `*` / `&` as prefix vs infix | Pratt handles naturally by position |
| 2 | `!` prefix (boolean-not) vs postfix (unwrap) | Prefix = no left operand; postfix = after expression |
| 3 | `->` | Function type (type context); pointer field access (after expr + ident); match arm (after pattern) |
| 4 | `\|` | Match arm pipe (start of peer in match block) vs bitwise OR (between expressions) |
| 5 | `name<Type>()` generic call vs comparison | Type names are syntactically distinct |
| 6 | `(T)expr` cast vs parenthesized expr | Resolved using known type name set from pass 1 |
| 7 | Negative pattern literals | Parser combines `MINUS` + `INT_LITERAL` in pattern context |

---

## Phase 2: C Compiler (`src/`)

### Project Layout

```
fc-lang/
  grammar.bnf
  Makefile                  # cc -std=c11, simple targets: fc, clean, test
  src/
    main.c                  # CLI entry point
    common.h / common.c     # Arena allocator, string interning, dynamic arrays
    token.h                 # Token enum + struct
    lexer.h / lexer.c       # Tokenizer with indentation handling
    ast.h / ast.c           # AST nodes (tagged unions), constructors
    parser.h / parser.c     # Recursive descent + Pratt expression parsing
    types.h / types.c       # Resolved type representation, operations
    pass1.h / pass1.c       # Top-level declaration collection
    pass2.h / pass2.c       # Type checking (bottom-up inference)
    monomorph.h / monomorph.c  # Generic instantiation
    codegen.h / codegen.c   # C11 code emission
    diag.h / diag.c         # Error reporting with source locations
  tests/
    cases/                  # .fc files + .expected (stdout) or .error (expected error)
    run_tests.sh            # Integration test runner
```

### Key Design Decisions

- **Arena allocation** for all AST/types/strings — no individual frees, single cleanup
- **String interning** from the start — identifier comparison becomes pointer comparison
- **Pratt parsing** for expressions — clean handling of 12 precedence levels + prefix/postfix
- **Lexer-driven indentation** — parser sees flat token stream with `INDENT`/`DEDENT`/`NEWLINE`
- **Single `.c` output** — compiler emits one translation unit, compiled with one `cc` invocation
- **Two-pass** matching spec — pass 1 collects names/types, pass 2 type-checks expressions

---

## Implementation Milestones

| Milestone | Focus | Test Target |
|-----------|-------|-------------|
| **M1** | Minimal end-to-end: Arena, string interning, lexer (no indentation yet), Pratt parser for arithmetic, minimal AST, type-check integers, emit C | `let x = 1 + 2` compiles and runs |
| **M2** | Functions + control flow: Indentation handling in lexer, lambdas, function calls, `if`/`then`/`else`, return, blocks | `let double = (x: int32) -> x * 2` |
| **M3** | Structs, unions, match: Declarations, struct literals, field access, match expressions, patterns | Shape example from spec |
| **M4** | Pointers, slices, options, loops: `T*`, `T[]`, `T?`, `&`/`*`, indexing, subslice, `some`/`none`, `!` unwrap, `loop`/`for`/`break`/`continue`, bounds checks | Array iteration, linked list |
| **M5** | Memory: `alloc`, `free`, `sizeof`, `default` | Heap data structures |
| **M6** | Modules + imports: `module`, `import`, `namespace`, `from`, `as`, `private`, qualified names, name mangling | Multi-module program |
| **M7** | Closures: Capture analysis (`let` only, `let mut` rejected), fat function pointers, context struct generation | Higher-order functions |
| **M8** | Generics: Type variables, `<T>` syntax, monomorphization, fixpoint iteration, deduplication, mangled names | Generic identity, pair, arraylist |
| **M9** | C interop + formatted output: `extern`, `from "header.h"`, `c"..."`, string interpolation, `std::io` (write/read/open/close/flush), stdin/stdout/stderr globals, str→cstr cast, `#if`/`#else`/`#end`, `std::sys`, main args as `str[]` | C interop, std modules |
| **M10** | Polish: Full match exhaustiveness, closure escape detection, stack-slice return detection, integer literal range checking, static type properties (`int32.min`, `float64.nan`, etc.), initialized heap alloc (`alloc(T{...})`) | Full suite |

---

## C Code Generation Patterns

| Construct | C Output |
|-----------|----------|
| Signed arithmetic | `(int32_t)((uint32_t)a + (uint32_t)b)` |
| Shift masking | `a << (b & 31)` |
| Bounds check | `assert(i >= 0 && i < s.len); s.ptr[i]` (or `abort()`) |
| Option types (non-pointer) | `struct { T value; bool has_value; }` |
| Option types (`T*?`) | Bare pointer (`NULL` = none) |
| Tagged unions | `enum` tag + `struct { tag; union { ... }; }` |
| Closures | `struct { fn_ptr; ctx; }` — ctx is stack-allocated capture struct or `NULL` |
| Slices | `struct { T *ptr; int64_t len; }` |

---

## Testing

- **Unit tests:** Lexer token sequences, parser AST shapes, type operation correctness
- **Integration tests:** `.fc` file + `.expected` (run output) or `.error` (expected compiler error)
- **Rule:** All tests pass before starting the next milestone

---

## Verification

1. **Grammar:** Manually trace spec examples through BNF productions to verify they parse correctly
2. **Each milestone:** Write `.fc` test cases, compile to C, compile C with `cc -std=c11 -Wall -Werror`, run and verify output
3. **Regression:** `make test` runs the full suite after every change
