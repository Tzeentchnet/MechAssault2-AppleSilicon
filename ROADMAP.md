# Apple Silicon roadmap: 30 FPS+ in menus and missions

Goal: native arm64 build that holds **≥ 30 FPS in the front-end menus and in
active campaign missions**, with no CPU fallback in steady state.

Where we are (2026-11-25, branch `apple-silicon/bringup`):

- `nv2a_metal.mm` is a GPU-resident Metal renderer. It has exact visibility
  counts, cube maps, stencil (`Depth32Float_Stencil8`) and fog factors that
  match the CPU path.
- Menu (title screen, idle, 60 s, `perf_probe.sh`):

  | Step | Median FPS | GPU waits / 60 s | Declined draws |
  |---|---|---|---|
  | Resident renderer (earlier session) | ~9.4 | n/a | G8B8 surfaces |
  | + G8B8 render targets | 7.3* | 8,186 | 1,364 (border colour) |
  | + any texture border colour | 10.0 | 11,852 | 0 |
  | + GPU render-to-texture (colour) | 12.0 | 1,600 | 0 |
  | + GPU depth-as-texture | **12.9** | **833** (≈1/frame) | **0** |

  \*Different screen from the earlier ~9.4: keypresses were not delivered.
- The menu now has no fallbacks and no mid-frame GPU waits. The pushbuffer
  thread is CPU-bound, and **~68% of it is CPU vertex programs**
  (`execute_program`). That is Phase 3 and now the biggest item.
- `RECOMP_PERF=1` logs `[PERF]`, `[PERF-VISIBILITY]` and `[NV2A-METAL]` lines:
  accepted and declined draws, declines grouped by reason, render-to-texture
  blits and reuses, sampled declines, and textures that force a write-back.
- Missions have not been measured yet.

Why declines matter: each declined draw falls back to the CPU rasterizer. That
needs a full GPU sync, a write-back to guest memory and a reload, so one
decline per frame can cost more than the rest of the frame.

---

## Phase 0: Measurement you can trust (do first; it gates everything else)

- [ ] **Clean profiling environment.** Before each run:
  - Check `top -o cpu` / Activity Monitor. Wait for Spotlight (`mds_stores`),
    `photoanalysisd`, `backupd` and similar to settle, or exclude the game,
    build and cache folders from Spotlight.
  - Use AC power and turn Low Power Mode off.
  - Record the start time. Abort and investigate if startup is far over the
    usual time; a slow start is a symptom in its own right, not noise.
- [x] **Startup time.** Solved: the game draws **~7 s** after launch. The
  "17-minute startup" was the probe waiting for `runtime-private.log`, which is
  only written with `MA2_PRIVATE_DIAGNOSTICS=1`.
- [x] **Perf-probe script:** `tools/apple_silicon/perf_probe.sh` (no private
  paths). It runs an isolated HOME with `RECOMP_PERF=1`, waits for drawing,
  captures N seconds plus an optional `sample` profile, and prints FPS, the
  last Metal counters and decline samples.
  - [ ] Input: keypresses use System Events, which needs Accessibility
    permission for the terminal. Grant it, or add an in-process scripted input
    hook, so runs can reach the main menu and a mission.
  - [ ] Add p50/p99 frame time (needs per-frame timestamps, not the 5 s
    `[PERF]` average).
- [ ] **Fixed benchmark scenes.** Pick three and keep them:
  1. main menu idle,
  2. first mission, first 60 s after the HUD appears (reuse the
     `RECOMP_BOT_DIAG` route),
  3. a heavy combat or explosion moment.
- [ ] **Profiling recipe.** Run `sample $(pgrep -x mechassault2_recomp_guest)`
  for CPU, with the Metal System Trace in Instruments for GPU and command
  buffer timing. Record per-thread time for the guest CPU thread, the
  pushbuffer thread, raster workers and audio.
- [ ] **Per-frame counters** for GPU waits (`finish()` calls), command buffers
  committed, bytes uploaded, texture-cache misses and pipeline-cache misses.

Exit: we can say what each frame's milliseconds are spent on, for the menu and
for a mission.

## Phase 1: Zero fallback in the menu

- [x] **G8B8 render targets (NV2A surface format `0xA`).** A resident BGRA8
  texture with only green and blue writable (R=0, A=1), packed to 16 bits on
  load and write-back, as in the D3D11 backend. Tested by `g8b8_tests`.
- [x] **Any texture border colour.** The menu samples its G8B8 target
  (`LIN_G8B8`, `0x17`) with border colour `0x80808080`, which Metal samplers
  cannot express. The sampler now uses a transparent-black border and the
  shader adds `border × (1 − filter coverage)`. This is exact for
  single-level, nearest or bilinear sampling, as the CPU path samples.
  Mipmapped or cube textures with custom borders still decline.
- [x] Menu re-run with `RECOMP_PERF=1`: **`declines: none`**.
- [x] Fixed the CPU fallback in `nv2a_pb_exec.c`, which packed G8B8 as R5G6B5.
  Tested by `backend_executor_g8b8_surface`.
- [x] **GPU render-to-texture.** A linear texture that is exactly a resident
  surface (`0x12`/`0x1E` from A8R8G8B8, `0x17` from G8B8, `0x12`/`0x1E` from
  Z24S8 depth) is copied on the GPU in command order. There is no write-back,
  no wait and no texel decode. Copies are reused while the source's version is
  unchanged. Tested by `render_to_texture_tests`.
- [ ] **Turn decline logging into a regression gate.** A bounded test route
  fails if any decline reason other than an allow-listed one appears.

Expected: the menu is no longer bound by fallbacks. What remains is
per-frame sync, uploads and vertex work (Phase 3).

## Phase 2: Mission coverage

- [ ] Run the mission scenes and rank the remaining declines by frequency and
  cost: `texture_format`, `texture_stage`, `polygon_offset`, `alias`,
  `clip_overflow`, `non_finite`, and so on.
- [ ] Close declines in order of cost. Likely candidates:
  - more texture formats (palettized P8, YUY2/UYVY, compressed formats on
    linear surfaces),
  - polygon offset through `setDepthBias`,
  - surface aliasing (the same memory bound as both a render target and a
    texture),
  - unusual primitive types (quads and quad strips converted to triangle
    indices on the GPU side).
- [ ] Check emissive and lamp lines (the purple/yellow edges from `TODO.md`)
  against the CPU oracle once the scene is drawn fully on the GPU.

## Phase 3: GPU vertex work

- [ ] **Run NV2A vertex programs on the GPU**, as the D3D11 backend already
  does (`nv2a_gpu_draw_program_indexed`, `nv2a_gpu_draw_buffers` in
  `nv2a_gpu_d3d11.c`).
  - Implement the optional `draw_program_indexed` and `draw_buffers` hooks in
    `Nv2aBackend` for Metal.
  - Translate vertex-shader microcode to MSL once per program hash, sharing the
    decoder with the D3D11 HLSL generator where possible.
  - Upload the 192 constant registers as one buffer per draw from an upload
    ring.
  - Cover fixed-function transform and lighting with a generated MSL path, or
    reuse the D3D11 generator's structure.
- [ ] **Pass vertex attributes straight through.** Bind guest vertex memory
  (unified memory) as `MTLBuffer` ranges, or copy it into a ring, instead of
  decoding vertices on the CPU. Handle the NV2A attribute formats
  (`CMP`/packed normals, `S32K`, `UB_OGL` vs `UB_D3D`) in the vertex fetch.
- [ ] **Pipeline-state cache keyed by a compact state hash.** Keep it in
  memory, and also on disk with `MTLBinaryArchive`, so a second launch does
  not compile shaders mid-frame. recompose ships persistent SPIR-V and
  pipeline caches next to the executable for the same reason.

## Phase 4: Textures and uploads

- [ ] Extend GPU render-to-texture to swizzled formats (`0x06`/`0x07`/`0x28`)
  and to sub-rectangles of a surface, if missions use them. Watch for
  `[NV2A-METAL] texture writeback` lines.

- [ ] **Faster texture-cache misses.** Decoding currently runs one texel at a
  time.
  - Unswizzle by rows and tiles with a Morton lookup table (the toolkit's D3D11
    path already does Morton unswizzle), or unswizzle in a compute shader.
  - Upload DXT1/3/5 straight as `BC1`/`BC2`/`BC3`. Apple Silicon Macs support
    BC formats, so no CPU decompression.
  - Expand palettes and odd formats in a compute kernel.
- [ ] **Cheaper cache invalidation.** Hash on upload, track dirty pages for
  guest writes, and skip re-hashing large static textures every frame.
- [ ] **Upload rings for vertices, indices, constants and staging**, with
  three frames' worth of space. The toolkit's 4 MB DrawPrimitiveUP ring is the
  model. No per-draw `newBuffer` calls.

## Phase 5: CPU and GPU overlap (frames in flight)

- [ ] **Stop waiting on the GPU every frame.** (The menu is down to about one
  wait per frame, for presentation.) `finish()` waits on every
  in-flight command buffer. Keep it only for real guest synchronization:
  - CPU reads of a GPU surface,
  - `GET_REPORT` / visibility results the guest actually polls,
  - semaphores and notifiers,
  - snapshots and dumps.
- [ ] **Bounded frames in flight (2–3).** Gate with a `dispatch_semaphore` and
  use completion handlers to recycle rings. recompose uses 3.
- [ ] **Resolve visibility queries lazily.** Hand the guest a deferred
  result, and wait only when it reads the report memory.
- [ ] **Host-owned presentation.** Present a `CAMetalDrawable` from the host,
  separate from guest flips, as listed in `TODO.md`. Drop the guest lock
  while waiting for present (recompose does this). Without that, a present
  that blocks on vsync stalls the guest; recompose measured "~8.9 ms a present
  vs 1.7 ms headless" on MoltenVK before fixing it.
- [ ] **Frame pacing.** Check what the existing `RECOMP_VBLANK=paced` /
  `RECOMP_FLIP_PACING` do on macOS. Avoid a vsync wait inside the
  guest-visible device: the toolkit saw Burnout 2 drop "from about 136 frames
  a second to 27" from one such wait. Consider `CAMetalLayer.displaySyncEnabled`
  and a configurable `present.pacing = sleep | spin` (as in ssx3-recomp).

## Phase 6: CPU-side throughput

- [ ] **Thread placement on Apple Silicon.** Pin, or give QoS
  `USER_INTERACTIVE` to, the guest CPU thread and the pushbuffer thread so they
  stay on P-cores. Leave audio and raster workers to the scheduler. Check
  whether `RECOMP_GUEST_CPUS` / `pc_cpu_topo.c` understand P/E clusters.
- [ ] **Raster worker pool** for whatever still falls back. Upstream reports
  "110 → 278 Mpx/s on four threads on an M4" with `RECOMP_RASTER_THREADS`.
  Confirm this build turns it on for macOS.
- [ ] **Pushbuffer executor hot spots.** Once the GPU work is gone, profile
  `nv2a_pb_exec.c` method dispatch. Check MMIO decode costs
  (`mmio_decode_a64.h`), lock contention, and per-method state hashing.
- [ ] **Audio cost.** Measure the APU/DSP thread in missions; it must not
  steal the P-core time the guest and pushbuffer threads need.
- [ ] **Compiler settings for the generated guest code:** `-O2`/`-O3`,
  `-mcpu=apple-m1`, and LTO for the runtime. Check that no debug or trace
  `getenv` checks run on hot paths. Cache env lookups once at startup.

## Phase 7: Acceptance (from `TODO.md`)

- [ ] Physical 1920×1080 with a correct 16:9 camera and HUD. Render scale
  1–4× (as BLiNX 2 offers) is a stretch option once 30 FPS holds at 1×.
- [ ] Cold-cache and warm-cache 180 s active-campaign samples:
  - average unique FPS ≥ 29.5,
  - p99 ≤ 36 ms,
  - no interval > 50 ms,
  - no stale frames, fallback, audio underrun, crash, hang or frozen
    simulation.
- [ ] The same 30 FPS bar in the menu, plus menu → mission → menu transitions
  without hitches from shader compiles.
- [ ] Regression gates: CPU/Metal parity, input, movie skip, memory/MMIO,
  audio, privacy, and Windows.

## Stretch: 60 FPS and polish

- [ ] Uncapped or 60 FPS mode, only if game logic tolerates it. Several
  toolkit ports run at 60 (Gladius on Apple Silicon at 60 fps; TimeSplitters 2
  and BLiNX at 60 fps under recompose).
- [ ] Anisotropic filtering and FSR or other upscaling shader profiles, as in
  blinx2-recomp.
- [ ] An optional in-game FPS overlay key (Gladius uses F9 for FPS and F10 for
  the frame cap).

---

## What other Xbox recomp ports on Apple Silicon do

| Project | macOS rendering | Reported macOS perf | Takeaways for us |
|---|---|---|---|
| [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (toolkit) | D3D8 HLE → OpenGL 3.3 on POSIX; NV2A pushbuffer executor + CPU rasterizer for static-XDK titles | Raster workers 110 → 278 Mpx/s (4 threads, M4) | Combiner shader cache, vertex programs decoded once per upload, 4 MB DrawPrimitiveUP ring, Morton unswizzle, `RECOMP_VBLANK=paced` |
| [Joshspeakman/recompose](https://github.com/Joshspeakman/recompose) | D3D8 HLE → Vulkan on MoltenVK | TimeSplitters 2 and BLiNX at 60 fps; TS2 frame 12.4 → 5.0 ms | Persistent SPIR-V and pipeline caches, 3 frames in flight with upload rings, guest lock released while waiting for present, IMMEDIATE present mode. D24S8 is missing on Metal (use D32S8). Metal always restarts strips at 0xFFFF |
| [elpetese/black-macos-recomp](https://github.com/elpetese/black-macos-recomp) | xemu's NV2A renderer, GL ES via ANGLE on Metal | ~30 fps in level 1 (ANGLE) vs ~15 fps on desktop GL | The closest match to our NV2A-level approach. Metal beat Apple's deprecated GL by 2× with no other changes. xemu's GPU vertex-shader and surface-cache design can be borrowed |
| [phobos665/gladius-recomp](https://github.com/phobos665/gladius-recomp) | D3D8 HLE (DirectXbox-Recomp) | 60 fps with sound, boot → first battle | HLE removes the pushbuffer cost entirely; worth it only for titles that keep D3D in a separate library |
| [apexJCL/ssx3-recomp](https://github.com/apexJCL/ssx3-recomp) | Metal (fork-specific) | Title → race tested; no FPS figure | `present.pacing = sleep/spin` setting |
| [apexJCL/blinx2-recomp](https://github.com/apexJCL/blinx2-recomp) | Metal | 30 fps (native rate), render scale 1–4× | Render scale, anisotropic filtering, scaling and shader profiles as user options |

Lessons:

1. Every Apple Silicon port at ≥ 30 FPS draws on the GPU with **no per-frame
   CPU fallback or readback**. Black only reached 30 fps once its GL path went
   through Metal.
2. The biggest gains elsewhere came from **frame pacing and present/lock
   handling, caches and upload rings**, not from the graphics API.
   recompose's 12.4 → 5.0 ms came from that kind of fix. Phases 4–5 matter as
   much as Phase 3.
3. **Persistent shader and pipeline caches** are standard. Without them, the
   first visit to each scene hitches.
4. For MA2, D3D is linked statically, so there is no D3D API boundary to
   replace (HLE). We stay at the NV2A level, which is xemu's model. Black is
   the closest reference point, at ~30 fps at 1920×1440 through Metal.
