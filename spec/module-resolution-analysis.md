# Module Resolution Architecture Analysis

## Problem Statement

The compiler had a recurring pattern of bugs related to module nesting and symbol resolution. Over 8+ commits, fixes followed the same template: a new scoping feature or nesting level works in pass2 but breaks in monomorph or codegen because they maintained independent copies of the lookup logic.

## Commit History

| Commit | What broke | Phase with bug |
|--------|-----------|----------------|
| `9dde48b` | Private module scoping; forward function references | pass2 (lookup order) |
| `055a9f7` | Extern struct fields referencing sibling structs | pass1 (duplicated resolution loops) |
| `88c53cd` | 5 separate bugs with generic structs/functions in modules | pass1 + pass2 + monomorph |
| `992565e` | Module-scoped generic structs used only as field types | monomorph |
| `24f7d3b` | Ancestor module chain for companion struct pattern | pass2 (12+ sites) |
| `2717167` | Cross-namespace type visibility leak | pass2 (10+ sites) |
| `14f64ee` | Cross-namespace module lookup fallback leak | pass2 (6 sites) |
| `40e3822` | Explicit `global::` namespace allowed | pass1 + pass2 |
| `b1c4a19` | Codegen couldn't resolve nested module callees for deferred generic calls | codegen |

## Root Cause: Symbol Resolution Was Not a Single Abstraction

The lookup priority chain (module_symtab -> parent_chain -> imports -> global symtab) was hand-written at every use site. There were three independent implementations: ~12 inline chains in pass2, a fallback chain in monomorph's `discover_in_expr`, and a near-identical fallback in codegen's deferred generic call path. When a new scope level was added, every site had to be patched independently.

## Remediation — Completed

### 1. Nesting-depth test coverage

Added 6 tests in `tests/cases/generics/` exercising generic-calling-generic at depths 2-3, companion structs, cross-sibling calls, multi-level qualified calls, and namespaced generic cross-references.

### 2. Consolidated lookup functions

Added `resolve_symbol(ctx, name)` and `resolve_symbol_kind(ctx, name, kind)` in pass2.c, encoding the standard 4-step priority chain in one place. Replaced 10 hand-written inline chains including `lookup_module`, `find_callee_symbol`, `check_decl_let`, struct literal resolution, field access, union variant resolution, dotted name resolution, and `resolve_type`.

Additionally fixed `find_callee_symbol` to handle multi-level EXPR_FIELD chains (e.g., `outer.inner.func()`), resolving a multi-level qualified generic call bug.

### 3. Unified type resolution

Simplified `resolve_type` from a 40-line interleaved struct/union cascade with a `base_name` fallback to a 6-line sequence using `resolve_symbol_kind`. Types now follow the same scoping rules as everything else — they're just names.

To make this work, pass1 now registers module-scoped types in the module's members table under both source name ("inner") and mangled name ("m__inner"), so canonicalized type stubs resolve directly in module_symtab without hitting namespace-filter rejection in the global path.

### 4. Eliminated re-resolution in later phases

Removed all fallback symtab re-resolution from monomorph (`discover_in_expr` for both EXPR_CALL and EXPR_STRUCT_LIT) and codegen (deferred generic call path). Both now rely solely on `resolved_callee`/`resolved_sym` set by pass2. Pass2 is the single source of truth for name resolution of calls and struct literals.

## Remaining Architectural Debt

- **EXPR_IDENT resolution** (pass2.c): Cannot be consolidated because each scope source has unique on-demand type-checking behavior (cycle detection, context save/restore) entangled with the lookup. A future refactor could separate "find symbol + report source" from "do source-specific post-processing."

- **`mono_resolve_type_names`** (monomorph.c): Still does its own symtab lookups for type name canonicalization. This operates on `Type` objects (not expressions) which don't carry `resolved_sym` pointers. Fixing this would require adding a `resolved_sym` field to the `Type` struct.

## Related Bugs Found and Fixed

### Generic unions with generic struct/union payloads (3 bugs)

Discovered while expanding test coverage for namespaced generic cross-references. Three independent bugs combined to make generic unions with generic payloads (e.g., `union wrapped = | val(inner<'a>) | empty`) produce incorrect C.

**Bug 1 — Topo sort missed union variant payloads (monomorph.c).** `topo_visit` only walked struct fields for by-value dependencies, not union variant payloads. A union `wrapped<int32>` with payload `inner<int32>` was emitted before `inner<int32>` was defined. Fix: extended `find_by_value_dep` to handle TYPE_UNION and `topo_visit` to walk variant payloads.

**Bug 2 — Pattern match used unsubstituted payload type (codegen.c).** `emit_pat_bindings` for PAT_VARIANT pulled the raw template payload type (e.g., `inner<'a>`) from the union definition instead of the monomorphized concrete type. Fix: look up the mono table's `concrete_type` for the union and get variant payloads from there. Also apply `g_subst` when inside a monomorphized generic context.

**Bug 3 — Stub canonicalization missed union targets (pass1.c).** `canonicalize_stub_names` only looked for DECL_STRUCT when canonicalizing TYPE_STRUCT stubs, but the parser creates ALL type references as TYPE_STRUCT stubs (it doesn't know the target kind yet). A reference to `wrapped<'a>` (a union) in a variant payload wasn't canonicalized to the mangled name, causing incorrect type names in the generated C. Fix: also try DECL_UNION when canonicalizing TYPE_STRUCT stubs.

### Known remaining bugs

**Generic struct with union-typed field** — `struct holder = item: wrapped<'a>` followed by `holder { item = wrapped<'a>.val(...) }` produces "field 'item': type mismatch in generic struct" in pass2. This is a type inference bug where pass2 can't unify a generic union value with a generic struct field of union type. Reproduces without modules/namespaces.

**No-payload variant construction for generic unions** — `m.wrapped<int32>.empty` (constructing a no-payload variant of a module-scoped generic union) emits the unmonomorphized type name. A codegen issue with how no-payload variant construction handles generic type arguments for module-scoped unions.
