# Apple Silicon runtime and retail bring-up

The native arm64 runtime now enters the generated retail executable, completes
kernel, storage, audio, USB, and renderer initialization, and reaches the
first campaign mission through the CPU NV2A reference executor and SDL
presenter. The private guest also registers the first synchronous downstream
Metal slice, with explicit CPU fallback for unsupported state. Scripted input
has established active mission execution. The primary fragmented-geometry bug
is fixed, and a matching retail A/B confirms the major purple-floor bands and
widespread horizontal flecks are gone after the dependent texture-stage work.
Thin coloured lines still need classification, lifted Bink output remains
corrupt, CPU rendering remains roughly 1-2.5 FPS depending on scene and worker
count, and physical controls/audio plus cooperative shutdown are unverified.
This is not yet an accepted playable port or a complete Metal renderer. The work
reconciles the MA2 runtime at
[`5ecc611d18e20de1ab04313b0c50e453d8d63b29`](https://github.com/Mazillion/MechAssault2-PC-Recomp/commit/5ecc611d18e20de1ab04313b0c50e453d8d63b29)
against the clean xboxrecomp v0.18.0 release at
[`37b61d589168adee3680fabd51805ee81427bf07`](https://github.com/sp00nznet/xboxrecomp/commit/37b61d589168adee3680fabd51805ee81427bf07).
See [baseline.md](baseline.md) for the frozen inventory and
[upstream.md](upstream.md) for the upstream policy. No runtime directory or
commit was copied wholesale or cherry-picked.

## Provenance and adaptations

The following paths are in the pinned **upstream** checkout. Each was a
source or contract reference for the corresponding local runtime work; the
local files were reconciled with MA2's independently evolved implementation.

| Area | Upstream v0.18.0 reference paths | Local adaptation under `kit/source/runtime` |
|---|---|---|
| Mapping and memory | `src/platform/win32_compat.c`, `src/platform/win32_compat.h`, `src/kernel/xbox_memory_layout.c`, `src/kernel/xbox_memory_layout.h`, `src/kernel/guest_vmem.c`, `src/kernel/guest_vmem.h` | Darwin fixed reservations, mirror ownership, free-run queries, cleanup and retry. MA2's RAM, contiguous/tiled aliases, apertures, allocator, diagnostics, and guest data layout remain. |
| Trapped devices | `src/platform/mmio_decode_a64.h`, `src/apu/apu_mmio_hook.c`, `src/usb/ohci.c`, `src/usb/ohci.h` | Reconciled ARM64 decoding, APU/AC97/OHCI dispatch, OHCI register lanes and worker lifecycle, with MA2 DSP register and descriptor/report behavior retained. |
| Audio | `src/apu/apu_sdl2.c`, `src/apu/CMakeLists.txt` | SDL queue implements MA2's existing `xa2_*` acceptance and wait contract; the VP/DSP monitor and software mixer remain authoritative. |
| Input | `src/input/xinput_device.c`, `src/input/xinput_xbox.h`, `src/input/keyboard_sdl.c`, `src/input/input_map.c`, `src/input/input_map.h` | SDL physical-to-Xbox mapping and instance-ID hotplug are reconciled with MA2's frontend, bindings, capture, active-pad selection, disconnect pause, feedback policy, and OHCI report path. Upstream's optional generic remapping settings are not copied over MA2's binding system. |
| Presentation | `src/video/fb_present.c`, `src/video/fb_present_sdl.c`, `src/video/CMakeLists.txt` | A main-thread SDL window and a bounded frame handoff are host primitives for a later reference presenter; they are not an NV2A implementation. |
| Embedding | `CMakeLists.txt` | `XBOXRECOMP_BUILD_D3D8` defaults to `ON`; an embedder can set it `OFF` or supply an `xbox_d3d8` target before adding the runtime. |
| Renderer contract | `src/kernel/nv2a_backend.h`, `src/kernel/nv2a_pb_exec.c`, `tests/nv2a_ffp_backend/nv2a_backend_fixture.c`, `tests/nv2a_ffp_backend/nv2a_backend_contract_test.c`, `tests/nv2a_ffp_backend/nv2a_present_sync_test.c` | A versioned, opt-in MA2 backend seam and synthetic fixture follow the renderer-neutral, host-owned presentation direction, but are **not ABI-compatible** with upstream's stage-0 contract. |

The relevant merged upstream directions are
[#20](https://github.com/sp00nznet/xboxrecomp/pull/20),
[#60](https://github.com/sp00nznet/xboxrecomp/pull/60),
[#185](https://github.com/sp00nznet/xboxrecomp/pull/185),
[#189](https://github.com/sp00nznet/xboxrecomp/pull/189),
[#191](https://github.com/sp00nznet/xboxrecomp/pull/191),
[#203](https://github.com/sp00nznet/xboxrecomp/pull/203),
[#211](https://github.com/sp00nznet/xboxrecomp/pull/211),
[#285](https://github.com/sp00nznet/xboxrecomp/pull/285),
[#304](https://github.com/sp00nznet/xboxrecomp/pull/304),
[#305](https://github.com/sp00nznet/xboxrecomp/pull/305), and
[#319](https://github.com/sp00nznet/xboxrecomp/pull/319). They are provenance
and design references, not claims that those commits were applied verbatim.
The open upstream Metal and D3D11 work in [upstream.md](upstream.md) remains
prior art rather than copied implementation; the local downstream Metal slice
uses the MA2 backend contract.

## Current retail execution boundary

The private `mechassault2_recomp_guest` target is a native arm64 Mach-O. It
loads a validated owner-controlled retail directory, establishes the complete
guest memory and generated ABI, starts the POSIX MMIO broker and devices, and
enters the generated XBE on the off-main guest thread. Retail data, generated
code, captures, symbols, and private logs remain outside Git.

The CPU reference path and the initial Metal-plus-fallback path both produce
changing 640x480 frames. Internal framebuffer readback has confirmed:

- the Day 1 Studios logo;
- the MechAssault 2: Lone Wolf title screen and `Press START` prompt;
- the campaign hub and difficulty-selection dialog after deterministic
  key actions delivered through the SDL host;
- the campaign transition and mission-loading screen;
- an active first-mission scene with characters, animation, audio processing,
  and continuously changing frames;
- the retail in-mission pause menu, including `Resume Game`, after the
  cinematic route advances;
- authoritative QueueFlip scanout alternating between the two expected
  surfaces.

A 330-second control probe appended B, held-forward, camera-left, and fire
commands after mission entry. Its 243 interval-two captures contained 197
unique images, but the commands all landed during the letterboxed in-engine
cinematic: no HUD or unambiguous player-controlled response appeared. The
process exited with the configured watchdog status 3 because the harness
scheduled `close` after the 330-second watchdog, not because of a renderer or
memory fault. Player control therefore remains unproven.

The dotted diagonal geometry was caused by clipping only against positive W.
The vertex-program epilogue already emits screen-space coordinates, so the
executor now converts each position to `(x*w, y*w, z*w, w)`, clips against W
and the homogeneous near-Z plane, then divides by W. A controlled
`RECOMP_CLIP_NEAR_Z=0` A/B reproduced the streaks; the default near-Z path
removed them from the same title frames. Triangle-strip winding was not the
cause of those diagonal streaks, but it later proved to be a separate source
of registered-fallback fragmentation: odd triangles now swap their first two
vertices before software culling, matching hardware strip semantics.
`RECOMP_CLIP_NEAR_Z=0` and `RECOMP_STRIP_ALT_WINDING=0` are diagnostic only.

First frame, menus, mission loading, and active mission execution are
established, but complete visual and behavioral parity are not. A native
process sample also resolved the apparent post-title “GPU event hang”: the
NV2A executor remained inside triangle rasterization while repeated telemetry
showed DMA GET catching DMA PUT between frames. The title waits on the
notification event at the device's `+0x1DCC` when the pushbuffer is full;
NV097 software method 5 signals that event through the original miniport
callback. The wait is valid backpressure, not a missing event. Removing the
pathological near-plane slivers improved the settled title rate to about 1.15
unique FPS. Rendering complete strips measures roughly 1.07-1.14 FPS in
complex title/hub scenes; the first active-mission probe remained CPU-bound
at roughly 1.18-1.41 FPS. A later corrected mission scene settled near
1.37 FPS and 728-734 ms of draw work per frame in serial mode, far below the
30 FPS target.

### Retail cache lifecycle

The title's cache copy is asynchronous and has the following verified order:

1. open the source resource on `D:`;
2. query free space on `Z:`;
3. create `Z:\filecopy.tmp`;
4. reserve the final length with `FileAllocationInformation`;
5. copy 128 KiB chunks using the requested asynchronous completion contract;
6. promote the temporary file with `FileRenameInformation`;
7. reopen and mount the cached resource.

The POSIX backend now implements allocation without changing logical EOF and
rename with counted Xbox names, absolute paths, root-relative paths,
replace/no-replace behavior, and collision errors. Darwin first tries
`F_PREALLOCATE` contiguous allocation, then general allocation; unsupported
hosts use a portable keep-size fallback. Disk-full and invalid-handle failures
remain errors. Missing allocation previously produced archive error 6; missing
rename produced error 7. Fresh-cache retail validation now completes all three
resource promotions without either archive error or the disc-error loop.

`RECOMP_ASYNC_IO=1` is required for this title: synchronous completion leaves
the first 128 KiB request in a state the copy worker does not consume.
`RECOMP_HEAP_RECLAIM=1` is also used by retail bring-up so released guest heap
and contiguous blocks can be reused. The private Apple guest now enables both
as title-scoped defaults before kernel initialization; they remain opt-in for
generic xboxrecomp embedders.

### Host-page and device behavior

On Darwin the runtime reserves fixed guest views without replacing unrelated
host mappings and tracks ownership of each mirror slice. Required mapping and
protection failures fail initialization. `VirtualProtect` and commits into
existing reservations refuse a partial host-page protection change rather
than silently changing adjacent 4 KB Xbox pages; an already-identical
protection is allowed. Consequently a 4 KB commit into an uncommitted 16 KB
host page fails explicitly, which a future host or guest-memory redesign must
handle. `VirtualQuery` reports protection boundaries and advances across
free runs, but it knows only mappings registered by this shim; an attempted
fixed reservation may still fail on an unrelated host mapping.

The contiguous allocator needs a stricter rule than ordinary guest mappings.
Xbox allocations are expressed in 4 KiB pages, but a released GPU allocation
can retain read-only protection until it is reused. Reprotecting a 4 KiB
subrange of a mixed 16 KiB Darwin page fails, which previously produced
`cannot reset allocated pages` immediately before a mission-load `SIGBUS`.
On POSIX hosts whose page size exceeds 4 KiB, contiguous allocations are now
aligned and rounded to the host page size so independently protected blocks
never share a host page. A regression protects an allocation read-only,
releases it, reallocates it, verifies host-page alignment and rounded size,
restores writability, and writes through the returned range. Both injected and
production memory-dispatch variants pass. A post-fix 112-second scripted
route under a 120-second watchdog reached active mission execution without the
reset error or signal fault.

The MCPX aperture has a guest view and an unprotected shared host alias.
Whole host pages are protected for APU registers, DSP reset registers, AC97,
and OHCI as needed. Ordinary neighbouring bytes continue to refer to the
same backing through the alias. Fault decoding reconstructs the instruction's
effective address, because Darwin can report the first inaccessible byte of
a cross-page transaction rather than its first byte. Unsupported instructions,
exclusive accesses, unsupported vector-pair forms, and non-owned addresses
are rejected without advancing the faulting host instruction. Opaque arm64e thread
state is explicitly unsupported.

`xbox_OhciIsEnabled()` allows a host to check the opt-in controller after
initialization. `xbox_OhciShutdown()` stops its worker and restores trapped
pages; a zero return means the host must retain the memory and retry. The
NV2A acknowledgement worker is joined before its memory is unmapped.
`xbox_MemoryLayoutShutdownChecked()` reports incomplete cleanup and retains
the remaining ownership records for a retry; initialization is refused until
cleanup succeeds. The existing void shutdown entry point remains available
and logs failures.

### Audio, input, and presentation

The SDL audio backend queues 48 kHz stereo signed 16-bit PCM produced by the
existing MA2 DSP/VP and mixer. It starts after two real blocks, bounds the
queue, and reports ready/timeout/device-or-stall failure to
`xa2_wait_for_space`. `RECOMP_AUDIO_BUF_SAMPLES` accepts multiples of 256
from 256 through 2048; `RECOMP_AUDIO_BUF_COUNT` accepts 2 through 8.
Partial monitor blocks are discarded on restart. A failed SDL device is
reported rather than silently replaced; recovery requires backend restart.
On POSIX, guest-backed APU initialization fails unless the matching guest
RAM and MCPX host alias are available; a null RAM pointer remains supported
for unbacked synthetic use.
Windows still uses its existing XAudio2 and waveOut paths, and ARM64 uses the
scalar libsamplerate path rather than x86 SIMD.

SDL controllers retain stable instance identities across hotplug, Xbox button
and axis widths, MA2's active-controller and disconnect behavior, and the
existing OHCI gamepad report delivery. The host window pumps SDL events on
the process main thread; guest threads may submit frames and query input.
The framebuffer handoff supports X8R8G8B8 and RGB565 and keeps only bounded
latest frames. Window/input shutdown must occur after their consumers stop.

The bundled D3D8/OpenGL compatibility target is **not** MA2's active NV2A
pushbuffer renderer. When it is omitted, no libepoxy lookup is needed. A
Windows host using the D3D8-dependent FMV player must still provide the
appropriate implementation. MA2's existing Windows D3D11 renderer, shader
cache, timing, diagnostic controls, DSP, and frontend behavior remain.

### Downstream backend contract

Register `Nv2aBackend` before the pushbuffer executor starts; unregister or
replace it only after that executor stops. The callbacks run synchronously
on the executor. The MA2 descriptors carry all four texture stages, register
combiners, surfaces, vertex programs, constants, and guest buffer attributes,
rather than upstream v0.18.0's smaller transformed stage-0 triangle interface.
A callback returning “not handled” for a clear or draw must have issued no
commands, so CPU fallback is safe. Synchronization/readback failure latches
and suppresses the snapshot and frame dumps instead of presenting stale
data. Optional `present_sync` prepares the selected surface; readback must
complete into guest memory before the host snapshots QueueFlip's authoritative
scanout. `flip` follows snapshot/dump and never owns the host window.

The Apple guest now registers a synchronous offscreen Metal implementation by
default; `RECOMP_PB_BACKEND=cpu` selects the CPU-only oracle. It supports
masked pitched clears and transformed A8R8G8B8 triangles with Z16/Z24S8
depth, blend, cull, and scissor state, plus supported 2D textures, register
combiners, and shaded alpha tests. Unsupported texture or draw state,
stencil tests, polygon offset, unsafe aliases, and unsupported formats decline
before submission. Issued command or readback failures latch the backend and
discard the frame rather than pretending the CPU can replay it.

Retail telemetry confirms registration and correct presentation without a
Metal failure. In the current title route, visibility counting and unsupported
draw state keep every sampled draw on the CPU. The backend accepted and
submitted the initial clear, but the executor's visibility-count gate prevented
the approximately 2,793 batches per frame in the settled scene from invoking a
Metal draw callback. This slice therefore establishes the contract and
correctness boundary, not meaningful acceleration yet. `RECOMP_PERF=1` emits
accepted/declined Metal operation and submission counters. The contract is not
a promise of source or ABI compatibility with later upstream revisions.

Investigation of `SET_ZPASS_PIXEL_COUNT_ENABLE` confirms that the executor
currently excludes registered hardware draws while counting is enabled, for
both programmable and legacy batches. The CPU retains the 32-bit count and
implements `CLEAR_REPORT_VALUE` and `GET_REPORT`; Metal has no counted-draw
result path yet. Exact Metal visibility counting and its failure, fallback,
and report-ordering tests remain unimplemented and unverified.

The CPU fallback also preserves NV2A type-2 colour surfaces in Morton order.
These surfaces are render-to-texture inputs; clearing or rasterizing them
row-major makes later sampling appear as scattered rectangular blocks. The
local executor now computes the swizzled extent for overlap validation, applies
clear rectangles in Morton order, and uses the same address mapping for
blending, colour masks, and pixel writes. `RECOMP_SWIZZLED_RT=0` remains a
controlled comparison switch. A synthetic 64x64 target verifies both a
partial clear and a two-triangle quad at their exact Morton indices. In a
controlled retail comparison, campaign-hub captures 5 through 27 were
byte-identical with the path enabled and disabled, and later dynamic mission
frames retained the same qualitative corruption. This is a required
title-independent correctness fix, but it is not the primary cause of the
current MA2 artifacts.

A separate CPU-oracle comparison isolated the major fragmented-geometry
failure. The CPU-only path rendered coherent mission characters, while merely
registering Metal enabled software face culling and exposed incorrect CPU
triangle-strip decomposition: every strip triangle used `(i, i+1, i+2)`, so
odd triangles had the opposite hardware winding and were culled. The
transformed/program and legacy CPU paths now share alternating odd-triangle
winding. A backend-declines-draw regression renders a culled four-vertex
program strip and requires the complete rectangle rather than one half.
Corrected retail hub and cinematic models are coherent.
`RECOMP_STRIP_ALT_WINDING=0` exists only for controlled diagnosis.

Texture addressing is also reconciled with the pinned upstream behavior.
Mirror mode reflects alternating integer coordinate intervals, while BORDER
sampling distinguishes the texture-format border-source bit from the
programmed `SET_TEXTURE_BORDER_COLOR` value. Exact textured-quad regressions
cover both behaviors. Corrected retail comparisons retained the remaining
white blocks, establishing that these semantics were necessary but were not
their source.

An active-mission frame trace then exposed a CPU texture-shader gap. Twenty-four
draws used programs `0x02621` or `0x22621`, whose stage sequence includes
`DOTPRODUCT` followed by the dependent `DOT_ST` lookup, and another draw used
`0x001E1` with a dependent AR lookup. The CPU executor implemented only modes
zero through five and silently returned zero for all three required modes.
It now follows the existing D3D shader behavior for stage-input selection,
the NV2A dot mappings, bump environment/luminance, dot accumulation,
dependent/cube/reflection modes 9 through 18, and dependent AR/GB lookups.
Reserved or unknown stage modes emit a one-time diagnostic instead of remaining
silent. A focused executor regression renders controlled `0x02621`, `0x001E1`,
and bump/reflection quads and requires their dependent texels through the final
combiner. A subsequent traced title frame recorded texture-control word
`0x00000041` on `0x02621`, exercising the signed stage-1 and HILO stage-2 dot
mappings rather than only the unsigned default; it emitted no unsupported
texture-stage diagnostic.

The matching retail comparison changed the first-mission floor from large
solid purple bands to a coherent dark surface and removed the widespread white
horizontal flecks from the same character shot. A later full-frame trace also
exercised dependent program `0x085E1`, and the selected floor pixel transitioned
from `0xBD060B15` to black during the composite without an unsupported-stage
diagnostic. Remaining thin purple/yellow lines around lamps and vehicle
geometry may be emissive detail; they are not yet classified as intended or
incorrect. This is evidence for the major texture-stage fix, not a claim of
complete mission visual parity.

CPU-only frame traces also read the mapped colour surface and host-side depth
buffer directly when the hardware debug-pixel hook is unavailable. This makes
`RECOMP_FRAME_TRACE_PIXEL` and `RECOMP_FRAME_TRACE_DEPTH` useful on macOS
instead of reporting only triangle coverage.

The CPU oracle also has an opt-in POSIX row-split raster pool for large
triangles. It uses persistent pthread workers, per-worker counters and DXT
decode caches, joins the workers before guest-memory teardown, and keeps
colour-surface feedback draws serial. A 192x192 synthetic quad checks every
output pixel with four workers and then exercises orderly shutdown. POSIX
remains serial by default: an earlier compatibility-event implementation
regressed retail frame time, so `RECOMP_RASTER_THREADS` will not become a
default optimization without a new controlled retail A/B.

An initial four-worker retail route improved a representative first-mission
passage from roughly 1.37 FPS serial to about 2.4-2.5 FPS, but the fixed
wall-clock input schedule reached different cinematic frames and later dense
shots fell below 1 FPS. The result proves useful parallel work but is not a
hash-aligned acceptance A/B and does not justify changing the default.

### Movie decode boundary and safe skipping

The remaining cinematic blocks are upstream of rasterization. A traced frame
contains one four-vertex fan, two rasterized triangles, a 640x480 output
surface, and one 720x480 linear X8R8G8B8 texture covering the screen. The
blocks are already present in that decoded/uploaded texture; NV2A only scales
and composites it. This separates movie corruption from the fixed strip and
near-plane geometry bugs.

The repository has an FFmpeg-based native Bink implementation, but the Apple
guest currently links no-op native decode/copy/release contracts. Playback
therefore falls through to the lifted retail decoder, where an ARM64
translation or media-copy defect remains possible. The current campaign
milestone does not claim native movie playback.

Safe-start instead rearms one request when each title/legal or attract/campaign
movie screen initializes. The update consumes it only once, allowing the
player's next update to finish its normal seek/stop transition rather than
restarting the skip every frame. The attract/campaign screen returns its normal
completion result so its existing exit slot releases the movie object. Native
fullscreen lifecycles rearm only when their movie count changes from inactive
to active, not for nested increments. Nested and sequential movie state plus
ordinary one-shot user skipping have focused coverage. Native FFmpeg Bink
playback remains a later parity task unless a required transition cannot be
skipped safely.

Retail integration confirmed why the request must be one-shot. Keeping it
asserted on every update made the Title/Legal player repeat its seek/stop path
and alternate indefinitely between the Day 1 Studios logo and its fade. With
per-lifecycle rearming, a 140-second scripted replay left that logo, traversed
difficulty selection and mission loading, and reached an active first-mission
scene without presenting the corrupted campaign movie. The run ended only at
the scripted close and the already-known non-cooperative retail join failure.

## POSIX MMIO signal contract and host handoff

The host, not the runtime, owns `SIGBUS` and `SIGSEGV` installation and
chaining. Install `SA_SIGINFO` handlers before any device pages are protected;
their `sa_mask` must block both signals while either handler runs. For genuine
synchronous memory faults, the outer handler calls
`xbox_PosixMmioFault(context, siginfo->si_addr)`; chain user-generated signals
without treating them as MMIO faults. Return only when it returns 1; on 0,
chain using the host's normal signal-safe policy. Returning to an unhandled
fault without resolving it just faults again.

The AArch64 runtime handler never calls a device, pthread operation, allocator,
stdio, SDL, clock, or logger. It reads the saved instruction and register
context, makes bounded volatile copies of its own signal frame, performs
lock-free atomic operations, and writes one byte to a nonblocking pipe. It
saves/restores `errno`; `_exit(124)` is the explicit failure path if an
accepted request exceeds the bounded spin budget. The unsanitized arm64
object's handler call graph was checked for library calls: notification uses
only `errno` access and `write`, and the failure path uses `_exit`.

Each registered producer has one of 32 permanent slots. Its atomic state
includes a generation advanced on release, so concurrent unregister/register
cannot make a scan claim another thread's slot through mixed stack bounds or
ABA. Slots retire rather than wrapping their generation. The handler first
decodes into a private context and records at most two operations, without
touching the device. It publishes the complete request with release ordering
and waits for the prestarted broker. The broker serializes device callbacks,
including OHCI trace output, outside signal context; AC97 reset bits are
cleared before the corresponding bytes are published. Only a fully accepted
request is replayed into the saved context. An instruction spanning distinct
trapped devices is rejected before either device is touched; ordinary alias
bytes alongside the selected device remain supported. OHCI checks worker
availability and performs the complete instruction under its controller lock,
serialized with worker exit. Unsupported instructions,
unowned addresses, absent APU, a stopped OHCI worker, unregistered or
stopping producers, and full registration capacity fail without changing the
context or fabricating a read. A full or interrupted wake pipe cannot lose a
request: slots remain published and the worker also polls and scans them.
If a callback stalls, the guest is not resumed with invented data; the process
terminates with status 124. The budget is an iteration count, not a wall-clock
deadline, and busy-waiting and serialized device work have not been
performance-qualified.

### Required T09 call order

1. Install the host handlers and chain policy. Keep `SIGBUS` and `SIGSEGV`
   unblocked on every producer outside the handler. Call
   `xbox_PosixMmioStart()` **before** `xbox_MemoryLayoutInit()` can trap
   APU/AC97 pages.
2. Initialize the memory layout, then `mcpx_apu_init_standalone()` when an APU
   is wanted. Call `xbox_PosixMmioAttach(apu)` once; `apu == NULL` permits
   AC97/OHCI but APU faults chain. Attach publishes `g_apu_state` with release
   ordering. Do not assign that global directly on POSIX.
3. If USB is requested, call `xbox_OhciInit()` and require
   `xbox_OhciIsEnabled()` before guest execution. A failed or exited OHCI
   worker makes its MMIO unhandled instead of pretending it is operational.
4. On **every** native thread that can run guest code, including threads
   created by the kernel bridge, call `xbox_PosixMmioRegisterThread()` before
   entering it and `xbox_PosixMmioUnregisterThread()` after its final guest
   instruction. A failed registration must prevent that thread entering the
   guest. APU and OHCI register around their own guest interrupt callbacks.
   Reserve capacity for those callbacks: with both devices active, allow two
   of the 32 slots for their concurrently executing IRQ threads. An IRQ that
   cannot register is not delivered on that attempt and is logged.
   The Apple guest worker, kernel-created guest threads, and APU/OHCI interrupt
   callbacks now adopt this contract. Synthetic saturation tests and retail
   startup through title rendering exercise it; cooperative post-entry shutdown
   still needs proof.
5. Prevent new guest threads, stop/join guest and executor producers, and
   require `mcpx_apu_quiesce(apu)` to park the APU frame/IRQ producer.
   Then call `xbox_PosixMmioSealProducers()` to prevent new registrations
   while any already-registered interrupt finishes. Retry
   `xbox_OhciShutdown()` until it succeeds; it refuses to stop or restore
   pages while producers remain. A failed quiesce or shutdown requires
   retaining the devices and memory, not resuming the guest.
6. Call `xbox_PosixMmioBeginShutdown()` and `xbox_PosixMmioStop()`. Stop
   refuses while a producer remains, joins the broker, and unpublishes the
   APU before returning. Now call `mcpx_apu_shutdown_checked(apu)`, then
   `xbox_MemoryLayoutShutdownChecked()`. Restore host handlers only after
   the trapped pages are gone. Stop/join any other direct
   `apu_guest_read32`/`apu_guest_write32` consumers before unpublication too.

Registration is bounded and rejects blocked fault signals, overlapping native
or alternate stacks, and registration by the broker thread. Do not change
signal masks or alternate stacks while registered, share an alternate stack,
or run guest code on an unregistered fiber/coroutine stack. Registration
disables pthread cancellation until unregister restores the caller's prior
state; do not re-enable it while registered, and stop guest threads
cooperatively. A pending cancellation can take effect as unregister restores
the state, after its slot has been retired. A thread-exit destructor retires
an otherwise-idle forgotten registration so reuse of the same native stack
cannot inherit it; an exit with an in-flight slot fails the process explicitly.
The faulting PC must be readable, and its instruction bytes must not be changed
concurrently. Nested/reentrant calls cannot reuse a busy slot; the host must
not use a nonlocal jump to abandon an active outer handler.
A thread must not hold a lock needed by the broker across a fault: such a
dependency can stall the worker and triggers the explicit failure path. Do not
fork while a broker or producers are active.

Windows retains its existing VEH handling and diagnostics; these POSIX
ordering and broker changes do not replace them. Open, pump, and close
`xbox_FramebufferHost*` on the process main thread and stop its consumers
before window/input shutdown. Synthetic workers retain a three-second startup
deadline. The private retail worker has a separate bounded 120-second deadline
because validated XBE, kernel, storage, audio, USB, and renderer initialization
has exceeded both 30 and 60 seconds under heavy diagnostics or host load; the guest is not
released into generated entry until initialization reports success.
A CPU diagnostic replay that missed the 60-second policy reached
`guest_kernel_ready` and generated entry after the deadline was raised to
120 seconds; no `SIGBUS` or `SIGSEGV` occurred.

The CPU-reference presenter, private game host, generated arm64 integration,
homogeneous near-plane fix, and initial Metal backend are active. Physical
controller/audio testing, cooperative retail shutdown, campaign validation,
remaining thin-line classification, native movie playback, and broad Metal
state coverage remain. Scripted active mission entry, the in-mission pause
menu, safe startup, and campaign movie transitions are established but do not
satisfy the physical-control, save/restart, visual, or 1080p30 acceptance
gates. SDL presentation is still only the host-owned final handoff; it is not
the game renderer.

## Native synthetic validation

The following ran on native macOS ARM64 with 16,384-byte host pages, Apple
Clang 21, CMake 4.4.4, SDL2-compat and OpenSSL 3. Tests use synthetic data
and generated-hook stand-ins, not game data or generated game code.

```sh
cmake -S kit/source/runtime/tests/apple_silicon \
  -B _work/apple-silicon/mmio-tests -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat)" \
  -DMA2_RUNTIME_TEST_INJECTION=ON
cmake --build _work/apple-silicon/mmio-tests --parallel 4
ctest --test-dir _work/apple-silicon/mmio-tests --output-on-failure --timeout 90

cmake -S kit/source/runtime/tests/apple_silicon \
  -B _work/apple-silicon/mmio-tests-production -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat)" \
  -DMA2_RUNTIME_TEST_INJECTION=OFF
cmake --build _work/apple-silicon/mmio-tests-production --parallel 4
ctest --test-dir _work/apple-silicon/mmio-tests-production \
  --output-on-failure --timeout 90
```

Both configurations passed **11/11**, preserving the previous 10 tests and
adding the broker stress test: embedded D3D8 replacement; real A64 decode/faults; bounded fake-SDL
audio; real DSP monitor block ordering and restart with a fake output and
explicit rejection of a missing guest layout;
backend registration and executor/QueueFlip snapshot ordering; SDL virtual
controller mapping, hotplug, capture and OHCI reports; Darwin memory, 16 KB
neighbours, extended VMA, cross-page device faults and real APU/OHCI
lifecycles; SDL dummy
framebuffer pixels; and guest widths/privacy. The injected configuration also
tests failed mapping, partial protection, OHCI rollback/restoration, and
failed memory-release retry. Rumble routing is intercepted around real SDL
virtual controllers; it is not physical motor validation.

Focused follow-up validation additionally passes the protected contiguous
free/reuse regression in both memory-dispatch configurations and the executor's
linear backend test plus its swizzled partial-clear/draw, registered-fallback
strip-culling, mirror/border texture-addressing, dependent texture-stage, and
POSIX parallel-raster tests. The swizzled test
would fail if either clear or raster output were written row-major; the strip
test would fail if odd triangles retained the old winding; the dependent-stage
test would fail if the traced `DOTPRODUCT`/`DOT_ST`, dependent AR, bump, or
reflection programs returned the former zero fallback; and the parallel test
checks every pixel of a large quad before joining its worker pool. The frontend movie
test covers one-shot safe-start consumption, explicit lifted-player rearming,
native outermost transitions, nested increments, sequential movies, and
ordinary user skip consumption.

The new synthetic broker test performs real protected-page SIGBUS and
unmapped-page SIGSEGV loads/stores, including pair/vector and writeback forms,
with callbacks verified on the broker thread. It checks read-after-write,
unchanged chained contexts, absent/stopping devices, nested faults on normal
and alternate stacks, a fault on the broker thread, 32-slot exhaustion, a
deliberately full wake pipe, thread-stack reuse, a forced concurrent
unregister/re-register ABA race, deferred cancellation, in-flight shutdown,
repeated restart, and a stalled worker's exit status. The injected
production-device fixture also observes real APU/AC97/OHCI callbacks on the
broker rather than the faulting thread, forces an OHCI worker to exit between
probe and dispatch, and simulates a 32 KB page to test rejection of a pair
spanning both controllers. No game data is used.

ASan+UBSan passed that real-fault test with
`ASAN_OPTIONS=handle_segv=0:handle_sigbus=0:detect_leaks=0:use_sigaltstack=0`
and `UBSAN_OPTIONS=halt_on_error=1`. The injected device-integration fixture
also passed ASan+UBSan with those settings and
`DYLD_LIBRARY_PATH="$(brew --prefix sdl3)/lib"` (without that loader path,
SDL2-compat displayed a modal load error before the test reached `main`).
ThreadSanitizer passed the same broker's `--synthetic-tsan` concurrent
publication/restart mode. Its instrumented real-fault mode exited 100 after
the test handler reported an unhandled test-window fault, so it is **not**
evidence for real-signal TSan coverage.
Sanitizer instrumentation itself is not part of the production signal-safety
argument.

```sh
cmake -S kit/source/runtime -B _work/apple-silicon/runtime-embed-fresh \
  -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_BUILD_D3D8=OFF \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat)"
cmake --build _work/apple-silicon/runtime-embed-fresh --parallel 4
cmake -S kit/source/runtime -B _work/apple-silicon/runtime-d3d8 \
  -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_BUILD_D3D8=ON \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat)"
cmake --build _work/apple-silicon/runtime-d3d8 --parallel 4
cmake -S kit/source/runtime -B _work/apple-silicon/runtime-macos-x86_64 \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release \
  -DXBOXRECOMP_BUILD_D3D8=OFF \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2-compat)"
cmake --build _work/apple-silicon/runtime-macos-x86_64 --parallel 4
lipo -verify_arch x86_64 \
  _work/apple-silicon/runtime-macos-x86_64/src/kernel/libxbox_kernel.a
```

Both static-library builds passed. The fresh embedded configuration contains
neither an `xbox_d3d8` target nor an epoxy discovery, the linked Release
fixture has no libepoxy dependency, and the production platform archive has
no failure-injection symbols. The default D3D8-on build was compile-checked,
not used as the game's renderer.

As neutral references, the pinned upstream `tests/mmio_decode_a64` passed
directly and under CTest (**1/1**, 30 real traps), and `tests/input_map`
passed its C and C++ tests (**2/2**). The provisioned `.venv` ran upstream
`tools/disasm/test_decode_at.py`, `tools/recomp/test_lifter_bit_scan.py`,
`tools/fusion/test_signatures.py`, and `tools/fusion/test_svod.py` with
bytecode and pytest cache disabled: **38 passed**. An x86_64 macOS static
runtime cross-build also succeeded, but was not run. No Windows build was
available. Existing unrelated compiler warnings remain; these checks do not
establish a warning-free build.
