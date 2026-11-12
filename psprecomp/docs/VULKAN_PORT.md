# Vulkan backend: design and plan

Status (2026-11-12): **works on desktop (NVIDIA) and on the Retroid Pocket 6 (Adreno 740), opt-in.** The route taken is a GL-on-Vulkan layer
(`runtime/src/vk/`), not a native rewrite of `psp_ge_draw`: the ~110 GL entry points the runtime uses are implemented on Vulkan and installed in glad's
function pointers (an equivalent pointer table on Android), so the PSP logic exists once. In-game: Settings > System > Graphics API (next start).
Verified: title, menus, settings, 3D battle, render scale 3, screen-space effects, touch overlay, resize; static screens match GL to 2/255; zero validation
errors on desktop. Measured on the GTX 1660 SUPER in a heavy fight (3.5k draws, render scale 3, no present): GPU about 2.2 ms/frame; render-thread CPU
about 3.2 ms for Vulkan vs 2.65 ms for NVIDIA's GL, so **Vulkan is not faster yet**; the layer's own CPU cost is ~0.4 ms/frame. Retroid Pocket 6 (Adreno 740, 120 fps mode, render scale 4, widescreen, SSAO/contact shadows/SSR on, same verified stage and fighters, Order's
Sanctuary, Squall vs Lightning): GLES about 112 fps, Vulkan about 110 fps; render-thread CPU 5.85+0.7 ms (GL) vs 6.97+0.56 ms (Vulkan), Vulkan GPU about 7 ms/frame.
With root on the RP6 (GPU pinned to 680 MHz via `devfreq/min_freq`, game set to 240 fps so nothing paces it) both APIs hit the same ceiling:
GLES 113 fps, Vulkan 114 fps, Vulkan GPU 6.4 ms/frame (room for ~150 fps). The limit is therefore not the renderer but the game's own CPU work.
**So parity, not a win.** Both sit near the 120 fps pacing, so headroom still has to be measured uncapped. Earlier device numbers that showed Vulkan faster (no
screen-space effects yet) or slower (stage not reliably selected) were measurement errors: the harness must verify the stage (`BT_SETUP_SHOT`). Known gaps (also: pixel-level differences in the screen-space effects were only checked by eye): partial colour mask on glClear, blit within one image, occlusion/timer queries are stubs, no ray tracing yet, no async pipeline creation or
pipeline cache on disk, FX shader hot reload untested on Vulkan, Android needs the SPIR-V cache (`vk_cache/`) filled by a desktop run (`vk_spirv_tool` compiles
what a device build reports in `vk_cache/miss`). Bugs found by device testing and worth remembering: per-frame-slot UBO offsets go stale when a program's
uniforms never change; debug-socket screenshots read the game's framebuffer, not the display (use `adb screencap`).

Profile of the same fight on the RP6 (simpleperf, root, GLES; unstripped libmain.so on the desktop; recipe: `simpleperf record -p <pid> -f 1500 --duration 8`):
two busy threads, render (~58% busy) and guest/game (~50% busy); at 240 fps selected the game still ends at ~113 (GLES) / ~116 (Vulkan, with or without
present), i.e. the two threads mostly alternate (frame ~8.6 ms ~ 5.1 + 4.4 ms) rather than overlap, so neither the graphics API nor display refresh is the cap.
Render thread: `getenv` 13-17% (fixed: `PSPRECOMP_GEOM_SELFTEST` was read on every draw), texture-cache content hash 15%, `build_key` 9%, vertex decode 6%, GL driver
~15%. Game thread: dynamic TLS resolver 13%, `RECOMP_LOOKUP`+`ovl_battle_lookup` 13%, VFPU interpreter (`vfpu_dot`, Read/WriteVector, `Int_*`) ~20%, `psp_trace_checkpoint` 5.6%.
Next levers: overlap the guest thread with GE processing (frame pipelining), avoid thread_local in hot paths, compile out the trace checkpoint, cheaper texture hashing.

**Game-thread overhead, frame pipelining, ray queries (2026-11-11, RP6 GLES, 240 fps setting, GPU pinned, same fight):**
* `psp_trace_checkpoint` is now one load + return unless a diagnostic needs it (`PSPRECOMP_FUNC_TRACE=1` and the watch flags, see ENV_FLAGS.md); hot thread_locals use initial-exec TLS on desktop (`psp_tls.h`; NOT on Android, bionic refuses it in a dlopen()ed library). Game-thread CPU 45% -> 41% at the same fps. Fight fps did not move: 113.6 (old), 113.6 (new), 113.1-113.7 (`PSPRECOMP_ASYNC_PRESENT=1`).
* Why: `PSPRECOMP_PROF_SEQ` shows per frame (8.33 ms = 2 slots of the 240 Hz vblank): the guest enqueues the list and keeps building it for ~3.6 ms while the render thread already processes it (list-begin ~0.015 ms after enqueue), the list ends ~1.2 ms after the guest's `DrawSync`, then the guest waits for the vblank and `SetFrameBuf` presents (~0.8 ms). So guest and GE already overlap inside the frame; the render thread (list ~4.8 ms + present ~0.8 ms = 5.5 ms) is the critical path and is above the 4.17 ms slot, hence 2 slots = 120 fps, with the odd 3-slot frame making ~113. **Game-thread trimming and pipelining the present cannot raise this; only cheaper render-thread work (fewer/cheaper draws: 3.5k draws and 306 batches per frame, prim ~4.6 ms) can.** `PSPRECOMP_ASYNC_PRESENT` (present queued instead of awaited) is kept as an off-by-default experiment; it only overlaps the present with the vblank sleep. Making `DrawSync(0)` return early would be the real overlap but is unsafe: games reuse the same display-list / vertex memory every frame and rely on that sync.
* Ray-queried shadows, stage 1: `vk/psp_vk_rt.{h,cpp}` (BLAS+TLAS from a world-space triangle soup, compute shader with `rayQueryEXT`), device features enabled in `psp_vk_context.cpp` when `want_ray_query` (instance 1.2). `test_vk_rt`: 20000 points against a CPU ray caster, 0 mismatches on llvmpipe and on the RX 7900 GRE (RADV; trace 2.3 ms, scene build 183 ms cold). Not on the GTX 1660 SUPER (no ray query) or Adreno 740. Stage 2 not started: capture world-space triangles in the draw path (`ge_transform_vertices` output is clip space; the world matrix is needed), reconstruct positions from depth in `psp_screenfx`, run the query pass in the Vulkan layer (custom compute pass, not a GL call) and use its mask in place of the contact-shadow term; dynamic fighters need a per-frame rebuild.
* Render-thread draw cost: every texture bind hashed the whole texture (about 9 MB per battle frame; 246 of 272 binds hashed memory hashed a moment before). The hash is now memoised per (address, size) until a new list-processing session / SIGNAL / FINISH / TRXKICK (`ge_texture_invalidate_hash_memo`, `PSPRECOMP_TEX_HASH_MEMO=0` turns it off): list time 2.63 -> 2.16 ms on the desktop (-18%), texhash 0.66 -> 0.10 ms; menu and battle pictures unchanged. `PSPRECOMP_PROF=1` now also reports texhash and memo hits. On the 7900 GRE under Vulkan a whole list takes ~1.2 ms and the fight holds 240 fps. Remaining list cost is spread over decode/upload/state; the RP6 number is not measured yet (device was offline).
* RP6 (GL and Vulkan, 240 fps setting, GPU pinned, render scale 4, same fight) with the texture-hash memo: list 5.17 -> 4.66 ms, texhash 0.61 -> 0.12 ms, render-thread CPU 60% -> 54%; fight fps still 113 (GL 113.0 -> 113.6, Vulkan 113.5), because list + present (5.4 ms) is still above the 4.17 ms vblank slot of 240 Hz. Another ~1.3 ms off the render thread (draw/decode/upload/state) is needed before 240 slots start to fit. Android build excludes the ray-query code (`psp_vk_rt.cpp`, `__ANDROID__` guards).
* **Leak found and fixed (2026-11-11, desktop):** the layer allocated a new command buffer for every submission and never freed any (`vkResetCommandPool` resets command buffers, it does not free them). Per-process GPU memory climbed steadily: 9-13 GB of GTT and 10 GB of VRAM after a long session, which filled the 16 GB card and took the game from 120 to 60 to 40 fps. Command buffers are now kept per frame slot and reused (6 in total); a fight holds GTT at 68 MB and VRAM at 937 MB. The `[VKMEM]` line (every 600 frames) reports VMA bytes, object counts and the command buffer count to catch the next one; the per-process view is `/proc/<pid>/fdinfo/*` (`drm-memory-vram`, `drm-memory-gtt`).
* Ray-queried shadows, stage 2 (PSPRECOMP_VK_RT=1, Vulkan only, desktop RX 7900 GRE): `RtAccel` rebuilds a BLAS+TLAS every frame inside the layer's command buffer (one per frame in flight, descriptor set 2); the layer accepts fragment shaders that declare `uniform accelerationStructureEXT` (SPIR-V 1.4, `#version 460` + GL_EXT_ray_query injected by the converter); `ge_draw` captures every depth-writing 3D draw as world-space triangles (second CPU decode with skinning + world matrix, `rt_capture`; blending cannot be used as a filter, the game leaves it on) and passes them through `g_psp_rt` (`psp_rt.h`); `psp_screenfx` builds a pass-1 variant (`rt_main_source`) whose contact-shadow loop is a ray query toward the world-space light (`PSPRECOMP_LIGHT_*`, length `PSPRECOMP_RT_LEN`, default 40). Verified on the desktop at a real display: no validation layer available there, but no errors, 240 fps held (list 1.2 -> 1.5 ms), and with `PSPRECOMP_CONTACT_AMT=1 PSPRECOMP_SSAO=0` the mask shows the fighters' and the stage structures' shadows (more of them than the screen-space march: structures shadow each other and themselves; some patchy self-shadowing on curved meshes is visible, bias/thickness not tuned). Not done: tuning (bias, softness, denoise: the existing blur is reused), a settings-menu row, skipping static geometry rebuilds, Android (unbuilt there; no ray query on Adreno 740 anyway), transparent casters. Bug found on the way: a copy/paste that left `load_procs` recursive passed the standalone test (it loads its own pointers) and crashed in the game with a null call from `RtAccel::allocate`.
* **Ray-traced reflections (2026-11-11, branch rt-reflections; PSPRECOMP_VK_RT_REFL=1 / settings row "Ray-Traced Reflections", Vulkan + ray query only, replaces the Reflections row while on).** The floor reflection (upward-facing surfaces, same gating and Fresnel as the screen-space one) is a closest-hit ray query along the reflected view ray, in the same pass-1 shader variant as the ray-queried shadows (`k_rt_refl` in `psp_screenfx.cpp`). The hard part is shading the hit, since the acceleration structure has positions only. Options and what was picked:
  * (a) full per-triangle UV + texture id with a bindless texture table: not done (a second capture, a descriptor-indexing table in the layer, mip/LOD and CLUT/format handling for every cached texture; large for a floor reflection that is blurred and Fresnel-weighted anyway).
  * (b) **per-triangle colour captured at draw time: done.** `rt_capture` already decodes every depth-writing 3D draw; it now also writes one RGBA8 per triangle = average texel colour of the bound texture (`TexCacheEntry::avg_rgba`, computed once at texture upload from the decoded RGBA over up to ~1024 evenly spaced alpha-weighted texels, `ge_texture_bound_avg_color()`) x the vertex colour (or the lit material: diffuse x 0.85 + 0.1; REPLACE texfunc ignores the vertex colour). It travels through `g_psp_rt.set_scene(xyz, tris, tri_cols)` into a host-visible storage buffer next to the vertex buffer in `RtAccel` (descriptor set 2 binding 1; the GLSL converter declares `uint rt_tri_col[]` in every shader that declares the acceleration structure; indexed with the ray query's primitive index, no index buffer so primitive = triangle).
  * (c) **screen-space assist: done, on top of (b).** The hit point is projected back into the current frame: where it is on screen and the depth buffer there agrees (|d| < 3% + 0.05) the real shaded scene colour is used (lighting, full texture, effects), otherwise the triangle colour. A miss, or a hit on the sky dome (beyond `PSPRECOMP_FX_SKY_DIST`), uses the scene's sky at the projected direction when it is on screen, else the dome triangle's average colour; sky reflections are weighted 0.4 (`PSPRECOMP_RT_SKY_AMT`) because the stage floor texture already carries a baked sky.
  Result on the Order's Sanctuary-style floor (Squall vs Lightning, render scale 3, RX 7900 GRE; `PSPRECOMP_RT_REFL_SPLIT=1` renders the left half of one frame with the screen-space reflection and the right half ray-traced; screenshots with `PSPRECOMP_SHOT_FULL=1`): the ray-traced half reflects structures the screen-space march loses (hidden behind a fighter, off the edge, behind the camera), fighters and ruins appear in the floor with the right shading where they are visible; `PSPRECOMP_RT_REFL_DEBUG=1` tints the floor by what the ray found (floor rays mostly hit the dome, the ruins and fighters are on-screen hits, nothing misses at length 300). Weak spots of (b): an off-screen / hidden hit is a flat per-triangle colour without lighting (blocky patches where a ruin is only seen in the reflection), no reflection of transparent / additive geometry (only depth-writing draws are captured), and glossy floors only (no roughness blur; the existing blur is not applied to the reflection).
  **Follow-ups (same day):** (1) draws whose texture is mostly transparent (mean alpha < 200/255) and that use texture alpha or alpha test are left out of the opaque scene (a horizon/glow card showed as a bright flat quad behind far ruins); hits on surfaces that are not visible on screen are dimmed (`PSPRECOMP_RT_HIDDEN`, their per-triangle colour is unlit), which tames bright patches under far saucer-shaped ruins. Not verified on an identical frame before/after (runs do not repeat exactly). (2) **Effects layer:** blended, depth-tested, non-depth-writing 3D draws (water orbs, beams, auras, glass) were invisible to the reflection (the ray-traced scene only held depth-writing draws, and the screen-space march only sees the depth buffer). They are now captured separately (`rt_capture(..., fx=true)`; alpha byte of the per-triangle colour = texture mean alpha x vertex/material alpha), built into a second BLAS and a second TLAS instance (mask 0x2; the opaque scene is mask 0x1, so shadows ignore effects), colours in storage buffer binding 2 (`rt_fx_col[]`). The reflection shader traces the closest effect in front of the opaque hit and blends it over the opaque reflection (real on-screen colour when it projects on screen). Verified: runs on the 7900 GRE with validation layers, ~3,000-3,400 effect triangles per scene, no messages; effect hits show in the debug tint. Not verified: the water orb itself (no frame with it captured), the cost of the second BLAS, additive effects (treated as alpha-blended). The screen-space method has the same gap for non-depth effects: issue #7.
  **Cost** (GPU ms per frame from the layer's `[VKGL]` line, RX 7900 GRE, render scale 3, same fight start, ~3500-3700 draws, SSAO and contact shadows off): no effect 0.53, screen-space reflections 0.61, ray-traced reflections 1.12, ray-traced shadows 1.11, both ray-traced 1.21. So the acceleration-structure build (about 9k triangles, rebuilt each frame) is the fixed cost (~0.5 ms) and each extra ray-query effect on top of it is about 0.1 ms; render-thread CPU for the second decode (`rt_capture`) is ~+0.05 ms of draw time and ~+8 points of one core's thread usage. Validation layers (`PSPRECOMP_VK_VALIDATION=1`, both ray-traced effects on, a 30 s fight): no messages; `[VKMEM]` stable at 409-427 MB, 6 command buffers. Not done: roughness / blur of the reflection, shading hits with the real texture (option a), skipping static-geometry BVH rebuilds, Android (no ray query on Adreno 740; the code is behind `__ANDROID__` guards like the shadows).

Earlier notes (milestones 0-2): the battle harness's Xvfb gives **llvmpipe (software) GL**; SDL's `offscreen` video driver gives real NVIDIA GL for fair timing.

Original status: planning. Goals: (1) better performance, (2) a base for ray-traced effects later.
Decisions (2026-11-11): Vulkan is a **selectable backend alongside GL** (GL stays the default and the fallback until
parity); targets **desktop (Linux/Windows/Deck) and Android**; user GLSL FX shaders keep working by **compiling GLSL to
SPIR-V at load time**. macOS (MoltenVK) is out of scope for now.

## What exists today (GL)

About 8k lines of renderer written directly against the GL state machine (about 750 `gl*` calls), single GL thread
(`render_queue`, main thread owns the context):

| File | Lines | GL role |
|---|---|---|
| `psp_ge_draw.cpp` | 3.9k | state -> GL, vertex upload (map/sub ring), batching, FBO pool per guest address, blits, readbacks, present, overlays |
| `psp_ge.cpp`, `psp_ge_vertex.cpp` | 2.3k | GE command decode, vertex decode (mostly GL-free) |
| `psp_ge_texture.cpp` | 1k | texture decode + cache + upload (`glTexImage2D`, mipmaps, texture packs) |
| `psp_ge_shader.cpp` | 0.6k | **one uber-shader** driven by uniforms (no permutations), GLSL 330 / ES 300 |
| `psp_fx_shader.cpp` | 0.5k | user fragment-shader overrides (`FX_SHADERS.md`), compiled with `PSP_FX` |
| `psp_screenfx.cpp`, `psp_post.cpp` | 0.9k | screen-space effects, post, depth blits |
| `psp_gl_stats.*`, `psp_touch.cpp`, `psp_event_loop.cpp` | | stats, touch overlay, SDL window/context |

Things that make Vulkan different here:

* Hard sync points are common: `glReadPixels` (screenshots, `GE_SYNC`, framebuffer->guest-RAM paths), `glBlitFramebuffer`
  (8 sites, incl. scaling and depth), in-place "glow" passes that read the buffer they draw into (`fb_snapshot`).
  Vulkan needs explicit barriers/copies for each; the snapshot case becomes a copy + input attachment/sampled read.
* State is set per draw from `GeState`. Vulkan needs pipeline objects: key = blend, depth, stencil, cull, colour mask,
  topology, vertex layout, shader variant, render pass formats. A pipeline cache keyed on that struct, plus dynamic
  state (viewport, scissor, stencil ref, blend constants) keeps the count low.
* `glDepthRange` / `ge_compute_viewport_depth`: Vulkan viewport depth is native (`minDepth/maxDepth`), simplest case.
* Y flip: Vulkan clip space is top-left. Either flip via negative viewport height (core in 1.1) or flip in the vertex
  shader. Texture-as-render-target UV remap (`ge_shader_set_tex_remap`) must be re-checked.
* The GE's alpha test, colour test, 2x alpha, stencil-in-alpha emulation are in the fragment shader and carry over.
  GE logic ops / "blend that GL cannot express" need the same fallbacks (dual-source blend, shader-side) as today.
* Threading: Vulkan has no "context current on one thread" rule, so the main-thread-only restriction relaxes, but keep
  the existing render queue as is for the first version (lowest risk).

## What to borrow from PPSSPP (GPL2+, same licence)

Cloned for reference at `~/code/_ref/ppsspp`. Use these as patterns, copying with attribution where it saves work:

* `Common/GPU/Vulkan/VulkanContext.*`, `VulkanLoader.*`: instance/device selection, extensions, swapchain, loader.
* `VulkanMemory.*`, `VulkanAlloc.h`: push buffers / ring allocator for per-frame vertex + uniform data.
* `VulkanBarrier.*`, `VulkanQueueRunner.*`, `VulkanRenderManager.*`: a recorded-steps render graph that merges render
  passes and derives barriers. This is the heavy piece (about 4-5k lines); a **much simpler first version** (one command
  buffer per frame, explicit barriers at the known sync points above) is enough for us because we have far fewer
  passes than PPSSPP and an uber-shader instead of generated shaders.
* `GPU/Vulkan/StateMappingVulkan.*`, `PipelineManagerVulkan.*`: GE state -> pipeline key.
* `GPU/Vulkan/FramebufferManagerVulkan.*`, `TextureCacheVulkan.*`: render-target and texture handling patterns.
* Shaders: PPSSPP generates GLSL per state and compiles with glslang. We do not need its generator.

## Architecture

New module `runtime/src/vk/` behind a small renderer seam. The seam is the important design choice: today the draw code
calls GL directly, so introducing Vulkan means first isolating the GL calls.

1. **`psp_gfx` backend interface** (narrow, not a full RHI): the operations `ge_draw` actually performs. Roughly:
   `create/destroy_target(w,h,has_depth)`, `bind_target`, `clear`, `create_texture/update_texture/destroy`,
   `upload_vertices -> offset`, `draw(pipeline_state, uniforms, tex bindings, first, count)`,
   `copy/blit(target->target, filter)`, `read_pixels(target) -> cpu`, `present(target)`, `begin_frame/end_frame`.
   `GlBackend` wraps today's code (a mechanical move, behaviour must stay identical); `VkBackend` is the new one.
2. **Backend selection** at start: `PSPRECOMP_RENDERER=gl|vulkan` (and a settings-menu option later), Vulkan falls back
   to GL if no device/extension support. SDL window flag `SDL_WINDOW_VULKAN` vs `SDL_WINDOW_OPENGL` decided before
   window creation (`psp_event_loop.cpp`).
3. **Shaders**: keep GLSL source as the single source. Preamble gets `#version 450` + `layout(set=0,binding=N)` blocks
   instead of loose uniforms (Vulkan has no default-block uniforms), so the uber-shader moves to UBO + push constants
   with a Vulkan-flavoured preamble selected by backend. Compile to SPIR-V with glslang (already installed on this
   host; vendor or use `shaderc` on Android) at startup and cache SPIR-V on disk.
4. **FX shaders**: authors write GLSL with loose uniforms today. The loader rewrites/wraps them: authors' declared
   uniforms collected into a generated UBO (names and `params` in `manifest.json` stay the same), then glslang at load.
   Document the few GLSL constructs that are not supported on Vulkan.
5. **Resources**: VMA (vendored header) for allocation; per-frame upload ring for vertices/uniforms (replaces the
   `map/sub` ring); texture cache keeps its ids, gains `VkImage` handles; render-target pool per guest address stays
   as is, with `VkImage` + `VkImageView` (+ depth/stencil image).
6. **Ray tracing hook (later, not in the port itself)**: design for it now by (a) keeping GE-transformed geometry
   available as a GPU buffer (GE vertex decode + skinning already run on CPU or in the vertex shader; ray tracing wants
   world-space triangles, so skinned meshes would need a compute skinning pass writing a vertex buffer per model),
   (b) device creation that can optionally enable `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
   `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`. First realistic feature: **ray-queried
   shadows/AO** in the existing screen-effects stage (`psp_screenfx.cpp`, which already reconstructs from depth),
   because it does not need a full RT pipeline, only ray queries from a fragment/compute shader. Not available on
   Android in general and not on MoltenVK; always optional, always with the raster path as fallback.

## Milestones (each is testable and leaves GL working)

0. **Baseline + seam (no behaviour change).** Capture reference frames and timings with the existing harness (see
   `games/dissidia/docs/testing.md`); profile where time goes first. If the frame is CPU-bound in the recompiled code or
   in vertex decode rather than in GL submission, the Vulkan port will not speed it up much, and we should know that
   before investing: `psp_prof.h` and the perf HUD exist for this. Then introduce the `psp_gfx` interface and move the GL
   calls behind `GlBackend`. Screenshots must be pixel-identical to before.
1. **Vulkan skeleton.** Loader, instance/device/swapchain on SDL, clear + present a coloured frame, validation layers
   on in debug, backend selection + fallback. Desktop first.
2. **Uber-shader to SPIR-V.** Vulkan-flavoured preamble, UBO layout, offline-or-runtime glslang, SPIR-V disk cache.
3. **Draw path.** Vertex upload ring, pipeline cache and state mapping, textures (decode path is shared), render-target
   pool, clears, depth/stencil. Goal: title screen and a 3D battle render matching GL.
4. **Sync-point features.** Blits and scaling, framebuffer readback, FB snapshot/glow passes, render scale 1..8,
   widescreen, screenshots/capture, overlays (savedata dialog, toast, perf HUD, touch controls), present with letterbox.
5. **Screen-space effects + FX shaders.** Port `psp_screenfx`/`psp_post`; GLSL FX loader with the generated UBO; check
   the shipped manifest renders the same.
6. **Parity and performance pass.** Pixel diff against GL across a scripted set of scenes; frame-time comparison at
   30/60/120/240 fps and at render scale 1x/4x; pipeline-creation hitches (pipeline cache + async creation);
   Steam Deck and Android runs.
7. **Android.** `VK_KHR_android_surface` through SDL, driver quirks, precompiled SPIR-V (no runtime glslang if too
   heavy), Vulkan 1.1 baseline, GL fallback kept.
8. **Ray tracing prototype** (optional device feature), per the hook above.

## Risks / open points

* **The speed-up is not guaranteed.** Needs the milestone 0 profile. The likely wins are lower driver CPU overhead per
  draw (many small batches), no `glFinish`-style implicit syncs, async pipeline compile, and a better path for
  multi-resolution rendering; the likely losses are pipeline compile hitches without a cache.
* The `psp_ge_draw.cpp` monolith (3.9k lines, 356 GL calls) is where nearly all the risk sits; the seam refactor must
  not change GL output.
* PSP-specific blend/stencil/alpha behaviour is fixed up in shaders today; some of it relies on GL semantics
  (e.g. FBO-as-texture feedback within a draw, undefined in Vulkan unless handled with a copy or input attachment).
* Runtime GLSL compile on Android is heavy; plan for precompiled SPIR-V for the shipped shaders and runtime compile
  only for user FX shaders.
* `localtuya`-style local patches do not matter here, but `glad.c` / `psp_gl.h` (GLES mapping) stay for the GL backend.

**Custom GPU drivers and ray query on Android (2026-11-12, RP6 Adreno 740):** `psp_gpu_driver.cpp` + libadrenotools load a Turnip driver (Settings > GPU Driver, live download list). Mesa Turnip v26.0.0 R8 comes up as "Turnip Adreno (TM) 740, Vulkan 1.4.335" and the game renders the title screen on it. The phone's own driver (Vulkan 1.3.128) already exposes `VK_KHR_ray_query` + `VK_KHR_acceleration_structure` on this device, so the earlier "no ray query on Adreno 740" note was wrong; `psp_vk_rt.cpp` and the `RtAccel` path are now compiled and enabled on Android (the compute test shader there is optional: Android has only the SPIR-V cache). Ray query initialises (`[VKGL] ray query: available`); shadows in a real fight on the phone are not verified yet, and the ray-query fragment variant needs its SPIR-V in `vk_cache/` like every other shader (a desktop run with ray-traced shadows fills it).


* RT scene fixes for broken-object shadows (issue #4): (a) the game culls draws outside the camera, so a caster just off screen dropped out of the RT scene and its shadow popped away; settled geometry (the same draw, byte for byte, in 3 frames) is now kept while its box is wholly outside the view and dropped once it is inside the view and no longer drawn (`PSPRECOMP_RT_NO_PERSIST=1` turns this off). (b) The broken-block debris (vertex type 0x99) is drawn with its own view matrix, so its world matrix alone put the shadows in the middle of the floor; such draws are re-expressed in the main view's world (`inv(view_main) * view_draw * p`, `PSPRECOMP_RT_NO_VIEWFIX=1` turns it off). Debugging aid: F9 in the window logs every 3D draw of the next frame with its state, bounding box and camera (`rtdump.log`) and saves `rtdump_N.tga`; `PSPRECOMP_RT_DUMP_EVERY=N` does it every N frames for automated runs.

## Screen-space reflections: effects layer (issue #7)

The screen-space reflection (`psp_screenfx.cpp`, pass 1) marches against the scene depth, which blended effects (orbs, beams, auras, glass: depth tested,
not depth writing) never touch. Every such draw (the same test as the RT effects layer) is issued a second time into a per-target layer (`fxl_*` in
`psp_ge_draw.cpp`): RGBA8 colour (the game's own blend over black = premultiplied; alpha = coverage, additive draws leave it alone) plus its own
DEPTH24_STENCIL8 depth texture (writes on = the nearest effect fragment per pixel; the Vulkan layer supports no other depth texture format). The march
hit-tests it beside the scene depth and blends the effect over the opaque reflection with a stronger base reflectivity than solid geometry
(`PSPRECOMP_SSR_FX_GAIN`). An effect only counts when it floats clearly in front of the scene at that pixel (the floor's own water sheet would
otherwise cover everything). The layer is cleared after the pass used it, so effects the game draws after its 3D run show up one frame late.
A ray that leaves into the sky samples the picture where its direction projects (the stage's sky beams). Not used with ray-traced reflections.
Cost: one extra full-size target per displayed buffer and a second submit of the blended draws. The Vulkan layer allows 4 samplers per program, which
this uses up. Also fixed: ray-traced reflections lost ray query when Ray-Traced Shadows was saved Off (`PSPRECOMP_VK_RT=0`), and RT descriptors are
rewritten after every acceleration-structure re-allocation (a reused handle value left them stale). Tested on RP6 (Turnip) and a GTX 1660 (GL).

* Stage looks and stage lights (issue #4): the game module reports the stage id (`psp_stage_id()`, Dissidia: u32 at 0x08B98854 while the battle overlay is loaded); with Ray-Traced Shadows on, `k_stage_looks` in `psp_screenfx.cpp` darkens a stage (Pandaemonium 0.3: enclosed, no sun; `PSPRECOMP_STAGE_DARK` overrides) and hand-placed point lights from `stage_lights.txt` (`stage <id>` / `light x y z r g b radius intensity`, re-read when the file changes, `PSPRECOMP_STAGE_LIGHTS` for another path) are added with ray-traced shadows: albedo = the scene colour, drawn into the reflection layer (premultiplied, additive). In game: F6 drops a light at camera + forward * distance (PageUp / PageDown change the distance, default 8) and turns edit mode on, F8 toggles edit mode (a glowing marker at every light, the selected one white with a yellow ring, dimmed behind walls), F7 removes the selected light. In edit mode (Shift = 4x speed): J / L left / right, I / K forward / back (camera relative, level), U / O down / up, , and . radius, - and = intensity, [ and ] select the previous / next light; changes are saved to the file shortly after you stop. Colour is edited in the file. Lights are omnidirectional points; radius is the reach (the light fades to nothing at that distance), intensity the brightness multiplier.
