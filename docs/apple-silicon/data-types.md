# Data types and ABI boundaries

Apple Silicon makes accidental host-width assumptions visible. The Xbox guest
is a 32-bit little-endian x86 program; the host is a 64-bit arm64 process.
Guest values and host implementation values must remain separate even when both
fit in a C integer during one test.

## Type rules

| Meaning | Required representation | Do not use |
|---|---|---|
| Guest virtual address or pointer | `uint32_t` until deliberately translated through the guest memory mapping | Host pointer, `long`, `size_t` |
| Host address used for arithmetic | `uintptr_t` or `ptrdiff_t`, as appropriate | `uint32_t`, guest register types |
| Guest x86 general register | `uint32_t`, thread-local where it models architectural state | Host register-sized integer |
| Guest signed 32-bit value | `int32_t` after an explicit signed interpretation | Plain `int` in serialized layouts |
| Serialized 8/16/32/64-bit field | Matching fixed-width integer plus explicit decode/encode | Native structs with implicit padding |
| Host object or allocation size | `size_t`; use `uint64_t` for stable persisted counts | Guest address type |
| Xbox wide character | Explicit 16-bit `WCHAR` in the compatibility layer | macOS `wchar_t`, whose width differs |
| On-disc float | Explicit 32-bit or 64-bit IEEE field as specified by the format | `long double` or compiler-dependent packing |

[architecture.md](../architecture.md) describes the simulated registers,
`MEM8/16/32` accessors, 32-bit pointer wrapping, and indirect-call dispatch.
[xbox_winnt.h](../../kit/source/runtime/src/platform/xbox_winnt.h) defines the
non-Windows Win32/Xbox vocabulary, including the 16-bit guest `WCHAR`.

## Guest execution model

- The supported XBE is linked at guest image base `0x00010000`.
- General registers are 32-bit thread-local globals. SSE and x87 state are also
  modeled explicitly.
- Guest RAM is accessed through an offset/mapping layer. A guest virtual address
  is not a host pointer and must not become one through a C cast alone.
- `XBOX_PTR`-style translation must preserve 32-bit wrap behavior before adding
  the host memory offset.
- Kernel imports use synthetic 32-bit guest addresses and dispatch through the
  bridge; host function pointers never belong in guest memory.
- Guest structures keep Xbox widths even when the macOS API uses wider types.

The first arm64 build should enable existing ABI and indirect-call diagnostics
before optimizing. Compiler errors caused by x86 intrinsics, MSVC extensions,
TLS syntax, or unaligned loads are separate issues; do not silence them with
unchecked casts.

## Serialized formats

### MGF

[mgf.h](../../editor/src/core/mgf.h) documents the current model:

- Header words and file records are 32-bit.
- A file record contains eight 32-bit words.
- A directory row contains six signed 32-bit words.
- Stream positions and archive sizes use 64-bit host-side values.
- Names are NUL-terminated bytes in a four-byte-aligned string table.

Parsing must not map the file directly onto compiler structs.

### WDF

[wdf.h](../../editor/src/core/wdf.h) defines:

- Five 32-bit header words.
- A 32-bit object marker, property count, and class key.
- A 32-bit property marker, type, key, size, followed by payload bytes.
- Integer property types with explicit 8/16/32-bit signed and unsigned widths,
  32/64-bit floats, strings, vectors, colors, and opaque blobs.
- Tree relations stored as 32-bit UIDs rather than native pointers.

Unknown keys and property payloads must survive a read/write round trip.

### Textures and models

[ma2models.h](../../editor/models/ma2models.h) defines decoded pictures as
tightly packed RGBA8 rows. Model transforms use 16 32-bit floats with
translation in elements 12 through 14, game space is Y-up, and distances are
metres. Preserve those conventions at renderer boundaries rather than changing
the CPU-side format for Metal or OpenGL convenience.

### NV2A texture-stage words

`SET_SHADER_STAGE_PROGRAM` packs four five-bit texture modes at
`stage * 5`. `SET_DOT_RGBMAPPING` and `SET_SHADER_OTHER_STAGE_INPUT` form one
control word: stages 1 through 3 take dot-map nibbles at bits 0, 4, and 8,
while stages 2 and 3 select a prior texture result at bits 16 and 20. Stages 0
and 1 always use prior result zero; later selectors are clamped to an earlier
stage.

Observed MA2 programs include:

- `0x02621`: projected stage 0, stage-1 `DOTPRODUCT`, and stage-2 dependent
  `DOT_ST`;
- `0x22621`: the same chain followed by stage-3 pass-through; and
- `0x001E1`: projected stage 0 followed by a dependent AR lookup.

Dot-map mode 4 treats sampled `A:R` and `G:B` byte pairs as two normalized
16-bit HILO values. Do not replace it with an ordinary RGB dot product.
Swizzled and compressed texture coordinates are normalized, while linear
textures use texel coordinates; the texture format, not the shader mode,
selects that convention.

## Arm64 hazards to test

| Hazard | Required test |
|---|---|
| Host pointer truncation | Compile warnings as errors for pointer/integer conversions in new code; exercise guest heap and kernel-return paths. |
| Alignment | Synthetic fixtures with fields and guest reads at every legal byte alignment; use byte copies where direct loads are undefined. |
| Endianness assumptions | CPU-only parser tests with hand-authored little-endian fixtures and exact expected values. |
| Structure padding | `static_assert` only for intentionally laid-out host structs; otherwise encode fields individually. |
| TLS semantics | Two host threads with different guest register values and a call through the real dispatch path. |
| Atomics and memory order | Existing lock/atomic tests on arm64, where x86 ordering cannot be assumed. |
| Floating point | Recompiler conformance tests for NaN, signed zero, x87 comparison state, conversions, and rounding modes. |
| SIMD translation | Build and run generated helper tests without assuming x86 SSE is available on the host. |
| Path case and separators | Archive lookup tests independent of the case sensitivity of the mounted macOS volume. |

Add a regression test before declaring a width or alignment issue resolved.

## Verified generated-code boundary

The generated arm64 ABI milestone is recorded in [abi.md](abi.md). Its
conformance suite now proves the rules above for the patched generated header
and complete private generated core:

- guest scalar loads and stores use one-byte-aligned, may-alias fixed-width
  types rather than host-aligned cast-and-dereference expressions;
- guest stack cleanup is checked with modulo-32-bit arithmetic;
- x87 24-bit precision honors all four guest rounding-control modes for finite
  binary64 values, and comparisons evaluate operands once;
- `PMADDWD` lane sums wrap as x86 requires without signed C overflow;
- indirect-call trace publication is atomic across guest-capable threads;
- guest `WCHAR` remains 16 bits even though macOS `wchar_t` is wider; and
- Win32 `LONG` saturation uses `INT32_MAX`, not the LP64 host `LONG_MAX`.

The generated source and header remain private and unchanged. A verified Clang
VFS overlay supplies the adaptations at compile time.
