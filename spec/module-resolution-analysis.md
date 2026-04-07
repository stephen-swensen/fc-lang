# Module Resolution Architecture Analysis

## Problem Statement

The compiler has a recurring pattern of bugs related to module nesting and symbol resolution. Over 8+ commits, fixes follow the same template: a new scoping feature or nesting level works in pass2 but breaks in monomorph or codegen because they maintain independent copies of the lookup logic.

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
| `b1c4a19`* | Codegen couldn't resolve nested module callees for deferred generic calls | codegen |

*The codegen fix at `b1c4a19` was adding `resolved_callee` support to codegen's deferred generic call resolution, which monomorph already had since `88c53cd`.

## Root Cause: Symbol Resolution Is Not a Single Abstraction

The lookup priority chain (module_symtab -> parent_chain -> imports -> global symtab) is hand-written at every use site. There are at least three independent implementations:

### 1. pass2 (~12+ lookup sites)

Each of these has its own version of the priority chain:
- `find_callee_symbol()` — resolves call targets
- `lookup_module()` — finds module symbols
- `resolve_type()` — resolves type names
- EXPR_IDENT resolution
- EXPR_FIELD resolution (module member access)
- EXPR_STRUCT_LIT resolution
- EXPR_CALL union variant construction
- `check_decl_let` for forward references
- Several others

### 2. monomorph.c (discover_in_expr, ~lines 130-177)

Re-resolves callees via global symtab when walking generic function bodies for transitive instantiation discovery. Uses `resolved_callee` from pass2 as a fast path (added in `88c53cd`).

### 3. codegen.c (deferred generic call resolution, ~lines 1686-1710)

Nearly identical copy of the monomorph lookup: check `resolved_callee` -> fallback to EXPR_IDENT symtab lookup -> fallback to EXPR_FIELD module-walk -> extract base_name -> call `mono_register`. This copy was missing `resolved_callee` support until the fix that prompted this analysis.

## Why Each Fix Creates the Next Bug

Each new scoping feature requires patching every lookup site independently:

- `24f7d3b` added `parent_chain_lookup` — had to patch 12+ call sites in pass2
- `2717167` added namespace filtering — had to wrap 10+ global lookups
- Neither commit touched monomorph or codegen, leaving them with stale lookup logic

The `resolved_callee`/`resolved_sym` fields (added in `88c53cd`) bridge pass2's correct resolution to later phases, but only for call expressions and struct literals. Type resolution in monomorph (`mono_resolve_type_names`) still does its own symtab lookups.

## Remediation Plan

### 1. Nesting-depth test coverage (short-term) -- DONE

Added 5 tests in `tests/cases/generics/` exercising generic-calling-generic at depths 2-3, companion structs, cross-sibling calls, and multi-level qualified calls.

### 2. Consolidated lookup function (medium-term) -- DONE

Added `resolve_symbol(ctx, name)` and `resolve_symbol_kind(ctx, name, kind)` in pass2.c. Replaced 10 hand-written inline chains with calls to these functions. Future scope changes now require patching 2 functions instead of 10+ call sites.

Additionally fixed `find_callee_symbol` to handle multi-level EXPR_FIELD chains (e.g., `outer.inner.func()`), resolving the multi-level qualified generic call bug.

### 3. Eliminate re-resolution in later phases -- PARTIALLY DONE

Removed the fallback symtab lookups from `discover_in_expr` (monomorph.c) and the deferred generic call path (codegen.c). Both now rely solely on `resolved_callee` from pass2. The `EXPR_STRUCT_LIT` fallback in monomorph and type canonicalization in `mono_resolve_type_names` still use direct symtab lookups — these would require adding a `resolved_sym` field to `Type` itself, which is a separate effort.

### Remaining architectural debt

- **EXPR_IDENT resolution** (pass2.c): The biggest lookup site cannot be consolidated because each scope source (module_symtab, parent_chain, import_chain, global) has unique on-demand type-checking behavior entangled with the lookup. A future refactor could separate "find symbol + report source" from "do source-specific post-processing."
- **`resolve_type` interleaved cascade** (pass2.c): The struct/union priority is interleaved at each scope level. Only the final fallback was consolidated; the main cascade cannot be flattened without changing priority semantics.
- **`mono_resolve_type_names`** (monomorph.c): Still does its own symtab lookups for type name canonicalization. Fixing this would require adding a `resolved_sym` field to the `Type` struct.
