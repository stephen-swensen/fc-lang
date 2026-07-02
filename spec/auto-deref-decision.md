# Decision: `->` pointer field access — SUPERSEDED (adopted single-level `.` auto-deref, on trial)

> **Superseded 2026-07-01 (branch `dot-deref`).** The original decision below (keep explicit `->`)
> was reversed and single-level `.` auto-deref was adopted **on a trial basis**, in both `fc-lang`
> and the `wolf-fc` corpus. `.` now auto-derefs exactly one pointer level (`p.field` whether `p` is
> `point` or `point*`; the front end resolves the object type and emits `.`/`->` in the generated C
> accordingly), and the `->` *deref* spelling was **removed** — `p->x` is now a compile error that
> points at `.`. The three *mapping-arrow* jobs of `->` (function type, lambda body, match arm) are
> unchanged.
>
> Two reasons carried the trial despite the analysis below: (1) single-level `.` deref is a modern
> standard (Go/Zig), and (2) reducing the number of distinct `->` roles from four to three removes
> the one *unrelated* (non-mapping) job of the token — worthwhile given how many arrows FC already
> carries via its ML-style syntax. A pleasant side effect: the `when`-guard parenthesization wart
> disappears (`when p.x > 0 -> 1` needs no parens, since `.` never collides with the arm `->`). For
> a `T**`, deref the extra level explicitly: `(*pp).field` or (equivalently) `(**pp).field` — there
> is deliberately no multi-level auto-deref.
>
> Implementation: pass2 rewrites a `.`-on-pointer `EXPR_FIELD` node to `EXPR_DEREF_FIELD`, so codegen
> and every `kind`-dispatched analysis follow with no further change (`src/pass2.c`
> `check_pointer_field`); the parser retires the `->`-deref production (`src/parser.c`,
> `case TOK_ARROW`); the LSP auto-derefs one level for `.` member completion (`src/lsp.c`
> `complete_members`).
>
> The "both-spellings compromise" the original rejected is *not* what was adopted here — `->` deref
> was removed outright, so FC's "one way" ethos still holds (one spelling for field access: `.`).
>
> *Everything below is the original 2026-06-30 rejection, retained for the record.*

---

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
   pointer instance. But that presupposes you can write `x.field` on a generic `'a` *at all*, and
   FC rejects member access on a type var at the definition site (`field access on non-struct type
   'a`). FC *does* duck-type a closed built-in operator set on type vars — arithmetic/ordering/
   bitwise, validity checked at instantiation — alongside the universally-total `==`/`default`/
   `sizeof`; what it does not do is defer *member* resolution to instantiation the way C++ templates
   or Zig comptime would. So there is no value-side `.field` on a generic to extend to the pointer
   case — the benefit doesn't exist in FC. (See `spec/generics-constraint-model.md` for the full
   three-tier picture.)

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
abstract field access on generics meaningful — a user-extensible constraint mechanism, or deferring
*member* resolution to instantiation (FC already defers *operator* validity, just not member
lookup) — which is not currently planned.
