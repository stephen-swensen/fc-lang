# Module System Evaluation

A design review of FC's module system, prompted by the concern that it might be simultaneously too complex at one level and too simple at another. The conversation examined three structural critiques against the designer's intent. All three were resolved in favor of the current design, with one remaining observation about cognitive weight.

## The Concerns Under Review

Three initial concerns were raised:

1. **Arbitrary-depth module nesting** — does the `ModuleScopeChain` machinery (interleaved member/import resolution up N ancestors) earn its keep, or could it be capped?
2. **Namespace hierarchies** — does multi-segment namespace naming (`acme::graphics::`) encode a tree that isn't load-bearing, when vendor isolation might be the only real job?
3. **Entry-point-only file-level globals** — is the rule that top-level `let`/`let mut`/`struct` outside a module is only permitted in the file containing `let main` a principled compromise, or an inconsistency?

A fourth emerged during review:

4. **The companion module/struct pattern** — does allowing a module to share a name with a struct/union (with a fallthrough resolution rule from module member to union variant) collapse two namespaces into one syntax in a way that should instead be a dedicated `impl` construct?

## Design Intent (as clarified)

### Nested modules serve real patterns

The pattern `module foo = ... private module c from "header.h" = ...` is used in stdlib (e.g., `stdlib/text.fc`) to keep C extern bindings private and local to their wrapper. This requires at least depth 2. Arguments for capping at 2 founder on the 0/1/N principle: picking any fixed finite depth above 1 is arbitrary, and 1 is too restrictive. Arbitrary N is the principled choice.

F# in practice rarely nests beyond 2–3 levels, and OCaml's deeper nesting is driven by features FC doesn't have (module signatures, functors). That does not argue *against* arbitrary nesting — it just means the generality is unused today. The stdlib is young; patterns requiring deeper organization may emerge. The machinery is already written and tested, so the marginal cost of keeping the generality is low.

### Namespaces are flat vendor isolation

`::` is a divider, not a tree navigator. `acme::graphics::` is conventionally read as a vendor/sublib pairing but is not semantically hierarchical — segments do not enable partial-path navigation. The purpose is to keep vendor-provided library code from polluting app-code namespace. By intent, `let main` only exists in the implicit `global::` namespace: applications are unnamespaced, libraries are namespaced. A vendor developing a library can still have test harnesses with `main` in the implicit global namespace.

This resolves the earlier objection about entry-point globals "disappearing" when an app declares a namespace: apps with a `main` are never in a namespace to begin with, so the scenario cannot occur.

Spec wording could state the "just a divider" interpretation more plainly than "one or more `::` segments," which reads tree-like.

### Entry-point globals are a three-tier compromise

Allowing file-level `let`/`let mut`/`struct` only in the file containing `let main` supports three usage tiers:

- **Scratchpad:** single-file FC programs can use top-level lets naturally.
- **App with helpers:** the main file holds shared app-level state, visible implicitly to other `global::` modules.
- **Library:** non-main files must use modules; no file-level global pollution.

This is a deliberate and coherent design, not an inconsistency. The restriction to the entry-point file is what contains the feature.

### Companion modules are an extension mechanism

The companion pattern is not a weak substitute for associated functions; it is an **extensibility feature**. A vendor ships `struct widget`, and a user can write `module widget = ...` in their own code to attach functions accessible as `widget.my_fn(...)`. This is equivalent in spirit to Rust extension traits, Kotlin/C# extension functions, Swift extensions — a recognized pattern for adding methods to types one doesn't own.

Proposing a dedicated `impl` construct would add a new language primitive for work the `module` primitive already does. Reusing `module` is economical: modules already have scoping, visibility (`private`), and import rules; opening a new module alongside someone else's type is a natural act.

The cost is the fallthrough resolution rule on unions: when a module shares a name with a union, dot-access resolves to the module member first, then to a variant constructor if no member matches. A user who writes `module shape = let circle = ...` alongside a `union shape = | circle(...) | ...` silently shadows the variant constructor at `shape.circle(...)`.

A compiler warning for this case was considered and rejected: the companion relationship is not a property of either the module or the union — it only materializes at the scope where both become visible. The warning would fire at import sites, flagging code neither author wrote. It is a non-local, context-dependent diagnostic. Further, in practice anyone writing a companion module is looking at the type being extended, so accidental variant-name collision is theoretical rather than practical.

## Outcome

All three structural concerns were resolved in favor of the current design:

| Concern | Resolution |
|---|---|
| Arbitrary-depth nesting | Justified by 0/1/N; machinery already paid for |
| Namespace hierarchy | `::` is a pure divider, not a tree — no hierarchy to critique |
| Entry-point globals | Coherent three-tier design; main cannot exist in a namespace by design |
| Companion pattern | Extension mechanism, economical reuse of `module` primitive |

## Remaining Observation

The module system carries genuine **cognitive weight**: a full description requires covering seven name-resolution levels (local → module members → module imports → parent members → parent imports → ... → file imports → globals), interleaved resolution up the module chain, the companion pattern with its fallthrough rule, namespace vs module vs file roles, and the entry-point-file special case. This is not a design flaw — it is the honest price of the expressiveness chosen. For a language targeting systems programmers, the ceiling is reasonable.

The actionable implication is not redesign but **documentation**: the specification should make the "just a divider" framing of namespaces explicit, and should present the companion pattern primarily as an extensibility mechanism rather than a static-method workaround. New-reader onboarding cost is where the remaining work lives.

## Decisions Recorded

- Keep arbitrary-depth module nesting. Do not invest further in constraining it.
- Keep the companion module/struct pattern unchanged. Do not add an `impl` construct.
- Do not add a variant-shadowing warning for companion modules. The diagnostic is non-local and the footgun is not practical.
- Consider spec wording improvements: clarify namespace `::` as a divider; frame companion modules primarily as extension points.
