# Generated arm64 ABI

The patched generated core now compiles completely with Apple Clang as native
arm64 without changing or copying private generated inputs into the
repository. This milestone proves the generated C ABI and helper behavior; it
does not link the retail guest into the host or enter the XBE.

## Private-header adaptation

The generated `recomp_types.h` is part of the exact-byte patch pipeline and
must not be edited after generation. On Apple Silicon,
[prepare_overlay.py](../../kit/source/game/src/apple/abi/prepare_overlay.py):

1. Verifies the private header against the final shipped patch/FastCode
   manifests.
2. Applies only the reviewed arm64 ABI substitutions to an owner-only output
   directory outside the repository.
3. Writes a Clang VFS overlay that maps the original header path to the adapted
   copy during compilation.
4. Leaves the private generated tree unchanged.

`MA2_ABI_OVERLAY_DIR` is mandatory for a private macOS generated-core build.
CMake rejects an absent, unverified, or repository-local private overlay
instead of compiling the unadapted header.

The tracked template follows the same path in synthetic tests. Template mode
is restricted to the graph and ABI test fixtures and verifies the exact
tracked template before adapting it.

## Corrected assumptions

| Area | Problem on arm64/LP64 | Adaptation |
|---|---|---|
| Guest memory access | Cast-and-dereference scalar macros have undefined behavior at unaligned guest addresses and can violate strict aliasing. | One-byte-aligned, may-alias scalar types preserve legal Xbox byte alignment without claiming host alignment. |
| x87 compare | The macro form evaluated operands more than once. | A helper evaluates each operand once and returns the existing unordered result for NaNs. |
| x87 precision control | A host `float` cast does not honor all x87 rounding-control modes. | Integer significand rounding implements 24-bit precision with nearest-even, down, up, and toward-zero behavior for finite binary64 values. |
| Guest stack return | Comparing against `before + 4` failed when the 32-bit guest stack wrapped. | The cleanup check uses modulo-32-bit subtraction and rejects implausible reverse movement. |
| `PMADDWD` | The two signed products can overflow a signed 32-bit C addition even though x86 requires modulo-32-bit lane results. | Products are added as unsigned 32-bit bit patterns. |
| Indirect-call trace | Plain volatile counters and ring writes raced when guest-capable host threads ran concurrently. | Atomic fetch-add and atomic slot stores publish trace records without a C data race. |
| Win32 `LONG` limit | `LONG` is correctly fixed at 32 bits, but C's `LONG_MAX` is the 64-bit host `long` limit on macOS. | The audio lifetime saturation check uses `INT32_MAX`. |
| Linkage and TLS | Generated C, C++ host helpers, and per-thread register state must agree on linkage and storage. | The forced compatibility header supplies C linkage and the runtime TLS contract; two-thread tests prove independent register, stack, TIB, and last-error state. |

The helpers are in
[arm64_helpers.h](../../kit/source/game/src/apple/abi/arm64_helpers.h), and the
forced include boundary is
[recomp_compat.h](../../kit/source/game/src/apple/recomp_compat.h).

## Conformance suite

Configure and run the synthetic suite from the repository root:

```sh
SDL="$(brew --prefix sdl2-compat)"
OPENSSL="$(brew --prefix openssl@3)"
cmake -S tests/apple_silicon/abi -B _work/apple-silicon/abi -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DOPENSSL_ROOT_DIR="$OPENSSL" \
  -DCMAKE_PREFIX_PATH="$SDL;$OPENSSL"
cmake --build _work/apple-silicon/abi --parallel 3
ctest --test-dir _work/apple-silicon/abi --output-on-failure --no-tests=error
```

The suite covers:

- fixed widths, guest pointer wrap, unaligned scalar/vector access, and
  16-KB-page boundary access;
- x87 comparisons, NaNs, signed zero, precision control, rounding modes, and
  MMX/SSE scalar helper semantics;
- direct and indirect dispatch, callee-saved registers, wrapped guest stack
  cleanup, unresolved dispatch, and atomic trace publication;
- C/C++ linkage, runtime atomics, explicit UTF-16 guest strings, TLS, TIB,
  kernel-created guest threads, and the audio reference-count boundary;
- ASan/UBSan numeric runs, a TSan concurrent-trace run, and `lipo` checks for
  every native test executable.

The optional x86 hardware reference is compiled separately. CTest reports an
explicit skip when x86_64 execution is unavailable; an arm64 build is never
accepted as an x86 comparison.

## Complete private-core audit

[audit_private.py](../../tests/apple_silicon/abi/audit_private.py) consumes an
owner-only build and reports aggregate counts only. It verifies:

- every final manifest entry and every discovered generated source;
- exactly one compile of each generated source;
- the verified VFS overlay on every generated and hand-written core
  compilation;
- arm64 slices in all audited objects and archives; and
- that every unresolved contract with a tracked implementation is either
  classified for the next platform milestone or fails the audit.

The validated private Release build had zero manifest mismatches, zero missing
or unexpected generated units, zero non-arm64 objects/archives, and zero
unexpected omitted tracked contracts.

Ninety tracked contracts remain explicitly assigned to guest-entry and host
service work. They are confined to native movie support, offline bots,
cinematic/diagnostic hooks, the Win32 settings and prompt layer, GUI bridge,
and scenario/editor integration. The audit uses a closed source allowlist:
adding a new omission or moving a definition outside that list fails. No weak
alias, fake success return, or silent fallback satisfies this gate.

## Guest-entry implementation

The private `mechassault2_recomp_guest` target now:

1. replaces the synthetic host worker with a guest bootstrap while preserving
   the no-private-data smoke mode.
2. links the generated objects and resolves the startup-critical platform contracts
   with real portable behavior or explicit fail-closed optional capability
   boundaries.
3. installs and chains the host/MMIO signal layers, registers every guest-capable
   thread, and follows the lifecycle in [runtime.md](runtime.md).
4. enters the retail XBE only after memory, kernel, APU, OHCI, dispatch, TLS,
   and guest stack initialization succeeds.

That implementation now reaches changing title/menu frames. The ABI milestone
alone still does not establish renderer correctness, audible output, active
gameplay, cooperative shutdown, or frame-rate acceptance.
