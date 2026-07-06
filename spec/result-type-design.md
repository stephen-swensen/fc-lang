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

## Follow-ups (in order)

1. **Implementation** of `T!` per this document, with tests (new `tests/cases/results/`
   category: construction, inference anchoring, matching incl. literal codes and
   exhaustiveness, unwrap abort path + exit code, `err(T, 0)` guard both compile-time and
   runtime, composition `T?!`/`T!?`, generics `'a!`, equality, `default(T!)`, pointer payloads,
   `-Werror`-clean emitted C at 16- and 32-bit `int`).
2. **Propagation operator `x?`** — separate design pass once `T!` is in hand; the grid reserves
   the spelling.
3. **Stdlib error-code convention** — ✅ RESOLVED 2026-07-06: compiler-owned code space via
   `error` declarations; see "Error-code organization" above. ✅ IMPLEMENTED 2026-07-06
   (tests in `tests/cases/errors/` + `backtraces/err_unwrap_named`; spec §Named error codes;
   grammar `error_decl`/const-path pattern/`error` type atom/`error_name_expr`).
4. **Stdlib migration** (`io`'s conflating options, `net`'s `-1` sentinels, `mkdir`'s bool) —
   trails the feature, lands on this branch before merge into `develop`.

## Revisit conditions

Revisit the fixed-`i32` decision only if lived experience shows per-call provenance genuinely
insufficient for diagnosing failures *and* out-params prove unworkable for the detail channel —
the specific failure mode the generic-`E` school claims and Zig's decade of practice has not
borne out.
