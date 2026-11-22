# Tools and workflows

This inventory separates tools needed to make the game run from the editor and
release tooling that can wait. A tool being written in Python or portable C/C++
does not by itself prove that its complete workflow works on macOS.

## Tool inventory

| Tool or area | Role | Apple Silicon work |
|---|---|---|
| [xiso](../../kit/toolkit/tools/xiso/) | Read or extract an owned Xbox disc image for local development. | Run against local input, verify failures are explicit, and keep all output outside Git. |
| [xbe_parser](../../kit/toolkit/tools/xbe_parser/) | Parse XBE metadata and emit analysis used by later stages. | Verify deterministic output with the supported title; do not commit the generated analysis if it contains excluded data. |
| [disasm](../../kit/toolkit/tools/disasm/), [func_id](../../kit/toolkit/tools/func_id/), and [abi_analysis](../../kit/toolkit/tools/abi_analysis/) | Recover functions, identify libraries, and infer guest calling conventions. | Establish which stages are pure Python and which invoke host-specific binaries. Record timings and failures. |
| [recomp](../../kit/toolkit/tools/recomp/) | Lift guest x86 into generated C and dispatch metadata. | Verify the generated dialect and helper headers compile with Apple Clang/arm64. Generated output remains local. |
| [doctor.py](../../kit/toolkit/tools/doctor.py) and [sdk_validate.py](../../kit/toolkit/tools/sdk_validate.py) | Diagnose toolchain artifacts and validate SDK assumptions. | Prefer extending reusable diagnostics over adding one-off game scripts. |
| [install.py](../../kit/installer/install.py) | Orchestrate disc validation, extraction, analysis, recompilation, patching, build, assets, and installation. | The current end-to-end flow is Windows-oriented. Reuse its stage boundaries, but do not make the GUI installer the first port target. |
| [make_patches.py](../../tools/make_patches.py) | Convert local changes to generated code into the repository's non-infringing patch representation. | Preserve exact replay and bulk-derived-code safeguards on every host. |
| [fetch_deps.py](../../tools/fetch_deps.py) | Fetch and verify pinned third-party dependencies. | Add platform artifacts only with pinned sources, full-file verification, and matching notices. |
| [privacy_scan.py](../../tools/privacy_scan.py) | Check release output for local paths and identities. | Retain this gate for future macOS bundles and debug symbols. |
| [editor format code](../../editor/src/core/) and [model library](../../editor/models/) | Read/write MGF, WDF, scenarios, models, and textures. | Useful CPU-only format code, but the editor GUI is not an early milestone. Make tests host-buildable before porting UI. |
| [fact wiki query](../wiki/wq.py) | Search existing reverse-engineering and runtime findings. | Use it before duplicating an investigation; add facts through the wiki's established process. |

## Native Apple Silicon setup

The setup below is for native macOS arm64, not an x86 shell under Rosetta. It
needs Xcode or the Command Line Tools, Git, Homebrew, CMake 3.20 or newer, and
the Homebrew formulae `python@3.13`, `pkgconf`, `sdl2-compat`, `libepoxy`, and
`openssl@3`. Check the preinstalled formulae with:

```sh
brew list --versions python@3.13 cmake pkgconf sdl2-compat libepoxy openssl@3
```

If any are missing, install only those prerequisites before running the
scripts; for example, the following installs the complete prerequisite set:

```sh
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 \
  brew install python@3.13 cmake pkgconf sdl2-compat libepoxy openssl@3
```

The scripts do **not** run `brew install` or otherwise install system packages.
Homebrew formulae are not version-locked by this repository; the versions below
are the observed baseline, not a guarantee about future formula revisions.

### Prepare and check

From the repository root, set `XBOXRECOMP_DIR` to an **unused path outside this
repository**, such as a directory under your session's `files/` area. Replace
the placeholder before running the commands:

```sh
export XBOXRECOMP_DIR="<outside-repository>/xboxrecomp-v0.18.0"
GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch v0.18.0 --single-branch \
  https://github.com/sp00nznet/xboxrecomp.git "$XBOXRECOMP_DIR"
test "$(git -C "$XBOXRECOMP_DIR" rev-parse HEAD)" = \
  37b61d589168adee3680fabd51805ee81427bf07
test -z "$(git -C "$XBOXRECOMP_DIR" status --porcelain --untracked-files=normal)"

PYTHON313="$(brew --prefix python@3.13)/bin/python3.13" \
  tools/apple_silicon/setup.sh
tools/apple_silicon/check.sh "$XBOXRECOMP_DIR"
```

For a checkout that already exists, omit `git clone` and run the two `test`
commands before setup. The check refuses an in-repository checkout, a different
commit, or tracked/non-ignored untracked changes. It never resets that checkout.
Setup is non-interactive and repeatable: it creates or reuses only the ignored
`.venv`, uses hash-pinned wheels (arm64/universal2 for native packages) from
[`requirements.txt`](../../tools/apple_silicon/requirements.txt), and runs
`pip check`. It refuses an incompatible, non-isolated, or symlinked `.venv`
rather than overwriting it. No shell profile or global Python is modified.
`PYTHON313` may instead name any native Python 3.13 executable.
Pillow matches the existing Enhancer pin for synthetic image/video tests; this
does not install the optional GPU Enhancer or PyTorch.

The installer's separately locked `kit/toolkit/vendor/capstone` contains the
Windows `capstone.dll`. It is not the native macOS wheel; do not put that vendor
directory ahead of `.venv` on `PYTHONPATH` for these checks. This setup does not
port the Windows-oriented installer orchestration.

The check validates native imports, exercises Capstone on synthetic x86 bytes,
and verifies the installed library came from the wheel. It resolves SDL2,
libepoxy, and OpenSSL with pkg-config and CMake, links and runs a small native
probe, and configures the embedded runtime. It then runs focused upstream Python
tests plus standalone ARM64 MMIO and C/C++ input-mapping CMake tests. Skipped or
expected-failing selected Python tests make the check fail; the MMIO executable
is also run directly so CTest's skip return code cannot count as success.
Outputs go under ignored `.venv/` and `_work/apple-silicon/`. No disc data,
Docker images, Rosetta, or FFmpeg are involved.

### Using the dependencies in another CMake build

OpenSSL is keg-only on the verified host, so a plain `pkg-config openssl` did
not find it. The check script sets these paths for its own subprocesses; to use
them in the current shell:

```sh
SDL="$(brew --prefix sdl2-compat)"
EPOXY="$(brew --prefix libepoxy)"
OPENSSL="$(brew --prefix openssl@3)"
export PKG_CONFIG_PATH="$SDL/lib/pkgconfig:$EPOXY/lib/pkgconfig:$OPENSSL/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$SDL:$EPOXY:$OPENSSL${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export TMPDIR="$PWD/.venv/.tmp"
pkg-config --modversion sdl2 epoxy openssl
cmake -S kit/source/runtime -B _work/apple-silicon/local-runtime \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL2_DIR="$SDL/lib/cmake/SDL2" \
  -DOPENSSL_ROOT_DIR="$OPENSSL" -DOPENSSL_INCLUDE_DIR="$OPENSSL/include" \
  -DOPENSSL_SSL_LIBRARY="$OPENSSL/lib/libssl.dylib" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL/lib/libcrypto.dylib"
```

This configures the local runtime; it does **not** prove that the entire runtime
or game compiles, links, or runs. FFmpeg remains a separate future requirement
for the game's native movie path, subject to the existing licensing and
dependency-verification rules. The trimmed local toolkit also does not contain
the historical `tools/macos/setup.sh`; the current upstream script has different
side effects and is not run here. See [upstream.md](upstream.md).

### Sanitized validation record

Observed on 2026-11-21, using an Apple M5, macOS 27.2.1 arm64, Xcode 27.2,
and Apple Clang 21.0.0:

| Component | Verified version |
|---|---|
| Homebrew Python | 3.13.17, native arm64 |
| Python packages | Capstone 5.0.7, pytest 8.3.5, pefile 2024.8.26, NumPy 2.2.3, Pillow 11.1.0 |
| pytest dependencies | iniconfig 2.0.0, packaging 24.2, pluggy 1.5.0 |
| Native tools | CMake 4.4.4, pkgconf 3.0.8 |
| Native libraries | SDL2 compatibility 2.32.74, libepoxy 1.5.10, OpenSSL 3.5.9 |
| Separate xboxrecomp checkout | v0.18.0, `37b61d589168adee3680fabd51805ee81427bf07`, clean |

`setup.sh` is repeatable; `pip check` found no broken requirements and the
Capstone, NumPy, and Pillow native binaries were arm64. `check.sh` passed its
dependency probe (1/1), local-runtime configure, skip-guard tests (4 passed),
and upstream Python selection (38 passed, no skips). The upstream MMIO test
passed (1/1; `30 traps, 1 unknown, 0 failures`; the unknown instruction is
deliberately rejected). Its standalone input-mapping tests passed for C and
C++17 (2/2). No upstream checkout files were changed.

The embedded toolkit's corresponding Python tests also passed (12/12):

```sh
REPO_ROOT="$(pwd -P)"
(
  cd kit/toolkit
  TMPDIR="$REPO_ROOT/.venv/.tmp" PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1 \
    PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 PYTHONPATH="$REPO_ROOT/tools/apple_silicon" \
    "$REPO_ROOT/.venv/bin/python" -m pytest -q -ra -p no:cacheprovider -p no_skips \
    tools/disasm/test_decode_at.py tools/recomp/test_lifter_bit_scan.py
)
```

Docker, Colima, Wine, NASM, FFmpeg, 7z, and ShellCheck were absent from `PATH`;
none was installed. Shell syntax was checked with `bash -n`. No Rosetta receipt
was found, but that alone does not establish whether Rosetta is installed.
Upstream's container-based 32-bit x86 conformance suite was **not run** and
requires additional tooling/images.

## Local development workflow

1. Keep disc images, extracted files, generated C, and build products outside
   tracked source directories. The root `.gitignore` is a safety net, not a
   license to place assets in the tree.
2. Start with a configure-only or compile-only milestone that does not need game
   data.
3. Once generation is needed, use the repository's validation stages against a
   locally owned supported disc and keep the work directory.
4. Make guest-code changes through `make_patches.py`; never commit the generated
   before/after files.
5. Run the smallest applicable CPU/runtime test before launching the title.
6. Sanitize commands and logs before adding them to an issue or pull request.

## Upstreaming workflow

This clone uses:

- `origin`: the Apple Silicon fork.
- `upstream`: `Mazillion/MechAssault2-PC-Recomp`.

For each change:

1. Classify it as reusable xboxrecomp runtime/tooling work, MechAssault 2 game
   integration, or Apple-specific packaging.
2. Reproduce the issue with the smallest suitable target. A generic runtime
   test is preferable to requiring MechAssault 2.
3. Keep reusable fixes independent of game-specific work where practical.
4. Add a focused regression test or explicit validation command.
5. Record the candidate upstream repository and any dependency between pull
   requests in [issues.md](issues.md).

Do not hide a game-specific assumption inside a generic compatibility layer to
make the port appear farther along.

## Command record template

```text
Date:
Host: macOS version, chip, architecture
Revision:
Issue or milestone:
Command:
Expected:
Actual:
Artifact or sanitized log:
Conclusion:
```

Commands that name a private home directory or disc path must be rewritten with
placeholders before they are committed.
