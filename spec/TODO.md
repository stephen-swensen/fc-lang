# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **`-Walloc-size-larger-than=` warning at `monomorph.c:532`** (visible under `-O2 -flto`): `calloc((size_t)t->count, sizeof(int))` — GCC's LTO-enabled range analysis can't prove `t->count >= 0`, and a negative `int` cast to `size_t` becomes a high-range value that trips the heuristic. Not a real bug (count is always non-negative in a well-formed program), but the fix is a one-liner: either change `count` to `size_t`/`uint32_t`, or insert `assert(t->count >= 0)` before the call so GCC picks up the invariant. Surfaced during wolf-fc's switch to building its local FC compiler copy with `-O2 -flto`.


