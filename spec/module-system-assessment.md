# Module System Assessment

Analysis of FC's namespace, module, type, generics, symbol resolution, and import system — strengths, comparisons, and areas for improvement.

## Strengths

### Uniform scoping model

Names resolve through a scope chain, innermost to outermost: local → module members → module imports → parent members → parent imports → ... → global. The same rules apply to every kind of name — structs, unions, functions, modules, constants. Most languages have special rules for types vs values vs modules. FC doesn't.

### Imports as lexical scoping

Imports follow the same shadowing rules as `let` bindings, scoped to the module where they appear. This is rare — most languages treat imports as file-level or package-level. A module's imports are its private vocabulary: children can see parents' imports but not vice versa. This avoids import pollution where importing a library changes what names existing code sees.

### Companion pattern

Having a struct/union and a module share a name gives associated functions without method syntax, vtables, or a trait system. `point.origin()` reads naturally. The union variant fallthrough (module first, then variants) is the right priority — you can always add a module function that intercepts a variant name if needed.

### Unconstrained monomorphization

No traits, no type classes, no where clauses. Write `let add = (a: 'a, b: 'a) -> a + b` and the compiler tries it at each call site. Errors happen at the call site, not at the definition. Same model as C++ templates (pre-concepts) and Zig's comptime. Avoids the complexity tax of a constraint system while remaining strictly sound. Fully forward-compatible with adding constraints later.

### No first-class modules

Modules are purely compile-time namespace mechanism. No module values, no functors, no runtime dispatch. The right choice for a C-targeting language — functors and module-level generics don't map well to C's compilation model.

## Comparison

| Aspect | FC | Go | Rust | OCaml | Zig |
|--------|----|----|------|-------|-----|
| Module nesting | Arbitrary depth | Flat (packages) | 2 levels (crate/mod) | Arbitrary depth | Flat (files) |
| Import scoping | Lexical (like let) | File-level | File-level | File-level | File-level |
| Type/value namespace | Unified | Unified | Split | Split | Unified |
| Generics model | Monomorphized, unconstrained | None (interfaces) | Monomorphized, constrained (traits) | Parametric polymorphism | Comptime, unconstrained |
| Associated functions | Companion modules | Methods on types | impl blocks | Functors/modules | Methods via comptime |
| Visibility | `private` keyword | Capitalization | `pub` keyword | `.mli` files | `pub` keyword |

FC is closest to Zig in spirit — both target C, both use unconstrained generics, both have manual memory management. FC's module system is more structured than Zig's (which is one module per file), and the lexical import scoping is more principled than most.

## Action items

### ~~Implement interleaved import/parent resolution in EXPR_IDENT~~ ✓ Done

Resolution order is now: at each module level, check members then that level's imports before moving to the parent. A child's import shadows a parent's member — consistent with "imports follow the same lexical scoping rules as let bindings."

**What was changed:**

1. **`resolve_symbol` / `resolve_symbol_kind`**: Rewritten to walk one module level at a time using `import_scope_lookup_until` / `import_scope_lookup_kind_until` with start/stop ImportScope boundaries from the `ModuleScopeChain`.

2. **`scope_lookup_capture`**: Stops at the current module boundary (the first `is_global` scope) instead of walking into parent module scopes. Closure capture semantics are preserved because captures only cross `is_lambda_boundary` scopes within the current module.

3. **EXPR_IDENT**: The separate parent_chain and import_chain blocks were merged into a single interleaved loop. At each level, imports are checked first (within that level's range), then parent members — each with correct on-demand type checking context.

4. **EXPR_FIELD**: The inline 4-step symbol lookup for union variant construction was replaced with a call to `resolve_symbol` for consistency.

### ~~Eliminate EXPR_FIELD re-resolution of identifiers~~ ✓ Done

EXPR_FIELD had three sites that re-resolved the object's name via `resolve_symbol` / `resolve_symbol_kind`, ignoring what EXPR_IDENT had already determined. This caused repeated bugs where parameters or locals with names matching modules or types were incorrectly re-resolved (e.g., a parameter named `data` being treated as module `data`).

**What was changed:**

- Added `resolved_sym` and `companion_module` fields to EXPR_IDENT in ast.h. EXPR_IDENT stores the Symbol it resolved to and, for struct/union names, the companion module if one exists.
- EXPR_FIELD, EXPR_CALL variant construction, and `find_callee_symbol` now read these stored pointers instead of re-resolving. No code path in EXPR_FIELD calls `resolve_symbol` or `resolve_symbol_kind` anymore.
- Fixed `import_table_add` to allow struct and companion module entries to coexist under the same name (different kinds), instead of the module replacing the struct.
- Fixed `import_scope_lookup_until` to use namespace-aware lookup for DECL_MODULE refs, preventing cross-namespace module confusion.

**Architectural invariant:** EXPR_IDENT is the single source of truth for identifier resolution. Later expression handlers (EXPR_FIELD, EXPR_CALL) read the stored result — they never re-resolve.

### ~~Fix nested module namespace propagation~~ ✓ Done

Nested modules (modules inside modules) did not inherit their enclosing namespace's `ns_prefix` — it was left as NULL. This meant nested modules couldn't import from same-namespace sibling modules (`import value from helpers` inside a nested module in `acme::` couldn't find `helpers`).

**What was changed:** Added `ns_prefix` parameter to `register_module_members` in pass1.c so nested module Symbols inherit the enclosing namespace. Also set `ns_prefix` on ImportRefs in `import_table_add` to match, so `import_scope_lookup_kind_until` uses namespace-aware lookup consistently. Removed the unused `ns_prefix` parameter from `resolve_nested_module_imports`.

### ~~Fix file-level import scoping and members-beat-imports rule~~ ✓ Done

Two issues fixed:

1. **File-level import leak:** File-level imports in the global namespace were added to the shared root scope, leaking across files. Fixed by removing the root-scope import addition entirely — file-level imports are now only visible through the per-file `import_scope` chain.

2. **Members beat imports uniformly:** Previously, file-level imports could shadow top-level `let` bindings (a special case). Removed this exception — members (declarations) now beat imports at every level, uniformly. This is consistent with how module members beat module imports.

### ~~Enforce imports-first ordering~~ ✓ Done

Imports must appear at the top of a file (after any `namespace` declaration) and at the top of a module body, before all other declarations. This eliminates ambiguity about source-order-dependent resolution and is the natural consequence of FC's order-independent member visibility within modules (where imports are the preamble, not interleaved declarations).

### ~~Simplify import syntax~~ ✓ Done

Removed bare `import MODULE` (without `from`) — same-namespace modules are already visible by name, making it redundant. Removed `import ... from global::` for both whole-module and member imports — the global namespace is implicit and not importable. Removed the global namespace fallback from non-global namespace import resolution — library code cannot implicitly access application-level (global namespace) declarations.

The three supported import forms are now:
- `import NAME from MODULE` — named member import
- `import * from MODULE` — wildcard member import
- `import MODULE from ns::` — cross-namespace whole-module import

### ~~Fix diagnostics showing mangled names~~ ✓ Done

Error messages were showing internal mangled C names (e.g., `outer__shape`) instead of qualified source names (e.g., `outer.shape`). Fixed 7 diagnostic sites in pass2.c to use `type_name()` which returns `qualified_name` — the fully qualified FC path including namespace prefix where applicable (e.g., `vendor::shapes.rect`).

### Eliminate the `base_name` field on Type

`base_name` exists on TYPE_STRUCT and TYPE_UNION for display purposes (showing source names in error messages). But `qualified_name` serves the same display role. If the display logic is unified to always derive the display name from `qualified_name` or the canonical name, `base_name` becomes dead weight. Removing it would simplify the Type struct and eliminate one of the lingering sources of "which name do I use?" confusion in the codebase.

### Investigate global registration of module-scoped types with ns_prefix=NULL

Module-scoped types are registered in the global symtab under mangled names (e.g., `std__data__array_list`) with `ns_prefix = NULL`. This means they bypass namespace isolation — they're accessible from any namespace context if you know the mangled name. In practice nobody writes mangled names in source code, but it means the global symtab is a flat namespace of mangled names that sidesteps the namespace rules. This was a pragmatic choice to make `resolve_type` work across namespaces, but it warrants investigation: can these entries carry their proper `ns_prefix` without breaking cross-namespace type resolution? Or is there a cleaner way to make module-scoped types findable without polluting the global namespace?

### Fix parser creating all type references as TYPE_STRUCT stubs

The parser creates all type references as TYPE_STRUCT stubs regardless of whether the target is a struct or union, because the parser doesn't know the target kind at parse time. This causes downstream complexity: `canonicalize_stub_names` must try both DECL_STRUCT and DECL_UNION, `resolve_type` must search for both kinds, and `unify` needs a struct/union kind mismatch handler. Most languages either resolve types to their kind during parsing (if there's a type namespace) or use a kind-agnostic "type reference" node. FC should adopt the latter — a TYPE_REF or TYPE_STUB kind that explicitly represents "unresolved type reference" without implying struct. This would make the stub → resolved type transition explicit and remove the need for kind-guessing fallbacks.

## Design decisions confirmed

### No re-export pattern

`import * from M` exports only M's own declarations — never re-exports. This is the correct default and we don't plan to add explicit re-export syntax. If a consumer needs child module names, they can import from the child directly. The "bring everything in" pattern creates fragile dependencies and makes it unclear where a name originates.

### Companion pattern as convention, not syntax

The companion pattern (struct/union + same-named module) is a naming convention, not special syntax like Rust's `impl` blocks. This is one less concept to learn and keeps the module system orthogonal. The companion fallthrough for union variants was added to make the pattern work seamlessly without requiring special syntax.
