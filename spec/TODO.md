# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Escape analysis — interprocedural gap

The intraprocedural escape analysis now catches: stack-ptr in struct/union construction, field/index access into stack-provenance aggregates, global assignment of stack-provenance values, and heap-struct field assignment (`h->f = &stack`). What remains is **interprocedural**: if a function receives a struct-with-pointer by value, the parameter is `PROV_UNKNOWN` inside the callee, so writing it to a global / heap field there isn't caught at the call site.

Example that still slips through:
```fc
let stash = (h: holder) -> gh = h   // h is PROV_UNKNOWN inside stash
let leak = () -> stash(holder { p = &local })
```

Fix direction: either (a) tag function parameters as "tainted by stack provenance" based on a summary pass, or (b) forbid passing provenance-carrying struct values by value to non-local functions unless the callee proves it doesn't escape them. (a) is closer to what Rust/C++ do; (b) is more conservative but simpler.

## `&fn` does not emit a raw C function pointer (spec/codegen mismatch)

**Spec claim** (`fc-spec.html` §Address-of and Transpilation notes): `&f` on a non-capturing function binding "extracts the raw `fn_ptr` field and produces a plain C-compatible function pointer", intended for storing into a C struct field — the canonical example given is an SDL audio callback.

**Actual codegen**: `&fn` is emitted as `&(fc_fn_..._..){.fn_ptr = fn, .ctx = NULL}` — the address of a compound literal of the fat-pointer struct, not the raw function pointer. Storing that into `SDL_AudioSpec.callback` hands SDL a pointer to a two-word struct; SDL then calls it as if it were a function and invokes whatever bytes happen to live at that address. The trampoline mechanism (`_ctramp_*`) only fires in `emit_extern_arg` when a function is passed as an argument to an extern call where the param type is `TYPE_FUNC` (see `codegen.c` `collect_trampolines_expr`) — struct field assignments never trigger it.

Observed in wolf-fc when trying to move audio off the render thread via `SDL_OpenAudioDevice`'s callback API. Workaround used: switch to `SDL_CreateThread` + `SDL_QueueAudio`, because a thread entry function *is* a function-typed extern-call argument, so the trampoline mechanism does the right thing there. The callback-mode path in SDL remains unreachable from FC today.

Fix directions (ordered roughly easiest → most principled):
1. **Extend trampoline collection** to also fire on `&fn` (EXPR_UNARY_PREFIX with TOK_AMP, operand is a top-level non-capturing function). Emit `_ctramp_<name>` instead of `&(fat-struct){...}`. This matches the spec's promise for the documented use case without broader ABI changes.
2. **Detect struct field assignment** where the LHS field has a C-function-pointer type (e.g. via a new annotation in extern struct fields, or by inferring from the field's declared type) and trigger trampoline emission there too. This is strictly more general than (1).
3. **Revise the spec** to say `&fn` is a fat-pointer handle, and require a different syntactic form for raw C function pointers (e.g. `extern as fn_ptr(fn)` or a magic cast). Only worth doing if (1)/(2) turn out to be infeasible.

Either way, worth a regression test that asserts `&my_fn` assigned to an `any*` (or function-pointer-typed) field produces something callable from C with the expected signature.
