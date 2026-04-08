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

### Implement interleaved import/parent resolution in EXPR_IDENT

The intended resolution order is: at each module level, check members then that level's imports before moving to the parent. This means a child's import can shadow a parent's member — consistent with "imports follow the same lexical scoping rules as let bindings."

**Current state:** `resolve_symbol` and `resolve_symbol_kind` still use the old order (all parent members before all imports). These need to be rewritten to walk one module level at a time, checking members then imports at each level. The `import_scope_lookup_until` and `import_scope_lookup_kind_until` helpers exist for searching imports within a single level (using the start/stop ImportScope pointer trick), and `import_scope_find_ref_until` exists for getting the ImportRef for on-demand context. The `ModuleScopeChain` already stores `import_scope` at each level.

**The deeper issue:** EXPR_IDENT resolution bypasses `resolve_symbol` entirely. It first calls `scope_lookup_capture`, which walks the entire scope chain including parent module scopes. Parent module `let` bindings are added to the scope chain during `check_decl_let`, so they're found via scope before the import check ever runs. This means parent members still beat child imports for bare identifier resolution even if `resolve_symbol` is fixed.

**The fix has two parts:**

1. **`resolve_symbol` / `resolve_symbol_kind`**: Rewrite to walk one module level at a time: current members → current imports → parent members → parent imports → ... → global. The helpers are already in place.

2. **`scope_lookup_capture`**: Must stop at the current module boundary (the nearest `is_global` scope) instead of walking into parent module scopes. The EXPR_IDENT interleaved loop then handles parent members and imports in the correct order. The complication: `scope_lookup_capture` also drives closure capture analysis (counting `is_lambda_boundary` crossings), so the change must not break capture semantics — local `let` bindings across lambda boundaries still need to be detected as captures.

3. **EXPR_IDENT**: The parent_chain and import_chain blocks need to be merged into a single interleaved loop. The on-demand type checking logic differs per source (parent members need parent context, import symbols need source module context), so the loop needs a shared on-demand handler parameterized by context.

### Eliminate the `base_name` field on Type

`base_name` exists on TYPE_STRUCT and TYPE_UNION for display purposes (showing source names in error messages). But `qualified_name` serves the same display role. If the display logic is unified to always derive the display name from `qualified_name` or the canonical name, `base_name` becomes dead weight. Removing it would simplify the Type struct and eliminate one of the lingering sources of "which name do I use?" confusion in the codebase.

### Investigate global registration of module-scoped types with ns_prefix=NULL

Module-scoped types are registered in the global symtab under mangled names (e.g., `std__data__array_list`) with `ns_prefix = NULL`. This means they bypass namespace isolation — they're accessible from any namespace context if you know the mangled name. In practice nobody writes mangled names in source code, but it means the global symtab is a flat namespace of mangled names that sidesteps the namespace rules. This was a pragmatic choice to make `resolve_type` work across namespaces, but it warrants investigation: can these entries carry their proper `ns_prefix` without breaking cross-namespace type resolution? Or is there a cleaner way to make module-scoped types findable without polluting the global namespace?

### Fix parser creating all type references as TYPE_STRUCT stubs

The parser creates all type references as TYPE_STRUCT stubs regardless of whether the target is a struct or union, because the parser doesn't know the target kind at parse time. This causes downstream complexity: `canonicalize_stub_names` must try both DECL_STRUCT and DECL_UNION, `resolve_type` must search for both kinds, and `unify` needs a struct/union kind mismatch handler. Most languages either resolve types to their kind during parsing (if there's a type namespace) or use a kind-agnostic "type reference" node. FC should adopt the latter — a TYPE_REF or TYPE_STUB kind that explicitly represents "unresolved type reference" without implying struct. This would make the stub → resolved type transition explicit and remove the need for kind-guessing fallbacks.

### Constraint syntax for generics (future)

The obvious next feature request. The unconstrained model is fully forward-compatible — constraints would add pre-checks that reject invalid instantiations earlier (at the definition site rather than the call site). The monomorphization infrastructure already supports this; it's a pass2 addition, not an architectural change.

## Design decisions confirmed

### No re-export pattern

`import * from M` exports only M's own declarations — never re-exports. This is the correct default and we don't plan to add explicit re-export syntax. If a consumer needs child module names, they can import from the child directly. The "bring everything in" pattern creates fragile dependencies and makes it unclear where a name originates.

### Companion pattern as convention, not syntax

The companion pattern (struct/union + same-named module) is a naming convention, not special syntax like Rust's `impl` blocks. This is one less concept to learn and keeps the module system orthogonal. The companion fallthrough for union variants was added to make the pattern work seamlessly without requiring special syntax.
