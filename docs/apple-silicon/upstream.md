# xboxrecomp upstream survey

Snapshot date: 2026-11-21.

This page records what was checked in
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) before starting
new Apple Silicon runtime work. The release comparison is pinned below; GitHub
main and pull-request state can change. Refresh the survey before acting on an
open pull request.

## Baselines

| Source | Observed revision | Meaning |
|---|---|---|
| Local MA2 | [`5ecc611d18e20de1ab04313b0c50e453d8d63b29`](https://github.com/Mazillion/MechAssault2-PC-Recomp/commit/5ecc611d18e20de1ab04313b0c50e453d8d63b29) | The checked-out 1.1.5 revision on `apple-silicon/bringup`. |
| xboxrecomp v0.18.0 | [`37b61d589168adee3680fabd51805ee81427bf07`](https://github.com/sp00nznet/xboxrecomp/commit/37b61d589168adee3680fabd51805ee81427bf07) | Pinned release commit; also main at survey time, not a promise about future main. |
| This repository's runtime | [CMake declares 0.9.0](../../kit/source/runtime/CMakeLists.txt) | Not a safe proxy for current upstream. |
| This repository's trimmed toolkit | [README describes v0.13.0](../../kit/toolkit/README.md) | Contains only selected tools/templates and warns that upstream may have changed. |
| MechAssault 2 integration | Project-specific runtime, renderer, APU/DSP, diagnostics, patches, and game host changes | Cannot be replaced wholesale by an upstream directory copy. |

[baseline.md](baseline.md) records the reproducible path-level inventory,
counts, representative three-way classifications and the Windows and generator
contracts. Version labels alone cannot choose a winner for a file.

## Relevant work already merged upstream

| PR | Result relevant to Apple Silicon |
|---|---|
| [#20](https://github.com/sp00nznet/xboxrecomp/pull/20) | Initial macOS build/stub support and 16-bit Xbox `WCHAR` handling. |
| [#59](https://github.com/sp00nznet/xboxrecomp/pull/59) | macOS conformance setup, including the split build/run container strategy needed for 32-bit x86 checks on Apple Silicon. |
| [#60](https://github.com/sp00nznet/xboxrecomp/pull/60) | POSIX Xbox memory model, Darwin fixed mapping, OS-chosen base, mirror safety, null trapping, cleanup, and synthetic tests. |
| [#185](https://github.com/sp00nznet/xboxrecomp/pull/185) | Correctly releases a Darwin placeholder when a fixed view fails. |
| [#189](https://github.com/sp00nznet/xboxrecomp/pull/189) | Additional POSIX memory/layout and contiguous allocator work from the portability series. |
| [#191](https://github.com/sp00nznet/xboxrecomp/pull/191) | ARM64 trapped-MMIO decoder and dispatcher, SDL2 framebuffer host, real SDL2 APU output behind the `xa2_*` contract, and shared keyboard mapping. |
| [#203](https://github.com/sp00nznet/xboxrecomp/pull/203) | Optional portable controller mapping rules. |
| [#263](https://github.com/sp00nznet/xboxrecomp/pull/263) | POSIX open-file limit handling and corrected vblank switch behavior. |
| [#283](https://github.com/sp00nznet/xboxrecomp/pull/283) | MMX integer tests build on ARM64 and report unsupported hardware comparisons as skipped instead of passing falsely. |
| [#285](https://github.com/sp00nznet/xboxrecomp/pull/285) | Backend readback synchronization before framebuffer snapshots without transferring presentation ownership. |
| [#304](https://github.com/sp00nznet/xboxrecomp/pull/304) | Embedders may omit or replace the bundled D3D8/OpenGL target, avoiding an unnecessary libepoxy dependency. |
| [#305](https://github.com/sp00nznet/xboxrecomp/pull/305) | Thread-safe SDL controller hotplug and synthetic regression tests on macOS ARM64. |
| [#319](https://github.com/sp00nznet/xboxrecomp/pull/319) | Reusable fake `Nv2aBackend` fixture and contract tests, validated on macOS ARM64. |

The pinned v0.18.0 tree contains `apu_sdl2.c`, `mmio_decode_a64.h`,
`src/kernel/nv2a_backend.h`, the backend contract tests, and
`tools/macos/setup.sh`. Those exact paths are absent from this repository's
embedded copy; this does not establish that all related behavior is absent.

## Open work and decisions

### Renderer policy

[Issue #306](https://github.com/sp00nznet/xboxrecomp/issues/306) records the
maintainer's current direction:

- Evolve `Nv2aBackend` incrementally with synthetic tests.
- Keep complete Metal and D3D11 renderer implementations downstream for now.
- Accept renderer-neutral executor hooks, helpers, documentation, and fixtures.
- Keep presentation as a host service; a renderer produces/readbacks surfaces
  and does not own the window.
- Do not require a GPU round trip for each draw.

PR #319 is the first merged step from that plan.

### Metal prior art

[Draft PR #184](https://github.com/sp00nznet/xboxrecomp/pull/184) is explicitly
an index, not a merge request. Its `apexJCL/xboxrecomp:blinx2/portability`
branch contains a Metal path (`src/d3d/nv2a_pb_metal.m`,
`src/d3d/metal_compile.m`, and `src/video/fb_present_metal.m`) plus broader
portability work. At its last recorded update it was based on v0.16.0, while
main is now v0.18.0.

Use that implementation as design and behavior evidence only. Do not merge the
tracking branch wholesale or revive its alternate backend seam. A MechAssault 2
Metal renderer should live in this fork and target the current upstream
contract.

### Current renderer evolution

[PR #346](https://github.com/sp00nznet/xboxrecomp/pull/346) is an open D3D11
hardware renderer based on v0.18.0. It proposes GPU vertex programs, register
combiners, texture stages, render-target writeback, shader caching, and
pixel-comparison tests. Although the implementation is Windows-only, its
contract extensions and synthetic CPU-versus-GPU tests may define the closest
current upstream shape for a downstream Metal backend. It is not merged and
must not be treated as an API guarantee.

[Issue #312](https://github.com/sp00nznet/xboxrecomp/issues/312) asks which
host-layer extension points upstream will accept for downstream SDL3 hosts.
There was no maintainer response at survey time. It may eventually inform MA2
host, pad, audio, and presentation boundaries, but cannot block the initial
inventory.

## Integration rules for this project

1. Do not start by writing new Darwin memory, SDL audio, controller hotplug, or
   ARM64 MMIO implementations. Compare and adapt the merged upstream versions.
2. Do not replace `kit/source/runtime` wholesale. Preserve MA2's DSP, renderer,
   diagnostics, shader, timing, and game-specific integration. Reconcile
   independently divergent paths using the inventory and tests.
3. Use upstream v0.18.0 tests as the neutral baseline. Add MA2 integration tests
   separately.
4. Keep the complete Metal renderer in this fork, following issue #306's
   presentation and threading boundaries.
5. Offer only renderer-neutral, title-independent improvements upstream, each
   with synthetic data and a focused test.
6. Treat open PRs as prior art until merged. Pin an exact commit when a local
   experiment depends on one.
7. Keep generator/template upgrades separate from runtime portability. The
   existing strict patch, FastCode and manifest hashes bind the current
   generated output; a generator change requires a separately reviewed
   rebaseline, not a bypass of those checks.

## Refresh commands

```sh
gh api repos/sp00nznet/xboxrecomp/commits/main \
  --jq '{sha: .sha, date: .commit.committer.date, message: .commit.message}'
gh pr list -R sp00nznet/xboxrecomp --state all --search 'macOS'
gh pr list -R sp00nznet/xboxrecomp --state all --search 'arm64'
gh pr list -R sp00nznet/xboxrecomp --state all --search 'Metal'
gh pr list -R sp00nznet/xboxrecomp --state all --search 'backend'
```

Record a new snapshot date and revision when the conclusions change.
