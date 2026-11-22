# Frozen port baseline

Snapshot: 2026-11-21. These are source-tree comparisons, not evidence that a
Windows build or an Apple Silicon game build was run.

| Input | Pinned revision |
|---|---|
| MA2 on `apple-silicon/bringup` | [`5ecc611d18e20de1ab04313b0c50e453d8d63b29`](https://github.com/Mazillion/MechAssault2-PC-Recomp/commit/5ecc611d18e20de1ab04313b0c50e453d8d63b29), the 1.1.5 source release |
| xboxrecomp v0.18.0 | [`37b61d589168adee3680fabd51805ee81427bf07`](https://github.com/sp00nznet/xboxrecomp/commit/37b61d589168adee3680fabd51805ee81427bf07) |
| Historical comparison only: xboxrecomp v0.13.0 | [`b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4`](https://github.com/sp00nznet/xboxrecomp/commit/b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4) |

The local [runtime CMake file](../../kit/source/runtime/CMakeLists.txt)
declares 0.9.0; the [trimmed toolkit README](../../kit/toolkit/README.md)
describes v0.13.0. Neither label proves that the embedded sources are a clean
snapshot of that release. The v0.13.0 tree below is a useful reference, **not**
a demonstrated common ancestor of every local file.

## Reproducible path inventory

From the MA2 repository root, with Git, Python 3, network access and an
authenticated `gh` CLI, first confirm the pins:

```sh
git rev-parse HEAD
gh api repos/sp00nznet/xboxrecomp/commits/v0.18.0 --jq .sha
gh api repos/sp00nznet/xboxrecomp/commits/v0.13.0 --jq .sha
```

The following reads the local **commit**, not uncommitted files, and GitHub's
recursive Git trees; it does not check out either upstream revision or read
game inputs. It emits one tab-separated row for every path in each union and
summary counts on stderr. A missing side is `-`. `identical` means the Git
blob and mode both match; `different` means both paths exist but the blob or
mode differs. This is a byte-level comparison, so line endings alone can
produce a difference. Renames are not inferred; an upstream-only path is not
proof that its function is absent under another name.

For differing shared paths, the last column compares both blobs with the
v0.13.0 reference. `both-differ` is three-way byte divergence, not proof of
independent authorship or semantic incompatibility. Inspect the sources and
tests before assigning a port action.

```sh
python3 - <<'PY'
import collections
import json
import subprocess
import sys

ma2 = "5ecc611d18e20de1ab04313b0c50e453d8d63b29"
upstream = "37b61d589168adee3680fabd51805ee81427bf07"
reference = "b3700e1d60bcd9c3dfaad734f45149b6f0bc5cb4"

def github_tree(revision):
    endpoint = f"repos/sp00nznet/xboxrecomp/git/trees/{revision}?recursive=1"
    tree = json.loads(subprocess.check_output(["gh", "api", endpoint]))
    if tree.get("truncated"):
        raise SystemExit("GitHub tree was truncated; do not use these counts")
    return {
        entry["path"]: (entry["mode"], entry["sha"])
        for entry in tree["tree"] if entry["type"] == "blob"
    }

def local_tree(root, single):
    data = subprocess.check_output(
        ["git", "ls-tree", "-r", "-z", ma2, "--", root]
    )
    result = {}
    for record in data.split(b"\0"):
        if not record:
            continue
        meta, raw_path = record.split(b"\t", 1)
        mode, kind, oid = meta.decode().split()
        if kind != "blob":
            raise SystemExit(f"Unexpected non-blob in {root}")
        path = raw_path.decode()
        if single:
            if path != root:
                raise SystemExit(f"Unexpected path {path}")
            relative = root.rsplit("/", 1)[-1]
        else:
            prefix = root + "/"
            if not path.startswith(prefix):
                raise SystemExit(f"Unexpected path {path}")
            relative = path[len(prefix):]
        result[relative] = (mode, oid)
    return result

current = github_tree(upstream)
old = github_tree(reference)
scopes = [
    ("runtime/src", "kit/source/runtime/src", "src", False),
    ("runtime/include", "kit/source/runtime/include", "include", False),
    ("runtime/tests", "kit/source/runtime/tests", "tests", False),
    ("runtime/CMake", "kit/source/runtime/CMakeLists.txt", "CMakeLists.txt", True),
    ("tools", "kit/toolkit/tools", "tools", False),
    ("templates", "kit/toolkit/templates", "templates", False),
]
classes = ("identical", "different", "local-only", "upstream-only")
print("scope\tclass\tlocal path\tupstream path\tv0.13 reference")
for label, local_root, upstream_root, single in scopes:
    left = local_tree(local_root, single)
    if single:
        right = {upstream_root: current[upstream_root]}
    else:
        prefix = upstream_root + "/"
        right = {
            path[len(prefix):]: blob
            for path, blob in current.items() if path.startswith(prefix)
        }
    counts = collections.Counter()
    for path in sorted(left.keys() | right.keys()):
        local_path = local_root if single else f"{local_root}/{path}"
        upstream_path = upstream_root if single else f"{upstream_root}/{path}"
        if path not in left:
            kind = "upstream-only"
        elif path not in right:
            kind = "local-only"
        elif left[path] == right[path]:
            kind = "identical"
        else:
            kind = "different"
        relation = "-"
        if kind == "different":
            previous = old.get(upstream_path)
            if previous is None:
                relation = "absent"
            elif left[path] == previous:
                relation = "upstream-differs"
            elif right[path] == previous:
                relation = "local-differs"
            else:
                relation = "both-differ"
        counts[kind] += 1
        print(label, kind, local_path if path in left else "-",
              upstream_path if path in right else "-", relation, sep="\t")
    summary = " ".join(f"{kind}={counts[kind]}" for kind in classes)
    print(f"{label}: local={len(left)} upstream={len(right)} {summary}",
          file=sys.stderr)
PY
```

The main runtime comparison maps `kit/source/runtime/src/` to upstream
`src/`, **not** the runtime repository root to `src/`. The remaining local
runtime root CMake file, `include/`, and `tests/` are separately mapped to
their upstream root counterparts. Counts are tracked Git blobs, including
README, license, and test files; directories and untracked/build files are
excluded.

| Local area → upstream area | Local | Upstream | Identical | Different | Local-only | Upstream-only |
|---|---:|---:|---:|---:|---:|---:|
| `kit/source/runtime/src/` → `src/` | 166 | 138 | 49 | 52 | 65 | 37 |
| `kit/source/runtime/include/` → `include/` | 1 | 1 | 1 | 0 | 0 | 0 |
| `kit/source/runtime/tests/` → `tests/` | 5 | 201 | 4 | 1 | 0 | 196 |
| `kit/source/runtime/CMakeLists.txt` → `CMakeLists.txt` | 1 | 1 | 0 | 1 | 0 | 0 |
| `kit/toolkit/tools/` → `tools/` | 168 | 293 | 117 | 51 | 0 | 125 |
| `kit/toolkit/templates/` → `templates/` | 7 | 7 | 3 | 4 | 0 | 0 |
| **Total** | **348** | **641** | **174** | **109** | **65** | **358** |

The upstream `tests/` count includes its entire test tree, not just tests for
files present locally. The local runtime total is 173 files; tools and
templates together contain 175. For each row, local equals identical +
different + local-only; upstream equals identical + different + upstream-only.

### Subsystem breakdown

The columns are identical / different / local-only / upstream-only Git blobs.
`(root)` means a file directly under the mapped directory.

| Runtime `src/` subsystem | Identical | Different | Local-only | Upstream-only |
|---|---:|---:|---:|---:|
| (root) | 0 | 0 | 0 | 1 |
| `apu` | 2 | 11 | 24 | 4 |
| `audio` | 5 | 0 | 0 | 1 |
| `d3d` | 11 | 4 | 0 | 1 |
| `input` | 0 | 3 | 3 | 10 |
| `kernel` | 10 | 19 | 32 | 9 |
| `nv2a` | 17 | 4 | 0 | 1 |
| `platform` | 1 | 4 | 5 | 6 |
| `usb` | 1 | 4 | 1 | 0 |
| `video` | 2 | 3 | 0 | 4 |

| Toolkit `tools/` subsystem | Identical | Different | Local-only | Upstream-only |
|---|---:|---:|---:|---:|
| (root) | 7 | 1 | 0 | 0 |
| `abi_analysis` | 3 | 1 | 0 | 1 |
| `capture` | 0 | 0 | 0 | 4 |
| `conformance` | 0 | 0 | 0 | 27 |
| `debug_symbols` | 5 | 0 | 0 | 0 |
| `diff` | 0 | 0 | 0 | 13 |
| `disasm` | 10 | 11 | 0 | 21 |
| `func_id` | 7 | 4 | 0 | 4 |
| `fusion` | 9 | 0 | 0 | 0 |
| `ghidra_naming` | 0 | 0 | 0 | 11 |
| `ida_naming` | 0 | 0 | 0 | 2 |
| `kernel_audit` | 5 | 0 | 0 | 0 |
| `linux` | 0 | 0 | 0 | 2 |
| `macos` | 0 | 0 | 0 | 1 |
| `provenance` | 0 | 0 | 0 | 4 |
| `recomp` | 50 | 31 | 0 | 30 |
| `resolve` | 0 | 0 | 0 | 4 |
| `rtti` | 2 | 1 | 0 | 1 |
| `seed_from_log` | 2 | 0 | 0 | 0 |
| `split` | 4 | 0 | 0 | 0 |
| `symbols` | 5 | 0 | 0 | 0 |
| `xbe_parser` | 3 | 0 | 0 | 0 |
| `xiso` | 2 | 2 | 0 | 0 |
| `xmv` | 3 | 0 | 0 | 0 |

Templates split into `new-game` (1 identical, 3 different) and `runtime`
(2 identical, 1 different); neither has a one-sided path.

### Representative classifications

| Classification | Evidence and consequence |
|---|---|
| Identical | Local [`d3d8_gamma.c`](../../kit/source/runtime/src/d3d/d3d8_gamma.c), [`xbe_parser.py`](../../kit/toolkit/tools/xbe_parser/xbe_parser.py), and [`xbox_memory.h`](../../kit/toolkit/templates/runtime/xbox_memory.h) match their mapped v0.18.0 blobs and modes. This does not guarantee their callers still match. |
| Upstream-newer / upstream-only | v0.18.0 adds [`apu_sdl2.c`](https://github.com/sp00nznet/xboxrecomp/blob/37b61d589168adee3680fabd51805ee81427bf07/src/apu/apu_sdl2.c), [`mmio_decode_a64.h`](https://github.com/sp00nznet/xboxrecomp/blob/37b61d589168adee3680fabd51805ee81427bf07/src/platform/mmio_decode_a64.h), and SDL keyboard support in [PR #191](https://github.com/sp00nznet/xboxrecomp/pull/191). The local `input/CMakeLists.txt` equals the v0.13.0 reference, whereas the upstream one changed in that PR. Upstream also has the [`Nv2aBackend` contract](https://github.com/sp00nznet/xboxrecomp/blob/37b61d589168adee3680fabd51805ee81427bf07/src/kernel/nv2a_backend.h) and [PR #319](https://github.com/sp00nznet/xboxrecomp/pull/319)'s fixture. These are candidates for adaptation, not drop-in replacements. |
| MA2-specific / local-only | [`apu_dsp_runtime.c`](../../kit/source/runtime/src/apu/apu_dsp_runtime.c) and the [`apu/dsp/`](../../kit/source/runtime/src/apu/dsp/) interpreter are built into the APU; the game includes [`ma2_audio_guest.inc`](../../kit/source/runtime/src/apu/ma2_audio_guest.inc) in [`recomp_manual.c`](../../kit/source/game/src/recomp_manual.c). [`nv2a_gpu_d3d11.c`](../../kit/source/runtime/src/kernel/nv2a_gpu_d3d11.c) and [`pc_frontend.c`](../../kit/source/runtime/src/platform/pc_frontend.c) support the MA2 renderer and host. Local-only means path absence upstream, not necessarily that every line is title-specific. |
| Independently divergent | [`xinput_device.c`](../../kit/source/runtime/src/input/xinput_device.c) contains MA2 frontend/capture and disconnect-pause behavior, while upstream changed the same path for SDL hotplug in [PR #305](https://github.com/sp00nznet/xboxrecomp/pull/305). Both blobs differ from v0.13.0 and each other. Likewise, local [`xbox_memory_layout.c`](../../kit/source/runtime/src/kernel/xbox_memory_layout.c) integrates MA2 GPU memory and mapping behavior while upstream developed POSIX mapping in [PR #60](https://github.com/sp00nznet/xboxrecomp/pull/60) and [PR #191](https://github.com/sp00nznet/xboxrecomp/pull/191); reconcile both sides, not just the platform `#ifdef`s. |
| Independently divergent generator | Local [`lifter.py`](../../kit/toolkit/tools/recomp/lifter.py) and [`recomp_types.h`](../../kit/toolkit/templates/runtime/recomp_types.h) include saved-result flag handling and 80-bit floating-point load/store support; upstream also changed these paths, including [PR #311](https://github.com/sp00nznet/xboxrecomp/pull/311) and [PR #335](https://github.com/sp00nznet/xboxrecomp/pull/335). Each local and v0.18.0 blob differs from the v0.13.0 reference and from the other. The absence of local-only toolkit paths does not mean the toolkit is interchangeable. |

## Windows and generator invariants

- The [game CMake target](../../kit/source/game/CMakeLists.txt) currently
  builds Windows GUI game and launcher targets with resources, Win32/DX11
  ImGui backends and Windows libraries. It discovers generated C with
  `CONFIGURE_DEPENDS`, disables MSVC inlining for two troublesome generated
  chunks, and sets `RECOMP_XMM_INTRIN=0` for the specified 27 chunks to match
  the release. Preserve these Windows choices while adding platform branches.
- [Game startup](../../kit/source/game/src/main.c) defaults to the D3D11
  pushbuffer renderer, vblank and input latch only when the corresponding
  environment variables are unset. The local
  [kernel build](../../kit/source/runtime/src/kernel/CMakeLists.txt) includes
  GPU memory, shader and texture integration; the separate
  [D3D8 target](../../kit/source/runtime/src/d3d/CMakeLists.txt)'s POSIX
  OpenGL path is not a replacement for that renderer.
- Preserve the [guest memory layout](../../kit/source/runtime/src/kernel/xbox_memory_layout.c):
  guest-to-host offset, shared RAM mirrors, tiled/contiguous aliases, device
  apertures, GPU-memory notifications and cleanup. The input implementation's
  gameplay-specific disconnect/focus pause and captured-frontend feedback
  policy also remain part of the Windows behavior.
- The [APU target](../../kit/source/runtime/src/apu/CMakeLists.txt) includes
  the DSP interpreter and samplerate library and links XAudio2 on Windows.
  The local [non-Windows sink](../../kit/source/runtime/src/apu/apu_xaudio2.c)
  is stubbed; linking SDL2 alone is not functional audio. Windows
  [video](../../kit/source/runtime/src/video/CMakeLists.txt) uses Media
  Foundation. Preserve these working Windows paths when adding host backends.
- The installer runs analysis,
  `tools.recomp --all --split 250 --game-name MechAssault2`, then
  [`recover_callbacks.py`](../../kit/source/game/bringup/recover_callbacks.py)
  and [`optimize_leafs.py`](../../kit/source/game/bringup/optimize_leafs.py)
  before patching. The generated dispatch, guest registers, flag/FPU state,
  memory access and template ABI must remain consistent across the lifter,
  templates, runtime and game hooks; see
  [`install.py`](../../kit/installer/install.py) and
  [`recomp_types.h`](../../kit/toolkit/templates/runtime/recomp_types.h).

## Strict patches, FastCode and manifests

This is the existing 1.1.5 repository metadata, not a generated game tree:

| Manifest/specification | Baseline |
|---|---|
| [`kit/patches/manifest.json`](../../kit/patches/manifest.json) | `edits-2`; 113 base files, 116 final files, 59 edited, 3 added, 0 removed. |
| [`fastcode.json`](../../kit/patches/fastcode/fastcode.json) | 14 whole-file selections, 710 hot-list records, 1 forced selection, 68 LF-normalized input files and 135 final file hashes; a post set is enabled. Hot-list records are not a count of all copies produced. |
| [`fastcode/post/manifest.json`](../../kit/patches/fastcode/post/manifest.json) | `edits-2`; 4 base and 4 final files, all 4 edited, 0 added or removed. |
| [`disc-manifest.json`](../../kit/installer/data/disc-manifest.json) | When present, the installer verifies the supported local input files by size and SHA-256; if absent, it explicitly falls back to the executable check. Do not copy its contents or local validation output into port notes. |

The counts can be reproduced without printing hashes, guest symbols or game
data:

```sh
python3 - <<'PY'
import json
import subprocess

rev = "5ecc611d18e20de1ab04313b0c50e453d8d63b29"
def read(path):
    return subprocess.check_output(["git", "show", f"{rev}:{path}"])
for path in ("kit/patches/manifest.json",
             "kit/patches/fastcode/post/manifest.json"):
    m = json.loads(read(path))
    print(path, "format=" + m["format"],
          *(f"{key}={len(m.get(key, []))}"
            for key in ("base", "final", "patched", "added", "removed")))
spec = json.loads(read("kit/patches/fastcode/fastcode.json"))
print("fastcode", *(f"{key}={len(spec.get(key, []))}"
                    for key in ("whole", "force", "lf", "final")),
      "post=" + str(bool(spec.get("post"))))
lines = read("kit/patches/fastcode/fastcode_hot.txt").decode().splitlines()
print("hot-list records", sum(bool(line.split("#")[0].strip()) for line in lines))
PY
```

[`gen_patch.py`](../../kit/installer/gen_patch.py) accepts only known edit
formats, verifies all base hashes (or an already-patched final hash on
resume), replays edits in reverse using references to locally generated lines,
and verifies every final hash. It writes each edited file atomically. The
[installer](../../kit/installer/install.py) regenerates after an interrupted
patch when its inputs no longer match, normalizes the specified FastCode inputs
to LF, generates selected/forced copies locally, applies the strict post set,
and rejects any final FastCode hash mismatch.
[`make_patches.py`](../../tools/make_patches.py) has a generated-looking-line
guard and replays the patch/FastCode pipeline before publishing a patch set.
See [how patches work](../patches.md) and its synthetic
[patch tests](../../tests/installer/test_engine.py).

**Keep generator upgrades separate from host/runtime portability.** A newer
disassembler, lifter, template or split policy can change generated bytes,
line numbers, helper ABI and FastCode eligibility even when gameplay should be
equivalent. The existing base/final/post hashes then correctly reject the
result. Do not bypass these checks to make the port build. A generator upgrade
needs its own reviewed rebaseline: compare synthetic conformance tests,
regenerate from a supported input locally, reconcile the lawful edits and
FastCode selections, replay all manifests, and test Windows as well as the
new host. Do not commit generated game code, input data or private logs.

The upstream survey and related PRs are in [upstream.md](upstream.md).
