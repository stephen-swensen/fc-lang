# Plan: make the C name of a type a function, not a field

Status: **not started** — written 2026-07-19 after BUGS.md §4.14, which was the
third or fourth recurrence of the same bug family. Do this in its own session.

## The recurring bug

Symptom, every time: `fcc` exits 0 and emits C that names a struct or function
that was never defined. Most recently (§4.14) `bx(bx(v))` inside a generic
function emitted a call to `fc__bx__7_fc__box` while defining
`fc__bx__14_fc__box__3_i32`. Earlier instances: §4.4 (a phantom `wide<64>`
typedef) and §4.5 (two typedefs for one signature).

## Why it recurs

A monomorphized instance's C name is **stored mutable state on the type node**
(`Type.struc.name` / `Type.unio.name`), not a function of the type. Sites that
need a name either read whatever is stored or overwrite it, so the name a type
ends up with depends on *which walk reached it first*. Two sites that disagree
about ordering disagree about the name, and the disagreement surfaces only as a
gcc error in generated output.

That is why fixing one site relocates the symptom rather than removing it: the
§4.14 session patched five producers before finding the sixth, and each one had
been producing a *differently* wrong name.

## The actual root cause (found while scoping, 2026-07-19)

`mangle_type_name` (src/types.c ~1601) spells a type for use inside a mangled
name. Its arms:

```c
case TYPE_STRUCT:  return str_dup(t->struc.name);   // reads stored state
case TYPE_UNION:   return str_dup(t->unio.name);    // reads stored state
case TYPE_STUB:
    if (t->stub.type_arg_count > 0) {               // RECURSES over its args
        char *r = mangle_cat(str_dup(t->stub.name), "__");
        for (int i = 0; i < t->stub.type_arg_count; i++)
            r = mangle_append_piece(r, mangle_type_name(t->stub.type_args[i]));
        return r;
    }
```

The `TYPE_STUB` arm already computes the name from structure, and its comment
describes the exact bug §4.14 chased:

> Without recursing into the args, a stub nested in another instance's type args
> (`box<box<i32>>`, where the inner arg is still an unresolved stub) would drop
> its args to the bare base name — making distinct instantiations collide and
> mis-resolving the C type name.

So this was already diagnosed and solved — **for one of the three
representations**. `TYPE_STRUCT` and `TYPE_UNION` are the untreated twins. This
is CLAUDE.md's "extend existing channels; never add a twin" rule, and the drift
it warns about, appearing in the wild.

## Revised size estimate

My first estimate (44 mutation sites, a multi-file refactor) was wrong — it
counted every assignment to `struc.name`, most of which are legitimate:

| sites | where | what | in scope? |
|---|---|---|---|
| 3 | parser.c | declaration name from source | no |
| 12 | pass1.c | module-scope mangling (`inner` → `m__inner`) | no — a *declaration* name, a separate concern |
| 2 | types.c | copy constructors propagating a name | no |
| 13 | monomorph.c | instance naming, in-place during walks | yes |
| 14 | pass2.c | instance naming at inference points | yes |

The real target is the ~27 instance-naming sites, and most become *deletions*
once naming is pure. The clean split to preserve: **pass1 owns declaration
names; the mapping function owns instance names**, and the latter takes the
former as its base.

## The change

1. Give `mangle_type_name`'s `TYPE_STRUCT` / `TYPE_UNION` arms the same
   recursion the `TYPE_STUB` arm has: when the node carries type arguments,
   spell `base "__" lp(mangle_type_name(arg))*`.

2. Take `base` from the **symbol**, never from the node:
   `resolved_sym ? resolved_sym->type->struc.name : struc.name`. This is what
   makes it idempotent — the base is the canonical template name whether or not
   the node has already been renamed in place, so re-naming an
   already-named node is a no-op rather than a double-mangle
   (`fc__box__3_i32__3_i32`). `discover_nested_types` already does exactly this
   lookup, with a symtab fallback for nodes lacking `resolved_sym`; reuse that
   shape.

3. Delete the in-place renames that exist only to make a *later* reader see the
   right name. Keep the ones that record a name for identity/dedup
   (`mono_register`'s table key).

4. Retire `mono_canonical_type_arg` and `generic_instance_c_name` (added in
   §4.14 as scaffolding) once the function is pure — they exist only to repair
   names after the fact.

## Open question to settle during implementation

`mangle_type_name` lives in types.c and has no symbol table. Step 2 needs one of:

- **(a)** thread `SymbolTable*` (or just the `Symbol*`) into `mangle_type_name`
  — it already takes no context, so this changes ~13 call sites;
- **(b)** rely solely on `resolved_sym` on the node, and treat a missing one as
  "name is already canonical" — simpler, but only sound if `resolved_sym` is
  reliably populated. It is currently set at just 3 sites (2 in pass1, 1 in
  types.c), so **verify coverage before choosing this**;
- **(c)** stop rewriting `struc.name` in place at all, so the stored name is
  *always* the template base and no disambiguation is needed. Cleanest, and the
  real end state, but the largest step.

Prefer (c) if the in-place renames turn out to be removable; (a) otherwise.

## Risk

Low-to-moderate, and the suite is a good net:

- **No test pins a mangled name.** The three test files matching `fc__…__N_…`
  mention names only in comments, so renaming cannot silently pass.
- **Dedup gets stricter, not looser.** The mono table keys on the interned
  mangled name; making naming pure merges entries that are currently distinct
  only because one was named early. Expect *fewer* instances, all correct.
- **Emission ordering** is computed from names (`find_by_value_dep_name` and
  the dependency sort). Re-verify that ordering holds once names change timing.
- Verify with `make test-all` and `make test-all-O2` (gcc + clang), `make
  test-lsp`, plus `spec/examples.fc`, the five demos, wolf-fc and euler-fc — the
  same sweep §4 used.

## Test to keep

`generics/nested_instance_arg_in_generic` already covers the family: two and
three levels, an instance reaching the argument through a wrapper constructor,
a generic union argument, the const-generic twin, and the non-generic twin. It
should keep passing unchanged — if the refactor is right, it is the *only* thing
protecting against the regression, and it should need no edits.
