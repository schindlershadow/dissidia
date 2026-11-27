#pragma once
#include <cstdint>

/// Computed GL viewport rectangle (FBO pixels, GL bottom-left origin) and
/// depth range, derived from the PSP viewport scale/offset + depth registers.
struct GeViewportDepth {
    int x, y, w, h;       // glViewport args (FBO pixels)
    float near_z, far_z;  // glDepthRange args, clamped to [0,1]
};

/// Pure math: PSP viewport scale/offset + depth -> GL viewport + depth range
/// (issue #23). Mirrors PPSSPP ConvertViewportAndScissor for the 1:1,
/// non-supersampled, non-accurate-depth FBO case. Separated from the GL call
/// site (apply in ge_draw_prim) so it is unit-testable without a GL context.
/// off_x_raw/off_y_raw are the raw OFFSETX/OFFSETY register words (1/16
/// subpixel, low 16 bits used). fb_height is the render-target height in
/// pixels (PSP top-left origin is flipped to GL bottom-left).
GeViewportDepth ge_compute_viewport_depth(
    float vp_x_scale, float vp_y_scale,
    float vp_x_center, float vp_y_center,
    float vp_z_scale, float vp_z_center,
    uint32_t off_x_raw, uint32_t off_y_raw,
    int fb_height);

/// Internal render scale n (PSPRECOMP_RENDER_SCALE, 1..16): every target is
/// 480n x 272n; viewports are multiplied by n. Valid after ge_draw_init().
int ge_draw_render_scale();

/// Widescreen (settings menu "Widescreen", PSPRECOMP_WIDESCREEN=1): the displayed buffers get as wide as the window's
/// shape needs (never narrower than 480 PSP pixels): the 3D scene fills the extra width, the HUD stays centred, a
/// full-screen background or fade is stretched. Render thread / game paused. Width: current buffer width in PSP pixels.
void ge_draw_set_widescreen(bool on);
bool ge_draw_widescreen();
int ge_draw_widescreen_width();
// Called by a game module once per frame while a full-screen 3D view is up (a battle): on a tall window the PSP's picture is then
// centred in the taller buffer instead of at its top (menus), with the 3D scene filling above and below. Any thread.
void ge_draw_note_scene_view();
// The game's 2D HUD on a tall window in battle (set once by a game module): a 2D draw entirely in the top `top_rows` rows of the PSP's
// screen is pinned to the top of the window (under the touch controls' top row), one entirely in the bottom `bottom_rows` rows to its
// bottom edge; the rest (lock-on markers) stays with the centred picture. The touch controls' bottom row sits above the bottom
// `bottom_visible` rows (where the HUD has content; a sprite in the zone can start higher with transparent pixels).
void ge_draw_set_hud_zones(int top_rows, int bottom_rows, int bottom_visible);
int ge_draw_hud_bottom_rows();   // the room kept for the HUD under the touch controls, PSP rows (0 = none)
// Textures (tex_addr & 0xFFFFFF) whose 2D draws are never pinned: markers that follow a fighter (the lock-on ring) stay on the picture.
void ge_draw_set_hud_fixed_tex(const uint32_t* addrs, int n);
// The world position (feet) of the target the lock-on ring is on, for gameplay frame `gen` (the interpolation tag's gen): newest `cur` and the frame before `prev` (null: unknown); cur null = the target has no usable position: the ring gets no offset. Game thread, every frame.
void ge_draw_set_follow_world(uint32_t gen, const float* prev, const float cur[3]);
void ge_draw_set_hud_fixed_tex_hash(const uint64_t* hashes, int n, const uint8_t* rdram);   // the same by content (FNV-1a of the first 256 bytes of the texture's memory): the address moves between builds
void ge_draw_set_hud_follow_scale(float k);   // 2D follow marks: move their centre about the screen centre by k (FOV change on the game side); 1 = off
// Test harnesses: 0 draw as usual, 1 rasterizer discard, 2 skip the draw calls (lists are still processed). Applied at the next present.
void ge_draw_request_no_raster(int mode);

/// Frame interpolation (a game that runs its logic at 30 Hz and re-issues its frame on the frames in between).
/// Game thread, before each frame's lists are enqueued (a list takes the tag current when it is enqueued, so the game module must publish
/// the tag of the frame whose list is about to be submitted: Dissidia submits a frame's list after its frame-end hook, see framerate.cpp):
/// gen = number of the gameplay frame whose state the frame shows,
/// phase = how far the frame is shown past that gameplay frame, in gameplay frames (0..1; k/n for the k-th of n-1 drawn-only frames
/// when paced by frame count, measured time when paced by the clock; down to -0.5 for a gameplay frame shown early); -1 turns it off. record = this is the gameplay frame
/// itself (its draws become the current table); seq numbers the frames.
/// The renderer records every 3D draw's world / view / projection / bone matrices on gameplay frames and draws each frame with
/// matrices between the last two gameplay frames (PSPRECOMP_INTERP=1, default: one gameplay frame behind) or projected ahead of
/// the last one (=2); =0 off. A draw is matched with the one of the previous gameplay frame by what it draws (vertex / index / texture
/// address, type, count, target) and its occurrence; unmatched draws and jumps (cuts, teleports) are drawn as issued.
struct GeInterpTag { uint32_t gen = 0, seq = 0; float phase = -1.0f; bool record = false; };
void ge_interp_set_frame(const GeInterpTag& tag);
GeInterpTag ge_interp_current_tag();                // render queue: the tag a list gets at enqueue
void ge_interp_list_tag(const GeInterpTag& tag);    // render thread: the tag of the list about to be processed
// Height (PSP rows) of the displayed buffers: 272, or more on a tall window. The settings screen draws a canvas that tall; an overlay
// canvas taller than 480:272 is drawn over the whole picture instead of the PSP's band.
int ge_draw_tall_rows();

/// Change the render scale live (settings menu): resizes every render target,
/// keeping its picture. Main (GL) thread, game paused. False if refused.
bool ge_draw_set_render_scale(int n);
bool ge_draw_scene_format_stale();  // the targets' format no longer matches psp_hdr_scene_live()
bool ge_draw_sync_scene_format();   // HDR Scene on/off (psp_post.h): reallocate the scene targets in fp16 / RGBA8. Game parked.

/// Settings menu, Debug page (main/GL thread, game paused; not saved).
/// GPU sync mode, as PSPRECOMP_GE_SYNC: 0 off, 1 glFinish per batch, 2 per
/// display list, 3 per present, 4 glFlush per batch.
int ge_draw_sync_mode();
void ge_draw_set_sync_mode(int mode);

/// Glow debugging (settings menu, Debug page). The fb prefilter softens the game's in-place glow passes at render
/// scale > 1 (see psp_ge_draw.cpp); the snapshot is what lets such a pass read the buffer it draws into (off: the
/// old memory path). Both default on; flipped live to find what makes a ghost around bright objects.
bool ge_draw_fb_prefilter();
void ge_draw_set_fb_prefilter(bool on);
bool ge_draw_fb_snapshot();
void ge_draw_set_fb_snapshot(bool on);
/// Vertex upload mode in use: 1 map, 0 sub (PSPRECOMP_GE_RING; changes at launch).
int ge_draw_ring_mode();
/// FX shader overrides: available = a manifest with usable entries was loaded
/// at start (PSPRECOMP_FX_SHADERS=0 or no manifest -> false). The live switch
/// only bypasses them; turning them back on needs no reload.
bool ge_draw_fx_available();
bool ge_draw_fx_shaders();
void ge_draw_set_fx_shaders(bool on);
/// Simple fps counter: just the frames-per-second number, white and partly transparent, in the top-right
/// corner (PSPRECOMP_FPS_COUNTER=1 turns it on at start).
bool ge_draw_fps_counter();
/// Ping to the other players next to the fps number, while in an online group (Network page "Show Ping", off by default).
bool ge_draw_ping_overlay();
void ge_draw_set_ping_overlay(bool on);
void ge_draw_set_fps_counter(bool on);
/// A game module's own 2D overlay: an RGBA canvas (bytes R, G, B, A; row 0 = top) of w x h pixels, laid over the picture
/// (the middle 480-pixel-wide area when widescreen), alpha-blended each present. A canvas that stays unchanged costs nothing;
/// call again to update it; null / 0 size removes it. Thread safe (the game thread sets it, the present thread draws it).
void ge_draw_set_game_overlay(const uint32_t* rgba, int w, int h);
/// Performance overlay (debugging): frames per second, average / worst frame time, presents per second, render
/// scale and process memory, in the top-left corner (PSPRECOMP_PERF_HUD=1 turns it on at start).
bool ge_draw_perf_overlay();
void ge_draw_set_perf_overlay(bool on);
/// Whether the game is in gameplay (a battle) right now: the overlay's average / 1% low frame rates count only those frames and restart
/// when gameplay begins again. Call it every frame; a game that never calls it has every frame counted.
void ge_draw_perf_set_gameplay(bool on);

/// A short message over the picture, top centre, for `ms` milliseconds (a new call replaces the old
/// message). Any thread. Used for "frame rate set to 30 fps" when a multiplayer group agrees on one.
void ge_draw_toast(const char* text, int ms, int y_px = 10);   // y_px: distance of the box from the top of the picture, in PSP pixels (272 high)

/// A bright frame around the picture (3% thick on every side, colour 0xRRGGBB) for `ms` milliseconds; a new call replaces the old one.
/// Any thread. Debug aid (the game module's timing helpers).
void ge_draw_flash(uint32_t rgb, int ms);

/// View blocker fade: while `active`, static scene geometry (not skinned) that lies between the camera and `target` (world space, the
/// player's chest) fades out to a see-through alpha, like the game's own wall fade does for its camera (the game computes that from its
/// own eye, so a camera we move ourselves needs it done here). Call once per camera update (game thread).
bool ge_draw_last_view_proj(float view[12], float proj[16]);   // view (4x3) and projection (4x4) of the last completed main scene (column-major, GE layout)
bool ge_draw_last_view_proj_tag(uint32_t* gen, float* phase);   // the frame-interpolation tag (gen, phase; -1 = none) of the display list that view / projection came from (any thread)
void ge_draw_last_eye(float eye[3], float fwd[3]);   // debug: eye and view direction of the last depth-tested 3D draw
void ge_draw_set_view_blocker(bool active, const float target[3], const float eye[3]);   // eye: the game camera's eye this update; only draws made from that view fade
// Camera collision against the stage geometry the main view has drawn (static, non-skinned, depth-writing draws, cached in world
// space per stage): ask for the segment from -> to (game thread); the answer, the fraction of the segment before its first hit
// (1 = clear), comes from the render thread a frame later. Returns the latest answer and the id of the request it belongs to.
void ge_draw_camera_probe_request(const float from[3], const float to[3], uint32_t id);
float ge_draw_camera_probe_result(uint32_t* id);

/// Game layer: a translucent RGBA image (straight alpha, byte order R,G,B,A, row 0 = top) that a game module draws over the game's
/// picture, covering the PSP's 480x272 area (w x h = 480*s x 272*s), under dialogs, menus and touch controls. Used to add elements to
/// the game's own screens (an extra menu row). The image is copied; it is shown until cleared, or for 0.15 s after the last call
/// (so a screen that stops calling it loses the layer by itself). Any thread.
void ge_draw_set_game_layer(const uint32_t* rgba, int w, int h);
void ge_draw_clear_game_layer();
/// The next ge_draw_set_game_layer stays shown this many ms without a newer one (default 150).
void ge_draw_set_game_layer_ttl(int ms);
/// Counts every set / clear of the layer; a module that caches its own image compares it to know the layer is still its own.
uint64_t ge_draw_game_layer_serial();
/// Keeps the layer shown as it is for another ttl (the one set by ge_draw_set_game_layer_ttl, else 150 ms) without a new image (no upload).
/// False when no layer is held (it expired or was cleared): publish again.
bool ge_draw_game_layer_touch();
/// Parts of the game's own picture redrawn elsewhere under the game layer, this frame's pixels (e.g. a neighbouring row's underline
/// for an added menu row, whatever texture pack draws it): source and destination rectangles in PSP units (480x272, y down), a
/// destination of another size stretches. Opaque. Kept with the layer image; set them before ge_draw_set_game_layer.
struct GeLayerCopy { float sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1; };
void ge_draw_set_game_layer_copies(const GeLayerCopy* c, int n);

/// Present the last frame with the dialog/menu overlay now (game paused).
/// Main (GL) thread.
void ge_draw_present_overlay_now();
/// Where the 480x272 overlay was last drawn, as fractions (0..1) of the window: lets the settings menu map a pointer to its canvas.
void ge_draw_overlay_rect(float* fx0, float* fy0, float* fx1, float* fy1);

/// Initialize the GE draw infrastructure: FBO, VAO/VBO, shader.
/// Must be called after SDL/GL init and after ge_init()/ge_texture_init().
void ge_draw_init();

/// Shutdown the GE draw infrastructure.
void ge_draw_shutdown();

/// Called at start of display list processing (bind FBO, etc.)
void ge_draw_begin_list();

/// Called at end of display list processing.
void ge_draw_end_list();

/// Bind the GL FBO for the guest framebuffer address fb_addr_raw (FRAMEBUFPTR
/// or sceDisplaySetFrameBuf), creating it in the generic per-address pool if
/// needed (issue #59). key==0 keeps the current target. Render (GL) thread.
void ge_draw_select_target(uint32_t fb_addr_raw);
void ge_draw_note_framebufptr();   // a GE_CMD_FRAMEBUFPTR was decoded (key 0 is now real)

/// Colour texture of the pool target whose normalised VRAM key matches (0 if
/// none, or if it is the current render target). w/h receive the target's
/// pixel size, 480*n x 272*n for render scale n.
unsigned int ge_draw_find_target_texture(uint32_t key, int* w, int* h);

/// Mark the front buffer dirty (a list was processed) for the liveness net.
void ge_draw_mark_dirty();

/// Liveness net: if content is dirty and no real page-flip happened within
/// PSPRECOMP_PRESENT_STALE_MS (default 100ms), present the front buffer once.
/// MUST be called from the main (GL) thread — wired into psp_event_loop.
void ge_draw_present_if_stale(uint8_t* rdram);

/// Full draw pipeline for a GE PRIM command:
/// decode vertices -> transform -> upload VBO -> set GL state -> draw.
void ge_draw_prim(
    uint8_t* rdram,
    int prim_type,
    int count
);

/// Present the FBO to the SDL2 window via glBlitFramebuffer.
/// Does NOT read rdram for pixel data (FBO blit only).
void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
);

/// Snapshot the GE draw counters (issue #35 debug socket I command).
/// Lock-free racy reads of the draw-thread counters -- values may be a
/// few prims stale, which is fine for diagnostics. Any pointer may be null.
void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
);

/// On-demand screenshot (issue #35 debug socket S command).
/// Callable from ANY thread: posts a capture request and blocks until the
/// render thread services it (or timeout_ms elapses). Returns true once
/// the TGA file has been written. Never touches GL itself.
bool ge_draw_capture_screenshot(const char* path, int timeout_ms);

/// Debug socket V command: capture the whole window (drawable size, with
/// letterbox bars) as a TGA. Same blocking contract as the screenshot call.
bool ge_draw_capture_window(const char* path, int timeout_ms);

/// Render-thread side of ge_draw_capture_screenshot: if a capture request
/// is pending, read the FBO and write the TGA. MUST be called from the
/// main (GL) thread -- wired into psp_event_loop next to
/// render_queue_process().
void ge_draw_service_screenshot_request();

/// Submit the pending batched draw, if any. Must be called (render thread)
/// before anything that depends on the draws having reached GL: display-list
/// exit / stall, render-target switches, clears, present, block transfers and
/// guest callbacks that may rewrite texture memory.
void ge_draw_flush();
void ge_draw_list_end();   // flush + optional per-list GPU sync (PSPRECOMP_GE_SYNC=2)
