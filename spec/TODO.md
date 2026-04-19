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
