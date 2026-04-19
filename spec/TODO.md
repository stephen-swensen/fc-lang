# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Escape analysis soundness holes

Current checks only examine the top-level expression's provenance; they don't recurse into struct/slice field contents. Each of the following currently compiles cleanly but creates a dangling pointer at runtime:

1. **Return a stack struct literal with a stack-pointer field** — `return holder { p = &stack_local }` passes.
2. **Assign a stack-struct-with-pointer to a global** — GCC's `-Wdangling-pointer` catches the obvious case but FC doesn't, and indirect/branching assignments slip past both.
3. **Field access provenance not propagated** — `s.ptr_field` and `s->ptr_field` don't inherit `s`'s provenance; returning them bypasses the stack-ptr return check.
4. **`&(heap_struct.field)` taking the address of a heap-struct field that holds a stack pointer** — not validated against the field's own provenance.

Fix direction: when a struct is constructed or returned, walk its field types and check the provenance of any pointer/slice fields; propagate provenance through field-access expressions.
