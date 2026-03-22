# Draft: String Interpolation & Simplified Text Output

This document proposes changes to the FC spec. It adds string interpolation to Part 1 (§Literals) and replaces Part 6's §Formatted Output with a simplified text output design.

---

## Part 1 Addition: String Interpolation

*(New subsection after §Escape Sequences, before §let and let mut)*

### String Interpolation

Any string literal may contain **interpolation segments** — format specifier / expression pairs that are evaluated at runtime and spliced into the string. A string literal with no interpolation segments remains a static, read-only `str` as before.

An interpolation segment has the form `%spec{expr}`, where `spec` is a printf-style format specifier and `expr` is an arbitrary FC expression:

```fc
let name = "world"
let age = 30
let greeting = "hello %s{name}, you are %d{age} years old"
// greeting is a str with value "hello world, you are 30 years old"
```

The compiler validates that each format specifier is compatible with the expression's type — a mismatch is a compile error:

```fc
let x = 42
let s = "value: %s{x}"    // compile error — %s expects str or cstr, got int32
let s = "value: %d{x}"    // ok
```

#### Static vs. interpolated strings

The compiler distinguishes two cases at parse time based on the presence of `%spec{` patterns:

- **No interpolation segments**: the literal is a static `str` — a fat pointer into read-only memory, zero runtime cost. This is the existing behavior for all string literals.
- **One or more interpolation segments**: the literal is an interpolated `str` — the compiler allocates a stack buffer, formats the string at runtime, and produces a `str` pointing to that buffer.

```fc
"hello world"                          // static — read-only memory
"hello %s{name}, age %d{age}"          // interpolated — stack buffer
"100% of users"                        // static — % not followed by spec{
"use %d format"                        // static — %d not followed by {
```

Both forms produce a value of type `str`. Code that consumes a `str` does not need to know which form produced it.

#### Format specifiers

The following format specifiers are valid in interpolation segments:

| Specifier | Valid types | Notes |
|-----------|------------|-------|
| `%d`, `%i` | all integer types | decimal |
| `%u` | all integer types | unsigned decimal |
| `%x`, `%X` | all integer types | hexadecimal (lower/upper) |
| `%o` | all integer types | octal |
| `%f` | `float32`, `float64` | fixed-point — **width required** |
| `%e`, `%E` | `float32`, `float64` | scientific notation |
| `%g`, `%G` | `float32`, `float64` | shortest representation |
| `%s` | `str`, `cstr` | string |
| `%c` | `char` | single character |
| `%p` | any pointer type | pointer address |
| `%%` | — | literal `%`, no expression |

Width, precision, and flag modifiers are allowed and passed through to the underlying C formatter. Examples: `%04d{x}`, `%+d{x}`, `%20s{name}`, `%.2f{pi}`, `%#x{flags}`.

#### Allocation and bounded output

An interpolated string is stack-allocated. The compiler computes a buffer size that is guaranteed to be sufficient — the formatted output will never exceed the buffer. The `str` result's `.len` reflects the actual number of bytes written, which may be less than the buffer size.

The buffer size for each segment is determined as follows:

**Literal text segments** — exact byte count, known at compile time.

**Integer segments** (`%d`, `%x`, `%o`, etc.) — bounded at compile time by the type's maximum formatted width. No explicit width is required — the compiler knows the bound:

| Type | `%d` / `%u` max | `%x` max | `%o` max |
|------|-----------------|----------|----------|
| `int8` | 4 | 2 | 3 |
| `int16` | 6 | 4 | 6 |
| `int32` | 11 | 8 | 11 |
| `int64` | 20 | 16 | 22 |
| `uint8` | 3 | 2 | 3 |
| `uint16` | 5 | 4 | 6 |
| `uint32` | 10 | 8 | 11 |
| `uint64` | 20 | 16 | 22 |

If a width modifier is specified (e.g., `%20d{x}`), the bound is the larger of the width and the type maximum. Flag modifiers (`+`, `#`, etc.) are accounted for by the compiler.

**String segments** (`%s`) — bounded at runtime by the expression's length:

- `%s{expr}` where `expr` is `str`: bound is `expr.len`
- `%s{expr}` where `expr` is `cstr`: bound is `strlen(expr)`, computed once before formatting

If a width modifier is specified (e.g., `%20s{name}`), the bound is the larger of the width and the string's length.

**Float segments** (`%f`) — **explicit width required**. The `%f` format can produce arbitrarily long output for large values (e.g., `%f` of `1e308` is 309+ characters). The programmer must specify a width, which becomes the maximum allocation for that segment:

```fc
let s = "weight: %8.2f{w} kg"     // ok — allocates 8 chars for the float
let s = "weight: %f{w} kg"         // compile error — %f requires explicit width
```

If the formatted float exceeds the specified width, the output is truncated to fit. The programmer chooses the width with this understanding.

**Float segments** (`%e`, `%E`, `%g`, `%G`) — bounded at compile time. Scientific notation and shortest-representation formats have a type-determined maximum width (approximately 24 characters for `float64`, 15 for `float32`). No explicit width is required, though one may be specified to request wider or narrower output.

**Char segments** (`%c`) — always 1 byte.

**Pointer segments** (`%p`) — bounded at compile time by the platform pointer width (typically 18 characters for 64-bit: `0x` prefix + 16 hex digits).

**`%%`** — always 1 byte (literal `%`).

The total buffer size is the sum of all segment bounds. For purely compile-time-bounded strings (no `%s` segments), the buffer is a fixed-size stack array. For strings with runtime-bounded segments, the buffer is allocated with `alloca`.

#### Escape rules

`%%` produces a single literal `%` character, consistent with C's printf convention. This is the only escape needed — bare `{` and `}` characters are not special and do not require escaping, since interpolation is only triggered by the specific `%spec{` pattern.

```fc
"100%%"                    // "100%"
"100%% complete"           // "100% complete"
"braces { } are literal"   // "braces { } are literal"
```

#### Expressions in interpolation segments

The expression inside `{ }` is an arbitrary FC expression. The compiler tracks brace depth to find the closing `}`:

```fc
"sum: %d{a + b}"                     // arithmetic
"name: %s{user.name}"                // field access
"first: %d{items[0]}"               // indexing
"result: %s{if x > 0 then "positive" else "negative"}"  // if expression
```

Nested string literals inside interpolation segments may themselves contain interpolation — the compiler handles this by tracking string delimiter and brace nesting depth during lexing.

#### Lifetime and return safety

String literals in FC fall into two categories with distinct lifetime properties:

**Static strings** — literals with no interpolation segments. The `str`'s `.ptr` points to read-only memory in the binary's data section. This memory lives for the entire program duration. Static strings may be freely returned from functions, stored in heap structures, or passed anywhere — they never become dangling.

```fc
let greet = () -> "hello world"     // ok — static memory, lives forever
```

**Interpolated strings** — literals with one or more `%spec{expr}` segments. The `str`'s `.ptr` points to a stack buffer in the enclosing function's frame. This memory is reclaimed when the function returns. Returning an interpolated string is a **compile error** — the pointer would be dangling.

```fc
let greet = (name: str) -> "hello %s{name}"   // compile error — returning stack-allocated str
```

The compiler tracks whether a `str` value originates from a static literal or an interpolated literal. This is a compile-time property, not a runtime tag — both produce the same `str` type. The restriction applies transitively: if a `let` binding holds an interpolated string, returning that binding is also a compile error.

```fc
let format_name = (name: str) ->
    let msg = "hello %s{name}"     // interpolated — stack buffer
    print(msg)                      // ok — consumed within same function
    msg                             // compile error — returning stack str

let get_label = () ->
    let label = "status"            // static — read-only memory
    label                           // ok — static str, safe to return
```

This same distinction applies to all stack-allocated slices — returning `int32[3] { 1, 2, 3 }` from a function is also a compile error. String interpolation does not introduce a new safety concern; it extends the existing stack-slice rule to a more commonly encountered case.

#### Heap-allocated strings

For strings that must outlive the creating function, use `alloc` to heap-allocate. The `alloc` operator accepts string literals (both static and interpolated) as an initialized form, consistent with its existing support for initialized structs and arrays:

```fc
alloc(point { x = 2 })!                   // heap struct with values
alloc(int32[4] { 1, 2, 3, 4 })!           // heap array with values
alloc("hello %s{name}")!                   // heap string — natural extension
```

`alloc` applied to a string literal returns `str?`. On success, the returned `str`'s `.ptr` points to heap memory that the caller is responsible for freeing:

```fc
let make_greeting = (name: str) -> str
    let msg = alloc("hello %s{name}")!    // heap-allocated, safe to return
    msg                                    // ok — heap memory outlives function

// caller is responsible for freeing
let g = make_greeting("world")
// ... use g ...
free(g)
```

For interpolated strings, the compiler computes the bound, allocates a heap buffer of that size directly with `malloc`, and formats into it — no intermediate stack buffer or copy is needed. For static string literals, `alloc("hello")` allocates a heap buffer and copies the static data into it, producing a mutable, freeable copy.

This replaces any need for manual buffer allocation and copy loops:

```fc
// unnecessary — alloc handles this directly
let msg = "hello %s{name}"
let heap_msg = alloc(uint8[msg.len])!
for i in 0..msg.len
    heap_msg[i] = msg.ptr[i]

// use alloc instead
let heap_msg = alloc("hello %s{name}")!
```

> **Transpilation**
>
> A static string literal (no interpolation) transpiles as before — a `str` fat pointer to static read-only data:
>
> ```c
> (fc_str){ .ptr = (uint8_t*)"hello", .len = 5 }
> ```
>
> An interpolated string transpiles to an `alloca`-backed buffer formatted with `snprintf`:
>
> ```c
> // "hello %s{name}, age %d{age}"
> ({
>     int64_t _flen = 7 + _l_name.len + 6 + 11;  // literal + str + literal + max_int32
>     uint8_t *_fbuf = alloca((size_t)(_flen + 1));
>     int _fw = snprintf((char*)_fbuf, (size_t)(_flen + 1),
>         "hello %.*s, age %d",
>         (int)_l_name.len, _l_name.ptr, _l_age);
>     (fc_str){ .ptr = _fbuf, .len = _fw >= 0 && _fw <= _flen ? _fw : _flen };
> })
> ```
>
> The `%s` specifier for `str` arguments is rewritten to `%.*s` with the length and pointer auto-inserted, matching the transparent `str` handling convention. The buffer includes space for a null terminator (for `snprintf`), but the `str`'s `.len` does not include it.
>
> When all segments have compile-time bounds (no `%s` segments), the compiler may emit a fixed-size array instead of `alloca`:
>
> ```c
> // "value: %d{x}, flag: %x{f}"
> ({
>     uint8_t _fbuf[7 + 11 + 8 + 8 + 1];  // fixed size, no alloca needed
>     int _fw = snprintf((char*)_fbuf, sizeof(_fbuf), "value: %d, flag: %x", _l_x, _l_f);
>     (fc_str){ .ptr = _fbuf, .len = _fw >= 0 && _fw < (int)sizeof(_fbuf) ? _fw : (int)sizeof(_fbuf) - 1 };
> })
> ```
>
> **Platform notes:** `alloca` is non-standard C but is universally available on hosted platforms (`<alloca.h>` on POSIX, `_alloca` on MSVC). On bare-metal targets where `alloca` is unavailable, the compiler restricts interpolation to compile-time-bounded segments only (no `%s` with `str` expressions) — all other interpolation forms use fixed-size arrays and work without `alloca`.

---

## Part 6 Replacement: Text Output

*(Replaces §Formatted Output entirely)*

### Text Output

Three built-in operators write strings to output destinations. These are compile-time operators — like `alloc` and `sizeof`, they are resolved by the compiler, cannot be stored in bindings, and cannot be passed as arguments.

| Operator | Signature | Destination |
|----------|-----------|-------------|
| `print(expr)` | `(str) -> void` | stdout |
| `eprint(expr)` | `(str) -> void` | stderr |
| `fprint(f, expr)` | `(any*, str) -> void` | file handle `f` |

Each operator takes a single `str` argument and writes its contents to the destination. Combined with string interpolation, this replaces the need for printf-style variadic format strings:

```fc
let name = "world"
let age = 30

// simple output
print("hello\n")

// interpolated output
print("hello %s{name}, you are %d{age} years old\n")

// error output
eprint("error: %s{msg}\n")

// file output
import io from std::
let f = io.open("log.txt", "w")!
fprint(f, "event %d{id}: %s{description}\n")
io.close(f)
```

When the argument is a static string literal (no interpolation), the compiler emits a direct write from read-only memory — no buffer allocation occurs. When the argument is an interpolated string, the buffer is allocated and formatted first, then written.

#### Replacing sprint

The `sprint` operator from earlier versions of the spec is replaced by string interpolation. Where `sprint` required a caller-provided buffer:

```fc
// old design (removed)
let mut buf = uint8[256] { }
let msg = sprint(buf, "x = %d, y = %d", x, y)
```

String interpolation handles buffer allocation automatically:

```fc
// new design
let msg = "x = %d{x}, y = %d{y}"
```

The compiler allocates the buffer, formats the string, and produces a `str`. No manual buffer management required.

> **Transpilation**
>
> ```c
> // print("hello\n") — static string, direct write
> fwrite("hello\n", 1, 6, stdout);
>
> // print("hello %s{name}\n") — interpolated string
> ({
>     int64_t _flen = 6 + _l_name.len + 1;
>     uint8_t *_fbuf = alloca((size_t)(_flen + 1));
>     int _fw = snprintf((char*)_fbuf, (size_t)(_flen + 1),
>         "hello %.*s\n", (int)_l_name.len, _l_name.ptr);
>     int64_t _slen = _fw >= 0 && _fw <= _flen ? _fw : _flen;
>     fwrite(_fbuf, 1, (size_t)_slen, stdout);
> })
>
> // eprint("error: %s{msg}\n")
> // same pattern, but writes to stderr
>
> // fprint(f, "event %d{id}\n")
> // same pattern, but writes to (FILE*)f
> ```
>
> When `print` receives a non-literal `str` expression (e.g., a binding or function return value), the compiler emits a simple `fwrite`:
>
> ```c
> // let msg = get_message()
> // print(msg)
> fwrite(_l_msg.ptr, 1, (size_t)_l_msg.len, stdout);
> ```

---

## Summary of Spec Changes

| Section | Change |
|---------|--------|
| §Literals | Add "String Interpolation" subsection after §Escape Sequences |
| §Formatted Output (Part 6) | Replace entirely with simplified "Text Output" section |
| `sprint` | Removed — replaced by string interpolation |
| `print`, `eprint`, `fprint` | Simplified from variadic `(fmt, args...)` to single-argument `(str)` |
| Reserved identifiers | Remove `sprint` from reserved list |
| §Memory Management | `alloc` gains string literal form: `alloc("hello %s{name}")` returns `str?` |
| §Slices & Strings | Add note: static string literals are safe to return; interpolated strings follow stack-slice rules |
| Grammar (BNF) | String literal production gains interpolation segment alternative |

## Design Rationale

1. **No new string prefix.** All string literals support interpolation — the compiler distinguishes static from interpolated strings by the presence of `%spec{` patterns. This avoids "two kinds of strings" and means existing code is unchanged.

2. **Format specifiers required.** Every interpolation segment explicitly names its format (`%d`, `%s`, `%x`, etc.). This is consistent with FC's philosophy of explicitness and makes the string self-documenting — a reader can see exactly how each value will be formatted without looking up its type.

3. **Bounded allocation.** The compiler guarantees that the stack buffer is always sufficient. Integer, char, pointer, and scientific-notation float formats are bounded by type. Fixed-point floats (`%f`) require an explicit width. String formats use the runtime `.len`. No dynamic growth, no heap allocation, no two-pass formatting.

4. **Simplified output operators.** With interpolation in the string itself, `print`/`eprint`/`fprint` become single-argument operators. No variadic arguments, no format string parsing at the call site, no `sprint`. The interpolation and the output are cleanly separated concerns.

5. **Static vs. interpolated safety.** The compiler distinguishes static string literals (read-only memory, safe to return) from interpolated strings (stack buffer, must not be returned). This is a compile-time check, not a runtime tag, and extends the existing stack-slice return restriction. The `alloc("...")` form provides a clean escape hatch for strings that must outlive the creating function.

6. **Platform compatibility.** Compile-time-bounded interpolation (integers, floats, chars, pointers) works on all platforms including bare metal. Runtime-bounded interpolation (`%s` with `str`) requires `alloca`, which is available on all hosted platforms. Bare-metal targets restrict to compile-time-bounded forms only.
