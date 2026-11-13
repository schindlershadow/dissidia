# Environment Variables and Build Options

Complete reference for every `PSPRECOMP_*` environment variable and CMake
option in the project. Each entry lists where it is read, its type, default,
effect, and when you would set it.

Three kinds of knob appear here:

- **env** — an environment variable read at runtime or during the Rust
  pipeline (`std::env::var` in Rust, `std::getenv` in C++).
- **cmake** — a CMake cache variable/`option()` passed at configure time with
  `-D<NAME>=<VALUE>`.
- **both** — a name used as a CMake variable *and* exported into the
  environment of a sub-process (e.g. `PSPRECOMP_CROSS_MID`, which CMake only
  prints in its regenerate hint while the Rust pipeline reads it from the env).

Unless stated otherwise, a boolean env flag is "on" when set to `1` and
otherwise off; several flags activate on mere *presence* (any value) — that is
called out per entry.

## Most users only need these

| Flag | Type | What it does |
|------|------|--------------|
| `PSPRECOMP_CROSS_MID` | env | Recompile with cross-function mid-jump recovery. The documented build line sets `PSPRECOMP_CROSS_MID=1` before `cargo run -- recompile`. |
| `PSPRECOMP_GAME` | cmake | Selects which `games/<id>` module compiles into the runtime (`dissidia` default, `none` for a generic build). |
| `PSPRECOMP_OUTPUT_DIR` | cmake | Points the runtime build at a recompiler output directory other than `../output`. |
| `PSPRECOMP_DISC0` | env | Path to the extracted disc image the runtime reads game data from (default `./disc0`). |
| `PSPRECOMP_STRICT` | env | Abort the runtime on the first dispatch lookup miss instead of returning a noop stub. |

Everything below "Debug and trace flags (advanced)" is for investigation only;
none of it is needed for a normal recompile-build-run cycle.

---

## Pipeline / recompile (Rust)

These are read by the `psprecomp` Rust binary during `analyze`/`recompile`/`dump`.

### `PSPRECOMP_CROSS_MID`
- **Read in:** `crates/psp-cli/src/recompile.rs`, `crates/psp-cli/src/config.rs`,
  `crates/psp-cli/src/dump.rs`, `crates/psp-cli/src/main.rs`,
  `crates/psp-cli/src/fingerprint.rs`
- **Type:** env (`=1` to enable) — also referenced in `runtime/CMakeLists.txt`
  as part of the documented regenerate command.
- **Default:** off.
- **Effect:** When `=1`, the recompile pipeline coalesces Ghidra-over-split
  shared-frame siblings and runs systematic cross-function mid-jump recovery
  (D2+D3a), and activates the manifest's `force_entries_cross_mid` list. Gated
  off by default because landing it makes the broken decompressor at
  `0x089D7xxx` run and stall (D1 baseline note in `recompile.rs`).
- **Set it when:** generating output (the project's documented build line uses
  it). Pass the same value to `dump` so a single function's dumped C++ matches
  its batch file.

### `PSPRECOMP_NO_COALESCE`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). Only has effect when `PSPRECOMP_CROSS_MID=1`.
- **Default:** off.
- **Effect:** Disables only the shared-frame sibling merge while leaving
  cross-function mid-jump injection on (an A/B diagnostic isolating the merge).
- **Set it when:** bisecting a regression introduced by `PSPRECOMP_CROSS_MID`.

### `PSPRECOMP_NO_RA_MODEL`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** Keeps the structural sibling merge but disables the Option-A
  LINK/RA lowering applied to coalesced owners (a diagnostic splitting the
  merge from the RA model).
- **Set it when:** isolating which half of the coalesce change moved behavior.

### `PSPRECOMP_COALESCE_DEBUG`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). *Advanced / diagnostic.*
- **Default:** off.
- **Effect:** Logs each coalesce owner that absorbed >= 4 siblings (owner
  address, sibling count, member list) via `tracing::info!`.
- **Set it when:** inspecting which functions the coalescer merged.

### `PSPRECOMP_ALLOW_STALE`
- **Read in:** `crates/psp-cli/src/fingerprint.rs` and
  `runtime/CMakeLists.txt` (as a CMake `option()`).
- **Type:** both (env for the pipeline-side check; CMake option for the
  configure-time check — `-DPSPRECOMP_ALLOW_STALE=ON`).
- **Default:** off / `OFF`.
- **Effect:** Downgrades the output-staleness fingerprint mismatch from a fatal
  error to a warning. The fingerprint compares the emitter source hash against
  `output/fingerprint.json`; a mismatch means `output/` does not reflect the
  current emitter sources.
- **Set it when:** you knowingly want to build/recompile against an out-of-date
  `output/` (accepting that runtime behavior will not match current sources).

---

## Recompile manifest selection

### `PSPRECOMP_GAME` (pipeline side)
- **Read in:** `crates/psp-cli/src/config.rs`
- **Type:** env. (Distinct from the CMake `PSPRECOMP_GAME` below; same name,
  different layer.)
- **Default:** unset.
- **Effect:** On the pipeline side, used while resolving the per-game config so
  the recompile uses the matching `games/<id>` manifest context.
- **Set it when:** scripting a recompile for a non-default game id. For a normal
  build the `--config games/<id>/game.toml` argument already pins the game.

---

## Runtime — run-time configuration (C++)

Read by the runtime binary at start-up or during execution.

### `PSPRECOMP_RENDERER`
`gl` (default) or `vulkan`. Vulkan runs the same renderer through a GL-on-Vulkan layer (`runtime/src/vk/`, [docs/VULKAN_PORT.md](VULKAN_PORT.md)); if the Vulkan
backend is not built in or no device is usable the runtime stops at start-up (set it to `gl`). Also chosen in Settings > Graphics > Graphics API (saved as
`renderer=gl|vulkan`, applied at the next start, and wins over this variable). Related, all Vulkan-only: `PSPRECOMP_VK_VALIDATION=1` (validation layer),
`PSPRECOMP_VK_DEVICE=<index|name>` (GPU), `PSPRECOMP_VK_CACHE_DIR` (SPIR-V cache; default `~/.cache/dissidia-vk`, `<workdir>/vk_cache` on Android),
`PSPRECOMP_VK_SUBMIT_EVERY=<draws>` (early GPU submits; 0 = only at sync points), `PSPRECOMP_VK_NO_PRESENT=1` (benchmark: never present),
`PSPRECOMP_VK_NO_MULTIDRAW=1`, `PSPRECOMP_VK_TRACE=1` (log every draw). `[VKGL]` lines in the log give draws, pipelines, GPU time and CPU cost per frame.

### `PSPRECOMP_DISC0`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (path string).
- **Default:** `./disc0`.
- **Effect:** Overrides the disc0 directory the I/O layer maps
  (`disc0:/...` guest paths). If the path does not exist the runtime warns and
  continues with no game data.
- **Set it when:** running against an extracted ISO that is not in `./disc0`.

### `PSPRECOMP_STRICT`
- **Read in:** `runtime/src/psp_dispatch.cpp`, `runtime/src/main.cpp`,
  `runtime/src/hle/psp_hle_dispatch.cpp`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** On any `RECOMP_LOOKUP` miss, prints the address and calls
  `std::abort()` instead of returning the noop stub. Boot also probes a known
  invalid address to exercise the path. The HLE dispatch path honors it too.
- **Set it when:** you want a missing function to crash loudly (debugging) rather
  than silently no-op.

### `PSPRECOMP_GE_TEST`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (`=1` / first char `'1'`).
- **Default:** off.
- **Effect:** Runs the GE renderer self-test after GL init and reports PASS/FAIL,
  then continues booting.
- **Set it when:** verifying the renderer brings up independently of game assets.

### `PSPRECOMP_GE_TEST_ONLY`
- **Read in:** `runtime/src/main.cpp`.
- **Type:** env (`=1` / first char `'1'`). Implies `PSPRECOMP_GE_TEST`.
- **Default:** off.
- **Effect:** Runs the GE self-test, captures a frame (`frame.tga`), and exits
  `0` before the game thread starts.
- **Set it when:** capturing a renderer-only reference frame in CI/headless runs.

### `PSPRECOMP_SCREENSHOT`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (path string; any non-empty value).
- **Default:** unset.
- **Effect:** Sets the file path the draw layer writes its captured screenshot
  to.
- **Set it when:** you want the captured frame written somewhere specific.

### `PSPRECOMP_PRESENT_STALE_MS`
- **Read in:** `runtime/src/psp_ge_draw.cpp` (declared in
  `runtime/include/psp_ge_draw.h`)
- **Type:** env (integer milliseconds).
- **Default:** `100`.
- **Effect:** Liveness-present budget. If content is dirty and no real
  page-flip happened within this many milliseconds, the front buffer is
  presented once (a no-flip safety net).
- **Set it when:** tuning the no-flip present heartbeat for a title that flips
  rarely.

### `PSPRECOMP_GEOM_SELFTEST`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (any non-empty value).
- **Default:** off.
- **Effect:** Synthesizes exactly one real type-4 (TRIANGLE_STRIP, non-clear,
  non-sprite) draw through the real counter + sentinel path, for the geometry
  verification harness (`scripts/verify_geometry.sh`).
- **Set it when:** running the geometry verification harness.

---

## Scheduler (#66)

### `PSPRECOMP_PREEMPT`
- **Read in:** `runtime/src/psp_scheduler.cpp` (declared in
  `runtime/include/psp_scheduler.h`); the emitter
  (`crates/psp-emitter/src/cpp_generator.rs`,
  `crates/psp-emitter/src/function.rs`) emits the back-edge preemption point
  unconditionally and documents the flag.
- **Type:** env (exactly `1`).
- **Default:** off.
- **Effect:** Default-off, the emitted loop back-edge `sched_preempt(ctx)` call
  only reloads `ctx->preempt_budget` and returns — guest behavior is identical
  to the pre-#66 scheduler. With `=1` it additionally takes a fair cooperative
  yield so a syscall-free busy-poll reaches a reschedule point. Read once and
  cached.
- **Set it when:** evaluating the preemptive-yield path (e.g. a many-boot
  freeze-rate harness, `docs/SCHEDULER-DESIGN.md` §6). Leave unset for the default baseline.

---

## Build options (CMake)

Passed at configure time: `cmake -B <dir> -S runtime -D<NAME>=<VALUE>`.

### `PSPRECOMP_OUTPUT_DIR`
- **Defined in:** `runtime/CMakeLists.txt`
- **Type:** cmake (`PATH`, cache variable).
- **Default:** `${CMAKE_CURRENT_SOURCE_DIR}/../output` (the in-repo `output/`).
- **Effect:** The recompiler output directory the runtime builds against. Must
  contain a generated `CMakeLists.txt` and define the `psp::recomp` alias target,
  or configure fails with the regenerate hint.
- **Set it when:** building the runtime against an output directory outside the repo, e.g.
  `-DPSPRECOMP_OUTPUT_DIR=$WORK/output` (what `games/dissidia/tools/pipeline.sh` passes).

### `PSPRECOMP_GAME`
- **Defined in:** `runtime/CMakeLists.txt` (also referenced by
  `runtime/tools/purity_gate.sh`, `runtime/src/psp_game_default.cpp`,
  `runtime/include/psp_game_module.h`, and every `games/<id>/runtime/*.cpp`
  via the build).
- **Type:** cmake (`STRING`, cache variable).
- **Default:** `dissidia`.
- **Effect:** Selects the `games/<id>/runtime/*.cpp` module compiled into the
  runtime (exactly one strong `psp_game_module()` definition per build).
  `none` (or empty) compiles `src/psp_game_default.cpp` — a pure generic build
  with zero game hooks. An unknown id with no sources is a fatal error.
- **Set it when:** building the generic runtime
  (`-DPSPRECOMP_GAME=none`) or a non-default game module.

### `PSPRECOMP_ALLOW_STALE`
- **Defined in:** `runtime/CMakeLists.txt` (`option()`).
- **Type:** cmake (`option`, `ON`/`OFF`) — see also the pipeline-side env use
  above.
- **Default:** `OFF`.
- **Effect:** Turns the configure-time output-staleness fingerprint mismatch
  into a warning instead of a fatal error.
- **Set it when:** intentionally configuring against a stale `output/`.

> Internal CMake locals (`PSPRECOMP_REGEN_CMD`, `PSPRECOMP_FP_RESULT`,
> `PSPRECOMP_FP_OUTPUT`, `PSPRECOMP_FP_ERROR`) are not user-settable; they hold
> the regenerate-command string and the fingerprint-check sub-process results.

---

## Debug and trace flags (advanced)

Everything below is for investigation only. None is needed for a normal
recompile-build-run cycle. Trace flags exist to gate logging that can produce
gigabytes per minute; probe flags add read-only diagnostics; the remaining
flags toggle individual experimental fixes/band-aids in a game module.

### Generic runtime trace / diagnostics

| Flag | Read in | Activation | Effect |
|------|---------|------------|--------|
| `PSPRECOMP_PC_TRACE` | `runtime/src/psp_dispatch.cpp` (declared `crates/psp-emitter/src/cpp_generator.rs`) | `=1` | Logs every generated-function entry with per-address frequency counting; dumps a summary at exit. No-op otherwise. |
| `PSPRECOMP_FUNC_TRACE` | `runtime/src/psp_dispatch.cpp` | `=1` | Turns the per-function-entry bookkeeping of `psp_trace_checkpoint` on (last/previous function address, the 32-entry ring used by `[LOOKUP_MISS_CTX]`/`NANTRAP`/`FN_WATCH_RING`/`PKG_TRACE` dumps, the cross-thread ring behind the debug socket `I` command). **Off by default**: the checkpoint is then one load and a return (it ran on every generated function entry, ~6% of the game thread). Also switched on by `MEM_WATCH`, `ARG_WATCH`, `FIRSTCALL`, `LKP_LOG`, `PC_TRACE`, `VFPU_NANTRAP`, `FN_WATCH_RING`, `PKG_TRACE` and the debug socket `M` command. |
| `PSPRECOMP_ASYNC_PRESENT` | `runtime/src/psp_render_queue.cpp`, `hle/psp_hle_display.cpp` | `=1` | Frame pipelining prototype: `sceDisplaySetFrameBuf` returns once the present is queued instead of when the render thread has finished it, so the next frame's game logic overlaps the present. Default off. |
| `PSPRECOMP_TEX_HASH_MEMO` | `runtime/src/psp_ge_texture.cpp` | `=0` disables | Texture content-hash memo (see VULKAN_PORT.md). Default on. |
| `PSPRECOMP_VK_RT` | `runtime/src/psp_screenfx.cpp`, `vk/vkgl_core.cpp` | `=1` / `=0` | Initial state of the Ray-Traced Shadows switch (settings row, saved as `rt_shadows`; the row shows on Vulkan with a ray-query GPU). The Vulkan layer always creates the ray-query device when the GPU has it (Android: the phone's own Adreno 740 driver has it; a Turnip custom driver may too) (`PSPRECOMP_VK_NO_RT=1` or `PSPRECOMP_VK_RT=0` at start prevents that); the switch decides whether scenes are captured and the contact shadows are traced. Needs `PSPRECOMP_CONTACT_SHADOWS` > 0. |
| `PSPRECOMP_RT_LEN`, `PSPRECOMP_RT_DEBUG`, `PSPRECOMP_CONTACT_AMT` | `psp_screenfx.cpp`, `psp_ge_draw.cpp`, `vk/*` | float / presence / float | Ray length (default 40 world units); log the ray-query scene build; override the contact-shadow strength (1.0 with `PSPRECOMP_SSAO=0` makes the shadow mask visible). |
| `PSPRECOMP_VK_RT_REFL` | `runtime/src/psp_screenfx.cpp`, `psp_ge_draw.cpp`, `vk/*` | `=1` / `=0` | Initial state of the Ray-Traced Reflections switch (settings row, saved as `rt_reflections`; same availability as Ray-Traced Shadows). On, the floor reflection is a ray query along the reflected view ray (closest hit) instead of the screen-space march, and the Reflections row is hidden. Independent of `PSPRECOMP_VK_RT` (either one makes the draw path capture the frame's triangles + per-triangle colours). |
| `PSPRECOMP_RT_REFL_LEN`, `PSPRECOMP_RT_SKY_AMT`, `PSPRECOMP_RT_SKY_R/G/B` | `psp_screenfx.cpp` | float | Reflection ray length (default 300 world units, long enough to reach the sky dome); strength of a reflection of the sky relative to geometry (0.4: the floor texture already carries a baked sky); colour shown when the ray hits nothing and the sky is off screen (0.45 0.55 0.75). `PSPRECOMP_FX_SKY_DIST` (view units, default 150) decides what counts as the dome. |
| `PSPRECOMP_RT_REFL_SPLIT`, `PSPRECOMP_RT_REFL_DEBUG`, `PSPRECOMP_REFL_ONLY` | `psp_screenfx.cpp` | presence | Debug views for the ray-traced reflection. SPLIT: the left half of the picture keeps the screen-space reflection, the right half is ray-traced (same frame, so a screenshot is a before/after). DEBUG: tints the floor by what the ray found (red hit shaded by the triangle colour, green hit visible on screen, yellow sky from the picture, blue miss). REFL_ONLY: write the reflection layer instead of blending it (the game's glow pass saturates it; DEBUG is more useful). |
| `PSPRECOMP_RT_FX_SPLIT`, `PSPRECOMP_RT_FX_OFF`, `PSPRECOMP_RT_HIDDEN` | `psp_screenfx.cpp` | presence / float | Effects layer of the ray-traced reflection (blended, depth-tested, non-depth-writing draws: orbs, beams, auras, glass; traced as a second instance and blended over the opaque reflection). SPLIT: the left half of the picture has no effects layer, the right half does (same frame). OFF: never trace it. `PSPRECOMP_RT_HIDDEN` (default 0.45) scales colour and strength of a hit on a surface that is not visible on screen (its per-triangle colour is unlit). `PSPRECOMP_RT_REFL_DEBUG=2` tints only the right half (magenta = effect hit) so the left half stays normal. |
| `PSPRECOMP_GE_TRACE` | `runtime/src/psp_ge.cpp` | first char `'1'` | Enables GE command tracing. |
| `PSPRECOMP_HLE_TRACE` | `runtime/src/hle/psp_hle_dispatch.cpp` (declared `runtime/include/hle/psp_hle.h`); used by `scripts/diff_traces.sh` | `=1` | Every HLE call logs thread name + function name to stderr. Used to produce the runtime trace for the PPSSPP differential. |
| `PSPRECOMP_SEMA_TRACE` | `runtime/src/hle/psp_hle_kernel_sema.cpp` | `=1` | Gates the per-call WaitSema/Signal logs (≈1 GB/min during stalls) behind a flag. |

> `PSPRECOMP_SPLEAK` no longer exists — the shadow-stack sp-leak detector it
> gated was a game-tuned probe deleted in issue #47 Phase 5
> (`runtime/src/psp_dispatch.cpp` retains only the explanatory comment). It is
> listed here only so older notes referencing it are not mistaken for a live
> flag.

### Game-module flags

Flags read only by a game module live with that module in `games/<id>/runtime/` and are
compiled only under its `-DPSPRECOMP_GAME=<id>`. For Dissidia 012 see
`games/dissidia/README.md` and `games/dissidia/docs/steamdeck.md`.

## Where each flag is referenced

Consolidated `PSPRECOMP_*` flags discovered in-tree so contributors do not have to grep the codebase.

| Flag | Where referenced |
|------|------------------|
| `PSPRECOMP_ALLOW_STALE` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_COALESCE_DEBUG` | `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_CROSS_MID` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `crates/psp-cli/src/config.rs` |
| `PSPRECOMP_DISC0` | `README.md`, `docs/ADDING_A_GAME.md`, `docs/ENV_FLAGS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_FP_ERROR` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_OUTPUT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_RESULT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_GAME` | `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `runtime/CMakeLists.txt`, `runtime/include/psp_game_module.h`, `runtime/src/main.cpp` |
| `PSPRECOMP_GEOM_SELFTEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_GE_TEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_GE_TEST_ONLY` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_GE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge.cpp` |
| `PSPRECOMP_HLE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/include/hle/psp_hle.h`, `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_NO_COALESCE` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_NO_RA_MODEL` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_OUTPUT_DIR` | `ARCHITECTURE.md`, `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_PC_TRACE` | `DEBUGGING.md`, `crates/psp-emitter/src/cpp_generator.rs`, `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_PREEMPT` | `crates/psp-emitter/src/cpp_generator.rs`, `crates/psp-emitter/src/function.rs`, `docs/ENV_FLAGS.md`, `docs/SCHEDULER-DESIGN.md`, `runtime/include/psp_scheduler.h` |
| `PSPRECOMP_PRESENT_STALE_MS` | `docs/ENV_FLAGS.md`, `runtime/include/psp_ge_draw.h`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_REGEN_CMD` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_SCREENSHOT` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_SEMA_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_SPLEAK` | `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_STRICT` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `docs/ADDING_A_GAME.md`, `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/src/main.cpp`, `runtime/src/psp_dispatch.cpp` |

Default values and semantics live next to the definitions; prefer those sources of truth when changing behavior.

## Custom GPU drivers (Android)

| Variable | Where | Values | What |
|---|---|---|---|
| `PSPRECOMP_GPU_DRIVER` | `psp_gpu_driver.cpp`, `vk/psp_vk_loader.cpp` | package id (folder under the driver directory), empty = the phone's own | Vulkan driver to load through libadrenotools. Normally set from the saved `gpu_driver` setting (Settings > Drivers > GPU Driver); needs `PSPRECOMP_RENDERER=vulkan` and an Adreno GPU. A driver that crashed during start-up is skipped until it is picked again. |
| `PSPRECOMP_GPU_DRIVER_DIR` | `psp_gpu_driver.cpp`, `psp_android.cpp` | path | Where drivers are installed (default `<app internal storage>/gpu_drivers`; it must not be on the sd card). |
| `PSPRECOMP_GPU_DRIVER_FEEDS` | `psp_gpu_driver.cpp` | `owner/repo[,owner/repo...]` | GitHub repositories whose releases hold adrenotools driver zips (default `K11MCH1/AdrenoToolsDrivers`). |
| `PSPRECOMP_VK_RECORD` | `vk/psp_vk_glsl.cpp` | `1` / `0` | Write every shader source handed to the SPIR-V cache to `<cache>/src/<hash>.<stage>.glsl` (default on for Android, off elsewhere): how `android/shaders/` is kept complete. |
| `PSPRECOMP_SHADER_PACK` | `vk/psp_vk_warmup.cpp` | file | Shader pack to compile at start ("Preparing graphics"); default `shaders.pack` (Android: the APK's `assets/shaders.pack`). |
| `PSPRECOMP_WARMUP_DELAY_MS` | `vk/psp_vk_warmup.cpp` | ms | Testing: wait this long after each shader of the start-up compile so the "Preparing graphics" screen can be looked at. |
| `PSPRECOMP_VBLANK_LATE` | `hle/psp_hle_display.cpp` | fraction of a vblank period, default `0.5`; `0` = off | Above 60 Hz a frame that finishes up to this fraction of a period after a vblank it ran across is let through at once instead of sleeping a whole extra period (one frame in ~16 was lost: 90 / 120 fps measured 84 / 112 on the RP6, now 89.8 / 119.5). |
| `PSPRECOMP_VK_PRESENT` | `vk/psp_vk_context.cpp` | `fifo` / `mailbox` / `immediate` | Swapchain present mode (when the device offers it); the chosen mode is logged. |
| `PSPRECOMP_VK_SWAP_LOG` | `vk/vkgl_core.cpp` | `1` | Every 600 frames: acquire / present call times and the last 64 swap intervals (frame pacing). |
| `PSPRECOMP_ADPF` | `psp_android.cpp` | `0` = off | Android performance hint session (API 33) reporting the frame time; harmless, no measurable gain on the RP6. |

