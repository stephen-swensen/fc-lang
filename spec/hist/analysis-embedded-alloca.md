# Analysis: alloca Usage and Embedded Targets

This document analyzes the compiler's use of `alloca` in generated C code, its implications for embedded platforms, and possible alternatives.

---

## Current alloca usage in codegen

The compiler emits `alloca` in three cases:

### 1. `str` to `cstr` cast

Copies slice data onto the stack and appends a null terminator. Size: `str.len + 1`. Unbounded at compile time.

```c
uint8_t *buf = (uint8_t*)alloca((size_t)(s.len + 1));
memcpy(buf, s.ptr, (size_t)s.len);
buf[s.len] = '\0';
```

### 2. String interpolation

Allocates the output buffer for `snprintf`. `alloca` is used instead of a VLA because the result lives inside a GCC statement expression (`({...})`), and VLA storage would be scoped to that inner block, while `alloca` survives until the function returns.

The buffer size is a sum of per-segment bounds. For numeric types (`%d`, `%x`, `%f`, etc.), bounds are compile-time constants derived from the type (e.g., `int32` → 11 bytes for `%d`). For `%s` with `str` or `cstr` arguments, the bound is runtime-dependent (`str.len` or `strlen()`).

**Precision-bounded `%s`**: When `%s` has printf-style precision (e.g., `%.256s{name}`), the precision is used as a compile-time bound instead of the runtime length. When all segments in an interpolation have compile-time bounds, the `alloca` argument becomes a constant expression. This enables the C compiler to hoist the allocation out of loops (turning it into a fixed stack slot in the function prologue) and allows static stack analysis tools to bound worst-case usage.

Truncation semantics follow printf: `snprintf` with `%.Ns` naturally truncates output to N bytes. For `str` arguments (which are not null-terminated), the emitted code passes `min(N, str.len)` as the `%.*s` precision argument to avoid reading past the slice data.

```c
// Without precision — runtime-dependent size:
int64_t _flen = _sa.len + 11;
uint8_t *buf = (uint8_t*)alloca((size_t)(_flen + 1));

// With precision (%.256s) — compile-time constant size:
int64_t _flen = 256 + 11;
uint8_t *buf = (uint8_t*)alloca((size_t)(_flen + 1));
```

### 3. `main` wrapper argv conversion

Converts C's `argc`/`argv` into an FC `str[]` slice. Size: `argc * sizeof(fc_str)`. Effectively bounded (argc is small in practice).

```c
fc_str *_args = (fc_str*)alloca((size_t)argc * sizeof(fc_str));
```

---

## Embedded platform survey

### ARM Cortex-M (STM32, nRF52, RP2040, SAMD, LPC)

The dominant embedded platform. Cortex-M0/M0+ for ultra-low-cost, M4/M7 for performance. Used in sensors, drones, medical devices, industrial controllers, wearables.

- **RAM**: 2KB (M0) to 1MB+ (M7). Typical: 64-256KB.
- **Stacks**: FreeRTOS/Zephyr tasks typically 512B-4KB. Bare-metal: one stack, sized by linker script (often 2-8KB).
- **Toolchains**: arm-none-eabi-gcc (dominant), armclang (LLVM-based), IAR. All support alloca.
- **Pros**: Massive ecosystem, cheap silicon, excellent tooling, huge community, every RTOS supports it. Widest vendor diversity (ST, Nordic, NXP, Microchip, TI, Infineon).
- **Cons**: Fragmented HALs (every vendor has their own). M0/M0+ is 32-bit but very constrained. Power consumption varies wildly by vendor.
- **FC relevance**: The primary target to care about. If FC works on Cortex-M4 with 64KB+ RAM, it covers the bulk of modern embedded.

### ESP32 (Espressif — ESP32, ESP32-S3, ESP32-C3, ESP32-C6)

WiFi/Bluetooth SoCs. The default choice for IoT prototyping and many production IoT products. ESP32-C3/C6 are RISC-V; the rest are Xtensa.

- **RAM**: 320-512KB internal, optional PSRAM (2-8MB).
- **Stacks**: FreeRTOS-based (mandatory). Default main task stack: 8KB. Typical task stacks: 2-8KB.
- **Toolchains**: GCC (xtensa-esp-elf-gcc or riscv32-esp-elf-gcc). LLVM support emerging. Alloca fully supported.
- **Pros**: Cheap ($1-3), integrated WiFi/BT, enormous Arduino/maker community, good documentation, very capable for the price. ESP-IDF is mature.
- **Cons**: Power-hungry compared to Cortex-M (WiFi radio). Xtensa is a niche ISA. Espressif is a single vendor (supply chain risk).
- **FC relevance**: Generous RAM makes stack-heavy patterns less dangerous. Good second target after Cortex-M.

### RISC-V (SiFive, GD32V, CH32V, BL602/BL616, K210)

Open ISA, growing fast. Used in everything from $0.10 microcontrollers (CH32V003) to Linux-capable SBCs.

- **RAM**: 2KB (CH32V003) to 8MB+ (K210). Most MCU-class: 16-64KB.
- **Stacks**: Same story as Cortex-M — depends on RTOS or bare-metal config.
- **Toolchains**: riscv32/64-unknown-elf-gcc, LLVM/Clang. Alloca fully supported.
- **Pros**: No licensing fees (unlike ARM), open ISA encourages innovation, rapidly growing ecosystem. China investing heavily (GigaDevice, WCH, Bouffalo Lab). Custom extensions possible.
- **Cons**: Ecosystem still maturing — debug tooling, RTOS support, and peripheral libraries lag behind ARM. Fragmented vendor-specific extensions. Less battle-tested silicon.
- **FC relevance**: Future-facing. Toolchain is GCC/LLVM so no technical barriers. RAM constraints on low-end parts (CH32V003 has 2KB) are extreme.

### TI MSP430 / CC13xx / CC26xx

MSP430: 16-bit ultra-low-power MCU, 20+ years old but still used. CC13xx/CC26xx: ARM Cortex-M based, sub-GHz and BLE radios. Used in metering, industrial sensors, building automation.

- **RAM**: MSP430: 0.5-8KB. CC26xx: 80KB.
- **Stacks**: MSP430 bare-metal: often 256B-1KB. CC26xx with TI-RTOS: 512B-2KB typical task stacks.
- **Toolchains**: MSP430: msp430-elf-gcc or TI's proprietary compiler. CC series: arm-none-eabi-gcc or TI armclang. MSP430 GCC supports alloca; TI proprietary compiler support is uncertain.
- **Pros**: Unmatched ultra-low-power on MSP430 (sub-uA sleep). CC series has excellent radio stacks. Strong in industrial/metering markets.
- **Cons**: Small community compared to ARM/ESP. Heavy tooling (Code Composer Studio). MSP430 is 16-bit — a dying form factor for new designs.
- **FC relevance**: CC series is just Cortex-M, so covered. MSP430 is 16-bit with tiny RAM — probably not a realistic FC target.

### Microchip AVR / PIC

AVR: 8-bit, powers Arduino Uno/Mega. PIC: 8/16/32-bit, huge in legacy industrial. PIC32 is MIPS-based.

- **RAM**: AVR: 2KB (ATmega328P) to 16KB (ATmega2560). PIC: 256B (PIC16) to 512KB (PIC32MZ).
- **Stacks**: AVR: typically 256B-1KB. PIC8/16: hardware call stack, 8-31 levels on PIC16. PIC32: normal C stack, 2-8KB typical.
- **Toolchains**: AVR: avr-gcc (alloca works). PIC32: XC32, GCC-based (alloca works). PIC8/16: XC8/XC16 (proprietary, no alloca, limited C standard support). XC8 doesn't even support recursion on some targets.
- **Pros**: AVR has the Arduino ecosystem (huge for education/prototyping). PIC has massive legacy install base in automotive, appliances, industrial.
- **Cons**: 8/16-bit is largely obsolescent for new designs — Cortex-M0+ competes at the same price with 32-bit. XC8/XC16 are poor C compilers.
- **FC relevance**: PIC32/XC32 works fine (GCC-based). AVR and PIC8/16 are too constrained for FC — not just alloca, but RAM, stack depth, and toolchain limitations.

### Renesas RA / RX / RL78

RA series is ARM Cortex-M based (the modern line). RX is Renesas's proprietary 32-bit ISA. RL78 is 16-bit. Strong in automotive and Japanese industrial markets.

- **RAM**: RA: 32KB-1MB. RX: 16KB-512KB. RL78: 2-64KB.
- **Toolchains**: RA: arm-none-eabi-gcc (alloca works). RX: GCC port exists but CC-RX (proprietary) is common — alloca support uncertain. RL78: mostly proprietary compiler.
- **Pros**: High-reliability silicon (automotive-grade). Competitive pricing. RA series is a solid Cortex-M offering.
- **Cons**: Smaller community outside Japan. Documentation quality varies. Proprietary ISAs (RX, RL78) have limited toolchain investment.
- **FC relevance**: RA series = Cortex-M, fully covered. RX/RL78 are niche.

---

## Summary

### alloca availability

The realistic embedded target set for FC is **Cortex-M, ESP32, and RISC-V** — all GCC/LLVM, all support alloca. This covers 90%+ of new embedded designs. The platforms where alloca is unavailable (XC8/XC16, some TI/Renesas proprietary compilers) are either legacy, 8/16-bit, or too RAM-constrained for FC anyway.

### The real problem: stack budget

`alloca` availability is not the issue. Stack budget is. Typical RTOS task stacks are 512B-4KB. The three codegen cases have these risk profiles:

| Case | Size bound | Risk |
|------|-----------|------|
| `str` to `cstr` cast | `str.len + 1` | Unbounded — large string on small stack = fault |
| String interpolation (bare `%s`) | Runtime-computed segment sum | Unbounded — format args control size |
| String interpolation (`%.Ns`) | Compile-time constant | Bounded — static analysis works |
| `main` argv wrapper | `argc * sizeof(fc_str)` | Very low — only fat pointer structs, not string data |

`alloca` cannot return NULL on failure. On embedded, a stack overflow from `alloca` causes silent corruption or a hard fault — not a recoverable error. Additionally, runtime-dependent `alloca` sizes make static worst-case stack analysis impossible, which is required for safety-critical RTOS task sizing.

---

## Alloca implications by context

### `str` to `cstr` cast

The `(cstr)s` cast copies `s.len + 1` bytes onto the stack. The size is entirely data-dependent and unbounded at compile time. This is the highest-risk alloca usage in the compiler.

Note that `str` and `cstr` **literals** do not involve alloca. String literals are compiled to static `const char[]` data in the C output — `str` literals are static fat pointers, and `cstr` literals (written as `c"..."`) are static null-terminated byte arrays. The alloca only occurs when a runtime `str` value is cast to `cstr`.

### String interpolation

With bare `%s{expr}`, the buffer size depends on the runtime length of the string argument. With precision-bounded `%.Ns{expr}`, the string segment contributes a compile-time constant `N` to the buffer size (or `max(N, width)` when a minimum width is also specified).

When **all** segments in an interpolation have compile-time bounds — which is the case when all `%s` segments use precision and all other segments are numeric/char/pointer — the entire `alloca` argument is a constant expression. Benefits:

- The C compiler can hoist the allocation out of loops, converting it to a fixed stack slot in the function prologue
- Static stack analysis tools (used for RTOS task sizing) can bound the worst-case usage
- No data-controlled allocation size (eliminates a class of stack overflow from untrusted input)

### `main` wrapper argv conversion

The `main` wrapper allocates `argc * sizeof(fc_str)` to wrap C's `argv` into an FC `str[]` slice. Critically, **no string data is copied** — each `fc_str` is a fat pointer (`ptr` + `len`) wrapping the existing `argv[i]` pointer directly, with `strlen` used only to compute the length field. The argv strings themselves live in OS-managed memory.

The alloca is therefore exactly `argc * 16 bytes` (on 64-bit). An `argc` of 512 costs 8KB — already an unusual number of arguments. The risk is a pathologically large `argc`, which is genuinely niche and well outside the range of normal program invocation.

---

## Possible further mitigations

- **Size guard**: Emit `if (len > LIMIT) abort();` before `alloca` calls. Caps stack damage, makes failures deterministic, but the limit is arbitrary.
- **Heap allocation**: Replace `alloca` with `malloc`/`free`. Safe and unbounded, but introduces hidden heap allocation in cast expressions — surprising in a manual-memory language.
- **Explicit user-provided buffers**: Make `str` to `cstr` conversion a function that takes a buffer, not an implicit cast. Most principled for a systems language, but verbose.
- **Sentinel-terminated slice type**: A type-system approach (like Zig's `[:0]u8`) that tracks null-termination through the type system. Eliminates the need for copies when the terminator is already present (string literals, values originating from `cstr`). Elegant but a significant type system addition.
- **Accept and document**: For desktop/server targets, `alloca` is fine. Document that the cast stack-allocates and let users make informed decisions. This doesn't preclude adding alternatives later.
