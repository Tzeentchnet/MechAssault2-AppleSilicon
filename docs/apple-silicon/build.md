# Apple Silicon build and host graph

The default macOS target builds a public-data native arm64 **host shell** with
an SDL/AppKit window and synthetic worker. When the owner supplies a verified
generated tree and private ABI overlay, the graph also builds
`mechassault2_recomp_guest`, which links the generated game core and enters the
validated retail XBE off the main thread. The private target now renders the
title, campaign hub, and difficulty dialog through the initial Metal backend
with explicit CPU fallback, or through the selectable CPU-only reference path.
It reaches active first-mission execution and the retail pause menu, but
player-controlled behavior and 1080p30 are not yet accepted. See [host.md](host.md) for
lifecycle, privacy, launch, and renderer selection details.

## Requirements

Use native arm64 macOS, Xcode or its Command Line Tools with the Metal
toolchain, Apple Clang, CMake 3.20 or newer, Ninja, SDL2, and OpenSSL. The
[toolchain setup](tools.md#native-apple-silicon-setup) describes the provisioned
Homebrew dependencies. Configure checks that:

- The host and selected architecture are arm64, and C, C++, Objective-C, and
  Objective-C++ use Apple Clang.
- SDL2 and OpenSSL can link as arm64; Objective-C and Objective-C++ can link
  AppKit, Foundation, QuartzCore, and Metal; `metal` and `metallib` are found.
- The runtime's bundled D3D8 library is omitted. Requesting it, a missing
  tool, or Windows-only PC packaging is an error, not a fallback.

## Configure without game-derived inputs

From the repository root:

```sh
SDL="$(brew --prefix sdl2-compat)"
OPENSSL="$(brew --prefix openssl@3)"
cmake -S kit/source/game -B _work/apple-silicon/game-no-generated -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DXBOXRECOMP_BUILD_D3D8=OFF \
  -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DOPENSSL_ROOT_DIR="$OPENSSL" \
  -DCMAKE_PREFIX_PATH="$SDL;$OPENSSL"
cmake --build _work/apple-silicon/game-no-generated --parallel 3
cmake --build _work/apple-silicon/game-no-generated --target ma2_macos_core --parallel 3
```

In this mode CMake explicitly reports that generated sources and their manual
hooks are disabled. The first command builds the host executable; the second
compiles the reconciled runtime and game-side sources that do not require
generated headers, including bot brain logic through a declaration-only Clang
adapter. Run the host directly, or use the isolated dummy-driver smoke command
in [host.md](host.md#launch-and-settings).

## Compile a private generated tree

First use the existing verified generation, patch, and FastCode workflow with
an owned, supported disc. Point `MA2_GENERATED_DIR` at its **existing**
`src/recomp/gen` directory; do not copy it into this repository:

```sh
SDL="$(brew --prefix sdl2-compat)"
OPENSSL="$(brew --prefix openssl@3)"
MA2_GENERATED_DIR="<outside-repository>/src/recomp/gen"
MA2_PRIVATE_BUILD="<owner-only-outside-repository>/game-private-build"
MA2_ABI_OVERLAY_DIR="<owner-only-outside-repository>/abi-overlay"
umask 077
cmake -S kit/source/game -B "$MA2_PRIVATE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DXBOXRECOMP_BUILD_D3D8=OFF \
  -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DOPENSSL_ROOT_DIR="$OPENSSL" \
  -DCMAKE_PREFIX_PATH="$SDL;$OPENSSL" \
  -DMA2_GENERATED_DIR="$MA2_GENERATED_DIR" \
  -DMA2_ABI_OVERLAY_DIR="$MA2_ABI_OVERLAY_DIR"
cmake --build "$MA2_PRIVATE_BUILD" \
  --target ma2_macos_core mechassault2_recomp_guest --parallel 3
```

The default `MA2_PATCH_DIR` is `kit/patches`; set it explicitly if using a
separately located patch kit. CMake requires the shipped patch and FastCode
manifests, their referenced patch payloads, and their expected outputs, plus
the generated headers needed by the manual hooks. These checks establish
presence for the complete input set and exact final-manifest identity for the
generated ABI header. They do not replace the generation pipeline's exact-byte
verification of every output. An incomplete or incorrectly configured tree is
an error.

Discovery remains the original non-recursive `*.c` glob with
`CONFIGURE_DEPENDS`, including dispatch, unresolved stubs, bring-up, and all
FastCode units. Required input changes trigger the presence checks again.
The Windows per-chunk scalar-XMM definitions and two `/Ob0`
exceptions are retained. On arm64 all generated units select the scalar XMM
helpers; the two corresponding chunks additionally use Clang's
`-fno-inline-functions`.

The private generated header remains byte-identical. CMake requires an
owner-only `MA2_ABI_OVERLAY_DIR` outside the repository, verifies the header
against the final shipped manifests, and compiles through a Clang VFS overlay
containing only the reviewed arm64 ABI adaptations. See [abi.md](abi.md) for
the corrected assumptions, sanitizer coverage, and complete-core audit.

Only two **hand-written repository headers** are staged in the build
directory to resolve the generated code's relative includes. No generated
source or header is copied into the repository. The adapted header and VFS
metadata remain in the owner-only overlay directory. Build directories,
objects, CMake caches,
`compile_commands.json`, diagnostics, and other metadata can themselves
contain private code or local paths: keep them local and out of Git and reports.

## Target boundaries

- `ma2_macos_core` builds the runtime libraries and `ma2_game_core` object
  target. With private inputs it also compiles the discovered generated code,
  FastCode, `recomp_manual.c`, and the portable game hooks selected by the
  graph. Without inputs those generated-dependent objects are not present.
- `ma2_apple_host` is an interface for SDL2/AppKit/Foundation/QuartzCore
  dependencies. `ma2_host_shell` uses it and the existing SDL framebuffer host
  to own the window and presentation on the main thread.
  `ma2_apple_metal` is the Apple-only offscreen NV2A backend. It compiles Metal
  shaders at startup and synchronously reads accepted operations back to guest
  memory; it does not own the window or drawable. Presentation remains
  host-owned.
- The default macOS build and the explicit `mechassault2_recomp` target produce
  the arm64 host executable. It does **not** link `ma2_game_core` or generated
  guest code even when a generated directory is configured. Unused guest-only
  runtime sections are dead-stripped instead of supplying fake guest callbacks.
  Select `ma2_macos_core` separately to compile the core.
- With both private inputs configured, `mechassault2_recomp_guest` links the
  generated core, the Apple guest bootstrap, and the portable runtime. It is
  deliberately absent from public/no-generated builds.
- Win32 GUI, RC, DX11/Win32 ImGui, Windows movie/FFmpeg, and other Windows host
  sources are excluded from the macOS game objects. The runtime keeps its
  cross-platform CPU counters and non-Windows GPU stubs in a historically
  D3D11-named source; its Windows renderer section is not compiled or linked.
  The separate legacy D3D8 PGRAPH translator and replay are removed from this
  game graph, and reaching that legacy dispatch fails explicitly.
- The narrow compile compatibility headers map the existing TLS declaration,
  atomic AND, yield, and compiler barrier to arm64 equivalents and expose the
  runtime's implemented POSIX primitives. They do not implement missing game
  hooks or pretend to be a complete Win32 SDK.

The private ABI audit classifies 90 contracts that were originally supplied by
Windows-only host services, cinematic/diagnostic hooks, bot/scenario
integration, settings/prompts, GUI integration, and native movie handling.
The campaign-critical startup subset now has portable behavior. Remaining
optional capabilities are disabled explicitly and logged; any unclassified
omission still fails. Weak aliases and fake success stubs are not accepted.

Guest register/XMM behavior, thread-local state, 16-bit guest strings,
unaligned access, x87 rounding, stack wrap, and concurrent dispatch tracing are
covered by the dedicated ABI suite. Compile success still does not start the
guest, display a frame, or prove gameplay.

## Focused tests

The graph regression tests create only synthetic inputs in an ignored work
directory and remove them when finished. They check enabled/disabled modes,
host executable boundaries, missing patch and FastCode inputs, source
re-globbing, explicit failures, the Windows source/packaging graph, and the
per-chunk options. They use Homebrew prefixes by default; `SDL2_DIR` and
`OPENSSL_ROOT_DIR` can supply other provisioned dependencies.

```sh
python3 tests/apple_silicon/test_build_graph.py
SDL="$(brew --prefix sdl2-compat)"
OPENSSL="$(brew --prefix openssl@3)"
cmake -S tests/apple_silicon -B _work/apple-silicon/build-graph-unit -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DOPENSSL_ROOT_DIR="$OPENSSL" \
  -DCMAKE_PREFIX_PATH="$SDL;$OPENSSL"
cmake --build _work/apple-silicon/build-graph-unit --parallel 2
ctest --test-dir _work/apple-silicon/build-graph-unit --output-on-failure
```

The CMake tests retain the compile adapter, explicit legacy-translator failure,
and synthetic executor/backend checks. Additional tests exercise real SDL host
ownership, worker start/stop/join, signal chaining, private paths and logs,
CLI failures, and instance contention. Windows structural checks use MinGW
cross-configuration when available and static assertions otherwise; they are
not an MSVC build.

The executor fixture also has a dedicated swizzled-surface mode. It checks that
a partial colour clear and a CPU-rasterized quad land at the exact Morton
indices of a type-2 64x64 render target rather than at row-major offsets. The
memory-dispatch fixture protects a contiguous allocation, releases and reuses
it, and verifies host-page alignment, rounded size, restored write permission,
and a successful write on large-page hosts.

The generated ABI suite is documented in [abi.md](abi.md). It registers
native, ASan/UBSan, TSan, architecture, runtime-thread, UTF-16, and optional
x86-reference checks with CTest.

## Sanitized validation record

On 2026-11-21, using native arm64 macOS, Apple Clang, Release, SDL2 and OpenSSL:

| Check | Result |
|---|---|
| No-generated configuration, arm64 host executable, and `ma2_macos_core` build | Passed |
| One locally verified private generation, including all discovered generated C, selected manual hooks, and the private guest executable | Passed configuration, core build, retail entry, title/menu rendering, mission loading, and scripted active mission entry |
| Synthetic configuration, input-change, packaging, and installer-option regressions | 7 passed |
| Existing compatibility, fail-fast, and synthetic runtime/backend CMake tests | 3 passed |
| Focused Metal backend, contract, executor, alias, and failure tests | 11 passed |
| Host lifecycle, signal-chain, and process CMake tests | 3 passed |
| Generated arm64 ABI suite | 23 checks passed; the optional x86 hardware reference was explicitly skipped |
| Complete private generated-core ABI audit | 184 final manifest entries and 178 generated sources verified; zero mismatches, omissions, unexpected sources, non-arm64 objects, or unexpected tracked contracts |
| Dummy-driver smoke, path/log redaction, and one-instance contention | Passed without private inputs; separate owner-only runs reached generated entry and changing retail frames |
| Windows graph | MinGW cross-configured; MSVC options checked structurally, not built or run |
| Whitespace | Owned paths clean; whole-worktree check passes with CRLF recognized |

The plain `git diff --check` also reports pre-existing CRLF-as-whitespace
diagnostics in runtime work outside this milestone. They were not changed.
No Windows SDK/MSVC build, active campaign gameplay, correct 1080p output, or
30 FPS acceptance run has been performed. Retail entry and post-title menu
rendering are established. Homogeneous near-plane clipping removed the known
diagonal streaks in a controlled A/B and raised the settled title path to about
1.15 FPS, still far below real time. The initial Metal slice registers and
presents correctly but current title draws remain on explicit CPU fallback.
