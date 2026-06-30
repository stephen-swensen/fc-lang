# Decision: keep explicit `->` for pointer field access (auto-deref rejected)

*Date: 2026-06-30. Status: decided — no change. This records why single-level `.` auto-deref
(Go/Zig style) was considered as a replacement for `->` pointer-field access and rejected, so the
question isn't re-litigated.*

## The question

`->` carries four jobs in FC: function type arrow (`(i32) -> i32`), lambda body (`(x) -> x*2`),
match arm (`pat -> result`), and pointer field access (`p->x`). The first three are one coherent
idea — a *mapping arrow*, left maps to right. The fourth is an unrelated C-ism (dereference and
access). The proposal: adopt **single-level `.` auto-deref** (write `p.x` whether `p` is `point`
or `point*`; the front end emits `.` or `->` per the resolved type), dropping the `->`-deref
meaning entirely. That would collapse `->` to one coherent meaning and remove the only concrete
grammar wart it causes — the `when`-guard parenthesization (`when (p->x) > 0 -> 1`), needed so the
deref `->` isn't mistaken for the arm `->`.

## Precedent landscape

- **C / kernel school:** explicit `->`. Pointer-ness is load-bearing; the programmer should see it
  at the use site.
- **Go:** `.` auto-derefs one level, no `->`. But its auto-deref exists chiefly to make **method
  calls** work uniformly across `T`/`*T` and value/pointer receivers; field access rides along for
  consistency.
- **Zig:** `.` auto-derefs one level (`ptr.*` for full deref), no `->`. Pairs with **comptime
  duck-typed generics**, where `.field` on a generic param type-checks at instantiation.
- **Rust:** `.` auto-derefs *arbitrarily* via the `Deref` trait with coercion — the magic,
  multi-level version FC explicitly does not want.

The single-level mechanical form is the Go/Zig move, not the Rust move (no trait, no coercion, pure
front-end desugar, zero runtime cost), so it would *not* have pushed FC out of its lane.

## Why it was rejected

The two arguments that could carry the change both depend on features FC deliberately lacks and
will not add (no interfaces, traits, or structural typing; no methods):

1. **Generics write-once over value-or-pointer — void, not merely rare.** The payoff would be a
   generic accessing `x.field` that monomorphizes to `.` for a value instance and `->` for a
   pointer instance. But that presupposes you can write `x.field` on a generic `'a` *at all* —
   which requires either a constraint mechanism (Rust traits) or duck-typed generic bodies
   (C++ templates, Zig comptime). FC has neither: there is no value-side `.field` on an
   unconstrained `'a` to extend to the pointer case. The benefit doesn't exist in FC.

2. **Precedent alignment — weaker than it looks.** Go's auto-deref is motivated by method-call
   ergonomics (FC has no methods); Zig's pairs with comptime duck-typed generics (FC has neither).
   Strip those away and what's left for FC is auto-deref purely for bare field access — spending the
   whole novelty budget to erase a token, enabling nothing.

Against those neutralized upsides stands a real, permanent cost: **loss of use-site pointer
visibility** across ~5,400 deref sites in the wolf-fc corpus, sharpest at `p.field = …` mutation
*through* a pointer — exactly the aliasing signal that matters most in a manual-memory engine
(`g->`, `e->`, `rc->` read as "long-lived, aliased, mutations escape this frame"). FC keeps `&` and
`*` explicit elsewhere; keeping `->` explicit is consistent with that, and with the project's
"don't chase a bar FC never set" posture. Adopting it now would polish a non-problem at the cost of
a stated design value.

A both-spellings compromise (allow `.` auto-deref *and* keep `->`) was also rejected: two ways to
spell one thing contradicts FC's "one way" ethos (no function overloading, no compound assignment)
and wouldn't de-overload `->` anyway.

## Decision

Keep explicit `->` for pointer field access. The `->` overloading is mild — the three mapping-arrow
uses are disambiguated by context and are not a source of real confusion. The one residual nit, the
`when`-guard paren requirement, is rare, already handled by the documented parenthesization rule,
and — if it ever genuinely nags — addressable in isolation (it concerns only how the guard's end is
delimited) without touching deref semantics. Revisit only if FC ever gains a feature that makes
abstract field access on generics meaningful (a constraint mechanism or duck-typed generic bodies),
which is not currently planned.
