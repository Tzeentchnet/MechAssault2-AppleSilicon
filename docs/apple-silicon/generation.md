# Private generation on Apple Silicon

The generation workflow was validated on native macOS arm64 at MA2 source
revision `5ecc611d18e20de1ab04313b0c50e453d8d63b29`, using the embedded
toolkit without upgrading it. This is **not** evidence that the game builds or
runs on macOS.

## Result

| Stage or check | Result |
|---|---|
| Supported retail quick check and complete disc-manifest verification | Pass |
| Analysis, recompilation, and helper generation | Pass |
| Every pristine generated base against the shipped patch manifest | Pass |
| Strict edit/reference replay and every patched final hash | Pass |
| FastCode LF inputs, generated copies, post-patch manifest, and final hashes | Pass |
| Interrupted patch replay, resume, and byte-for-byte comparison of the complete result | Pass |
| Interrupted FastCode-state installer recovery and exact final replay | Pass |
| Completed generation resume and repeat verification | Pass |
| PC overlay manifest, cache marker, and stale-asset regeneration | Pass |
| Native game build, installation, and packaging | Not supported yet |

The disc image, extracted assets, generated sources, state, and detailed logs
remain in private session storage **outside the repository**. Logs and analysis
can contain paths, identities, or game-derived material; do not publish them.
The bonus disc was not used. No manifest was regenerated or bypassed.

## Reproduce

Use the native Python environment and dependencies described in
[tools.md](tools.md). Set the variables below to your own main-disc image, a
new private destination, and private scratch/profile directories. Do not put
the image, extraction, work directory, or logs in Git.

```sh
export OWNED_DISC="<owned main-disc image>"
export PRIVATE_DEST="<new private destination>"
export TMPDIR="<private scratch directory>"
export LOCALAPPDATA="<private profile directory>"
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$TMPDIR" "$LOCALAPPDATA"

.venv/bin/python -B kit/installer/install.py \
  --xiso "$OWNED_DISC" --dest "$PRIVATE_DEST" --until generate --keep-work
.venv/bin/python -B kit/installer/install.py \
  --dest "$PRIVATE_DEST" --until patch --keep-work
.venv/bin/python -B kit/installer/install.py \
  --dest "$PRIVATE_DEST" --until patch --keep-work
.venv/bin/python -B kit/installer/install.py \
  --dest "$PRIVATE_DEST" --pc-assets-only --keep-work
```

The first command performs the existing quick and full disc checks before
analysis. A different or damaged disc must fail those checks. The second
resumes through strict patching and FastCode; the third verifies and skips the
completed stages. If the supported extracted XBE changes, including
same-length corruption, the extraction marker and every downstream generation
stage are invalidated; the next run re-extracts it before analysis continues.
Start with a new destination when pristine pre-patch output is needed. The
separate asset-only mode requires completed extraction and patching, rechecks
the full disc, and verifies the overlay against the shipped asset manifest. It
does **not** mark the build as done or change the Windows build-before-assets
order. Missing or changed generated results are treated as stale on a
subsequent generation run; interrupted patch replay keeps the existing
recovery behavior. Missing, empty, or inconsistent generation manifests are
rejected, not recreated.

Generated translator and helper text uses UTF-8/CRLF to match the frozen base.
FastCode inputs are explicitly normalized to LF and its output is checked
against the existing manifests; patch replay itself preserves the referenced
bytes. The asset cache marker uses the same CRLF bytes as the Windows workflow.
MSVC, the Windows FFmpeg package, and optional enhancement tools are not
prerequisites for these stages. Requesting the Windows build or installation
stages on macOS fails explicitly.

## Host-safe tests

```sh
.venv/bin/python -B -m pytest -q -p no:cacheprovider \
  kit/installer/test_disc_validation.py \
  kit/toolkit/tools/recomp/test_write_if_changed.py \
  kit/toolkit/tools/recomp/test_dispatch_flat.py \
  kit/toolkit/tools/recomp/test_translator_ebp_init.py \
  tests/installer/test_engine.py::GenPatch \
  tests/installer/test_engine.py::Engine::test_patch_after_a_stopped_run_translates_again \
  tests/installer/test_engine.py::Engine::test_step_titles_are_plain \
  -k 'not one_sentence_about_game_data'
```

The excluded text-only check imports the Windows GUI and is not host-safe; the
patch-engine replay, mismatch, interruption, and reference tests run. These
tests use synthetic inputs, not the private disc or generated sources. The
complete patch-engine class also passed with the Windows UI adapter stubbed.
An existing Windows build-root stale-marker test passed with its Windows path
classification simulated; it does not run unchanged on POSIX.
