# Generics: universal, built-in-constrained, or explicit — why FC needs no traits

*A descriptive record (2026-06-30) of what a generic body may do to a type variable, and why
the boundary sits where it does. The rules are in `spec/fc-spec.html` Part 5, §Scope of
operations on type variables and §Errors at monomorphization.
`spec/auto-deref-decision.md` relies on the "no abstract `.field` on a generic" fact
established here.*

FC's generics are monomorphized and have **no user-declarable constraints**: no traits,
interfaces, typeclasses, or structural typing, and none are planned. That is not the same as "a
generic body can't touch its type variables." What a generic may do to a `'a` falls into three
tiers, and the boundary between them is the reason FC doesn't need a trait system.

## The three tiers

**Tier 1 — Universal (total over every type).** Accepted in the body and valid for *any*
concrete `'a`, with no instantiation-time check:

- `==` / `!=` — structural equality, generated per type; works even when `'a` is a struct.
- `default('a)`, `sizeof('a)`, `alignof('a)`
- copy / move / store-in-aggregate / return

```fc
let same = (a: 'a, b: 'a) -> a == b      // valid; same(box{v=1}, box{v=1}) → true
let zero = (x: 'a) -> default('a)        // valid for every 'a
```

**Tier 2 — Built-in ad-hoc constraint.** Accepted in the body, but the operator carries a
*closed, compiler-known* type constraint that is enforced **at instantiation** against the
concrete type (the error names the instantiation, e.g. `in add(bool, bool) …`):

| operators | constraint |
|---|---|
| `+ - * / %` (arithmetic) | numeric |
| `< > <= >=` (ordering)   | numeric, pointer, or same enum |
| `& \| ^ << >>` (bitwise/shift) | integer |

The built-in static type properties (`'a.min`, `'a.max`, `'a.bits`, and the float ones) belong
here too: they name a property of the type, not a member of a value, and are checked per
instantiation.

In the body, the result of any of these operators (other than a comparison) has the type
variable's type, so an instantiation is also rejected when the same operation on the concrete
types would have a different one: `x + 300` at `'a = u8` widens to `i32` rather than computing a
wrapped `u8`. An instance means exactly what the same code written with concrete types means.

```fc
let greater = (x: 'a, y: 'a) -> x > y    // body OK; checked per instantiation
// greater(10, 5)      → 'a = i32   → OK
// greater(box, box)   → error at the call: "ordering comparison requires numeric or pointer types"
```

This tier is the key observation: FC *does* have constraints. It ships a **fixed set of built-in
ones** for the highest-value cases (generic numeric code: `min`/`max`/`clamp`, arithmetic
helpers). In effect these are a few hardcoded typeclasses (`Num`, `Ord`, `Integral`) welded into
the compiler, where the whole "constraint" is a one-predicate check at monomorphization
(`is_numeric?`). There is no solver, no inference, no coherence or orphan rules, no
where-clauses, no associated types, and no surface syntax to name or extend them.

**Tier 3 — Unsupported: member access on a type variable.** `'a.field` is rejected **at the
definition site**, before any instantiation, because a type variable has no known members:

```fc
let get_x = (p: 'a) -> p.x
//                       ^ error: field access on non-struct type 'a   (definition-time, no instantiation frame)
```

Note the asymmetry with Tier 2: operator *validity* is deferred to instantiation, but member
*resolution* is not. The reason is principled. Deferring member lookup to instantiation is the
duck-typing over user structure that C++ templates and Zig comptime do, and it creates an
*undeclared* structural constraint ("`'a` must have a field `x`"). Taming such a constraint
(declaring it, checking it, reporting it legibly) is what drags a language into traits or
concepts and the error-message complexity that comes with them. FC stops at the edge where the
semantic cost spikes.

## The through-line

> **Built-in constraints are implicit (operators); user constraints are explicit (function
> parameters).**

Because there is no way to *declare* a capability on a user type for a generic to discover, you
**pass the capability as an argument**. This is the C idiom (`qsort`'s comparator), made
type-safe and monomorphized. The stdlib follows it consistently: `data.fc` threads
`pred: ('a) -> bool` and `cmp: ('a, 'a) -> i32` through `filter`/`any`/`all`/`find`/`sort`:

```fc
let sort = (s: 'a[], cmp: ('a, 'a) -> i32) -> …       // ordering passed in, not resolved
```

Need to reach into `'a`'s structure? Pass an accessor `key_of: ('a) -> 'k`. That single rule
closes the loop: Tier 1 is free, Tier 2 is free, and everything else is "pass the function." FC
never needs a trait system to be expressive over user behavior; it moves that behavior from
implicit resolution to an explicit parameter.

## Where this sits among precedents

- **C** — no generics at all. FC's three tiers already go well past it.
- **ML / System F (OCaml core)** — strictly parametric: a `'a` is opaque, no operators on it. FC
  is *more* capable here (Tier 2 gives it built-in numeric generics OCaml lacks without
  functors).
- **C++ templates / Zig comptime** — duck-type *everything* at instantiation, including member
  access. FC duck-types Tier 2 but **not** Tier 3, avoiding the open structural constraints (and
  the error walls) that model produces.
- **Rust** — user-extensible traits with coherence, associated types, where-clauses. The full
  machinery FC chooses not to buy.
- **Go** — shipped a decade with no generics, then added only *constrained* ones (1.18), driven
  mainly by library authors. FC's audience is application and systems authors, well served by
  Tiers 1 and 2 plus explicit behavior-passing.

## Relationship to `.` auto-deref

Tier 3's absence (no abstract `.field` on a generic) is unchanged by `.` auto-deref (see
`spec/auto-deref-decision.md`). Auto-deref applies to a single *concrete* pointer level
(`p.field` on a `point*`); it does not add member access on an unconstrained type variable, and
it fires only once the object's concrete type is known to be a pointer to a struct. The two
questions are orthogonal. Any future move to add member access on type variables, or a
user-extensible constraint mechanism, should reopen both records together.
