# Generics: universal, built-in-constrained, or explicit — why FC needs no traits

*Date: 2026-06-30. Status: descriptive — documents existing behavior and the design rationale.
Companion to `spec/auto-deref-decision.md`, which leans on the "no abstract `.field` on a generic"
fact established here.*

FC's generics are monomorphized and have **no user-declarable constraints** (no traits, interfaces,
typeclasses, or structural typing — and none are planned). That is not the same as "a generic body
can't touch its type variables." What a generic may do to a `'a` falls into three tiers, and the
boundary between them is the whole reason FC doesn't need a trait system.

## The three tiers

**Tier 1 — Universal (total over every type).** Accepted in the body, valid for *any* concrete
`'a`, no instantiation-time check required:

- `==` / `!=` — structural equality, generated per type; works even when `'a` is a struct.
- `default('a)`, `sizeof('a)`, `alignof('a)`
- copy / move / store-in-aggregate / return

```fc
let same = (a: 'a, b: 'a) -> a == b      // valid; same(box{v=1}, box{v=1}) → true
let zero = (x: 'a) -> default('a)        // valid for every 'a
```

**Tier 2 — Built-in ad-hoc constraint.** Accepted in the body, but the operator carries a
*closed, compiler-known* type constraint that is enforced **at instantiation** against the concrete
type (the error names the instantiation, e.g. `in add(bool, bool) …`):

| operators | constraint |
|---|---|
| `+ - * / %` (arithmetic) | numeric |
| `< > <= >=` (ordering)   | numeric **or pointer** |
| `& \| ^ << >>` (bitwise/shift) | integer |

```fc
let greater = (x: 'a, y: 'a) -> x > y    // body OK; checked per instantiation
// greater(10, 5)      → 'a = i32   → OK
// greater(box, box)   → error at the call: "ordering comparison requires numeric or pointer types"
```

This tier is the key observation: FC *does* have constraints — it just ships a **fixed set of
built-in ones** for the highest-value cases (generic numeric code: `min`/`max`/`clamp`, arithmetic
helpers). It is, in effect, a couple of hardcoded typeclasses (`Num`, `Ord`, `Integral`) welded into
the compiler, where the entire "constraint" is a one-predicate check at monomorphization
(`is_numeric?`) — no solver, no inference, no coherence/orphan rules, no where-clauses, no
associated types, and no surface syntax to name or extend them.

**Tier 3 — Unsupported: member access on a type var.** `'a.field` is rejected **at the definition
site**, before any instantiation, because a type variable has no known members:

```fc
let get_x = (p: 'a) -> p.x
//                       ^ error: field access on non-struct type 'a   (definition-time, no instantiation frame)
```

Note the asymmetry with Tier 2: operator *validity* is deferred to instantiation, but member
*resolution* is not. The reason is principled — deferring member lookup to instantiation is exactly
the duck-typing-over-user-structure (C++ templates / Zig comptime) that creates an *undeclared*
structural constraint ("`'a` must have a field `x`"). Taming such a constraint — declaring it,
checking it, reporting it legibly — is what drags a language into traits/concepts and the
error-message complexity that comes with them. FC stops at the edge where the semantic cost spikes.

## The through-line

> **Built-in constraints are implicit (operators); user constraints are explicit (function
> parameters).**

Because there is no way to *declare* a capability on a user type for a generic to discover, you
**pass the capability as an argument**. This is the C idiom (`qsort`'s comparator), made type-safe
and monomorphized. The stdlib already lives this consistently — `data.fc` threads `pred: ('a) ->
bool` and `cmp: ('a, 'a) -> i32` through `filter`/`any`/`all`/`find`/`sort`:

```fc
let sort = (s: 'a[], cmp: ('a, 'a) -> i32) -> …       // ordering passed in, not resolved
```

Need to reach into `'a`'s structure? Pass an accessor `key_of: ('a) -> 'k`. That single rule closes
the loop: Tier 1 is free, Tier 2 is free, and everything else is "pass the function." FC never needs
a trait system to be expressive over user behavior — it relocates that behavior from implicit
resolution to an explicit parameter.

## Where this sits among precedents

- **C** — no generics at all. FC's three tiers already go well past it.
- **ML / System F (OCaml core)** — strictly parametric: a `'a` is opaque, no operators on it. FC is
  *more* capable here (Tier 2 gives it built-in numeric generics OCaml lacks without functors).
- **C++ templates / Zig comptime** — duck-type *everything* at instantiation, including member
  access. FC deliberately duck-types Tier 2 but **not** Tier 3, avoiding the open structural
  constraints (and the error walls) that model produces.
- **Rust** — user-extensible traits with coherence, associated types, where-clauses. The full
  machinery FC is choosing not to buy.
- **Go** — shipped a decade with no generics; added only *constrained* ones (1.18), driven mainly by
  library authors. FC's audience is application/systems authors (e.g. wolf-fc), well served by
  Tier 1+2 plus explicit behavior-passing.

## Consequence (do not re-litigate without this)

The fact that **Tier 3 does not exist** — no abstract `.field` on a generic — is load-bearing for at
least one other decision: it is why single-level `.` auto-deref would buy FC nothing in generics (no
value-side `.field` to extend uniformly to pointers), part of why explicit `->` was kept. See
`spec/auto-deref-decision.md`. Any future move to add member access on type vars (or a
user-extensible constraint mechanism) should reopen both this note and that one together.
