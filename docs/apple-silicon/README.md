# Apple Silicon port

This directory tracks the work needed to run the recompiled game natively on
arm64 macOS. It records evidence and decisions that can become focused pull
requests for this repository or for the underlying xboxrecomp project.

The port must preserve the repository's existing legal boundary: game data,
disc images, extracted files, generated game code, shader caches, and converted
assets stay on the developer's machine. See
[CONTRIBUTING.md](../../CONTRIBUTING.md) and [LEGAL.md](../../LEGAL.md).

## Priorities

1. Build and run the game on Apple Silicon.
2. Make the runtime paths needed for gameplay portable: memory, kernel,
   graphics, input, audio, storage, and video.
3. Keep the local disc-to-build workflow reproducible without committing its
   inputs or generated outputs.
4. Separate reusable xboxrecomp fixes from MechAssault 2-specific changes so
   they can be proposed upstream.
5. Port the installer, scenario editor, and auxiliary GUI tools after the game
   is playable. Command-line tools needed to build or diagnose the game are not
   deferred.

## Current baseline

| Area | State in this clone | Port implication |
|---|---|---|
| Game target | [The game CMake file](../../kit/source/game/CMakeLists.txt) preserves the Windows GUI/launcher graph, builds a public native arm64 host shell, and conditionally builds a private generated guest with explicit SDL/AppKit and Metal boundaries. | The private guest now enters the retail XBE and reaches an active first-mission scene; behavioral acceptance and distribution remain. |
| Platform primitives | The runtime's [platform target](../../kit/source/runtime/src/platform/CMakeLists.txt) selects `win32_compat.c` on non-Windows hosts. That implementation already contains Darwin-specific memory and timing paths. | Reuse and validate this layer instead of replacing it with game-specific wrappers. |
| Memory | [xbox_memory_layout.c](../../kit/source/runtime/src/kernel/xbox_memory_layout.c) implements validated Darwin reservations, mirrors, 16 KB protection handling, and guest allocation/reclaim paths. | Host-page-isolated contiguous reuse removes the mission-load protection fault; synthetic tests and a 112-second scripted route under a 120-second watchdog pass without `SIGBUS`. |
| Graphics | The CPU NV2A executor is the correctness reference. Homogeneous near-plane clipping removes the earlier diagonal streaks, hardware-correct alternating strip winding removes registered-fallback fragmentation, type-2 render targets preserve Morton-order output, and the matching mission A/B confirms the dependent texture-stage port removes the large purple floor bands and widespread white flecks. An Apple-only synchronous Metal correctness slice registers by default with explicit CPU fallback. | Serial CPU rendering remains about 1.37 FPS in the settled mission shot; four opt-in raster workers reach roughly 2.4-2.5 FPS in a representative passage but are not a hash-aligned acceptance result. Exact Metal visibility counts are required before retail draws can leave the CPU path. |
| Input | SDL mapping, hotplug, keyboard fallback, capture, and OHCI delivery have synthetic coverage. Deterministic host key actions traverse `Press START` through mission loading, active mission execution, and the retail pause menu. A 330-second movement/camera/fire probe remained inside the letterboxed cinematic, so it did not prove control. | Re-run commands after an unambiguous HUD/player-control transition with close scheduled before the watchdog, then repeat with a physical controller and validate prompts, rumble, disconnect behavior, audio, and save/restart. |
| Audio | SDL implements the existing `xa2_*` queue contract and retail APU/DSP startup runs. | Physical audible output, long-run pacing, device loss, and underrun behavior remain to be accepted. |
| Video | The Apple guest currently uses lifted retail Bink decode because its native Bink contract is stubbed. A fullscreen-quad trace proves observed white blocks are already present in the decoded 720x480 source surface, not introduced by NV2A rasterization. | One-shot per-lifecycle safe-start skipping has traversed the retail startup and campaign route into the first mission through normal screen completion; native FFmpeg Bink parity remains later work. |
| Game host layer | The SDL/AppKit shell owns presentation and lifecycle while the private bootstrap owns retail validation, guest services, dispatch, and generated entry. | Cooperative shutdown after real retail entry and native settings/distribution remain. |
| Editor and packaging | The editor is Win32/Direct3D 11 and the installer builds with MSVC and Windows SDK tools. | Defer GUI parity and release packaging until the native game loop works. |

This clone does not contain the latest xboxrecomp runtime as one clean
snapshot. Its runtime declares version 0.9.0, while its trimmed toolkit README
describes v0.13.0 and includes later project-specific work. Current xboxrecomp
main is v0.18.0 and already contains additional Apple Silicon foundations.
See [upstream.md](upstream.md) before implementing or importing a runtime fix.

## Milestones

| Milestone | Exit condition |
|---|---|
| AS0: Baseline | A documented, repeatable arm64 macOS configure attempt identifies failures without modifying or committing game data. |
| AS1: Portable build graph | The runtime and a game host target compile with Apple Clang for arm64. Platform-specific sources and dependencies are selected explicitly. |
| AS2: Guest initialization | A locally generated build loads the supported retail XBE, maps guest memory, initializes the kernel bridge, and reaches the guest entry point. |
| AS3: First frame | The game creates a window and presents a correct frame through a non-D3D11 path. |
| AS4: Playable loop | Controller/keyboard input, storage, timing, and enough graphics work to enter and play a mission. |
| AS5: Media parity | APU audio and required movie playback work with recoverable device errors. |
| AS6: Distribution | A self-contained macOS app can be built locally; signing, notarization, updater/installer work, and editor support are tracked separately. |

Passing a milestone requires a recorded command, host details, result, and a
small reproducible test. "Compiles" and "runs" are separate results.

## Documentation map

- [baseline.md](baseline.md): pinned revisions, reproducible path inventory,
  Windows invariants, and strict generator/patch baseline.
- [formats.md](formats.md): game containers and resource formats, existing
  parsers, and unknowns.
- [tools.md](tools.md): repository tooling, host dependencies, and the
  upstreaming workflow.
- [data-types.md](data-types.md): guest/host ABI and serialized data rules that
  matter on arm64.
- [issues.md](issues.md): prioritized, evidence-backed port issue register.
- [upstream.md](upstream.md): current xboxrecomp main/PR survey and the rules
  for integrating its Apple Silicon work.
- [disc-validation.md](disc-validation.md): private retail-input validation
  boundaries and sanitized results.
- [runtime.md](runtime.md): reconciled portable runtime behavior, tests, and
  lifecycle constraints.
- [generation.md](generation.md): deterministic private generation, patch,
  FastCode, and asset-validation workflow.
- [build.md](build.md): native arm64 core and host graph, target boundaries,
  validation commands, and remaining guest work.
- [abi.md](abi.md): verified private-header adaptation, arm64 conformance
  tests, complete generated-core audit, and guest-entry contract inventory.
- [host.md](host.md): native SDL/AppKit shell, worker lifecycle, private
  per-user paths, structured logging, and signal-chain boundary.
- [architecture.md](../architecture.md): current Windows architecture and
  static recompilation pipeline.
- [fact-index.md](../fact-index.md): searchable long-form research already in
  the repository.

## Evidence rules

Use one of these labels when adding a claim:

- **Confirmed:** directly supported by source, build output, or a repeatable
  test.
- **Observed:** seen in one local run with the command and environment recorded.
- **Hypothesis:** a plausible explanation that still needs a discriminating
  test.
- **Resolved:** the former failure and its regression test are both recorded.

Do not put private paths, usernames, disc file listings, extracted game
content, generated C, game strings, screenshots, or hashes not already allowed
by the upstream project into these documents. Refer to repository source paths
and sanitized diagnostics instead.
