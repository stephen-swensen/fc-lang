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

### 1. Nesting-depth test coverage (short-term)

Add tests that exercise generic-calling-generic at multiple nesting depths (1, 2, 3 levels of module wrapping). This catches the class of bug where pass2 resolves correctly but monomorph/codegen re-resolution fails at deeper nesting.

### 2. Consolidated lookup function (medium-term)

Replace the ad-hoc lookup sequences with a single `resolve_symbol(ctx, name, kind)` function encoding the full priority chain. Every lookup site in pass2 calls this one function. When a new scope level is added, one function changes instead of 12.

### 3. Eliminate re-resolution in later phases (longer-term)

Extend the `resolved_callee` pattern so that every call/struct-lit/type-ref that pass2 resolves carries its resolution forward. The fallback lookups in monomorph and codegen become dead code. Pass2 is the single source of truth for name resolution.
