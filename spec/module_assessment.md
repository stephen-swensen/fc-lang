# Module System Assessment (2026-03-25)

Post-implementation assessment of the revamped module system after the lexically scoped imports work on branch `scratch-modules`.

## Strengths

**The mental model is clean.** "Imports are `let` bindings for names from other modules" is easy to explain and reason about. Once you internalize that one sentence, the scoping, shadowing, and non-forwarding rules all follow naturally. You don't need to learn a separate set of rules for imports.

**Non-forwarding is the right default.** Most languages get this wrong or make it complicated (Rust's `pub use`, Python's `__all__`). The rule — `import *` only exports own declarations — is simple, unsurprising, and eliminates the transitive dependency leak problem entirely. No annotation needed.

**Per-file scoping solves a real problem.** Library code can import what it needs without polluting consumers. This is something Go got wrong (all files in a package share scope) and Rust got right (each file is its own module). FC landed on the right side.

**Qualified diagnostics pay for themselves.** `std::types.tuple2<int32, uint8> vs local.tuple2<int32, uint8>` immediately tells you the problem. Before this work, you'd see `tuple2<int32, uint8> vs tuple2<int32, uint8>` — identical strings for different types.

## Weaknesses

### Whole-module imports are still global

`import io from std::` goes to the global symtab, not the file or module import scope. This is a pragmatic compromise (module aliases need to be findable for dotted-name resolution), but it's an inconsistency in the "imports are lexically scoped" story. A file importing `import math from acme::` makes `math` visible everywhere.

**To fix:** Store whole-module imports in the ImportTable alongside member imports. The lookup helpers would need to return module Symbols (with `members` pointer) from the import chain, not just member Symbols. The EXPR_FIELD dotted-name resolution path needs to check the import chain for modules.

### Dual import storage mechanisms

Member imports use `ImportTable` with live `source_members` lookup. Whole-module imports use the old approach of copying into the global `SymbolTable`. This dual path means bugs could lurk in the interaction — the type-associated module test failure we hit during development is an example. The conceptual model is unified but the implementation isn't yet.

**To fix:** Unifying whole-module imports into the import table (same fix as above) would eliminate this.

### Generic union type identity through import aliases is fragile

`w<int32>.val(42)` doesn't work where `w` is an import alias of a generic union — the monomorphized type loses its type args. The non-generic case works fine. The union variant construction path in pass2 has its own assumptions about how names resolve that weren't fully updated by the canonical-name fix.

**To fix:** Trace the `EXPR_FIELD` → `EXPR_CALL` path for generic union variant construction when the union is accessed via an import alias. The variant construction code likely needs the same canonical-name treatment that fixed generic structs.

### No cycle detection across import chains

Pass1 phase 4 detects cycles in module *expressions* (A's code references B, B's code references A). But module-level imports could create new dependency patterns that aren't checked. If module A imports from B and B imports from A, the on-demand type checking in pass2 could theoretically loop. In practice, the existing cycle detection probably catches this indirectly, but it's not explicitly verified.

**To fix:** Extend phase 4's cycle detection to include module-level import edges, or add a visited set to the on-demand type-checking path in pass2.

### `qualified_name` propagation is a maintenance burden

We fixed `type_substitute` in types.c to copy `qualified_name`, but any other place that copies a Type struct (arena copies, shallow copies for aliases) needs to propagate it too. Missing it produces silently degraded diagnostics rather than a compile error, so it's easy to regress.

**To fix:** Consider a `type_copy()` helper that always copies all fields, used instead of manual `*copy = *original` patterns. Or audit all Type copy sites.

## Complexity Assessment

The conceptual complexity is low — the rules fit in a paragraph. The implementation complexity is moderate-to-high:

- `ImportScope` linked list (stack-allocated, pushed/popped on module entry/exit)
- `ImportTable` with `ImportRef` entries (per-module and per-file)
- `FileImportScopes` (per-file import tables passed between passes)
- ~12 lookup sites in pass2 updated to check the import chain
- On-demand type checking for imported functions via source module context switch
- Recursive nested module import processing in pass1

The previous attempt (reverted 2026-03-24) was more complex — dual registration, scope chain injection, `is_import` flags. This version is cleaner because the live `source_members` lookup avoids copying type data entirely.

## Priority Items

1. **Unify whole-module imports into ImportTable** — eliminates the dual-path inconsistency and completes the scoping story. This is the most impactful remaining architectural debt.
2. **Fix generic union identity through import aliases** — bounded fix in the variant construction path, not an architectural issue.
3. **Import cycle detection** — low urgency if expression-level cycle detection catches most cases, but should be verified.
4. **`qualified_name` propagation audit** — worth doing once to prevent diagnostic regressions.
