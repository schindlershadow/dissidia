# Vulkan backend: design and plan

Status (2026-11-11): **milestone 1 (skeleton) done**: `runtime/src/vk/psp_vk_context.*` (instance, device pick, VMA, SDL
swapchain) and `tests/test_vk_context.cpp` (`test_vk_context [--window]`) pass on the NVIDIA, Intel and lavapipe devices of the dev
host, headless and with a swapchain under Xvfb. Not yet linked into the runtime; GL code untouched.

Test-host facts found in milestone 0: the battle harness's Xvfb gives **llvmpipe (software) GL**, so GL numbers from it say
nothing about real GPU speed, while Vulkan under the same Xvfb does reach the real GPU (NVIDIA swapchain works). Fair GL vs
Vulkan timing needs a real display or a headless GL path. Software-GL profile of a 60 fps battle: ~3.5k draws and 300 batches per
frame, list=7.5 ms, draw submission 4.2 ms, 4 feedback snapshots/frame. The validation layer is not installed on this host.
GL state already goes through ~15 cached setters (`gl_blend_func`, ... in `psp_ge_draw.cpp`), which is the natural seam
for a `PipelineState` struct.

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
