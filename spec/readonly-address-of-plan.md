# Read-only address-of: `&` on immutable bindings yields `const T*`

> Status: **draft plan — not yet approved.** Additional design questions are still
> open before implementation begins. Captured from the planning session so it can
> be resumed later.

## Context

Today `&x` requires `let mut`; taking the address of an immutable `let` (or a
`let` parameter) is a compile error. The historical reason is sound but narrow:
a plain `T*` permits `*pp = v`, which is reassignment-through-an-alias — exactly
what `let` forbids. That argument does **not** apply to a pointer that forecloses
the write. `let mut` came first and `const` came later, so the read-only case was
left conservative.

We now make `&p` on an immutable binding yield a read-only **`const T*`** (on a
`let mut` it stays `T*`, unchanged). This removes a real *false-`mut` tax*: code
that needs a pointer for plumbing (extern `const T*` params like `setsockopt`
/`SDL_FillRect`/`nanosleep`, big-struct by-reference helpers, read-only views)
must currently declare `let mut` purely to take an address — and in FC that costs
something concrete, since `let mut` is uncapturable. The `mut` is dishonest, and
the workaround `let mut a = addr` even forces a spurious copy of a parameter.

The same change closes a **latent soundness bug**: `&<immutable global>` and
`&mod.member` bypass the mutability check entirely today and produce a *writable*
`T*` (with wrong `PROV_STACK` provenance), so this compiles, links `-Wall
-Werror` clean, and silently mutates an immutable constant:

```fc
module cfg =
    let scale = 7
let main = (args: str[]) ->
    let pt = &cfg.scale
    *pt = 99            // no error today — writes through an immutable const
    cfg.scale - 99      // exits 0
```

All four originally-contested design questions are already **decided** in
`spec/TODO.md` (observability = C's meaning of const; field address-of stays
`F*`; `&f` on a `let` lambda stays an error; the resulting `const T*` is an
ordinary capturable value). This plan also implements the **optional scope item 5**:
`&` through a *const path* (`&cp.field` where `cp: const T*`) currently errors —
it will instead yield `const F*`, the symmetric read-only completion.

Intended outcome: the feature ships, the bug is fixed, the spec (prose + every
code example) reflects the new rule, and no `let mut x = …; &x` false-`mut`
traces remain in stdlib, demos, wolf-fc, examples.fc, or tests.

## Design summary — the new address-of rule

`&e` classifies its operand and produces a pointer whose **const-ness tracks the
writability of the thing addressed**, and whose **provenance tracks its storage**:

| operand `e` | result type | provenance | change |
|---|---|---|---|
| immutable binding — `let` local, `let` param, immutable global, immutable `mod.member` | `const T*` | STACK (local/param) / STATIC (global/member) | **NEW / bug fix** |
| mutable binding — `let mut` local/global/`mod.member` | `T*` | STACK (local) / STATIC (global/member) | prov fix for globals |
| content address through a **non-const** path — `p.field`, `t[i]`, `arr[i]` | `F*` | STACK | unchanged (decided) |
| content address through a **const** path — `cp.field`, `cp[i]`, `*cp` | `const F*` | STACK | **NEW (item 5)** |
| function binding — `&f` on immutable `let` lambda | *error* | — | carve-out (unchanged) |
| function binding — `&f` on non-capturing `let mut`/top-level fn | `any*` (C fn ptr) | STATIC | unchanged |

Everything is additive except the one intended behavioral change: writes through
`&<immutable global/member>` that compiled unsoundly before are now correctly
rejected (they were bugs).

## Part A — Compiler change (`src/pass2.c`, the `TOK_AMP` case, ~lines 5289–5340)

Rewrite the address-of branch. All inputs are already on the type-checked
operand — no new lookups. Keep the existing early guards (fixed-array-field
error at 5295; `EXPR_FUNC` capturing-lambda error at 5321).

1. **Classify the operand** into "names a whole binding" vs "content address":
   - `EXPR_IDENT` → `is_binding = true`; `binding_mut = operand->ident.is_mut`;
     `binding_static = !operand->ident.is_local`. (`is_mut`/`is_local` are set at
     pass2.c:4686/4967 for locals *and* globals.)
   - `EXPR_FIELD` with `operand->field.resolved_member` and
     `resolved_member->decl->kind == DECL_LET` → module-member binding:
     `is_binding = true`; `binding_mut = resolved_member->decl->let.is_mut`;
     `binding_static = true`. (`resolved_member` is non-NULL only on the
     module-member path, pass2.c:7285 — struct-field access leaves it NULL, so
     this cleanly distinguishes `&mod.member` from `&value.field`.)
   - anything else → `is_binding = false` (content address / deref).

2. **Compute `make_const`:**
   - `ot->kind != TYPE_FUNC && is_binding && !binding_mut` → `true` (immutable binding).
   - `ot->kind != TYPE_FUNC && is_write_through_const(operand)` → `true` (**item 5**: const path).
   - **Remove** the current `is_write_through_const(operand) → diag_error("cannot take
     mutable address through const pointer")` branch at 5326–5330; it becomes the
     second `make_const` clause. (The write is still rejected — at the assignment,
     via `is_write_through_const` at pass2.c:6622.)

3. **Function-binding carve-out** — keep the existing `let mut` + non-capturing
   rule; do *not* let a func binding fall into the new const rule. When
   `ot->kind == TYPE_FUNC`: preserve today's behavior — for a *local* ident,
   `!is_mut` → error `"address-of requires mutable binding"` (this is the
   explicit `&f`-on-`let`-lambda carve-out), and capturing → `"cannot take address
   of capturing closure"`; a non-local func ident / func `mod.member` falls
   through to the `any*` C-function-pointer path unchanged.

4. **Emit the result:**
   - `TYPE_FUNC` → `type_any_ptr()`, `PROV_STATIC` (unchanged).
   - else → `Type *pt = type_pointer(ctx->arena, ot); if (make_const) pt =
     type_make_const(ctx->arena, pt); e->type = pt;` and
     `e->prov = (is_binding && binding_static) ? PROV_STATIC : PROV_STACK;`.

Reuse existing infrastructure: `type_make_const` (types.c:241), `type_pointer`
(types.c:273), `is_write_through_const` (pass2.c:3206). **Codegen is unchanged** —
`&` still emits `(&<operand>)` (codegen.c:3490); a `const T*` binding assigns from
a plain-C `&global` fine (adding a const qualifier is implicit in C), and the
`T* → const T*` coercion for call arguments is already wired (`unify`,
pass2.c:2591; widening, types.c:725).

Verify the case matrix behaves: `&let`→`const T*`, `&let mut`→`T*`,
`&param`→`const T*`, `&global`(immut)→`const T*`/STATIC, `&mod.member`(immut)→
`const T*`/STATIC, `&mod.counter`(`let mut`)→`T*`/STATIC, `&p.field`→`F*`,
`&cp.field`→`const F*`, `&f`(let lambda)→error, `&topfn`→`any*`.

## Part B — Spec update (`spec/fc-spec.html`)

Section titles are referenced by name (JS auto-numbers/anchors); keep headings
stable so `§Name` cross-refs don't break. Edit surface (from the spec inventory):

- **`### Address-of` (~3304–3334)** — the primary rewrite. `&<let>` is no longer
  an error: `let y = 42; let py = &y` now yields `const i32*`. State observability
  in C's terms ("no writes *through this pointer*, not 'nobody writes'"). Keep the
  `&f`-needs-`let mut`-non-capturing paragraph, and add one explicit sentence that
  `&f` on an immutable `let` lambda stays an error (a const C function pointer is
  not a meaningful interop artifact).
- **`#### One rule, three knobs` (1133–1150)** — the conceptual heart. Recast
  "Addressability tracks reassignability": addressing is now universal, but the
  *result's const-ness* tracks reassignability — `&let`→`const T*` (foreclosing
  `*pp = v`, so it does not reopen the door `let` closed), `&let mut`→`T*`. Update
  the `// &p compile error` code comment (1145) and the F# paragraph (1148) whose
  "`&x` requires a mutable local" parallel no longer holds (F# still matches on
  reassignment + uncapturable mutables; the address-of parallel is dropped).
- **`## let and let mut` properties table (1072–1075)** — the `let` row's
  `Addressable` cell changes from `no` to read-only (`&` → `const T*`).
- **`### Unary Operators` table row (1576)** — Address-of "Valid on" now includes
  `let` (result `const T*`) and `let mut` (result `T*`).
- **`#### Const sources` (1173–1181)** — add a fourth source: address-of an
  immutable binding (`&p` where `p` is a `let`/param/immutable global) yields
  `const T*`.
- **`### Parameters are let bindings` (2356–2358)** — parameters are addressable
  read-only as `const T*`; qualify "non-addressable".
- **`## Closures & Capture` (~2440, 2496)** — one sentence: the `const T*` from
  `&<let>` is itself an ordinary `let`-bound pointer value, hence capturable by
  copy like any pointer; the binding stays capturable (unchanged). (User flagged
  the "address of what" capture concern: since the address is read-only it doesn't
  matter — but call it out in the spec.)
- **Example cleanups** — `#### Explicit casts` (1210–1214) and `#### Write
  rejection` (1241–1242): the `let mut x = 42; let cp = (const i32*) &x` freeze
  idiom simplifies to `let x = 42; let cp = &x` (already `const i32*`); keep at
  most one "before/after" note showing the cast is now redundant. Flip the
  `### Address-of` example (3308–3313) from error to success.

## Part C — `spec/examples.fc` and `CLAUDE.md`

- **examples.fc:960–961** — `let mut v = 42; let cp = (const i32*) &v` →
  `let v = 42; let cp = &v` (already `const i32*`); adjust the comment.
- **examples.fc:988–990** — inner-scope `let mut inner = 77; &inner` (read-only)
  → `let inner = 77; &inner`; update the surrounding lifetime comment to say
  `let` bindings. Leave the genuine write-through examples as `let mut`
  (`:811 &n`+`*np=100`, `:818 &pt`+`pp.y=20`, atomics `:910`).
- **examples.fc doc lines** — `:219` ("addressable with &"), `:810`
  ("`// &x requires let mut — gives T*`"), `:966–969` note: reword to the new
  rule (`&x` on `let` → `const T*`; `let mut` needed only to write through).
- **CLAUDE.md `### Bindings`** — the `let` bullet currently says "not
  addressable"; change to addressable read-only (`&x` → `const T*`). The `let mut`
  bullet (`&x` → `T*`) is unchanged.

## Part D — Remove false-`mut` workarounds (stdlib, demos, wolf-fc)

Each site declares `let mut` (often copying a param) solely to take `&x` for a
`const`-pointer extern; drop `mut` and, where a copy exists purely for the
address, address the original binding directly. Genuine out-params / write-through
sites listed in the survey stay `let mut`.

- **stdlib `net.fc`** — `:301` `let mut val = 1` → `let val = 1`; `:309`/`:327`/
  `:353` `let mut a = addr; … (const any*) &a` → address the `addr` **parameter**
  directly (`(const any*) &addr`), deleting the copy. Keep `getsockname`
  out-params (`:368–370`). *Optional, demonstrates the feature:* fix the
  `nanosleep` extern signature (`sys.fc:25`) to `const timespec*` so `req`
  (`sys.fc:105`) can drop `mut` too.
- **demos** (all pass `&x` to a `const T*` param; `x` never reassigned) —
  `fasteroids/main.fc:164, 1074`; `face-invaders/main.fc:175, 182, 1299`;
  `fibbles/main.fc:92, 98, 220, 362`; `fing/main.fc:92` (the `(const any*)
  &timeout` setsockopt case). Drop `mut`. **Do not touch** the sibling
  `&obtained` / `&event` / `&out_w` / `&rng` etc. (real out-params / RNG state).
- **wolf-fc** (`/home/ryan/Projects/wolf-fc`, a sibling repo) — `src/main.fc:625`
  `let mut spec = sdl2.audio_spec { … }` → `let spec` (pure literal, passed as
  `const audio_spec*`; sibling `&obtained` stays `let mut`).

Note: demos and wolf-fc are outside `make check`; verify them by transpiling +
compiling (Part F), not the test runner.

## Part E — Tests

- **Flip** `tests/cases/pointers/addr_of_let_err` — `&<let>` is now legal. Convert
  to a positive test: `let x = 42; let p = &x` infers `const i32*`, `*p == 42`.
  Add a companion negative test that **writing** through it (`*p = 1`) is the error
  (`cannot assign through const pointer/slice`).
- **Update** `tests/cases/const/addr_through_const` — under item 5 `&(cp.x)` is now
  legal (`const i32*`); the error moves to `*px = 10`. Change the `.error` string to
  the write-through message and keep it a compile-error test.
- **New positive tests** (category `pointers/` unless noted):
  - `&` of an immutable `let` local → `const T*`, read through it.
  - `&` of a `let` **parameter** → `const T*` (the extern-call ergonomics case);
    add an `extern/` test passing `&<let>` into a C `const T*` param signature.
  - `&` of an immutable **global** and of an immutable **`mod.member`** → `const
    T*` (category `modules/`), read through it; assert returning such a pointer is
    allowed (PROV_STATIC).
  - `&cp.field` through a `const T*` → `const F*` (item 5), read through it.
  - capture-a-pointer composes: a closure captures `&<let>` (a `const T*` value).
- **New negative tests:**
  - write through `&<immutable global/member>` is rejected — the exact bug program
    above (category `modules/` or `memory/`).
  - `&f` on an immutable `let` lambda still errors (`address-of requires mutable
    binding`) — the function-binding carve-out.
- **Unchanged / must still pass:** all `&<let mut>` write-through tests
  (`pointer.fc`, `let_ptr_write.fc`, `deref_field.fc`, `inner_scope_*`,
  `ptr_to_ptr_out_param.fc`), the escape suite (`escape/*`), and the
  capturing-closure address-of errors.

## Part F — Verification

1. `make dev` (clean `-O0` build) → `make check` (gcc **and** clang) green,
   including flipped/new tests. Then `make test-all-O2` for optimizer-surfaced UB.
2. `make test-lsp` — LSP still passes; spot-check that hover on `&<let>` shows
   `const T*` (analyze path reuses pass2, so it follows automatically).
3. Transpile + compile every demo and wolf-fc to confirm the migrations build:
   for each, `./run.sh <main.fc …>` (or `fcc` + `gcc -std=c11 -Wall -Werror` for
   the graphical ones that can't run headless). Confirm no `-Werror` regressions
   from the dropped copies.
4. `./run.sh spec/examples.fc` runs clean (exit 0) after the example edits.
5. Grep the tree for residual false-`mut`: no `let mut … = …` remains whose only
   use is a single `&`/`(const …*) &` with no reassignment or whole-value write.
6. Re-run the bug program from Context: it must now fail to compile with
   `cannot assign through const pointer/slice` at `*pt = 99`.

## Notes / risks

- The only meaning change is that previously-accepted **unsound** writes through
  `&<immutable global/member>` now fail to compile. If any real program in the
  suite/demos/wolf relied on that, it was a latent bug; fix it at the source
  (make the binding `let mut` if the write is intended, or remove the write).
- `is_mut`/`is_local` on `EXPR_IDENT`, and `resolved_member->decl->let.is_mut` on
  module-member `EXPR_FIELD`, are the single source of truth — no re-resolution,
  consistent with the single-resolution invariant.
- Keep spec headings verbatim so the name-based `§` cross-references (`§Address-of`,
  `§One rule, three knobs`, `§Deep const`, `§Write rejection`) stay valid.

## Design deliberation outcome (2026-07-24) — the plan stands

Before approving, we stress-tested FC's whole `let` / `let mut` / `const` model
against an F# value-vs-variable lens (the doubt: "why is `const` only on
pointer/slice types, not values like C?"). **Outcome: the model is sound — more
coherent than C's — and this feature is *forced* by it, not bolted on.** No
rework.

- **Unifying framing.** Mutability is a property of *paths* (a root + hops).
  `let`/`let mut` sets the root's rebind permission; `const` sets the write
  permission at each dereference (lending) hop. `&` stamps a path with exactly the
  permission it already had — which *derives* both the new rule (`&<let>` →
  `const T*`) and item 5 (`&cp.field` → `const F*`) with zero free choices. FC's
  split (root axis = keyword, hop axis = qualifier) is strictly less ambiguous
  than C cramming both into `const` placement.
- **"Why no `const` on value types?"** Value-immutability is already spelled
  `let` (for a scalar, `let` ≡ C's `const int`); `const` is not a second
  immutability system, only the annotation for the one thing `let` can't express —
  permission on a lend. The only thing neither knob expresses is a frozen-contents
  *owned* value (C's `const struct` local), which FC omits by design: **the owner
  controls content.**
- **Explored and declined:** filling the rest of the `{rebind?} × {patch?}` square
  FC's model makes expressible — `let const` (frozen) and `let mut const`
  (rebind-no-patch ≈ F# `let mutable` of an immutable record). Both coherent, but
  local content-freeze is author self-discipline (FC punts style to the
  programmer) and rebind-no-patch gives consumers no guarantee `let mut` + a const
  view doesn't. **The owner-controls-content axis is affirmed as-is.**
- **Left open (future, unscheduled):** at most a `const` modifier at
  **global/module scope** for genuinely frozen, ROM-able constants — closing the
  edge that a module/file-level `let config = point{…}` today permits `config.x =
  3` from anywhere. Tracked in `spec/TODO.md` ("Deferred (future): a `const`
  binding for frozen constants at global scope"). Orthogonal to and larger than
  this plan — **ship the read-only address-of feature first.**

No changes to Parts A–F below result from this deliberation; it only confirms the
approach and records why the `const`-on-values question is settled.
