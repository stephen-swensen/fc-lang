# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Packed structs and bit-level layout control**: Support `packed struct` for exact bit layout without padding, and potentially bit fields. Currently FC emits C structs with C's default layout rules, offering no control over padding or alignment beyond what the C compiler provides. Packed structs would enable memory-mapped I/O, binary protocol parsing, and compact serialization formats.

- **SIMD / vector types (evaluate)**: Investigate first-class vector types for SIMD operations (e.g., `float32x4`, `int32x8`). Would require language-level support — the types (`__m128`, `__m256`) aren't C structs and can't be represented through extern, and wrapping intrinsics in function calls defeats inlining. The natural fit is emitting GCC/Clang `__attribute__((vector_size(N)))` typedefs, which get arithmetic operators for free in C. **Use cases**: image/audio processing, physics simulations, batch data transforms, cryptography, neural net inference — anywhere you're doing the same math on many values at once. **Counterpoint**: GCC/Clang already auto-vectorize many loops, so FC programs may get SIMD "for free" through the C compiler without any language changes. Explicit vector types only matter when auto-vectorization fails or you need guaranteed vectorization. Low priority unless FC targets performance-critical numeric workloads.


