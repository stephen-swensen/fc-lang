# Decision: single-level `.` auto-deref for pointer field access

*Decided 2026-07-02, reversing an earlier decision (2026-06-30) to keep C's `->`. The rule is in
`spec/fc-spec.html` §Field access; this record keeps the question, both sides of the argument,
and why the second answer won.*

## The question

`->` used to carry four jobs in FC: function type arrow (`(i32) -> i32`), lambda body
(`(x) -> x*2`), match arm (`pat -> result`), and pointer field access (`p->x`). The first three
are one idea, a *mapping arrow* inherited from ML: left maps to right. The fourth is an
unrelated C-ism (dereference, then access a member).

The proposal: **single-level `.` auto-deref**. Write `p.x` whether `p` is a `point` or a
`point*`; the front end resolves the object type and emits `.` or `->` in the generated C. The
`->` deref spelling goes away entirely, leaving `->` one coherent meaning. It also removes the one
concrete grammar wart the deref arrow caused: a `when` guard needed parentheses
(`when (p->x) > 0 -> 1`) so the deref `->` was not mistaken for the arm `->`.

## Precedent landscape

- **C / kernel school:** explicit `->`. Pointer-ness is load-bearing, so the programmer should
  see it at the use site.
- **Go:** `.` auto-derefs one level, no `->`. The auto-deref exists chiefly to make **method
  calls** uniform across `T`/`*T` and value/pointer receivers; field access rides along.
- **Zig:** `.` auto-derefs one level (`ptr.*` for a full deref), no `->`. It pairs with
  **comptime duck-typed generics**, where `.field` on a generic parameter is checked at
  instantiation.
- **Rust:** `.` auto-derefs *arbitrarily* through the `Deref` trait with coercion. This is the
  magic, multi-level version FC does not want.

The single-level mechanical form is the Go/Zig move, not the Rust one: no trait, no coercion, a
pure front-end desugar with zero runtime cost.

## The case against (why `->` was first kept)

The two strongest arguments *for* auto-deref in other languages both depend on features FC lacks
and will not add (no interfaces, traits, structural typing, or methods):

1. **Write-once generics over value-or-pointer do not exist in FC.** The payoff would be a
   generic that writes `x.field` and monomorphizes to `.` for a value instance and `->` for a
   pointer instance. But FC rejects member access on a type variable at the definition site
   (`field access on non-struct type 'a`). FC does defer a closed set of built-in *operators* on
   type variables to instantiation, but never *member* resolution. So there is no value-side
   `.field` on a generic for auto-deref to extend. (See `spec/generics-constraint-model.md` for
   the three tiers.)
2. **The precedent is weaker than it looks.** Go's auto-deref is motivated by methods and Zig's by
   comptime generics. Without those, what is left is auto-deref purely for bare field access.

Against those, the cost is real: **loss of use-site pointer visibility** across the ~5,400 deref
sites in the wolf-fc port, sharpest at `p.field = …`, a mutation *through* a pointer. In a
manual-memory engine, `g->`, `e->` and `rc->` read as "long-lived, aliased, mutations escape this
frame". FC keeps `&` and `*` explicit; keeping `->` explicit would be consistent with that.

A both-spellings compromise (`.` auto-deref *and* `->`) was rejected outright: two spellings for
one thing contradicts FC's "one way" ethos (no overloading, no compound assignment), and it would
not de-overload `->` anyway.

## Why auto-deref won

The deciding argument is the syntax conflict itself. FC's `->` is an ML mapping arrow, and the C
deref arrow was the one role fighting that inheritance. With `.` the accepted modern spelling
(Go, Zig), the tie breaks in favor of removing the conflict. Supporting arguments:

- **A decision-free accessor.** Choosing `.` or `->` requires knowing a binding's pointer-ness,
  which is often far from the use site (an inferred `let`, a parameter declared elsewhere). With
  auto-deref there is no wrong choice, so a persistent error class for humans and code
  generators disappears. When a C-trained reflex writes `p->x`, the diagnostic contains the fix.
- **Alignment with FC's neighbors.** Go, Zig, Rust and ML all spell field access `.`. The `->`
  deref was the one place FC matched C against the grain of the rest of its syntax.
- **Fewer arrow roles.** Going from four jobs to three removes the only non-mapping job of a
  token FC already uses heavily. The `when`-guard parenthesization disappears as a side effect
  (`when p.x > 0 -> 1` needs no parens).
- **The cost is real but contained.** `p.field = …` no longer announces mutation through a
  pointer at the use site, a carve-out from the "representation-crossing operations are
  explicit" posture (`&`, `*`, casts). But it is a fixed single-level desugar with zero cost and
  no user extensibility (nothing like Rust's `Deref`). Address-*taking*, the direction that
  creates aliases, stays fully explicit, and pointer-ness remains visible at binding sites,
  signatures, and in LSP hover and inlay types. Go and Zig hold the same combination as a stable
  design point.

The first argument of the case against still stands: auto-deref enables nothing for generics
(see below). The decision rests on the syntax and ergonomics arguments alone.

## The rules

- `.` auto-derefs exactly **one** pointer level: `ptr.x` is `(*ptr).x`. The front end rewrites a
  `.` on a pointer into a deref-field access, so everything downstream sees an explicit deref.
- There is deliberately no multi-level auto-deref. For a `T**`, deref the extra level yourself:
  `(*pp).x` (equivalently `(**pp).x`).
- `->` has no deref meaning. `p->x` is a compile error that points at `.`.
- The three mapping-arrow uses of `->` (function type, lambda body, match arm) are unchanged.

FC still has one spelling for field access, so the "one way" ethos holds.

## Relationship to generics

Auto-deref fires only once the object's concrete type is known to be a pointer to a struct. It
does **not** add member access on an unconstrained type variable: a generic body still cannot
write `x.field` on a bare `'a`. The two questions are orthogonal, and any future proposal to add
member access on type variables, or a user-extensible constraint mechanism, should reopen this
record and `generics-constraint-model.md` together.

## Revisit conditions

Revisit only if living with the converted corpus shows mutation through pointers becoming
genuinely harder to audit in review. That is the failure mode the original analysis predicted,
and hands-on use has not borne it out.
