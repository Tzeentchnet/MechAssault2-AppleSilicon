# Native macOS host shell

The macOS graph builds two native arm64 executables:

- `mechassault2_recomp` is the public-data host shell. It opens an SDL/AppKit
  window, pumps events on the process main thread, and runs a synthetic worker.
- `mechassault2_recomp_guest` is available only when the private generated
  source directory and ABI overlay are supplied. It validates an
  owner-controlled extracted retail directory, initializes memory, kernel,
  storage, APU, USB, generated dispatch, and the NV2A executor, then enters the
  retail XBE on the off-main worker.

The private target now reaches changing Day 1 Studios and MechAssault 2 title
frames through the initial Metal backend with explicit CPU fallback, or through
the CPU-only reference renderer. Deterministic SDL-hosted key actions can
advance it through `Press START`, the campaign hub, and difficulty selection.
The scripted route reaches active first-mission execution, although the
fallback-heavy renderer remains far below real time and physical input,
save/restart, and cooperative shutdown are not accepted; see
[runtime.md](runtime.md).

## Launch and settings

Build the executable as described in [build.md](build.md), then launch it
directly; the Windows launcher is not used:

```sh
_work/apple-silicon/game-no-generated/mechassault2_recomp --help
_work/apple-silicon/game-no-generated/mechassault2_recomp
```

When the private target has been configured as described in
[build.md](build.md), launch it only against the validated owner-controlled
directory:

```sh
MA2_GAME_DIR="<owner-only-validated-extracted-directory>" \
  <private-build>/mechassault2_recomp_guest --safe-start
```

`MA2_GAME_DIR` must name the validated extracted runtime directory, not the
generation workspace. One diagnostic run against the generation workspace
reached generated entry and then faulted at `0x70ABCDEF11` while
`sub_000788C0__fast` followed guest marker `0xABCDEF01`. Restoring the validated
runtime directory eliminated the fault and survived the bounded entry probe.
Treat this signature as invalid runtime input; do not weaken memory or manifest
validation to bypass it.

The private Apple guest selects Metal when `RECOMP_PB_BACKEND` is absent or is
`metal`. Set `RECOMP_PB_BACKEND=cpu` for the CPU-only correctness oracle. Any
other value fails closed before memory or device startup. Metal remains
offscreen and synchronous; SDL/AppKit continues to own the window and final
presentation. The guest also enables the title-required asynchronous file-I/O
and heap-reclamation modes before kernel initialization.

The public shell defaults to a resizable 960x540 logical window,
`safe_start=false`, and `smoke=false`. These are host defaults, not a game
resolution. Closing the window, SIGINT, or SIGTERM requests cooperative
shutdown.

- `--smoke` opens, pumps, and closes the real framebuffer host and exits after
  at least three successful pumps and a worker-start handshake. It does not
  render test frames or run guest code.
- `--safe-start` keeps the public shell behavior deterministic and, in the
  private guest, rearms one movie-skip request for every startup and campaign
  movie screen. Each update consumes the request once, then follows the normal
  seek/stop or completion result, allowing existing exit slots to release the
  movie rather than returning from arbitrary decoder callers. Nested and
  sequential movie state has focused synthetic coverage. A 140-second retail
  route confirms that the one-shot lifecycle leaves the Day 1 logo, passes
  difficulty selection and mission loading, and reaches an active first-mission
  scene without presenting the corrupted campaign movie.
- `--guest-init-only` is private-target-only. It validates and initializes the
  retail runtime, but deliberately skips generated entry so startup/teardown
  can be tested without executing game code.
- `--help` prints usage without creating per-user directories. Unknown or
  positional arguments fail without echoing their contents.

Orderly shutdown and a successful smoke return 0; invalid arguments return 2,
an instance conflict returns 3, and other host failures return 1.

For an isolated, deterministic headless check from the repository root:

```sh
mkdir -p _work/apple-silicon/host-smoke-home _work/apple-silicon/host-scratch
HOME="$PWD/_work/apple-silicon/host-smoke-home" \
TMPDIR="$PWD/_work/apple-silicon/host-scratch" \
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  _work/apple-silicon/game-no-generated/mechassault2_recomp --smoke --safe-start
```

The existing SDL framebuffer attempts accelerated presentation and may use
software presentation if acceleration is unavailable. The shell reports the
actual choice as `presentation_accelerated` or the explicit warning
`presentation_software`; neither is an NV2A/game renderer. The dummy driver
normally selects software. A window, texture, or pump failure is an error.

### Deterministic diagnostic input

The SDL host can schedule input against its own game-facing key state without
injecting desktop events:

```sh
RECOMP_KEYBOARD=1 \
RECOMP_INPUT_LATCH=1 \
RECOMP_UI_SCRIPT='18000:key=13down,24000:key=13up,32000:key=13down,38000:key=13up'
```

`RECOMP_UI_SCRIPT` is a comma-separated list of `<milliseconds>:<action>`
entries. Supported actions are `key=<virtual-key>`, `key=<virtual-key>down`,
`key=<virtual-key>up`, and `close`. A tap sends down then up in one scheduled
action. `RECOMP_INPUT_LATCH=1` retains the press edge until the guest
acknowledges it, which is necessary while CPU-rendered frames take seconds.
Virtual key 13 is Return and maps to Xbox Start in the legacy keyboard layout.
The script clock begins when the SDL host opens, before private guest
initialization. For diagnostic startup that can approach the retail deadline,
set `RECOMP_UI_LIVE` to an owner-controlled command file and append the same
actions, one per newline, only after the structured `guest_entry` event. This
keeps menu timing relative to generated entry rather than variable host
initialization time. The POSIX SDL presenter now implements this append-only
file path; previously only the Windows presenter consumed it, so a macOS run
ignored all queued keys and ended at the watchdog. The framebuffer regression
now appends a Return action after opening the host and requires the latched
press.
This diagnostic is covered by the framebuffer host regression and has advanced
the retail UI through the campaign hub, difficulty selection, mission loading,
and active first-mission execution.

## Ownership and teardown

The process main thread alone invokes `xbox_FramebufferHostOpen`,
`xbox_FramebufferHostPump`, and `xbox_FramebufferHostClose`; the shell rejects
those operations from other threads. The private guest submits completed
guest-memory scanout frames through the bounded handoff; SDL texture upload and
presentation remain on the main thread.

The joinable worker initializes a distinct host thread-local context and
monotonic start time, then runs its supplied initializer followed by its
callback on that same off-main thread. In the private target that initializer
establishes guest register TLS, memory, device, and dispatch state before entry.
Its stop request is synchronized and wakes a waiting callback. The main thread
requests stop and successfully joins the worker **before** closing the SDL
window, restoring handlers, releasing the instance lock, or closing other
resources. It checks SDL video/event ownership on close. Synthetic worker
initialization has a three-second monotonic deadline; private retail
initialization has a bounded 120-second deadline because validated guest memory,
kernel, storage, audio, USB, and renderer setup has exceeded both 30 and 60
seconds under heavy diagnostics or host load. Callback completion has a five-second monotonic deadline before
the blocking `pthread_join`. On a missed deadline the process terminates
without tearing resources out from under the worker, rather than detaching it
or reporting success. Arbitrary blocking thread-exit destructors cannot be
timed out by `pthread_join` on macOS; the synthetic callback installs none, and
the retail guest path must ensure its thread-exit cleanup also terminates.
A CPU diagnostic replay that missed the former 60-second deadline subsequently
reached `guest_kernel_ready`, `host_ready`, and `guest_entry` under the
120-second policy without a guest fault.

The public shell's worker TLS is not guest state. The private initializer now
establishes the guest register set, stack, TIB, XBE TLS, generated dispatch,
thread-scoped intercepted exit, and MMIO producer registration on that same
worker. Kernel-created guest threads register independently. Cooperative
shutdown is still unproven after real retail entry; watchdog probes terminate
diagnostically rather than claiming orderly teardown.

## Paths, instance lock, and privacy

The host requires an existing absolute `HOME` owned by the effective user. It
creates these standard per-user directories, with bounded paths:

| Purpose | Directory |
|---|---|
| Application support and lock | `~/Library/Application Support/MechAssault2` |
| Cache | `~/Library/Caches/MechAssault2` |
| Structured log | `~/Library/Logs/MechAssault2` |

Directories are created with mode 0700 subject to the process umask; the lock
and log are restricted to 0600. Paths, including the lock/log filename and
terminating byte, must fit in 1024 bytes. Overlong, relative, root,
control-character, or `.`/`..`-component homes are rejected. The final `HOME`
component and each created/opened `Library` component are opened without
following symlinks; files must be regular, owned by the effective user, and not
hard-linked.
Failures are reported without printing a path. `HOME` is the documented base,
which also permits the isolated smoke test above.

There are two mandatory locks: a UID-specific Mach bootstrap service in the
current login bootstrap namespace and an advisory lock on the persistent
`host.lock` in Application Support. The first prevents simultaneous instances
with different `HOME` values in the same login namespace; the second also
prevents instances using the same home in different namespaces. A conflicting
instance fails with an instruction to close it and retry. The lock file is not
unlinked, avoiding inode-replacement races; process death releases both locks.
Distinct bootstrap namespaces *and* distinct home directories are separate
profiles and are not globally serialized. The shell uses the macOS
`bootstrap_register` API, which Apple marks deprecated; registration may also
be denied in restricted/sandboxed contexts. Either failure is explicit, not a
fallback.

Host logs are fixed-vocabulary JSON lines written to the original stderr and
`host.jsonl`; no argument, environment value, path, SDL error string, signal
address, register, or stack is interpolated. Runtime/SDL stderr is suppressed
while the shell is active because those strings can contain private paths;
the shell emits sanitized, actionable status instead, including presentation
mode and failures. OS crash reports and diagnostics produced independently of
this process are outside that logging policy.

## Signals and the guest-entry handoff

Before SDL or the worker starts, the shell installs its signal handlers on the
main thread. Inherited custom handlers for its managed signals are rejected
without modification: Darwin's `sigaction` query does not reliably expose an
inherited `SA_RESETHAND` flag, so replacing an unknown handler could silently
change its semantics. The error advises launching without that inherited
handler. An inherited ignored SIGINT/SIGTERM stays ignored. Otherwise those
signals only set a lock-free stop flag; orderly shutdown restores the original
dispositions.

For SIGBUS, SIGSEGV, SIGABRT, SIGILL, and SIGFPE, the handler uses only
lock-free atomics and async-signal-safe operations to write a fixed, redacted
signal record, restores the default disposition, and re-raises the signal.
An ignored synchronous fatal fault is treated as default rather than resumed.
Fatal termination is not an orderly join; the OS tears down the process and
releases its locks. The record is best effort, contains no fault address or
backtrace, and cannot survive every corrupted process or closed output sink.
The private guest installs its MMIO SIGBUS/SIGSEGV layer **after** this shell,
saves the shell's `sigaction`, handles only validated guest MMIO faults after
broker startup, and chains every unhandled fault with the original signal,
`siginfo_t`, and context. It removes the outer handler before shell
restoration. The signal handler itself performs only bounded decode, atomic
slot publication, and a nonblocking wake; device work runs on the prestarted
broker. Synthetic fault, lifecycle, and saturation tests cover this boundary,
and retail entry exercises it through APU, AC97, and OHCI startup.

## Focused validation

The native CMake tests exercise main-thread rejection, worker-local state,
initializer failure, early stop, bounded join and subsequent join, SDL teardown,
inherited-handler rejection, outer-handler chaining, and default signal
termination. Process tests use isolated synthetic homes and the dummy video
driver to check smoke lifecycle ordering, CLI errors, path/symlink rejection,
redaction, simultaneous instances (including different homes), signal shutdown,
fatal diagnostics, and lock reacquisition. See [build.md](build.md#focused-tests)
for the commands. Those public tests do not use retail data. Separate private
validation establishes generated arm64 entry and title frames, but not campaign
gameplay or the final 1080p30 gate.
