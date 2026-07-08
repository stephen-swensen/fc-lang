# Design: built-in result type `T!` — ADOPTED (implemented 2026-07-06)

*Date: 2026-07-05. Branch: `result-type`. Status: design decided; this records the decision, the
precedent analysis, and the rejected alternatives so none of it is re-litigated. Implementation
landed 2026-07-06 (`ok`/`err` as hard keywords, mirroring `some`/`none`; existing user
identifiers named `ok`/`err` were renamed in the same change). The trailing stdlib migration
lands on this branch before merge into `develop`.*

## The problem

FC has been deliberately silent on error propagation: every known model either infects the type
system (Rust's generic `Result<T, E>`) or imposes runtime machinery incompatible with the cost
model (exceptions). But a non-decision is also a decision, and the stdlib shows its bill — three
conventions coexisting in one library:

- `std::io` returns `T?` — `read_all`'s `none` conflates "no such file", "permission denied",
  and "out of memory" into one bit.
- `std::net` returns `-1` sentinels.
- `io.mkdir` returns `bool`.

None can say *why* an operation failed. The rc.6 audit named "error-propagation sugar" the
single highest-leverage ergonomic gap, but the sugar framing was backwards: sugar needs a
blessed carrier type to desugar over. Sugar over `T?` alone throws away the failure cause;
sugar over ad-hoc user result unions hits the error-type-mismatch wall. The carrier comes first.

## Precedent landscape

- **Exceptions (C++/Java/ML):** non-local control flow, invisible in signatures, unwinding
  machinery in the runtime. Categorically incompatible with FC's cost transparency and C11
  target. Off the table.
- **Rust `Result<T, E>` + `?`:** the infection has a precise mechanism — **`E` is generic, so
  every propagation across an abstraction boundary needs an `E → E′` conversion**. That's why
  `?` grew an implicit `From::from` call (a hidden function call — a cost-model violation in FC
  terms) and why the ecosystem needed `anyhow`/`thiserror`/`Box<dyn Error>` to be livable. Also
  requires traits, which FC has permanently ruled out.
- **Go `(T, error)`:** no type infection, but ceremony at every call, and `error` is an
  interface (dynamic dispatch).
- **Kernel C / negative `errno`:** in-band `int`, `if (ret < 0) return ret;`. Propagation is
  manual but *one uniform line* — because **the error type is fixed globally**. Scaled to the
  largest C codebase in existence.
- **Zig `!T` + `try`:** the modern success, and its key move is the same one — **errors are
  bare codes (u16 behind the scenes), never generic, never carrying payloads**. `try`
  propagates without conversion because there is nothing to convert. Error *sets* add inference
  machinery FC doesn't want; the degenerate form — one flat code space — is what FC adopts.

The pattern across the two systems-school survivors: **fixing the error type is what stops
propagation from infecting signatures.** The mismatch problem that poisons Rust doesn't exist
when the error passes through every boundary unchanged and only the success side is generic.

## The decision

### 1. Type: `T!`, a built-in like `T?`

A result is `ok('a) | err(i32)`. The error payload type is **permanently `i32`** — this is the
load-bearing decision (see precedents above), recorded here the way `auto-deref-decision.md`
records dot-deref, so "let `err` carry a struct" doesn't resurface. Diagnostic *detail* (message
text, position info) travels via out-params, FC's established pattern — a decided limitation,
not an oversight.

`!` is a postfix type modifier binding tightly to the left, exactly like `?` and `*`:

```fc
i32!        // result: ok(i32) | err(code)
u8?!        // result of option: ok(some/none) | err — "fallible read of optional byte"
u8!?        // option of result — legal by regularity, rarely wanted
point*!     // result carrying a pointer
```

Both composition orders are legal (suffixes compose by position, no special cases), but `T?!`
is the idiomatic order — e.g. `read_char: u8?!` where `ok(some(b))` is a byte, `ok(none(u8))`
is EOF, and `err(u8?, code)` is an I/O error. This composition is what replaces any temptation
to merge option and result (see Rejected alternatives).

### 2. The `?`/`!` grid

`T!` coexists with the existing expression-postfix `x!`. Types and expressions are disjoint
grammatical contexts; the result is a 2×2 that teaches itself:

|      | type suffix            | expression postfix                 |
|------|------------------------|------------------------------------|
| `?`  | `T?` — may be absent   | `x?` — propagate upward *(future)* |
| `!`  | `T!` — may fail        | `x!` — unwrap or abort             |

`?` is unused in expression position today; it is **reserved** for the propagation operator
(separate follow-up design: a pure local desugar to the `match`-with-diverging-arm you'd have
written — `return`s the `none`/`err` upward, no conversion ever needed since `err` is `i32` on
both sides by construction; composes with `defer` for free since `return` already unwinds
defers). Both operators work uniformly on both types.

`T!` is Zig's exact spelling for the same concept (`!T` error union) — the strongest precedent
available. (Swift's `T!` means implicitly-unwrapped optional, a regretted feature; FC's meaning
is Zig's, not Swift's.)

### 3. Constructors: intrinsics, like the option pair

```fc
ok(v)            // infers bottom-up from payload — the some(v) analog
err(T, code)     // type-anchored — the none(T) / bitcast(T, x) analog
```

`err` needs the type argument because FC's directional inference cannot infer a generic union's
type from a payload-free (or payload-only-`i32`) case — the same reason `none(T)` exists.
Intrinsic constructors are also the argument for *built-in* over *library union*: a stdlib
`result<'a>` union is condemned to the full `result<i32>.err(2)` spelling forever (the rc.6
audit's verbosity complaint); intrinsics get to take a type where functions can't. `T` may be a
type variable in generic code (`err('a, code)`), like the other type-taking intrinsics.

### 4. Representation: C's "0 on success" as a layout

```c
struct { int32_t err; T value; }   /* err == 0  ⇔  ok */
```

- The tag *is* the code — no separate discriminant. One `i32` of overhead, stated cost.
- **`err(T, 0)` is unrepresentable** and guarded exactly like `some(null)`: compile error when
  the code is provably zero, elided runtime guard otherwise.
- Zero-initialization (`= {0}`, `arena_alloc`) yields `ok(default(T))`, and consequently
  `default(T!) = ok(default(T))`. This is *forced*, not chosen: `default` is in the universal
  generics tier (every type must support it), and the all-zeros bit pattern is `err == 0`. It
  is also semantically defensible — a result's zero value succeeding mirrors C's `0 = success`
  heritage — but the honest statement is that the repr decides it. (Options differ: all-zeros
  `T?` is `none`. The asymmetry is priced in.)

  *Precedent and the rejected `err(-1)` default (considered 2026-07-05):* the zero-value school
  is unanimous that all-zeros reads as success — C (zeroed status int), Go (`nil` error means
  no error), Odin (`.None = 0`), and Zig's internal repr (error codes number from 1; 0 is
  reserved for "no error" — our exact layout). Rust's answer (`Result` implements no `Default`;
  asking is a compile error) is unavailable: FC's `default` is universally total by design, and
  a carve-out would make the tier partial while zero-filled aggregates deliver `ok(0)` through
  the back door anyway. Defining `default(T!) = err(-1)` was rejected because it breaks the
  invariant *default ≡ zero-filled memory*: `i32![n] {}` and `arena_alloc` are calloc-cheap
  precisely because fresh zeros *are* default values, and restoring consistency would inject a
  per-element stamping loop into every zero-fill path whose type transitively contains a result
  — hidden runtime machinery FC doesn't accept. Re-encoding the tag so all-zeros decodes as an
  error (offset codes) was likewise rejected: it forfeits the repr's point (the tag *is* the C
  status code, debugger-readable, zero on success). Residual risk, stated: a result defaults to
  "falsely fine" where an option defaults to "safely absent"; exposure is limited to explicitly
  zero-filled aggregates (FC has no uninitialized bindings), where zeros-as-data is already the
  contract, and matches C's own zeroed-status behavior rather than introducing a novel trap.
- No pointer-packing specialization needed (unlike `T*?`'s null-sentinel): the code field is
  the tag, so `T*!` is the uniform repr. `T*?!` composes — the value field holds the
  null-sentinel option.

### 5. Operations — full parity with options

- **`x!`** unwraps a result: yields the `ok` payload or aborts — with the error code in the
  abort message, a diagnostic options can't offer.
- **Match:**
  ```fc
  match parse_port(s) with
  | ok(p) -> use(p)
  | err(2) -> retry()      // literal code patterns — existing nested-literal machinery
  | err(e) -> fail(e)
  ```
  Exhaustiveness requires both variants covered; literal-code arms don't exhaust `err`, so a
  binding arm (`err(e)`) or `_` must follow, same as integer matching today.
- **`.is_ok` / `.is_err`** synthetic members, mirroring `.is_some`/`.is_none` (match remains
  the idiom; parity keeps the types symmetric).
- **`==`** works when `T` supports it, like options: equal iff same variant and (for `ok`)
  equal payloads; two `err`s are equal iff codes are equal.
- No implicit widening through results: `i32!` and `i64!` are distinct, conversions are
  explicit at the payload level. `T!` participates in generics as any concrete type does
  (monomorphized; universal tier `==`/`default`/`sizeof` applies).

### 6. Grammar and parser notes

- Lexer: no change — `!` is already `TOK_BANG`.
- Type grammar: `!` joins the postfix-modifier loop with `?`/`*`/`[]` (update `grammar.bnf`).
- Casts: `(IDENT!)` joins the cast trigger set beside `(IDENT*)` (`src/parser.c` cast
  disambiguation, backtracking). The one residual ambiguity — `(foo!)` as a parenthesized
  unwrap of a bare identifier vs. a cast-to-`foo!` — is resolved **in favor of the cast** when
  followed by an expression start; redundant parens around a bare postfix unwrap are dead
  syntax (`foo!` binds tightest without them). Documented rule, same class as the existing
  `(a*) b` vs `(a * b)` resolution.

## Error-code organization: `error` declarations — ADOPTED 2026-07-06, IMPLEMENTED 2026-07-06

*Second design pass on this branch, resolving follow-up 3 below (code-space ownership). Decided
in discussion 2026-07-06; this records the decision and the rejected alternatives.*

### The problem

`err` carries a raw `i32`, but nothing says who owns the numbers. Manual per-module ranges are
the kernel/FreeBSD approach and they rot (someone always squats on someone else's range); a
registry file is coordination overhead. Zig's answer — the compiler owns a global table and
assigns every named error a unique small integer — is the right one, and FC is unusually well
placed for it: compilation is already whole-program, so the compiler sees every declaration in
one invocation.

### The decision: compiler-owned code space, declared through `error` groups

```fc
error file_io =
    | not_found
    | invalid_path

let open = (path: str) ->
    ...
    err(handle, file_io.not_found)

match open(p) with
| ok(h) -> use(h)
| err(file_io.not_found) -> create(p)
| err(e) -> fail(e)
```

- **A group is a pseudo-module of `i32` constants, not a type.** `file_io` never appears in
  type position; the err tag's user-facing type is the global **`error`**, a display alias of
  `i32` on the `str`/`cstr` precedent (affects `type_name()` output only, never equality or
  semantics). Declared constants and `| err(e)` bindings show `error` in diagnostics/hover but
  are `i32` everywhere — raw codes at C boundaries need no casts, `(e: error)` params for
  logging helpers work for free, and literal-code tests (`err(2)`) stay valid.
- **No unification, contra Zig:** `file_io.not_found` and `parse.not_found` are distinct codes.
  Zig unifies names globally because its error *sets* must merge; FC has no sets, so
  unification buys nothing — and non-unification keeps provenance in the name, consistent with
  per-call-provenance interpretation.
- **Qualification is mandatory**, in expressions and in patterns. A bare `| err(not_found)`
  would be indistinguishable from a binding (the classic enum-in-pattern trap); qualified
  constant paths in patterns (`| err(file_io.not_found)`) are the main new grammar surface.
- **No per-group exhaustiveness.** The err payload's type is the global `error`, so an `err`
  arm always needs a binding or `_` after any literal/named arms — same as integer matching
  today. `error` groups are namespaces of constants, not sum types; stating this here so
  union-style exhaustiveness isn't asked of them later.
- Group and member names follow FC's lowercase snake_case like all user names. `error` becomes
  a hard keyword (the `ok`/`err` precedent).

### Code-space layout

| range         | meaning                                                              |
|---------------|----------------------------------------------------------------------|
| `0`           | `ok` — never an error code (repr invariant)                          |
| `[1, 65535]`  | **reserved: platform passthrough** (errno, Win32/WSA error codes)    |
| `≥ 65536`     | compiler-assigned declared errors                                    |
| negative      | sign-bit-encoded platform conventions (HRESULT, kernel `-errno`)     |

- **Assignment is deterministic:** fully-qualified declared names are sorted and numbered
  sequentially from 65536. Every declared name is therefore provably nonzero — the
  `err(T, 0)` guard is elided at compile time for named codes.
- **Passthrough is sound by construction:** the errno domain already shares FC's zero
  convention (`errno == 0` means "no error", exactly `err == 0 ⇔ ok`), so a failing C call's
  errno wraps into `err(T, e)` unchanged — no translation table, the debugger shows the same
  number `strace` does. The runtime `err(T, 0)` guard even catches the buggy-wrapper case
  (wrapping errno when the call actually succeeded).
- **Why 65536 and not the kernel's MAX_ERRNO (4095):** FC targets Windows, where the natural
  passthrough values are Win32 system error codes (`GetLastError`) and Winsock errors
  (`WSAECONNRESET` = 10054) — 16-bit-range values that would collide with a table starting at
  4096. Reserving the full u16 space subsumes errno (Linux tops out ~133), MAX_ERRNO, and the
  Win32/WSA space; `i32` leaves two billion codes above it, so the reservation costs nothing.
  A code's value also classifies itself: below 65536 = platform code passed through, at or
  above = declared FC error, findable by name.
- **HRESULTs pass through in the negative space:** failing HRESULTs always have the severity
  bit set, so as `i32` they're negative; `S_OK` = 0 aligns with `ok`. Caveat: `S_FALSE` = 1 is
  a nonzero *success* HRESULT — a wrapper must test `FAILED(hr)` (the sign bit), never
  `hr != 0`. Negative codes generally are representable and legal, governed by per-call
  provenance; `std::net`'s current `-1` sentinels migrate to real errno passthroughs instead.
- **Codes are not stable across builds** (adding or renaming a declared error shifts every
  code after it in sort order). Accepted, as Zig accepts it: the mitigation is printing names,
  not persisting numbers — see `error_name`.

### Name delivery: `error_name`, `--backtraces` aborts, `--emit-error-codes`

Three channels deliver names, each with its own cost home (decided 2026-07-06):

- **`error_name(e)` intrinsic** yields `str?`: `some` of the fully-qualified name
  (`"file_io.not_found"`) for a declared code, `none` for anything else (reserved-range and
  negative codes — the caller formats the number via interpolation; returning a formatted
  string would hide an allocation). Backed by a whole-program static table emitted **only when
  `error_name` is used** — a static cost, no runtime machinery. Unconditionally embedding the
  table in every binary that unwraps would be unsought bloat, so by default the `x!` abort
  message prints the numeric code.
- **Named aborts ride `--backtraces`.** The flag's existing contract is exactly "pay static
  data for readable failures" — it already embeds a `_fc_symtab[]` name table so
  `fc_dump_backtrace` renders FC-level frames, and `FC_ABORT()` under the flag is already the
  fancy path. With `--backtraces` on, the error-name table is emitted unconditionally and the
  `x!` abort message prints the qualified name alongside the code; lean builds keep numbers.
  `error_name` and the abort path share the one table when both apply. No dedicated flag —
  that would split the single diagnostic-fidelity-vs-size axis across two knobs.
- **The `.errcodes` map** — the "strip the binary, keep the map" channel (linker
  map / PDB / split-DWARF school): writes `code<TAB>qualified_name<TAB>decl file:line` per
  declared error, for decoding numeric codes from production logs and for publishing alongside
  a release. Zero binary cost; trivially generated since assignment is deterministic and
  whole-program. Deterministic output makes it diffable — CI can diff the map across builds to
  see exactly which codes shifted when a declaration was added, making the one sharp edge of
  unstable assignment visible. Scope: declared errors only (≥ 65536; reserved-range
  passthrough codes belong to the platform's own documentation), regenerated per build like a
  symbol map. *(Revised 2026-07-06, same day: emission is **automatic**, not a flag. The
  original `--emit-error-codes[=path]` opt-in was implemented and then dropped — since codes
  are deliberately not build-stable, the map is the entire mitigation, and an opt-in's only
  failure mode is not having the map for the build that shipped. Every successful compile that
  declares errors writes `<output>.errcodes` beside the C output; a build that declares none
  removes any stale map so the map can never lie about the `.c` next to it.)*

### Rejected alternatives (error codes)

- **Manual per-module ranges / a registry file** — rot and coordination overhead; the
  compiler already sees the whole program.
- **Zig-style global unification of names** — exists to serve error-set merging, which FC
  doesn't have; loses namespacing for nothing.
- **Mandatory translation at C boundaries** (the Zig `std.posix` school: switch on errno,
  return named errors) — a per-platform mapping table in every wrapper plus an
  "unexpected errno" fallback path; exactly the hidden layer FC's C-interop story avoids.
  Translation stays *available* to the stdlib where a named contract is worth it, never forced.
- **Explicit `= N` pins on declared errors** — reintroduce the collision problem the feature
  exists to kill; stability wants `error_name`, not frozen numbers.
- **Generalized `code` declarations** (named integer constant groups for non-error channels) —
  deferred. That's a plain-enum design with different constraints (no reserved zero, real
  type-position and exhaustiveness questions) and no current stdlib demand. If a general enum
  design ever lands, `error` reframes cleanly as its specialized case; nothing here paints
  that door shut.

### Implementation notes

- The pseudo-module can likely ride the existing companion-module machinery (a group registers
  as a module of `i32` consts); the err-binding's type node carries the `error` display alias.
- Pattern grammar grows qualified-constant paths (`grammar.bnf` update alongside).

*(As implemented 2026-07-06: the parser desugars `error g = | m …` directly to a `DECL_MODULE`
flagged `is_error_group` whose members are synthesized immutable i32-const lets — so
registration, imports, privacy, member access, const-folding, LSP hover/completion/go-to-def,
and codegen all ride the module machinery with zero special cases. pass1 assigns codes at the
end of collection by patching each member's `EXPR_INT_LIT` placeholder (sorted fully-qualified
names, numbered from `FC_ERROR_CODE_BASE`); the registry behind `error_name` /
`--emit-error-codes` lives in pass1 with accessors. Pattern paths parse to `PAT_CONST_PATH`,
resolve through a synthesized expression chain (so imports/privacy/shadowing match expression
positions exactly), and rewrite in place to `PAT_INT_LIT` — exhaustiveness, duplicate-arm
analysis, and codegen see a plain integer literal. `error_name` yields `const str?` — the name
lives in a static table, same rule as string literals.)*
- Stdlib-migration follow-ups, not blockers: an errno accessor shim (`errno` is a C macro, not
  a symbol — an `__errno_location`-style extern or one-line C helper), `GetLastError` on
  Windows, and *optionally* per-platform named errno const groups behind the existing
  conditional-compilation flags — only for codes the stdlib actually matches on (values are
  platform-dependent: EAGAIN is 11 on Linux, 35 on macOS).

## C interop: extern result mapping via error protocols — ADOPTED 2026-07-06 (design decided; implementation pending)

*Third design pass on this branch. Decided in discussion 2026-07-06; this records the decision,
the precedent analysis, and the rejected alternatives so none of it is re-litigated.*

### The problem

The existing option mapping at extern boundaries is a **repr identity**: `T*?`/`any*?`/`cstr?`
compile to a bare nullable pointer, so `extern fopen: … -> any*?` needs zero adaptation — the C
return value already *is* the FC value. `T!` cannot work that way: its repr
(`{ int32_t err; T value; }`) matches no C function's ABI, and — more fundamentally — the error
code usually isn't in the return value at all (`errno` is a thread-local the callee sets
out-of-band). So mapping a C failure into `T!` requires a synthesized adapter: call, test the
failure convention, capture the code, build the struct.

And "the failure convention" is not one thing. Standard C signals failure through at least
seven distinct, incompatible protocols, varying on two axes — what the failure *test* is, and
where the *code* lives. Leaving the wrapping entirely to hand-written FC code re-opens the
wound this design exists to close (every wrapper author improvising a scheme) and hits a
mechanical wall besides: `errno` is a C macro, not a symbol, so FC code can't even read it
without a per-platform shim.

### Precedent landscape

- **Zig (`std.posix`):** hand-written wrappers that `switch` on errno into named error sets —
  the mandatory-translation school this document already rejected (per-platform mapping tables,
  an "unexpected errno" fallback path in every wrapper). Zig *has* to translate because its
  errors are named sets; FC's passthrough model (raw code, no translation) is what makes
  automation viable at all.
- **Rust:** no automation — `libc` + manual wrappers + `io::Error::last_os_error()`. Same root
  cause: generic `E` means there is no canonical wrapping to generate.
- **C# P/Invoke — the strong precedent:** `[DllImport(SetLastError = true)]` tells the
  *marshaller* to capture `GetLastError()` immediately after the call, precisely because
  reading it from managed code later is unreliable. A declaration-site annotation driving
  generated capture code — exactly this feature.
- **Go (`syscall` package):** generated wrappers return `(r1, r2, errno)` — the same move,
  generator-side.

### The decision: a closed set of declared error protocols

`extern` function declarations grow an optional `from <protocol>` tail, required exactly when
the declared return type is a result (`T!` return without a protocol, or a protocol without a
`T!` return, is a compile error — the two halves must agree):

```fc
module io from <sys/stat.h> =
    extern open: (const cstr, i32) -> i32! from errno(-1)
    extern fopen: (const cstr, const cstr) -> any*! from errno(null)
    extern mkdir: (const cstr, u32) -> void! from errno(-1)
    extern pthread_mutex_lock: (any*) -> void! from status
```

(The per-declaration `from` slot is free — only extern structs/unions use `from` today, and the
reuse reads correctly both ways: the definition comes *from* a header; the error comes *from*
errno.)

The protocol set is **closed** — one entry per crisp, documented C convention:

| protocol            | failure test          | code source          | typical payload | examples                        |
|---------------------|-----------------------|----------------------|-----------------|---------------------------------|
| `errno(-1)`         | `ret == -1`           | `errno`              | `ret` or void   | `open`, `read`, `mkdir`, `close`|
| `errno(null)`       | `ret == NULL`         | `errno`              | `ret`           | `fopen`, `opendir`              |
| `status`            | `ret != 0`            | `ret` itself         | void only       | `pthread_*`, C11 threads        |
| `neg_errno`         | `ret < 0`             | `ret` itself (raw)   | `ret` or void   | raw syscalls, io_uring          |
| `hresult`           | `ret < 0`             | `ret` itself (raw)   | `ret` or void   | COM                             |
| `last_error(<s>)`   | `ret == s` (0/null/-1)| `GetLastError()`     | `ret` or void   | Win32 (`CreateFile`, BOOL APIs) |
| `wsa_error(-1)`     | `ret == -1`           | `WSAGetLastError()`  | `ret` or void   | `send`, `recv`, `connect`       |

- **Payload-ness is declared by the return type, orthogonal to the protocol:** `i32! from
  errno(-1)` for `open` (the fd is data), `void! from errno(-1)` for `mkdir` (the 0 is noise).
  `i32! from hresult` keeps the success-mode HRESULT observable (`ok(1)` = `S_FALSE`);
  `void! from hresult` discards it. Payload kind is compile-checked against the protocol's
  sentinel (integer sentinels need integer/void payloads, `null` needs a pointer payload); the
  payload type otherwise follows the existing extern type-mapping rules.
- **Sentinel tokens are protocol syntax, not expressions.** `null` spelled inside
  `errno(null)`/`last_error(null)` exists only there — it does not introduce a null literal to
  the language. `last_error` takes an explicit sentinel because Win32 genuinely varies (`FALSE`,
  `NULL`, `INVALID_HANDLE_VALUE`); `errno`'s sentinel is kept explicit for the same reading
  even though C practice pins it per payload kind.
- **No code arithmetic, ever** (decided against negating kernel-style `-errno` into the
  passthrough range): the number in the FC value is the number the platform produced, verbatim
  — what the debugger and strace show. Everything below 65536 and everything negative is
  platform-owned wild-west, interpreted by per-call provenance, exactly as the code-space table
  above already states. The one cost — `EAGAIN` is 11 via libc but −11 via io_uring — is what
  per-call provenance already prices in.
- **Failed call but `errno == 0`** (a buggy C library breaking its own contract): the adapter
  builds `err(T, 0)` and the existing runtime guard aborts — the buggy-wrapper case the
  code-space section already blesses. No new rule.
- **Payload on the err path is `default(T)`** (zero), never the sentinel — the sentinel is
  protocol noise, not data.
- Protocols apply to **extern declarations only**. FC functions construct results directly.

### `void!` — the payload-less result

The largest single class of fallible C functions returns a meaningless 0 on success —
`mkdir`, `close`, `unlink`, `rmdir`, `chdir`, `fsync`, `bind`, `listen`, `setsockopt`, every
`pthread` call. Typing these `i32!` would force `ok(0)` into existence and an `| ok(_) ->` wart
into every match — precisely the meaningless-values-floating-around outcome FC avoids by having
no unit type. So `void!` becomes legal, and it is the **anti-unit** choice:

- **`ok` is a payload-less variant** (the `| empty` precedent), not `ok(unit)`. `match r with
  | ok -> … | err(e) -> …`; `| ok(v)` on a `void!` is a compile error, same as `| empty(v)`.
- **No void value ever materializes.** `r!` is a void expression (legal exactly where a void
  call is); `let x = r!` hits the existing "cannot bind void expression" error; there is no
  `()` literal, no void parameter, no `void?` (absence-of-nothing has no use case and no C
  convention behind it — the same judgment that made `T?!` idiomatic and `T!?` merely legal),
  and `void` remains banned as a generic type argument, so `'a!` never instantiates to it and
  generic bodies never meet a payload-less `ok`.
- **Repr: a lone `int32_t`** — the tag alone; C's status int reified as a type. This makes the
  `status` protocol a **repr identity** (bit-for-bit, no adapter — the third free mapping after
  `T*?` null-sentinel and errno passthrough).
- `void!` is a normal type — return position (a user function propagating `mkdir(p)?`
  returns `void!`, anchored by an explicit bare `ok` on its success paths — see §Propagation
  operator; an earlier draft of this line said the type would be *inferred* from the `?`,
  which the adopted no-lift model supersedes), struct fields, slice elements ("status of
  last operation" is legitimate data). No extern-only carve-out.
- `default(void!) = ok`, zero-filled memory, consistent with the repr section.

With `x?` (follow-up 2) this completes the pipeline for the statement-shaped case:
`io.mkdir(path, 493u32)?` propagates a real errno upward or continues as a plain void
statement (the enclosing wrapper spells its own `void!` with a bare `ok` on the success
paths — see §Propagation operator).

### Adapter emission and cost

Each protocol emits a ~4-line `static` C helper per extern (the existing
`static __attribute__((unused))` regime), e.g. for `mkdir` above:

```c
static fc_result_void fc_mkdir_adapter(const char *p, uint32_t m) {
    return (fc_result_void){ mkdir(p, (mode_t)m) == -1 ? (int32_t)errno : 0 };
}
```

The cost is exactly the wrapper a user would hand-write, declared visibly in the signature —
static, stated, no hidden machinery. Because the adapter is *generated C*, it reads `errno`
directly (`<errno.h>` macro, captured immediately after the call, same thread, nothing able to
intervene) — dissolving most of the errno-accessor follow-up. `status` emits no adapter at all.

### The irregular tail (deliberately out of scope)

What the closed set excludes is only the tail where *the C API itself* is ambiguous and a human
must decide what the FC type means: `readdir` (NULL means both end-of-stream and error; needs
errno zeroed before the call — natural FC type `dirent*?!`, ok(some)=entry, ok(none)=EOF),
`getpriority` (−1 is a legal success value), `strtol` (sentinel overlaps the value domain). No
annotation can capture those judgments. The fallback is **not** roll-your-own error types: the
user declares the raw C signature and hand-writes a 3-line FC wrapper *into the same carrier* —
`err(T, code)` with the code passed through raw. Carrier, code space, `error_name`,
`.errcodes`, `x?` all still apply; what's hand-rolled is one adapter, never an error scheme.
This is why the stdlib keeps a one-line per-platform errno accessor extern
(`__errno_location` / `__error` / `_errno`, behind the existing conditional-compilation flags)
for exactly these wrappers. The readdir class has a recognizable sub-pattern (POSIX's own
"zero errno before the call" protocol) that could become an `errno0(null)` protocol later —
door open, not now.

### Rejected alternatives (extern mapping)

- **A general predicate language** (arbitrary failure tests / code expressions in the
  annotation) — the 20% it would add over the closed set is exactly the per-function-judgment
  tail no annotation can capture anyway; the cost is a mini-DSL in the grammar forever.
- **Inferring the protocol from the return type shape** (`any*!` ⇒ errno(null), `i32!` ⇒
  errno(-1)) — reads as magic, collides immediately (`i32!` is `open`'s errno(-1), a status
  return, an HRESULT, or a Winsock call), and hides the one fact the reader needs at the
  boundary.
- **`i32!`-carrying-0 instead of `void!`** — creates the meaningless value it claims to avoid;
  see §`void!`.
- **Negating `neg_errno` codes into the passthrough range** — code arithmetic breaks
  "the debugger shows the same number the platform produced"; wild-west passthrough below
  65536 is the simpler contract.
- **Extern-only `void!`** — a user function propagating `mkdir(p)?` must itself return `void!`;
  restricting the type to extern signatures is incoherent.
- **Hand-written wrappers as the only mechanism** (the Zig school) — re-opens per-wrapper
  improvisation, and FC code cannot even read `errno` without a shim; see the precedent
  landscape.

*(As implemented 2026-07-06: the protocol is an `ExternProtocol` enum on `DECL_EXTERN`
(`d->ext.protocol`), parsed by `parse_extern_protocol` in `src/parser.c` (sentinels are
protocol-local token checks, no expression machinery); agreement checks live in pass1's
`validate_extern_protocol`. No named adapter functions are emitted — codegen wraps each call
site inline in a statement expression (`emit_protocol_extern_call` in `src/codegen.c`, sharing
`emit_raw_extern_call` with plain extern calls, so the cstr/void* boundary casts are identical),
which makes variadic protocol externs work for free and keeps prototypes to the C header.
`status` with `void!` emits a single struct wrap of the return. Guarded protocols reuse the
existing `fc_zero_err` helper; `#include <errno.h>` is emitted only when an errno-protocol call
is reachable. `void!` compiles to `struct { int32_t err; }`; bare `ok` is EXPR_OK/PAT_OK with a
NULL payload throughout (every walker already NULL-guards); the same pass ALSO closed the
pre-existing `some(<void expr>)` hole, which used to emit invalid C (`void value;`). Tests:
`tests/cases/extern/proto_*` (runtime protocols via portable libc calls — dup/close, fopen,
malloc, strcmp; neg_errno/hresult value-driven through atoi/atoll since no libc function
returns −errno; Windows protocols declare-only) and `tests/cases/results/void_result_*` +
`ok_bare` + exhaustiveness twins.)*

## Propagation operator `x?` — ADOPTED 2026-07-06, IMPLEMENTED 2026-07-06

*Fourth design pass on this branch, resolving follow-up 2 (the grid's reserved cell). This
records the decision and the rejected alternatives. An initial implementation shipped with an
implicit return-type lift; the user rejected it the same day as an explicitness violation and
the model below (pure desugar, explicit carrier) replaced it — the lift is recorded under
Rejected alternatives so it isn't re-proposed.*

### The decision

`x?` is a postfix expression operator at unwrap's precedence, working uniformly on both
carriers: success yields the payload exactly as `x!` does; failure **returns the failure from
the enclosing function** — `err(code)` upward verbatim (i32 on both sides by construction, no
conversion ever, the payoff of the fixed code space), `none` upward as `none`. It is a pure
local desugar of the match-with-diverging-arm, so `defer` unwinding comes free (`return`
already unwinds), and it acts on the **top layer only** (`?` on `u8?!` yields `u8?`).

**`?` contributes nothing to inference.** The enclosing function must already return the
matching carrier at its top layer, anchored the way every FC return type is anchored: by the
explicit constructions on its success paths — `ok(...)`/`err(...)` (bare `ok` for `void!`),
`some(...)`/`none(T)`. A `?` in a body whose success paths yield a plain `T` or void is a
compile error at the `?`, naming the fix. The void-shaped wrapper therefore reads:

```fc
let make_dirs = (a: str, b: str) ->
    io.mkdir(a, 493u32)?               // must succeed before we go on
    io.mkdir(b, 493u32)                // tail: its void! IS the result — no ?, no ok

let ensure_dir = (path: str) ->
    if exists(path) then return ok
    io.mkdir(path, 493u32)?
    log_created(path)
    ok                                 // (str) -> void!
```

— one visible `ok` token per success exit is the stated price of a signature that is always
spelled somewhere in the body. And note the price is only paid where `?` is actually needed:
`?` unwraps mid-body so work can *follow* a fallible call; a fallible call in tail position
is already the function's result and needs neither `?` nor `ok`. (This supersedes the earlier "a user function containing
`mkdir(p)?` infers `void!` itself" line in the §`void!` section, which was written with the
lift in mind.) Everything else follows with zero special cases: return paths mix
`return ok(v)` / `return err(T, code)` / propagation freely (all the same type); recursion
works (the base case anchors the carrier before self-calls resolve —
`if v <= 1 then ok(1) else ok(v * fact(v - 1)?)`); result-typed tails pass through.

**The one sanctioned non-local construction.** `?`'s failure exit fabricates a whole value —
`err` at the enclosing function's return type with the local code injected, or that type's
`none` — whose type appears nowhere at the site and cannot be spelled there. Nothing else in
FC constructs a value this way: every other construction is type-anchored on the spot
(`err(T, code)`, `none(T)`, `default(T)`, struct literals, bare `ok` self-anchoring), and the
language's two weaker "type from afar" effects — a recursive self-call typed by a base case
elsewhere in the body, a literal widened by a callee's parameter type — only type a *use* or
adjust a representation, never synthesize a value. This is recorded as a deliberate, scoped
exception to directional inference, not a new mode of it, on two grounds. First, the
non-locality is `return`'s, inherited honestly: `?` desugars to a return, and a return is the
one construct whose meaning is inherently about the function boundary — every `return v`
sends a value "afar"; here the compiler merely spells the value. (Rust's `?` and Zig's `try`
construct their error-returns from the function signature the same way; FC has no signatures,
which is exactly why the next point matters.) Second, the no-lift rule above is what keeps
the far anchor visible: the induced type is never *invented* from afar — it is always a type
the same function body spells explicitly on its success paths, so a reader tracing a `?` has
a guaranteed in-body place to look, and the diagnostic points there. The construction is also
constitutive, not conveniencing: without it the operator cannot exist (spelling the payload
type at each site — `x?(config)` — would duplicate what the body already states and destroy
chaining; annotations are off the table). One non-local construction, targeting an
always-spelled type, in exchange for the entire propagation feature.

Together with the fixed-`i32` carrier this closes the original infection anxiety from both
ends: the fixed code means *nothing needs converting* at any boundary (no `From` ladders, no
`anyhow`, no error-set algebra), and the body-anchored failure construction means *nothing
needs restating* at any propagation site (no per-site payload spelling, no nested rethrow
matches). The manual alternative — the match-with-diverging-arm — remains exactly what `?`
desugars to, written once in this document instead of at every call site.

**Restrictions** (all compile errors): result propagation in a function not returning a
result, option propagation in one not returning an option — both reported at the `?` site
(this subsumes mixing the two kinds in one function: no return type satisfies both); `x?`
inside `defer` (it is a return; the same walk now also catches a `return` hidden inside a
loop in a defer, a pre-existing hole); `x?` with no enclosing function, and in `main`
(pinned to `i32` — wrap the fallible work in a helper and match on it).

### Rejected alternatives (propagation)

- **Return-type lift with implicit ok/some-wrapping** (the Zig coercion school; implemented
  first, then rejected 2026-07-06) — a body containing `?` whose success paths yielded plain
  `T` had its return type lifted to `T!`/`T?`, success paths auto-wrapped, void bodies
  inferring `void!` with `ok` on fall-through. Ergonomic (`mkdir(p)?` as a complete
  one-liner), and arguably cost-transparent (the wrap is the struct construction an explicit
  `ok` emits) — but it writes a return type the source never spells and inserts
  constructions the programmer never wrote, and the scoped return-path coercion it required
  ("with `?` present, `return 7` and `return err(...)` both accepted") made the return rules
  mode-dependent. It also forced a recursion restriction (self-calls typed against the
  pre-lift placeholder go stale — lift + self-reference had to be banned outright).
  Explicitness won: FC return types are always anchored by visible constructions, `?` or no
  `?`.
- **`try`/`orelse` keyword forms** (Zig spelling) — the grid already reserved `?`, and a
  postfix operator chains (`f(g()?)?`) where a prefix keyword nests.
- **Propagating an option into a result-returning function** (auto-converting `none` to some
  blessed code) — a hidden conversion with an invented code; write the `match` (or compose
  the types as `T?!`) instead.

*(As implemented 2026-07-06: TOK_QUESTION joins the postfix precedence level and reuses
EXPR_UNARY_POSTFIX — every walker in pass2/codegen/monomorph/lsp rides along; the node gains
`prop_fn_ret`, stamped by pass2 once the enclosing function's return type is resolved.
LambdaCtx collects propagation sites like it collects returns; EXPR_FUNC validates each site
against the derived return type right after return-type derivation (result-prop ⇒ TYPE_RESULT,
option-prop ⇒ TYPE_OPTION, else diag at the `?`). Codegen emits a GNU statement expression:
temp, failure test (`err != 0` / `!has_value` / null), pending-defer unwind, `return` of the
rebuilt failure ((RetC){ .err = t.err } / none / NULL), else the payload —
return-out-of-statement-expr is documented-permitted GNU C. `x?` reports as side-effectful so
argument sequencing stays left-to-right around it. Tests: `tests/cases/results/prop_*`,
`tests/cases/options/prop_option_*`, `tests/cases/defer/defer_return_in_loop`,
`tests/cases/generics/prop_on_type_var`; spec §Propagation; grammar postfix `?` + Rule 6 note
on `(x?)`.)*

## Rejected alternatives (carrier type)

- **Generic error type (`result<'a, 'b>`)** — the Rust path; see precedent analysis. The
  conversion-at-boundaries problem is structural, and FC has no traits to paper over it.
- **Error codes on `none`** (`none(T, code)`) — collapses under its own repr: `T*?` compiles to
  a bare nullable pointer, so codes either break the zero-cost guarantee or silently don't work
  for pointer options (a partial feature — exactly what completeness-over-partiality forbids);
  non-pointer options would all grow 4 bytes for a channel most never use; and fixing *that*
  means making code-carrying options a separate type — i.e., reinventing `T!` with worse
  syntax. Also erases signature legibility: `dict.get(k) -> v?` says absence is normal;
  `io.open(...) -> handle!` says failure has a reason. Both the ML school and Zig keep the two
  types separate; composition (`u8?!`) provides the unification benefit without the cost.
- **`$` suffix** — free and unambiguous, but semantically mute (shell/Perl sigil baggage) and
  forfeits the `?`/`!` grid.
- **`#` suffix** — reclaimable but requires reworking the lexer's directive scan (the `#` case
  fires anywhere, not just line-start, and `# if` currently spells `#if`); preprocessor/comment
  baggage; no semantic win over `$`.
- **Freeing `!` by renaming unwrap** — `checked` is taken by the overflow axis (and an
  expression modifier is the wrong granularity for mid-expression chains anyway); an
  `unwrap(x)` intrinsic turns the most common idiom in the language, `alloc(u8[n] {})!`, into
  inside-out noise; and `!` remains prefix boolean-not regardless, so the rename buys no
  single-purpose token — only migration churn.

## `alloc` stays `T?` — the option/result boundary for allocation — DECIDED 2026-07-07

*Raised at the top of the stdlib migration (follow-up 5): now that `T!` exists, should the
`alloc` intrinsic move from the option family (`T*?`/`T[]?`/`str?`) to the result family
(`T*!`)? Decided in discussion 2026-07-07 to **keep it an option**; recorded here so the
migration doesn't reopen it.*

### The decision: `alloc` remains `T?`

Allocation failure is **one bit** — the allocator returned NULL — with no discriminable reason.
`malloc`/`calloc` (what `alloc` lowers to) have exactly one failure signal; C11 doesn't even
guarantee `errno` is set on failure (POSIX guarantees `ENOMEM`, a single value). The one
arguable second reason, `calloc(n, size)` **overflow**, surfaces as the same NULL and properly
belongs to the *size computation* — FC's `checked` overflow axis — not to the allocator's error
channel. So the failure carries no menu to branch on, which is precisely the contract of `T?`
under this document's own signature-legibility rule (`v?` = absence, one bit; `handle!` =
failure has a reason). Typing `alloc` as `T!` would promise a discriminable reason and deliver a
lone constant — the same signature dishonesty the "Error codes on `none`" rejection guards
against, run in reverse.

Two reinforcing costs, both this branch's stated priorities:

- **Repr.** `T*?` is the null-sentinel — zero-overhead, a repr identity. `T*!` is
  `{ int32_t err; T* value; }` — strictly bigger and *not* a repr identity for the pointer case
  — paid on `alloc(u8[n] {})!`, the most common idiom in the language, and felt most on the
  16-bit-int targets. Carrying a reason that doesn't exist is exactly the unsought machinery FC
  declines.
- **Idiom family.** Keeping `alloc` an option keeps its `!` meaning "unwrap the present value,"
  consistent with every other option, rather than splitting the canonical allocation idiom onto
  the result rail.

### Precedent

Every serious language treats allocation failure as a single condition, and none hands back a
reason menu: **Rust** aborts by default (`Box::new`) and its fallible `AllocError` is a
*zero-field unit struct* — the "you asked for too much" case (`CapacityOverflow`) lives one
layer up in the collection's size logic, mirroring FC's overflow-is-`checked` split; **Zig**'s
`Allocator.Error` is the *single-member* set `error{OutOfMemory}`, so what its `!T` buys is
`try`-uniformity, not reason-conveyance (a result whose `err` is a foregone conclusion);
**kernel C** returns NULL and the *caller* synthesizes `-ENOMEM`; **C++** `new` throws a
payload-less `bad_alloc`.

### The `x?` gap is intentional, not a wart

Because `alloc` is an option, a function returning `foo!` cannot write `alloc(...)?` (option
propagation into a result-returning function is a compile error by design). Closing that gap
would require `none → err(code)` — the invented-code hidden conversion the propagation section
already rejects. The friction is the no-implicit-rewriting rule working: an allocation failure
that must travel upward as a result is spelled explicitly at the site that *decides* what it
means — `match alloc(...) with | some(p) -> … | none -> err(T, …)`, or `!` to abort.

### Rejected / deferred

- **`alloc : T!` with a lone `sys.out_of_memory` code** — a result whose `err` case is a
  foregone conclusion is an option in a costume, at strictly higher repr cost; see the decision
  above. If allocation ever grows a genuinely multi-reason story (custom allocators:
  arena-exhausted vs system-OOM vs overflow), it arrives as a *separate, explicitly-`T!`*
  allocator surface — Rust's `try_new`/`try_reserve` beside the abort-y default — with a blessed
  `error sys` group at that point, not by taxing every `alloc` now. Door open, not now.
- **Raw errno passthrough on allocation failure** (had `alloc` gone `T!`) — rejected in favor of
  a blessed named condition *if* a code were ever needed: FC owns `alloc` (an intrinsic with
  escape analysis and zero-init, not a bare extern), allocation's condition is platform-uniform,
  and errno-after-malloc is not C11-guaranteed. Raw passthrough (`from errno(null)`) is for the
  extern boundary where the platform produced the number; a language-owned intrinsic's failure
  is a language-owned named condition. (Moot while `alloc` stays `T?`.)

*Consequence for the migration:* `alloc` is untouched. A blessed `error sys` group is **not**
forced by allocation; it enters only if the genuinely multi-reason modules (`io`, `net`) want
named conditions, decided when the migration reaches them.

## Follow-ups (in order)

1. **Implementation** of `T!` per this document, with tests (new `tests/cases/results/`
   category: construction, inference anchoring, matching incl. literal codes and
   exhaustiveness, unwrap abort path + exit code, `err(T, 0)` guard both compile-time and
   runtime, composition `T?!`/`T!?`, generics `'a!`, equality, `default(T!)`, pointer payloads,
   `-Werror`-clean emitted C at 16- and 32-bit `int`).
2. **Propagation operator `x?`** — ✅ DESIGNED 2026-07-06, ✅ IMPLEMENTED 2026-07-06: pure
   local desugar over an explicitly-anchored carrier return type (no inference
   contribution; the implicit lift was rejected); see "Propagation operator `x?`" above
   (implementation notes at the end of that section).
3. **Stdlib error-code convention** — ✅ RESOLVED 2026-07-06: compiler-owned code space via
   `error` declarations; see "Error-code organization" above. ✅ IMPLEMENTED 2026-07-06
   (tests in `tests/cases/errors/` + `backtraces/err_unwrap_named`; spec §Named error codes;
   grammar `error_decl`/const-path pattern/`error` type atom/`error_name_expr`).
4. **C-interop extern result mapping** — ✅ DESIGNED 2026-07-06, ✅ IMPLEMENTED 2026-07-06:
   closed protocol set + `from <protocol>` extern tail + `void!`; see "C interop: extern
   result mapping" above (implementation notes at the end of that section). Tests in
   `tests/cases/extern/proto_*` and `tests/cases/results/void_result_*`; spec §Extern error
   protocols + §`void!`; grammar `error_protocol` / `void` type-atom note / bare-`ok` forms.
5. **Stdlib migration** (`io`'s conflating options, `net`'s `-1` sentinels, `mkdir`'s bool) —
   trails the feature, lands on this branch before merge into `develop`. Consumes items 2
   and 4: externs move to `T!`/`void!` returns via protocols; wrappers propagate with `x?`.
   `alloc` is explicitly *not* in scope — it stays `T?` (see "`alloc` stays `T?`" above).

## Revisit conditions

Revisit the fixed-`i32` decision only if lived experience shows per-call provenance genuinely
insufficient for diagnosing failures *and* out-params prove unworkable for the detail channel —
the specific failure mode the generic-`E` school claims and Zig's decade of practice has not
borne out.
