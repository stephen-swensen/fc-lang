# Design record: the built-in result type `T!`

*Decided July 2026. The rules live in `spec/fc-spec.html`: §Result Types (Part 4), §Extern
error protocols: mapping C failures to `T!` (Part 8), §Heap Allocation, and the error contract
at the top of Part 9. This record keeps the problem, the precedents, the alternatives weighed,
and the reasons for each choice.*

## The problem

FC was at first deliberately silent on error propagation: every known model either infects the
type system (Rust's generic `Result<T, E>`) or needs runtime machinery incompatible with the
cost model (exceptions). But the stdlib showed the bill of not deciding, with three conventions
in one library: `std::io` returned `T?` (so `read_all`'s `none` conflated "no such file",
"permission denied" and "out of memory"), `std::net` returned `-1` sentinels, and `io.mkdir`
returned `bool`. None could say *why* an operation failed.

Propagation sugar looks like the missing piece, but sugar needs a blessed carrier to desugar
over. Sugar over `T?` throws away the cause; sugar over ad-hoc user result unions hits the
error-type-mismatch wall. The carrier comes first.

## Precedent landscape

- **Exceptions (C++/Java/ML):** non-local control flow, invisible in signatures, unwinding
  machinery in the runtime. Incompatible with FC's cost transparency and C11 target.
- **Rust `Result<T, E>` + `?`:** **`E` is generic, so every propagation across an abstraction
  boundary needs an `E → E′` conversion.** That is why `?` calls `From::from` implicitly (a
  hidden function call) and why the ecosystem needed `anyhow`/`thiserror`. It also needs traits,
  which FC has ruled out.
- **Go `(T, error)`:** no type infection, but ceremony at every call, and `error` is an interface
  (dynamic dispatch).
- **Kernel C / negative `errno`:** `if (ret < 0) return ret;` is manual but *one uniform line*,
  because **the error type is fixed globally**.
- **Zig `!T` + `try`:** errors are bare codes, never generic, never carrying payloads, so `try`
  has nothing to convert. Error *sets* add inference machinery FC doesn't want; the degenerate
  form, one flat code space, is what FC adopts.

The two systems-school survivors share one move: **fixing the error type is what stops
propagation from infecting signatures.**

## The carrier

**The error payload is permanently `i32`.** A result is `ok('a) | err(i32)`, and everything else
in this record derives from that. Diagnostic *detail* (message text, positions) travels through
out-params, FC's established pattern: a decided limitation, not an oversight.

**Spelling.** `!` is a postfix type modifier like `?` and `*`. `T!` is Zig's spelling for the
same concept; Swift's `T!` (an implicitly-unwrapped optional, a regretted feature) is not the
meaning. With the expression postfixes `x!` and `x?`, the suffixes form a grid that teaches
itself, since types and expressions are disjoint grammatical contexts:

|      | type suffix            | expression postfix         |
|------|------------------------|----------------------------|
| `?`  | `T?` — may be absent   | `x?` — unwrap or propagate |
| `!`  | `T!` — may fail        | `x!` — unwrap or abort     |

Suffixes compose by position with no special cases, so both `u8?!` and `u8!?` are legal; `T?!`
is the idiomatic one (`read_char: u8?!`: `ok(some(b))` a byte, `ok(none(u8))` EOF,
`err(u8?, code)` an I/O error). That composition is the answer to merging options and results.

The one parse ambiguity, `(foo!)`, resolves **in favor of the cast** to `foo!` when an expression
start follows. Parentheses around a bare postfix unwrap are dead syntax (`foo!` already binds
tightest), so nothing is lost; it is the same class of rule as `(a*) b` versus `(a * b)`.

**Constructors are intrinsics.** `ok(v)` infers from its payload like `some(v)`; `err(T, code)`
takes the type like `none(T)`, because directional inference cannot infer a generic union's type
from a case with no generic payload. That is also the argument for a built-in over a library
union: a stdlib `result<'a>` union would spell `result<i32>.err(2)` forever, while an intrinsic
may take a type where a function can't.

**Representation: C's "0 on success" as a layout.**

```c
struct { int32_t err; T value; }   /* err == 0  ⇔  ok */
```

The tag *is* the code: one `i32` of overhead, debugger-readable, and no pointer-packing
specialization (`T*!` uses the same layout; `T*?!` holds the null-sentinel option in `value`).
`err(T, 0)` is unrepresentable and guarded like `some(null)`: a compile error when the code is
provably zero, a runtime check otherwise.

Zero-filled memory is therefore `ok(default(T))`, and `default(T!) = ok(default(T))`. That is
forced, not chosen: `default` is total over every type, and all-zeros is `err == 0`. The
zero-value school agrees all-zeros reads as success: C (a zeroed status int), Go (`nil` error),
Odin (`.None = 0`), Zig's internal layout (error codes start at 1). Two alternatives were
rejected:

- **`default(T!) = err(-1)`** breaks *default ≡ zero-filled memory*. Zero-filling is calloc-cheap
  because fresh zeros *are* default values; restoring consistency would inject a stamping loop
  into every zero-fill path whose type contains a result.
- **Re-encoding the tag** so all-zeros decodes as an error forfeits the point of the layout (the
  tag is the C status code, zero on success).

Rust's answer (`Result` has no `Default`) is unavailable, because FC's `default` is total by
design and zero-filled aggregates would deliver `ok(0)` through the back door anyway. The
residual risk, stated plainly: a result defaults to "falsely fine" where an option defaults to
"safely absent", limited to explicitly zero-filled aggregates (FC has no uninitialized bindings).

Results otherwise get every operation options have (`x!`, `match` with literal-code arms,
`.is_ok`/`.is_err`, `==`, generics), so the two carriers stay symmetric. `x!` on a result can
print the error code in its abort message.

### Rejected alternatives (carrier)

- **A generic error type (`result<'a, 'b>`)**, the Rust path. It is consistent with FC's other
  principles but makes conversion-free propagation impossible, and FC has no traits to paper over
  the conversions.
- **Error codes on `none`** (`none(T, code)`). `T*?` is a bare nullable pointer, so codes either
  break its zero cost or silently don't work for pointer options (a partial feature); non-pointer
  options would all grow 4 bytes for a channel most never use; and fixing that means a separate
  code-carrying option type, which is `T!` with worse syntax. It also erases signature
  legibility: `dict.get(k) -> v?` says absence is normal, `io.open(...) -> handle!` says failure
  has a reason. The ML school and Zig keep the types separate; composition (`u8?!`) gives the
  unification benefit without the cost.
- **A `$` suffix:** unambiguous but semantically mute, and it forfeits the `?`/`!` grid.
- **A `#` suffix:** needs the lexer's directive scan reworked, carries preprocessor and comment
  baggage, and wins nothing over `$`.
- **Freeing `!` by renaming unwrap:** `checked` is taken by the overflow axis; an `unwrap(x)`
  intrinsic turns the most common idiom, `alloc(u8[n] {})!`, into inside-out noise; and `!` stays
  prefix boolean-not regardless, so no single-purpose token is gained.

## Propagation operator `x?`

`x?` yields the payload on success, exactly as `x!` does. On failure it **returns the failure
from the enclosing function**: `err(code)` verbatim (the code is `i32` on both sides, so nothing
is converted) or `none` as `none`. It is a pure local desugar of the match-with-diverging-arm,
so `defer` unwinding comes free, and it acts on the top layer only. Spec: §Propagation: `x?`.

**`?` contributes nothing to inference.** The enclosing function must already return the
matching carrier, anchored as every FC return type is: by explicit constructions on its success
paths (`ok(...)`, `err(...)`, bare `ok` for `void!`, `some(...)`, `none(T)`). Otherwise the `?`
is a compile error that names the fix. The price is one visible `ok` per success exit, and only
where work *follows* a fallible call; a fallible call in tail position is already the result.
With the carrier always explicit, nothing else needs a special case: return paths mix
`return ok(v)`, `return err(T, code)` and propagation freely, and recursion works because the
base case anchors the carrier (`if v <= 1 then ok(1) else ok(v * fact(v - 1)?)`).

**The one sanctioned non-local construction.** `?`'s failure exit builds a whole value (an `err`
at the enclosing return type, or that type's `none`) whose type appears nowhere at the site.
Every other FC construction is type-anchored on the spot. `?` is accepted as a scoped exception
because the non-locality is `return`'s (every `return v` sends a value to the function boundary;
here the compiler merely spells it, as Rust's `?` and Zig's `try` do from a signature FC doesn't
have) and because the no-lift rule keeps the far anchor visible: the type is always one the same
body spells on its success paths. The construction is constitutive, not a convenience. Spelling
the type at each site (`x?(config)`) would duplicate what the body states and destroy chaining.

The fixed code means nothing needs *converting* at any boundary, and the body-anchored failure
means nothing needs *restating* at any propagation site. Together they close the infection
problem from both ends.

`x?` is rejected where no correct return exists: a propagation kind that doesn't match the
function's carrier (which also rules out mixing both kinds in one function), inside `defer` (it
is a return), and at top level or in `main` (pinned to `i32`).

### Rejected alternatives (propagation)

- **Return-type lift with implicit ok/some wrapping** (the Zig coercion school). A body using `?`
  whose success paths yield a plain `T` would get its return type lifted to `T!`/`T?`, with
  success paths auto-wrapped and void bodies becoming `void!`. It was built, then rejected. It is
  ergonomic (`mkdir(p)?` as a whole function body) and arguably cost-transparent, but it writes a
  return type the source never spells and inserts constructions nobody wrote. It also made the
  return rules mode-dependent ("with `?` present, `return 7` and `return err(...)` are both
  accepted") and forced a recursion ban (self-calls typed against the pre-lift placeholder go
  stale). Explicitness wins: return types are always anchored by visible constructions.
- **`try`/`orelse` keywords** (Zig's spelling): the grid already reserves `?`, and a postfix
  operator chains (`f(g()?)?`) where a prefix keyword nests.
- **Propagating an option into a result function** by turning `none` into some blessed code: a
  hidden conversion with an invented code. Write the `match`, or compose the types as `T?!`.

## Error-code organization: `error` declarations

**The problem.** `err` carries a raw `i32`, but nothing says who owns the numbers. Manual
per-module ranges (the kernel/FreeBSD approach) rot, because someone always squats on someone
else's range, and a registry file is coordination overhead. Zig's answer, a compiler-owned table
that assigns every named error a unique integer, is the right one, and the FC compiler sees every
declaration in one invocation.

**The decision.** `error file_io = | not_found | invalid_path` declares a group; code refers to
`file_io.not_found`; a pattern matches `| err(file_io.not_found)`. The design points and why:

- **A group is a pseudo-module of `i32` constants, not a type.** The code's user-facing type is
  the global **`error`**, a display alias of `i32` on the `str`/`cstr` precedent, so raw codes at
  C boundaries need no casts, `(e: error)` parameters work for free, and literal codes (`err(2)`)
  stay valid.
- **No unification, contra Zig.** `file_io.not_found` and `parse.not_found` are distinct codes.
  Zig unifies names because its error *sets* must merge; FC has no sets, and distinct codes keep
  provenance in the name.
- **Qualification is mandatory**, because a bare `| err(not_found)` is indistinguishable from a
  binding (the classic enum-in-pattern trap).
- **No per-group exhaustiveness.** The payload type is the global `error`, so a binding or `_`
  must follow named arms, as with integers. Groups are namespaces of constants, not sum types.

**The code space** (table in spec §Named error codes): `0` is `ok`; `[1, 65535]` is reserved for
platform passthrough; declared errors are numbered from 65536 in sorted order of their
qualified names; negative values carry sign-bit platform conventions.

- **Passthrough is sound by construction.** errno already shares FC's zero convention
  (`errno == 0` means no error), so a failing C call's errno wraps into `err(T, e)` unchanged. No
  translation table, and the debugger shows the number `strace` does.
- **Why 65536, not the kernel's MAX_ERRNO (4095):** Windows passthrough values (Win32 codes,
  `WSAECONNRESET` = 10054) would collide with a table starting at 4096. Reserving all of u16
  subsumes errno and the Win32/WSA space, and a code's value classifies itself: below 65536 is a
  platform code, at or above is a declared error findable by name.
- **HRESULTs pass through as negatives** (failing ones have the severity bit set). A wrapper
  tests `FAILED(hr)`, never `hr != 0`, since `S_FALSE` = 1 is a nonzero success.
- **Every declared code is provably nonzero**, so the `err(T, 0)` guard is elided for them.
- **Codes are not stable across builds**; adding a declaration shifts later codes. Zig accepts
  the same. The mitigation is printing names, not persisting numbers.

**Delivering names.** Each channel has its own cost home. `error_name(e)` returns the name as
`const str?` (`none` for platform codes; returning a formatted number would hide an allocation),
and the name table is emitted only when it is used. `--backtraces` already means "pay static data
for readable failures", so it also turns on named `x!` aborts; a separate flag would split one
fidelity-versus-size axis across two knobs. The `.errcodes` map ("strip the binary, keep the
map", like a linker map or split DWARF) is written **automatically** by every compile that
declares errors. An opt-in flag was rejected: because codes are not build-stable, the map is the
whole mitigation, and an opt-in's only failure mode is lacking the map for the build that
shipped. The map is deterministic, so CI can diff it to see which codes moved.

### Rejected alternatives (error codes)

- **Manual per-module ranges or a registry file:** rot and coordination overhead.
- **Zig-style global unification of names:** serves error-set merging, which FC lacks.
- **Mandatory translation at C boundaries** (Zig's `std.posix`: switch on errno, return named
  errors): a per-platform table in every wrapper plus an "unexpected errno" path, the hidden
  layer FC's C interop avoids. Translation stays *available* (see the stdlib contract), never
  forced.
- **Explicit `= N` pins on declared errors:** reintroduce the collisions the feature removes.
- **Generalized `code` declarations** for non-error channels: a different problem (no reserved
  zero, real type-position and exhaustiveness questions), which FC answers separately with `enum`
  declarations (spec §Enums).

## C interop: extern result mapping via error protocols

**The problem.** Options map to C by layout identity: `T*?` is a bare nullable pointer, so
`extern fopen: … -> any*?` needs no adaptation. `T!` can't work that way; its layout matches no
C ABI, and the code usually isn't in the return value at all (`errno` is a thread-local set out
of band). So each fallible C call needs an adapter: call, test the failure convention, capture
the code, build the result. And C has at least seven incompatible failure conventions, varying
in the *test* and in where the *code* lives. Leaving adapters to hand-written FC code reopens
the problem (every wrapper improvising a scheme) and hits a wall besides: `errno` is a C macro,
so FC code can't even read it without a per-platform shim.

**Precedents.** Zig's `std.posix` hand-writes wrappers that translate errno into named sets; it
must, because its errors are named sets, while FC's raw passthrough is what makes automation
possible. Rust has no automation (manual wrappers, `io::Error::last_os_error()`); a generic `E`
leaves nothing canonical to generate. The strong precedent is **C# P/Invoke**:
`[DllImport(SetLastError = true)]` makes the marshaller capture `GetLastError()` right after the
call, because reading it later is unreliable. A declaration-site annotation driving generated
capture code is exactly this feature. Go's generated `syscall` wrappers make the same move.

**The decision: a closed set of declared protocols.** An `extern` returning `T!` must name its
protocol in a `from <protocol>` tail, and a protocol requires a `T!` return. The set is closed,
one entry per crisp, documented C convention: `errno(-1)`, `errno(null)`, `status`,
`neg_errno`, `hresult`, `last_error(<sentinel>)`, `wsa_error(-1)` (table in the spec).

- **Payload-ness comes from the return type, orthogonal to the protocol:** `i32! from errno(-1)`
  for `open` (the fd is data), `void! from errno(-1)` for `mkdir` (the 0 is noise).
- **Sentinels are protocol syntax, not expressions.** The `null` in `errno(null)` exists only
  there; FC still has no null literal. `last_error` takes an explicit sentinel because Win32
  genuinely varies (`FALSE`, `NULL`, `INVALID_HANDLE_VALUE`).
- **No code arithmetic, ever.** The code is the number the platform produced; kernel-style
  `-errno` is not negated into the passthrough range. The cost (`EAGAIN` is 11 through libc but
  −11 through io_uring) is what per-call provenance already prices in.
- **A failure reported with `errno == 0`** (a C library breaking its own contract) builds
  `err(T, 0)`, and the existing guard aborts. No new rule.
- The wrap is generated inline at each call site: exactly the code a user would hand-write,
  declared visibly in the signature, reading `errno` directly on the same thread right after the
  call. Inline wrapping (rather than a named adapter per extern) also makes variadic externs work.

**`void!`, the payload-less result.** The largest class of fallible C calls returns a
meaningless 0 on success (`mkdir`, `close`, `bind`, every `pthread` call). Typing them `i32!`
would force `ok(0)` into existence and `| ok(_) ->` into every match. So `void!` exists, as the
**anti-unit** choice: `ok` is a payload-less variant (the `| empty` precedent), and no void value
ever materializes (no `()` literal, no void parameter, no `void?`, and `void` stays invalid as a
generic argument). Its layout is the lone `int32_t` tag, C's status int reified, which makes the
`status` protocol a layout identity. It is a normal type, not extern-only: a function propagating
`mkdir(p)?` must itself be able to return one.

**The irregular tail is deliberately out of scope.** The closed set excludes APIs where the C API
itself is ambiguous and a human must decide what the FC type means: `readdir` (NULL means both
end-of-stream and error; the natural FC type is `dirent*?!`), `getpriority` (−1 is a legal
success value), `strtol` (the sentinel overlaps the value domain). The fallback is a short
hand-written FC wrapper *into the same carrier*, passing the code through raw, so the code space,
`error_name` and `x?` all still apply. The stdlib keeps a per-platform errno accessor extern for
these wrappers. **Open:** the `readdir` class follows POSIX's "zero errno before the call"
pattern, which could become an `errno0(null)` protocol later.

### Rejected alternatives (extern mapping)

- **A general predicate language** for failure tests: what it adds over the closed set is the
  per-function-judgment tail no annotation can capture, at the cost of a mini-DSL forever.
- **Inferring the protocol from the return type** (`any*!` ⇒ `errno(null)`): reads as magic,
  collides at once (`i32!` could be errno, a status, an HRESULT or Winsock), and hides the one
  fact the reader needs at the boundary.
- **`i32!` carrying 0 instead of `void!`:** creates the meaningless value it claims to avoid.
- **Negating `neg_errno` codes:** code arithmetic breaks "the debugger shows the platform's
  number".
- **Hand-written wrappers as the only mechanism** (the Zig school): reopens improvisation, and FC
  can't read `errno` without a shim.

## `alloc` stays `T?`

Allocation failure is **one bit**: `malloc` has one failure signal, and C11 doesn't guarantee
`errno` is set. The one arguable second reason, `calloc` size overflow, belongs to the *size
computation* (FC's `checked` axis), not the allocator. A failure with no menu to branch on is
exactly `T?`'s contract; typing `alloc` as `T!` would promise a reason and deliver a lone
constant, at higher cost (`T*?` is a zero-overhead null sentinel, `T*!` a two-field struct, paid
on `alloc(...)!`, the most common idiom in the language). Precedent agrees: Rust's `AllocError`
is a zero-field unit struct, Zig's is the single-member `error{OutOfMemory}`, kernel C returns
NULL and the caller synthesizes `-ENOMEM`, and C++'s `bad_alloc` carries nothing.

So a result-returning function can't write `alloc(...)?`; closing that gap would need the
invented-code `none → err` conversion rejected above. The site that decides what an allocation
failure *means* spells it, or unwraps with `!`. **Open:** a genuinely multi-reason allocation
story (custom allocators: arena exhausted, system OOM, overflow) would arrive as a separate,
explicitly `T!` allocator surface beside the aborting default (Rust's `try_reserve` pattern),
not by taxing every `alloc`.

## Stdlib error contract: named conditions at the wrapper boundary

Do the stdlib's public wrappers hand callers raw platform codes, or map them to declared FC
errors? **Curated named conditions, with raw fallthrough.**

**The decisive argument:** raw platform codes **cannot be pattern-matched portably in FC.**
Patterns need compile-time constants, and the only compile-time error constants are declared
ones. `ECONNREFUSED` is 111 on Linux, 61 on macOS and 10061 on Windows, and an extern constant
(whose value arrives from headers at C-compile time) cannot appear in a pattern. Even C never
matches raw numbers; portable C matches `errno == ECONNRESET`, a platform-resolved name. Someone
must own the name table, and the stdlib writes it once or every user writes it badly. The stdlib
is also FC's portability layer, and raw codes would leak per-platform numbering through the seam
it exists to seal.

**Precedents.** Zig translates totally, with an `unexpectedErrno` dead end for unmapped codes, a
known wart. Rust's `io::Error` carries the raw OS code *and* a curated portable `ErrorKind`; Go
matches named sentinels (`errors.Is(err, fs.ErrNotExist)`) over a preserved `syscall.Errno`.
Those keep both channels because their error is a struct. FC's `err` is one `i32`, so each value
is either the named code or the raw one.

**The rules:**

- **The lowest FC wrapper maps once**, in a cold path that runs only on failure; everything above
  propagates with `?`. Extraction stays automated through extern protocols.
- **Curation:** a condition gets a name only if a caller plausibly *branches* on it (ENOENT drives
  create-if-missing; ECONNREFUSED and EAGAIN drive retries). The rest passes through **raw**,
  self-describing by the code-space partition. There is no catch-all `other` code: it would
  destroy the number for the tail. Adding a name later is non-breaking.
- **No hand-maintained numbers:** the map compares against extern constants
  (`extern ECONNREFUSED as refused: i32`), so the platform headers supply the values.
- **Groups are named for the failing domain** and nest in their module (`io.file.not_found`,
  `net.conn.refused`, `text.parse.invalid`); `error net` inside `module net` would stutter.
- **Operations return results; predicates stay `bool`** (false is an answer, not a failure);
  **absence stays `T?`** (`env`, a container's `get`).
- **`read`/`write` stay raw byte counts:** partial I/O is data, and `fread`/`fwrite` have no
  sentinel to test. **`close`/`flush` return `void!`:** `fclose` failure loses buffered writes.
- **`parse_*` return `T!`** with an all-named `error parse`: parse failure has reasons, overflow
  must be visible, and every parse error is FC-judged, so nothing passes through.
- **Allocation inside stdlib functions aborts** rather than surfacing as `err`, so a function's
  `err` always means its operation failed, never that memory ran out (the Rust default-allocator
  school).
- **Open:** a `write_all: void!` convenience.

### Rejected alternatives (stdlib contract)

- **Raw passthrough as the public contract:** founders on the pattern-matching cliff above.
- **Total mapping with a catch-all** (`io.other`): Zig's wart, and it destroys the raw number.
- **Per-platform errno constant groups for callers to match:** pushes the table to every consumer,
  multiplied per platform, and extern constants still can't appear in patterns.

## Results cannot be silently ignored

A result in statement position is a compile error: a dropped result is a silently lost failure,
the exact outcome the carrier exists to prevent (without the rule, a bare `io.close(f)` drops its
error invisibly). Zig hard-errors on discarded error unions; Rust warns through `#[must_use]`. FC
has no warning severity, and this is important enough to fail the build. Spec: §Results cannot
be silently ignored.

- **Results only.** A plain value in statement position (a byte count from `write`) stays legal,
  C-style: values are data you may not need, results are the error channel. Extending the rule to
  `T?` or all values was considered and not adopted.
- **Escapes:** `ignore expr`, `let _ = expr`, and `defer`, which ignores by contract (cleanup at
  scope exit is best-effort by nature).

**`ignore` is a per-value operator, not a region.** It evaluates its operand and yields `void`.
Its deciding job is voiding a **value-returning tail** (a run of `io.write` calls ending a match
arm) so no trailing `void()` is needed; `let _ =` can't, because a `let` is not a tail. Nim's
`discard` is the precedent and pairs with the same hard-error rule. Why not a block, like
`checked` and `unguarded`? Those markers select between two real runtime behaviors (wrap versus
trap, check versus skip) and must change the emitted code. A dropped result has one behavior; an
`ignore` region would emit the same C as per-site `ignore` and only silence a diagnostic across a
span, reopening the bulk-silent-failure hole. So `ignore` sits with `!` and `?`. The name is ML's
(F#/OCaml); Nim's `discard` was tried first and dropped because it reads too close to `defer`,
another statement-position prefix keyword with an adjacent meaning.

**Answer ergonomic pressure with better APIs.** Ignoring `mkdir(dir)` on a "make sure this
directory exists" path swallows every failure, including a real `denied`, just to skip the
expected `err(file.exists)`. The fix is `io.ensure_dir`, which folds `file.exists` into `ok` and
surfaces the rest as an honest `void!`, not a quieter way to drop results.

## Smaller decisions

- **Empty match arms are rejected.** `| ok ->` with nothing after the arrow was briefly adopted
  as a do-nothing arm, then removed: `void()` (spec §`void()` — the explicit void value) already
  is that arm, so an empty body is a second spelling, and it reads as an editing accident rather
  than a decision. If no-op arms come up again, the answer is `void()`'s discoverability.
- **No `unwrap_or` / `ok_or` stdlib adapters.** The or-default shape is a three-line `match`, and
  a plain generic function (`(r: 'a!, d: 'a)`) can provide it today. `match` stays the idiom;
  small helpers invite an adapter zoo. Revisit if real code shows the three-line match
  dominating.

## Revisit conditions

Revisit the fixed-`i32` payload only if experience shows per-call provenance genuinely
insufficient for diagnosing failures *and* out-params prove unworkable for the detail channel.
That is the failure mode the generic-`E` school predicts, and Zig's practice has not borne it
out.
