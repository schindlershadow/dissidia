#include "psp_gl_window.h"
#include "psp_quit.h"
#include "hle/psp_savedata_dialog.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_adhoc.h"
#include "psp_post.h"
#include "psp_screenfx.h"
#include "psp_rt.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "psp_ge_vertex.h"
#include "psp_ge_texture.h"
#include "psp_ge_shader.h"
#include "psp_fx_shader.h"
#include "psp_refl_table.h"
#include <unordered_set>
#include "psp_fep_log.h"
#include "psp_event_loop.h"
#include "psp_prof.h"
#include "psp_memory.h"

#include <SDL.h>
#include "psp_gl.h"
#include "psp_touch.h"
#include "psp_perf.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "psp_settings.h"
#include <array>
#include <atomic>
#include <unordered_map>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <array>
#include <string>
#include <algorithm>
#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <random>
#include "psp_platform.h"
#include <cstdio>
#ifdef __linux__
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
void gl_stats_frame();   // psp_gl_stats.cpp

// ---- Generic FBO pool (issue #59 — honor fb_addr / double buffer) ----
// One GL FBO per distinct guest VRAM framebuffer address. The GE binds the
// FBO for the current FRAMEBUFPTR; present blits the FBO matching the
// displayed (sceDisplaySetFrameBuf) address. Keying on the guest's own
// addresses keeps this generic: a single-buffered title keys everything to
// one slot and degenerates to the old single-surface behavior.
static constexpr int GE_MAX_TARGETS = 8;  // Dissidia story scenes cycle through 5+ keys; 4 thrashed (evicted RTT contents)

struct GeTarget {
    uint32_t key = 0;          // fb_addr normalized to eDRAM offset (0 = empty)
    GLuint fbo = 0;
    GLuint color_tex = 0;
    GLuint depth_rb = 0;
    GLuint ab_fbo = 0, ab_tex = 0, ab_depth = 0;   // PSPRECOMP_GE_AB twin (legacy-path shadow)
    uint64_t last_use = 0;     // LRU stamp (monotonic counter)
    int rs = 1;                // this target is (w480*rs) x (272*rs): g_rs for the buffers shown on screen, 1 for the others
    int w480 = 480;            // width in PSP pixels: 480, or wider for a displayed buffer when widescreen is on
    int h272 = 272;            // height in PSP pixels: 272, or taller for a displayed buffer on a tall window (see g_tall_px)
    bool mip_dirty = true;     // drawn into since its mipmaps were last built (see ge_draw_find_target_texture)
};

// Render scale (PSPRECOMP_RENDER_SCALE, 1..16 or auto; default 2): targets are 480*g_rs x 272*g_rs.
static int g_rs = 1;
// HDR Scene (psp_post.h): the scene targets are fp16 while it is live, so additive effects keep values above 1.0.
// g_scene_fp16 is the format the targets currently have; ge_draw_sync_scene_format() reallocates them when it changes.
static bool g_scene_fp16 = false, g_scene_inited = false;
static GLint scene_ifmt() { return g_scene_fp16 ? GL_RGBA16F : GL_RGBA8; }
static GLenum scene_type() { return g_scene_fp16 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE; }
static GeTarget g_targets[GE_MAX_TARGETS];
// Only the buffers that reach the screen are rendered at the render scale. Every other target the game draws into
// (the chain of smaller buffers it blurs for glow / bloom) stays at the PSP's own resolution, as in PPSSPP: games
// assume 480x272 and run a fixed number of blur steps, which at 4x are too few for a smooth blur and leave a hard,
// ghosted copy around bright objects and characters. Only the first copy from the big picture into the first small
// buffer aliases; the later steps blur normally. PSPRECOMP_RENDER_SCALE_ALL=1 scales every target again.
// Widescreen (PSPRECOMP_WIDESCREEN=1, settings menu): the displayed buffers are wider than 480 PSP pixels when the window is
// wider than 16:9. The 3D scene fills the extra width (the horizontal field of view grows, the vertical stays), the
// HUD and every other 2D draw stay centred at their original size, and a full-screen background or fade is
// stretched over the whole width (see ge_set_wide in psp_ge_vertex.h).
// -1 = not read yet: the env var is read on first use, because the saved settings.ini is applied to the environment
// only after static initialisation.
static std::atomic<int> g_wide_state{-1};
static bool wide_on() {
    int v = g_wide_state.load();
    if (v < 0) { const char* e = std::getenv("PSPRECOMP_WIDESCREEN"); v = (e && e[0] && e[0] != '0') ? 1 : 0; g_wide_state.store(v); }
    return v != 0;
}
static int g_wide_px = 480;      // width of the displayed buffers, PSP pixels (even, 480..960)
// Tall windows (a folding phone's inner screen, see psp_touch_tall_layout): the displayed buffers are taller than 272 so the
// picture fills the window. The PSP's 480x272 picture keeps the TOP 272 rows of the buffer (the HUD, the menus and every 2D draw
// stay there, above the touch controls); a 3D draw over the whole screen is given a projection that keeps that band exactly as
// before and goes on below it (more of the scene under the band); clears cover the whole buffer; a 2D draw that covers the whole
// screen with no texture (a fade, a flash) is stretched over the whole height. PSPRECOMP_TALL_FILL=0 turns it off (the picture
// then stays 480x272 at the top of the window).
static int g_tall_px = 272;      // height of the displayed buffers, PSP pixels (even, 272..960)
// Where the PSP's band sits in a tall buffer this frame, PSP pixels from its top: 0 (menus, the default), or centred (moved up to clear
// the touch controls' bottom row if need be) while the game reports a battle view (ge_draw_note_scene_view, by the game module).
static int g_band_off = 0;
static std::atomic<uint32_t> g_scene_note{0};   // present count when the game last reported a battle view (0 = never)
static uint32_t g_present_count = 0;
// GL row (from the bottom) of the band's lowest row in a buffer `h` PSP pixels tall (0 for a normal buffer)
static int band_b(int h) { return h > 272 ? h - 272 - std::min(g_band_off, h - 272) : 0; }
void ge_draw_note_scene_view() { g_scene_note.store(g_present_count + 1, std::memory_order_relaxed); }
static int g_hud_top = 0, g_hud_bottom = 0, g_hud_visible = 0;   // ge_draw_set_hud_zones
static bool g_hud_live = false;               // this frame: battle view on a tall buffer, HUD zones pinned
static int g_hud_top_pin = 0;                 // PSP row (from the buffer's top) the top zone starts at: under the touch controls' top row
void ge_draw_set_hud_zones(int top_rows, int bottom_rows, int bottom_visible) {
    g_hud_top = std::max(0, top_rows); g_hud_bottom = std::max(0, bottom_rows); g_hud_visible = std::clamp(bottom_visible, 0, g_hud_bottom);
}
int ge_draw_hud_bottom_rows() { return g_hud_visible; }
int ge_draw_tall_rows() { return g_tall_px; }
static uint32_t g_hud_fixed[8];
static int g_hud_fixed_n = 0;
void ge_draw_set_hud_fixed_tex(const uint32_t* addrs, int n) {
    g_hud_fixed_n = std::clamp(n, 0, 8);
    for (int i = 0; i < g_hud_fixed_n; i++) g_hud_fixed[i] = addrs[i] & 0xFFFFFFu;
}
// The same textures by content (ge_draw_set_hud_fixed_tex_hash): the battle HUD archive is not at one address in every run (the lock-on ring was at
// 0xC33070 when the table above was made, and is at 0x81D0F0 now, in 1v1 and in the tag team alike), so the ring stopped following. FNV-1a of the
// first 256 bytes of the texture's memory.
static uint64_t g_hud_fixed_hash[8];
static int g_hud_fixed_hash_n = 0;
static const uint8_t* g_hud_fixed_ram = nullptr;
void ge_draw_set_hud_fixed_tex_hash(const uint64_t* hashes, int n, const uint8_t* rdram) {
    g_hud_fixed_hash_n = std::clamp(n, 0, 8); g_hud_fixed_ram = rdram;
    for (int i = 0; i < g_hud_fixed_hash_n; i++) g_hud_fixed_hash[i] = hashes[i];
}
static bool hud_fixed_tex(const GeState& st) {
    if (!st.texture_enable) return false;
    const uint32_t a = st.tex_addr[0] & 0xFFFFFFu;
    for (int i = 0; i < g_hud_fixed_n; i++) if (g_hud_fixed[i] == a) return true;
    if (g_hud_fixed_hash_n > 0 && g_hud_fixed_ram && a >= 0x040000u && a < 0x1000000u) {
        uint64_t hv = 1469598103934665603ull; const uint32_t ba = (a | 0x08000000u) & 0x07FFFFFFu;
        if (ba + 256u > 0x08000000u) return false;
        for (uint32_t k = 0; k < 256; k += 4) { uint32_t w; std::memcpy(&w, g_hud_fixed_ram + ba + k, 4); hv = (hv ^ w) * 1099511628211ull; }
        for (int i = 0; i < g_hud_fixed_hash_n; i++) if (g_hud_fixed_hash[i] == hv) return true;
    }
    return false;
}
// Field of view changed on the game's side (psp_ge_draw.h): the 2D marks that follow a fighter (lock-on ring, target pointer) were placed by
// the game with its own FOV. Their centre is moved about the screen centre by the ratio of the focal lengths; their size is kept.
static std::atomic<float> g_hud_follow_k{1.0f};
void ge_draw_set_hud_follow_scale(float k) { g_hud_follow_k.store(k > 0.1f && k < 10.f ? k : 1.0f); }
bool ge_draw_hud_follow_scale(const GeState& st, float* k) {
    const float v = g_hud_follow_k.load();
    if (v == 1.0f || !hud_fixed_tex(st)) return false;
    *k = v; return true;
}
static uint32_t g_main_keys[8];
static int g_main_n = 0;
static const bool g_scale_all = [] { const char* e = std::getenv("PSPRECOMP_RENDER_SCALE_ALL"); return e && e[0] && e[0] != '0'; }();
static bool is_main_key(uint32_t key) {
    if (g_scale_all) return true;
    for (int i = 0; i < g_main_n; i++) if (g_main_keys[i] == key) return true;
    return false;
}

static int g_current_target = -1;   // slot the GE is rendering into
static int cur_rs();                // scale of that target (g_rs when none)
static int g_front_target = -1;     // slot last presented / most recent render
static uint64_t g_target_clock = 0; // monotonic LRU counter

// Normalize any guest framebuffer address to its eDRAM 2 MB offset key.
// Strips the cached/uncached/eDRAM base via the canonical 0x07FFFFFF PSP
// mask, then takes the 16-byte-aligned eDRAM offset (PPSSPP model). Used by
// both ge_draw_select_target (FRAMEBUFPTR) and ge_present_frame
// (sceDisplaySetFrameBuf) so the two address spaces reconcile.
static inline uint32_t ge_fb_key(uint32_t addr) {
    return (addr & 0x07FFFFFFu) & 0x001FFFF0u;
}

// Framebuffer-as-texture: if the current texture's address is a VRAM address
// whose ge_fb_key matches a live pool target, return that target's colour
// texture (and its pixel size). Returns 0 when the texture should come from
// memory (not VRAM, no such target, or the target is the one being rendered
// into -- a feedback loop, logged once).
static int cur_rs() { return (g_current_target >= 0 && g_targets[g_current_target].fbo) ? g_targets[g_current_target].rs : g_rs; }
static int cur_w480() { return (g_current_target >= 0 && g_targets[g_current_target].fbo) ? g_targets[g_current_target].w480 : g_wide_px; }
static int cur_h272() { return (g_current_target >= 0 && g_targets[g_current_target].fbo) ? g_targets[g_current_target].h272 : g_tall_px; }
static void flush_batch();
static void delete_scratch();
// One scratch copy per target scale (the blur chain at 1x and the main picture at g_rs alternate every frame).
static GLuint g_fb_scratch_tex = 0, g_fb_scratch_fbo = 0;       // the one for the current target's scale
struct ScratchEntry { int w = 0, h = 0; GLuint tex = 0, fbo = 0; };
static ScratchEntry g_scratch[6];     // one per target size (the 1x blur chain and the wide main picture alternate every frame)
static int g_scratch_rs = 0;

// A pass that reads the framebuffer it draws into (Dissidia's glow: the picture shifted a pixel or two and blended
// back over itself) relies on the PSP's bilinear filter averaging neighbouring PSP pixels. At a render scale above 1
// one PSP pixel is rs x rs texels, so the same shift of a sharp picture draws a hard second copy a few pixels away
// instead of a soft blur: a ghost around every character and bright object. The snapshot is therefore sampled through
// its mipmaps at level log2(rs) (each tap covers the area of one PSP pixel, as on hardware).
// PSPRECOMP_FB_PREFILTER=0 turns it off; the debug socket's `P <0|1>` flips it at run time.
static std::atomic<bool> g_fb_prefilter{[] { const char* e = std::getenv("PSPRECOMP_FB_PREFILTER"); return !(e && e[0] == '0'); }()};
void ge_draw_set_fb_prefilter(bool on) { g_fb_prefilter.store(on); std::fprintf(stderr, "[DRAW] feedback prefilter %s\n", on ? "on" : "off"); }
bool ge_draw_fb_prefilter() { return g_fb_prefilter.load(); }
// PSPRECOMP_GE_FEEDBACK=0 / the settings menu: a pass reading the buffer it draws into takes the old memory path
static std::atomic<bool> g_fb_snapshot_on{[] { const char* e = std::getenv("PSPRECOMP_GE_FEEDBACK"); return !(e && e[0] == '0'); }()};
void ge_draw_set_fb_snapshot(bool on) { g_fb_snapshot_on.store(on); std::fprintf(stderr, "[DRAW] feedback snapshot %s\n", on ? "on" : "off"); }
bool ge_draw_fb_snapshot() { return g_fb_snapshot_on.load(); }
/// Should `tex` be sampled through its mipmaps, and with what LOD bias? Two cases while the prefilter is on:
///  * the feedback snapshot of a scaled target (a pass shifting the picture it draws into): the source and the
///    destination are the same size, so the bias log2(scale) picks the PSP-pixel-sized level;
///  * a scaled target read while drawing into a smaller one (the first copy of the game's glow chain: the big
///    picture into a 480x272 buffer, one bilinear tap per output pixel): the GPU already picks the level that
///    averages the n x n block each output pixel stands for (bias 0), so the copy is the supersampled version of the
///    PSP picture, not every n-th pixel of it (which aliases and ghosts).
bool ge_draw_prefilter(GLuint tex, float* bias) {
    *bias = 0.f;
    if (!tex || !g_fb_prefilter.load()) return false;
    if (tex == g_fb_scratch_tex) {
        if (g_scratch_rs <= 1) return false;
        *bias = std::log2(static_cast<float>(g_scratch_rs));
        return true;
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i)
        if (g_targets[i].fbo && g_targets[i].color_tex == tex) return g_targets[i].rs > cur_rs();
    return false;
}

// The GE reads a texture from the very buffer it is rendering into (in-place
// blur / shift passes: Dissidia's bloom strips draw 0xCC000 into 0xCC000). The
// hardware samples the pre-draw contents; GL forbids sampling an attachment
// (undefined feedback loop), so snapshot the bound render target into a scratch
// texture first. Pending batched draws are flushed so the snapshot is current,
// and every feedback draw takes its own snapshot (find is called once per PRIM).
// PSPRECOMP_GE_FEEDBACK=0 restores the old behaviour (memory path).
static GLuint feedback_snapshot(int* w, int* h) {
    if (!g_fb_snapshot_on.load()) return 0;
    flush_batch();
    const int rs = cur_rs();
    const int W = cur_w480() * rs, H = cur_h272() * rs;
    ScratchEntry* se = nullptr;
    for (auto& e : g_scratch) if (e.tex && e.w == W && e.h == H) { se = &e; break; }
    if (!se) {
        for (auto& e : g_scratch) if (!e.tex) { se = &e; break; }
        if (!se) {   // full: replace the first
            se = &g_scratch[0];
            glDeleteTextures(1, &se->tex);
            glDeleteFramebuffers(1, &se->fbo);
            *se = ScratchEntry{};
        }
        se->w = W; se->h = H;
        glGenTextures(1, &se->tex);
        glBindTexture(GL_TEXTURE_2D, se->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), W, H, 0, GL_RGBA, scene_type(), nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &se->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, se->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, se->tex, 0);
        ge_texture_invalidate_bind_cache();
    }
    g_fb_scratch_tex = se->tex;
    g_fb_scratch_fbo = se->fbo;
    g_scratch_rs = rs;
    GLint cur = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)cur);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fb_scratch_fbo);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)cur);
    if (rs > 1 && g_fb_prefilter.load()) {   // the mip chain the sampler reads (see g_fb_prefilter)
        glBindTexture(GL_TEXTURE_2D, g_fb_scratch_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        ge_texture_invalidate_bind_cache();
    }
    if (w) *w = W;
    if (h) *h = H;
    return g_fb_scratch_tex;
}

static void delete_scratch() {
    for (auto& e : g_scratch) {
        if (e.tex) { glDeleteTextures(1, &e.tex); glDeleteFramebuffers(1, &e.fbo); }
        e = ScratchEntry{};
    }
    g_fb_scratch_tex = g_fb_scratch_fbo = 0;
}

static int g_fbtex_w480 = 480;   // width (PSP pixels) of the render target ge_draw_find_target_texture last returned
static int g_fbtex_h272 = 272;   // ... and its height

unsigned int ge_draw_find_target_texture(uint32_t key, int* w, int* h) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo == 0 || g_targets[i].key != key) continue;
        if (i == g_current_target) {
            // PSPRECOMP_NO_FBSNAP=1 (diagnostic A/B): skip the whole-target copy and use the memory path.
            static const bool no_snap = [] { const char* v = std::getenv("PSPRECOMP_NO_FBSNAP"); return v && *v && *v != '0'; }();
            if (!no_snap)
                if (const GLuint t = feedback_snapshot(w, h)) {
                    g_fbtex_w480 = cur_w480();
                    g_fbtex_h272 = cur_h272();
                    psp_prof::fb_snaps++;
                    if (w && h) psp_prof::fb_snap_px += uint64_t(*w) * uint64_t(*h);
                    return t;
                }
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr, "[DRAW] texture key=0x%06X is the bound "
                    "render target (feedback); using memory path\n", key);
            }
            return 0;
        }
        g_targets[i].last_use = ++g_target_clock;
        g_fbtex_w480 = g_targets[i].w480;
        g_fbtex_h272 = g_targets[i].h272;
        if (w) *w = g_targets[i].w480 * g_targets[i].rs;
        if (h) *h = g_targets[i].h272 * g_targets[i].rs;
        if (g_targets[i].rs > cur_rs() && g_fb_prefilter.load() && g_targets[i].mip_dirty) {   // mip chain for the down-scaling read
            flush_batch();
            glBindTexture(GL_TEXTURE_2D, g_targets[i].color_tex);
            glGenerateMipmap(GL_TEXTURE_2D);
            ge_texture_invalidate_bind_cache();
            g_targets[i].mip_dirty = false;
        }
        return g_targets[i].color_tex;
    }
    return 0;
}

// ---- Liveness present (no-flip safety net) ----
static bool g_present_dirty = false;
static std::chrono::steady_clock::time_point g_last_present;
static int g_present_stale_ms = 100;   // PSPRECOMP_PRESENT_STALE_MS

// Stencil emulation mode (debug socket `ST <bits>`, env PSPRECOMP_GE_STENCIL=<bits>):
// bit 0 = stencil test + ops on the GL stencil buffer, bit 1 = keep the colour alpha
// channel in step with REPLACE/ZERO ("stencil to alpha"). Default 1: bit 1 breaks the
// Story free-roam scene (terrain vanishes) and is still under investigation. 0 = off.
static std::atomic<int> g_stencil_mode{[] {
    const char* e = std::getenv("PSPRECOMP_GE_STENCIL");
    return e ? std::atoi(e) : 1;
}()};
void ge_draw_set_stencil_mode(int m) { g_stencil_mode.store(m); }
// Debug socket `D <dir>`: write every live render target (colour + alpha plane) at the next present.
static std::mutex g_dump_mu;
static std::string g_dump_dir;
static std::atomic<bool> g_dump_armed{false};
// Debug socket `FT <n>`: trace every draw of the next n presented frames ([FTRACE] lines, see ge_draw_prim_one).
static std::atomic<int> g_ftrace_frames{0};
static bool g_ftrace_drawn = false;   // a draw was traced since the last frame end
void ge_draw_frame_trace(int n) { g_ftrace_frames.store(n); }
bool ge_draw_frame_trace_on() { return g_ftrace_frames.load(std::memory_order_relaxed) > 0; }
void ge_draw_dump_targets(const char* dir) {
    { std::lock_guard<std::mutex> lk(g_dump_mu); g_dump_dir = dir; }
    g_dump_armed.store(true);
}
static std::atomic<int> g_dyn_trace_n{0};
static std::atomic<int> g_xp_nofilter{0};  // PSPRECOMP_XP_FILE mode bit 32: lift the PSPRECOMP_DT_ONLY_VT filter
static std::atomic<int> g_dyn_armed{0};
// Arm a trace of the next n prims, starting at the next display list.
void ge_draw_dyn_trace(int n) { g_dyn_armed.store(n); }
void ge_draw_set_fast(bool on);
bool ge_draw_dyn_trace_active() { return g_dyn_trace_n.load() > 0; }

// ---- Module state ----
static GLuint g_vao = 0;
static GLuint g_vbo = 0;
static uint32_t g_frame_counter = 0;
static bool g_has_drawn_prims = false;
static bool g_screenshot_enabled = false;
static bool g_screenshot_done = false;
static char g_screenshot_path[256] = {};
static std::atomic<bool> g_draw_ready{false};
// PSPRECOMP_NO_RASTER=1: GL_RASTERIZER_DISCARD (draws submitted, nothing
// rasterised); =2: skip glDrawArrays entirely. Measures CPU-side cost only.
static int g_no_raster = 0;
static std::atomic<int> g_no_raster_request{-1};
// Test harnesses (games/dissidia frame-rate parity runs): stop drawing once the fight's HUD has been seen. 0 draw, 1 rasterizer discard,
// 2 no draw calls (the GE lists are still processed). Taken over by the render thread at the next present.
void ge_draw_request_no_raster(int mode) { g_no_raster_request.store(mode); }
static int g_no_raster_mode = 0;       // requested mode (PSPRECOMP_NO_RASTER)
static double g_no_raster_after = 0.0; // PSPRECOMP_NO_RASTER_AFTER=<s>: enable only after s seconds
static std::chrono::steady_clock::time_point g_t_start = std::chrono::steady_clock::now();

// ---- PRIM counters (file scope so ge_draw_get_stats can read them) ----
// Written only by the render thread inside ge_draw_prim; read racily by
// the debug socket's I command (a few prims stale is fine).
static int g_prim_type_count[8] = {0};   // PRIM types 0-6 + slot 7 safety
static int g_prim_type_count_clear[8] = {0};
static int g_prim_type_zero_count = 0;   // count <= 0 (degenerate)
static int g_prim_total_observed = 0;

// ---- On-demand screenshot request (issue #35 debug socket S command) ----
// Requester (any thread) posts a path + waits on the condvar; the render
// thread polls ge_draw_service_screenshot_request() each event-loop pass
// and performs the GL readback there (GL never leaves the main thread).
static std::mutex g_ss_req_mutex;
static std::condition_variable g_ss_req_cv;
static char g_ss_req_path[512] = {};
enum SsReqState { SS_IDLE = 0, SS_PENDING = 1, SS_DONE = 2, SS_FAILED = 3 };
static int g_ss_req_state = SS_IDLE;

// ---- Packed vertex for VBO upload ----
// Layout: 3 floats (pos) + 2 floats (uv) + 4 bytes (color)
// Total: 12 + 8 + 4 = 24 bytes
struct PackedVertex {
    float pos[4];
    float uv[2];
    uint8_t color[4];
};
static_assert(sizeof(PackedVertex) == 28,
              "PackedVertex must be 28 bytes");

// ---- Screenshot (TGA format -- no external dependency) ----

static bool write_tga(
    const char* path,
    const uint8_t* pixels,
    int width,
    int height
) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr,
            "[DRAW] Cannot open %s for screenshot\n",
            path);
        return false;
    }

    // TGA header (18 bytes) -- uncompressed RGBA
    uint8_t header[18] = {};
    header[2]  = 2;  // Uncompressed true-color
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;  // bits per pixel
    header[17] = 0x28; // origin upper-left + 8 alpha bits

    std::fwrite(header, 1, 18, f);

    // Write pixels (BGRA for TGA)
    // Input is RGBA, need to swap R and B
    std::vector<uint8_t> bgra(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        bgra[i * 4 + 0] = pixels[i * 4 + 2];  // B
        bgra[i * 4 + 1] = pixels[i * 4 + 1];  // G
        bgra[i * 4 + 2] = pixels[i * 4 + 0];  // R
        bgra[i * 4 + 3] = pixels[i * 4 + 3];  // A
    }
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);

    std::fprintf(stderr,
        "[DRAW] Screenshot saved: %s (%dx%d)\n",
        path, width, height);
    return true;
}

// ---- Cached GL state setters ----
// Thousands of PRIMs per frame mostly repeat the previous state; skipping the
// redundant GL calls is a large part of the per-draw CPU cost on real drivers.
// Every GL state change in this file goes through these.
namespace glc {
static GLuint vao = ~0u;
static int vp[4] = {-1, -1, -1, -1};
static double dr[2] = {-1.0, -1.0};
static int blend_en = -1;
static GLenum bsrc = 0, bdst = 0, beq = 0;
static int bsep = -1;                       // 1: alpha uses (ONE, ZERO, ADD) - see stencil alpha override
static int st_en = -1, st_mask = -1;
static GLenum st_func = 0, st_sfail = 0, st_zfail = 0, st_zpass = 0;
static int st_ref = -1, st_fmask = -1;
static float bcol[4] = {-1, -1, -1, -1};
static int cmask = -1;
static int depth_en = -1, dmask = -1;
static GLenum dfunc = 0;
static int cull_en = -1;
static GLenum cull_mode = 0;
static bool tex0 = false;
static void invalidate() {
    vao = ~0u; vp[0] = vp[1] = vp[2] = vp[3] = -1; dr[0] = dr[1] = -1.0;
    bsep = -1; st_en = -1; st_mask = -1; st_func = st_sfail = st_zfail = st_zpass = 0; st_ref = st_fmask = -1;
    blend_en = -1; bsrc = bdst = beq = 0; bcol[0] = bcol[1] = bcol[2] = bcol[3] = -1;
    cmask = -1; depth_en = -1; dmask = -1; dfunc = 0; cull_en = -1; cull_mode = 0; tex0 = false;
}
}  // namespace glc

static inline void gl_bind_vao(GLuint v) {
    if (glc::vao != v) { glBindVertexArray(v); glc::vao = v; }
}
static inline void gl_viewport(int x, int y, int w, int h) {
    if (glc::vp[0] != x || glc::vp[1] != y || glc::vp[2] != w || glc::vp[3] != h) {
        glViewport(x, y, w, h);
        glc::vp[0] = x; glc::vp[1] = y; glc::vp[2] = w; glc::vp[3] = h;
    }
}
static inline void gl_depth_range(double n, double f) {
    if (glc::dr[0] != n || glc::dr[1] != f) {
        glDepthRange(n, f);
        glc::dr[0] = n; glc::dr[1] = f;
    }
}
static inline void gl_blend_enable(bool e) {
    if (glc::blend_en != (int)e) {
        if (e) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glc::blend_en = (int)e;
    }
}
// `alpha_set`: the destination alpha (== GE stencil) must be *replaced* by the
// fragment alpha, not blended (stencil REPLACE/ZERO emulation): alpha factors
// (ONE, ZERO) and equation ADD, independent of the colour blend.
static inline void gl_blend_func(GLenum s, GLenum d, bool alpha_set = false) {
    if (glc::bsrc != s || glc::bdst != d || glc::bsep != (int)alpha_set) {
        if (alpha_set) glBlendFuncSeparate(s, d, GL_ONE, GL_ZERO); else glBlendFunc(s, d);
        glc::bsrc = s; glc::bdst = d; glc::bsep = (int)alpha_set;
    }
}
static inline void gl_blend_equation(GLenum e, bool alpha_set = false) {
    const GLenum key = alpha_set ? (e | 0x10000u) : e;
    if (glc::beq != key) {
        if (alpha_set) glBlendEquationSeparate(e, GL_FUNC_ADD); else glBlendEquation(e);
        glc::beq = key;
    }
}
static inline void gl_stencil_enable(bool e) {
    if (glc::st_en != (int)e) { if (e) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST); glc::st_en = (int)e; }
}
static inline void gl_stencil_mask(unsigned m) {
    if (glc::st_mask != (int)m) { glStencilMask(m); glc::st_mask = (int)m; }
}
static inline void gl_stencil_func(GLenum f, int ref, int mask) {
    if (glc::st_func != f || glc::st_ref != ref || glc::st_fmask != mask) {
        glStencilFunc(f, ref, mask); glc::st_func = f; glc::st_ref = ref; glc::st_fmask = mask;
    }
}
static inline void gl_stencil_op(GLenum sf, GLenum zf, GLenum zp) {
    if (glc::st_sfail != sf || glc::st_zfail != zf || glc::st_zpass != zp) {
        glStencilOp(sf, zf, zp); glc::st_sfail = sf; glc::st_zfail = zf; glc::st_zpass = zp;
    }
}
static inline void gl_blend_color(float r, float g, float b, float a) {
    if (glc::bcol[0] != r || glc::bcol[1] != g || glc::bcol[2] != b || glc::bcol[3] != a) {
        glBlendColor(r, g, b, a);
        glc::bcol[0] = r; glc::bcol[1] = g; glc::bcol[2] = b; glc::bcol[3] = a;
    }
}
static inline void gl_color_mask(bool r, bool g, bool b, bool a) {
    const int m = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
    if (glc::cmask != m) { glColorMask(r, g, b, a); glc::cmask = m; }
}
static inline void gl_depth_enable(bool e) {
    if (glc::depth_en != (int)e) {
        if (e) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glc::depth_en = (int)e;
    }
}
static inline void gl_depth_func(GLenum f) {
    if (glc::dfunc != f) { glDepthFunc(f); glc::dfunc = f; }
}
static inline void gl_depth_mask(bool m) {
    if (glc::dmask != (int)m) { glDepthMask(m ? GL_TRUE : GL_FALSE); glc::dmask = (int)m; }
}
static inline void gl_cull_enable(bool e) {
    if (glc::cull_en != (int)e) {
        if (e) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        glc::cull_en = (int)e;
    }
}
static inline void gl_cull_face(GLenum m) {
    if (glc::cull_mode != m) { glCullFace(m); glc::cull_mode = m; }
}
static inline void gl_active_tex0() {
    if (!glc::tex0) { glActiveTexture(GL_TEXTURE0); glc::tex0 = true; }
}

// Resolve the FBO id of the most-recently-presented (front) target, or the
// current render target as a fallback. Returns 0 if no target exists yet.
static GLuint front_fbo_id() {
    if (g_front_target >= 0) return g_targets[g_front_target].fbo;
    if (g_current_target >= 0) return g_targets[g_current_target].fbo;
    return 0;
}

/// Read the front FBO back as a native 480x272 top-down RGBA image. At render
/// scale > 1 the n x n block of each native pixel is box-averaged (round to
/// nearest), so `S` captures stay comparable across scales; PSPRECOMP_SHOT_FULL=1
/// returns the full (480n x 272n) image instead (w/h report the size).
/// Render (GL) thread only.
static int rs_of_fbo(GLuint fbo) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) if (g_targets[i].fbo == fbo || (g_targets[i].ab_fbo && g_targets[i].ab_fbo == fbo)) return g_targets[i].rs;
    return g_rs;
}
static int w480_of_fbo(GLuint fbo) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) if (g_targets[i].fbo == fbo || (g_targets[i].ab_fbo && g_targets[i].ab_fbo == fbo)) return g_targets[i].w480;
    return 480;
}
static int h272_of_fbo(GLuint fbo) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) if (g_targets[i].fbo == fbo || (g_targets[i].ab_fbo && g_targets[i].ab_fbo == fbo)) return g_targets[i].h272;
    return 272;
}
static void read_fbo_native(GLuint fbo, std::vector<uint8_t>& out, int* ow = nullptr, int* oh = nullptr) {
    const int n = rs_of_fbo(fbo);
    const int fw = w480_of_fbo(fbo) * n, fh = 272 * n;   // a tall buffer: its top 480x272 band (the PSP's picture)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<uint8_t> pixels(size_t(fw) * fh * 4);
    glReadPixels(0, band_b(h272_of_fbo(fbo)) * n, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    static const bool full = std::getenv("PSPRECOMP_SHOT_FULL") != nullptr;
    if (n == 1 || full) {
        // Flip vertically (GL origin is bottom-left)
        out.resize(pixels.size());
        for (int y = 0; y < fh; y++)
            std::memcpy(out.data() + size_t(y) * fw * 4,
                        pixels.data() + size_t(fh - 1 - y) * fw * 4, size_t(fw) * 4);
        if (ow) *ow = fw;
        if (oh) *oh = fh;
        return;
    }
    out.resize(size_t(fw / n) * 272 * 4);
    const int area = n * n;
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < fw / n; x++) {
            int acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < n; j++) {
                // output row y is the top-down row; GL row index counts from the bottom
                const uint8_t* row = pixels.data() + (size_t(fh - 1 - (y * n + j)) * fw + size_t(x) * n) * 4;
                for (int i = 0; i < n; i++)
                    for (int c = 0; c < 4; c++) acc[c] += row[i * 4 + c];
            }
            for (int c = 0; c < 4; c++)
                out[(size_t(y) * (fw / n) + x) * 4 + c] = uint8_t((acc[c] + area / 2) / area);
        }
    }
    if (ow) *ow = fw / n;
    if (oh) *oh = 272;
}

// Composite the active savedata-dialog overlay (rendered at an integer
// multiple of 480x272) into a top-down RGBA readback of w x h pixels, box
// filtering when the sizes differ. No-op when no dialog is active.
struct GameCanvas { std::mutex m; std::vector<uint32_t> px; int w = 0, h = 0; bool dirty = false; };
static GameCanvas g_game_canvas;   // ge_draw_set_game_overlay: a game module's own 2D overlay (RGBA, row 0 = top)

// The game module's overlay canvas alpha-blended into a captured frame (screenshots), centred at the canvas's own aspect.
static void composite_game_overlay(std::vector<uint8_t>& rgba, int w, int h) {
    std::lock_guard<std::mutex> lk(g_game_canvas.m);
    const int cw = g_game_canvas.w, ch = g_game_canvas.h;
    if (cw <= 0 || ch <= 0 || g_game_canvas.px.size() < size_t(cw) * size_t(ch) || rgba.size() < size_t(w) * h * 4) return;
    const int dw = std::min(w, int(int64_t(h) * cw / ch)), x0 = (w - dw) / 2;
    for (int y = 0; y < h; y++) {
        const int sy = std::min(ch - 1, int(int64_t(y) * ch / h));
        for (int x = 0; x < dw; x++) {
            const int sx = std::min(cw - 1, int(int64_t(x) * cw / dw));
            const uint32_t p = g_game_canvas.px[size_t(sy) * cw + sx];
            const unsigned a = p >> 24;
            if (!a) continue;
            uint8_t* d = rgba.data() + (size_t(y) * w + x0 + x) * 4;
            d[0] = uint8_t((d[0] * (255 - a) + (p & 255) * a) / 255);
            d[1] = uint8_t((d[1] * (255 - a) + ((p >> 8) & 255) * a) / 255);
            d[2] = uint8_t((d[2] * (255 - a) + ((p >> 16) & 255) * a) / 255);
        }
    }
}

static void composite_dialog_overlay(std::vector<uint8_t>& rgba, int w, int h) {
    static std::vector<uint32_t> ovl;
    int ow = 0, oh = 0;
    if (!psp_sddialog::overlay_fetch(ovl, &ow, &oh) || ow <= 0 || oh <= 0 ||
        ovl.size() != size_t(ow) * size_t(oh) || rgba.size() < size_t(w) * h * 4)
        return;
    if (ow == w && oh == h) {
        std::memcpy(rgba.data(), ovl.data(), size_t(w) * h * 4);
        return;
    }
    for (int y = 0; y < h; y++) {
        int sy0 = int(int64_t(y) * oh / h), sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * oh / h));
        for (int x = 0; x < w; x++) {
            int sx0 = int(int64_t(x) * ow / w), sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * ow / w));
            unsigned acc[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1 && sy < oh; sy++)
                for (int sx = sx0; sx < sx1 && sx < ow; sx++) {
                    uint32_t p = ovl[size_t(sy) * ow + sx];
                    acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255; n++;
                }
            uint8_t* d = rgba.data() + (size_t(y) * w + x) * 4;
            if (n) { d[0] = uint8_t(acc[0] / n); d[1] = uint8_t(acc[1] / n); d[2] = uint8_t(acc[2] / n); d[3] = 255; }
        }
    }
}

static bool capture_fbo_to_tga(const char* path) {
    ge_draw_flush();
    GLuint fbo = front_fbo_id();
    std::vector<uint8_t> flipped;
    int w = 480, h = 272;
    read_fbo_native(fbo, flipped, &w, &h);
    // An active savedata dialog is composited over the frame at present
    // time; include it so the capture matches the window.
    composite_dialog_overlay(flipped, w, h);
    composite_game_overlay(flipped, w, h);
    return write_tga(path, flipped.data(), w, h);
}

// ---- GL state mapping ----

static GLenum map_blend_factor_src(int factor) {
    switch (factor) {
    case GE_SRCBLEND_DSTCOLOR:       return GL_DST_COLOR;
    case GE_SRCBLEND_INVDSTCOLOR:
        return GL_ONE_MINUS_DST_COLOR;
    case GE_SRCBLEND_SRCALPHA:       return GL_SRC_ALPHA;
    case GE_SRCBLEND_INVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_SRCBLEND_DSTALPHA:       return GL_DST_ALPHA;
    case GE_SRCBLEND_INVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    // 2*As / 1-2*As: GL has no such factor. The draw path doubles the
    // fragment alpha in the shader (u_alpha_double) and uses the plain
    // (1-)SRC_ALPHA factor; if that is not possible it degrades to the
    // single factor. Doubled DST alpha is always approximated by the single.
    case GE_SRCBLEND_DOUBLESRCALPHA:
        return GL_SRC_ALPHA;
    case GE_SRCBLEND_DOUBLEINVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_SRCBLEND_DOUBLEDSTALPHA:
        return GL_DST_ALPHA;
    case GE_SRCBLEND_DOUBLEINVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_SRCBLEND_FIXA:           return GL_CONSTANT_COLOR;
    default:                         return GL_SRC_ALPHA;
    }
}

static GLenum map_blend_factor_dst(int factor) {
    switch (factor) {
    case GE_DSTBLEND_SRCCOLOR:       return GL_SRC_COLOR;
    case GE_DSTBLEND_INVSRCCOLOR:
        return GL_ONE_MINUS_SRC_COLOR;
    case GE_DSTBLEND_SRCALPHA:       return GL_SRC_ALPHA;
    case GE_DSTBLEND_INVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_DSTBLEND_DSTALPHA:       return GL_DST_ALPHA;
    case GE_DSTBLEND_INVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_DSTBLEND_DOUBLESRCALPHA:
        return GL_SRC_ALPHA;
    case GE_DSTBLEND_DOUBLEINVSRCALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case GE_DSTBLEND_DOUBLEDSTALPHA:
        return GL_DST_ALPHA;
    case GE_DSTBLEND_DOUBLEINVDSTALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case GE_DSTBLEND_FIXB:           return GL_CONSTANT_COLOR;
    default:
        return GL_ONE_MINUS_SRC_ALPHA;
    }
}

// GE stencil test function (GE_COMP: 0 NEVER, 1 ALWAYS, 2 EQUAL, 3 NOTEQUAL,
// 4 LESS, 5 LEQUAL, 6 GREATER, 7 GEQUAL) and stencil op (0 KEEP, 1 ZERO,
// 2 REPLACE, 3 INVERT, 4 INCR, 5 DECR) -> GL. Same mapping PPSSPP's GL backend uses.
static GLenum map_stencil_func(int f) {
    static const GLenum m[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return m[f & 7];
}
static GLenum map_stencil_op(int o) {
    static const GLenum m[6] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR};
    return o >= 0 && o < 6 ? m[o] : GL_KEEP;
}

static GLenum map_blend_equation(int op) {
    switch (op) {
    case GE_BLENDMODE_MUL_AND_ADD:              return GL_FUNC_ADD;
    case GE_BLENDMODE_MUL_AND_SUBTRACT:
        return GL_FUNC_SUBTRACT;
    case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
        return GL_FUNC_REVERSE_SUBTRACT;
    case GE_BLENDMODE_MIN:                      return GL_MIN;
    case GE_BLENDMODE_MAX:                      return GL_MAX;
    case GE_BLENDMODE_ABSDIFF: {
        // |src*sf - dst*df| has no GL equivalent (needs MAX of both
        // subtractions); approximate with src-dst, clamped at 0.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE_BLEND] ABSDIFF blend equation approximated as "
                "GL_FUNC_SUBTRACT\n");
        }
        return GL_FUNC_SUBTRACT;
    }
    default:                                    return GL_FUNC_ADD;
    }
}

// Map the PSP depth-compare to a GL depth func.
//
// NO direction flip, even for a reversed depth range (PSP ZSCALE<0 ->
// glDepthRange near>far). glDepthRange(n, f) maps NDC z to the window depth
// n + (f - n) * (z + 1) / 2, which for n = (zc - zs)/65535 and
// f = (zc + zs)/65535 is exactly the PSP's zscreen = zc + zs * z (/65535).
// The GL depth buffer therefore holds the SAME values the PSP's does, and the
// PSP compare (e.g. GEQUAL with a negative ZSCALE, which Dissidia uses) must
// be applied unchanged. An earlier version flipped the inequality here
// ("GL stores depth on the flipped axis") -- that is wrong, and turned
// Dissidia's GEQUAL into LEQUAL against a buffer cleared to the far value,
// so no 3D fragment ever passed the depth test (every battle draw produced
// 0 samples).
static GLenum map_depth_func(int func, bool /*reversed*/) {
    switch (func) {
    case GE_COMP_NEVER:    return GL_NEVER;
    case GE_COMP_ALWAYS:   return GL_ALWAYS;
    case GE_COMP_EQUAL:    return GL_EQUAL;
    case GE_COMP_NOTEQUAL: return GL_NOTEQUAL;
    case GE_COMP_LESS:     return GL_LESS;
    case GE_COMP_LEQUAL:   return GL_LEQUAL;
    case GE_COMP_GREATER:  return GL_GREATER;
    case GE_COMP_GEQUAL:   return GL_GEQUAL;
    default:               return GL_LEQUAL;
    }
}

static GLenum map_prim_type(int prim_type) {
    switch (prim_type) {
    case GE_PRIM_POINTS:         return GL_POINTS;
    case GE_PRIM_LINES:          return GL_LINES;
    case GE_PRIM_LINE_STRIP:     return GL_LINE_STRIP;
    case GE_PRIM_TRIANGLES:      return GL_TRIANGLES;
    case GE_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case GE_PRIM_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    case GE_PRIM_RECTANGLES:     return GL_TRIANGLES;
    default:                     return GL_TRIANGLES;
    }
}

// PSP-native render-target dimensions. Targets are allocated at
// (480*g_rs) x (272*g_rs), g_rs = PSPRECOMP_RENDER_SCALE (1..16, default 1;
// PPSSPP-style "render resolution"). All geometry is expressed in PSP pixels
// (3D via the PSP viewport registers, 2D "through" vertices via the
// 240/136 NDC mapping in ge_transform_vertices), so only the GL viewport has
// to be multiplied by g_rs: through-mode NDC already spans the whole target,
// and FBO-as-texture UVs are normalised so they are scale independent.
// Window upscaling happens later at present (blit).
static constexpr int PSP_FB_WIDTH = 480;
static constexpr int PSP_FB_HEIGHT = 272;
int ge_draw_render_scale() { return g_rs; }

// Compute the PSP viewport scale/offset + depth-range -> GL viewport/depth
// range for the live state. Generic for all games (issue #23). The math lives
// in the pure ge_compute_viewport_depth (tests/test_ge_viewport.cpp) so it is
// unit-testable without a GL context; callers issue the GL calls themselves.
static GeViewportDepth compute_state_viewport_depth(const GeState& state) {
    return ge_compute_viewport_depth(
        state.viewport_x_scale, state.viewport_y_scale,
        state.viewport_x_center, state.viewport_y_center,
        state.viewport_z_scale, state.viewport_z_center,
        state.offset_x, state.offset_y, PSP_FB_HEIGHT);
}

// A reversed depth range (PSP ZSCALE<0 -> glDepthRange near>far, issue #23)
// means the depth axis is flipped: the GL "far" value is the smaller of the
// two and is what a depth clear must fill so geometry passes the test, and
// the depth-compare direction must flip to match.
static bool depth_range_reversed(const GeViewportDepth& vp) {
    return vp.near_z > vp.far_z;
}

// ---- PSPRECOMP_GE_AB=1: same-frame fast-vs-legacy comparison ----
// Every render target gets a twin. Each PRIM / clear is executed twice with
// identical GE state and guest memory: the fast path into the real target,
// the legacy path into the twin. At each present the front target (and all
// other live targets) are read back and compared; mismatches are logged and
// the worst frames dumped as TGA to PSPRECOMP_AB_DIR (default /tmp). This is
// the only way to compare the two paths on the SAME frame (menu / battle
// animation makes separate captures differ).
static bool g_ab = false;
static bool g_ab_shadow = false;     // inside the legacy re-execution of a PRIM
static int g_ab_dumped = 0;


// Create one render target (FBO + RGBA8 color texture + Depth24 renderbuffer,
// 480x272, NEAREST) for the given normalized key. Clears it to black. The
// pool owns the GL objects; ge_draw_shutdown deletes them.
static GeTarget make_target(uint32_t key, int rs, int w480, int h272 = 272) {
    if (!g_scene_inited) { g_scene_inited = true; g_scene_fp16 = psp_hdr_scene_live(); std::fprintf(stderr, "[DRAW] scene targets: %s\n", g_scene_fp16 ? "RGBA16F (HDR Scene)" : "RGBA8"); }
    GeTarget t;
    t.key = key;
    t.rs = rs;
    t.w480 = w480;
    t.h272 = h272;
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

    glGenTextures(1, &t.color_tex);
    glBindTexture(GL_TEXTURE_2D, t.color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), w480 * rs, h272 * rs, 0,
                 GL_RGBA, scene_type(), nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, t.color_tex, 0);

    glGenRenderbuffers(1, &t.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth_rb);
    // Depth + 8-bit stencil: the GE stencil buffer is the framebuffer's alpha
    // channel (STENCILTEST / STENCILOP), emulated with a real GL stencil
    // buffer next to the colour alpha (see apply_gl_pipeline_impl).
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w480 * rs, h272 * rs);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, t.depth_rb);
    if (g_ab) {
        glGenFramebuffers(1, &t.ab_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.ab_fbo);
        glGenTextures(1, &t.ab_tex);
        glBindTexture(GL_TEXTURE_2D, t.ab_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w480 * rs, h272 * rs, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.ab_tex, 0);
        glGenRenderbuffers(1, &t.ab_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.ab_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w480 * rs, h272 * rs);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.ab_depth);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[DRAW] FBO incomplete: 0x%04X (key=0x%06X)\n",
                     status, key);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // a draw may have narrowed it
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glc::invalidate();                 // raw GL state calls above
    ge_texture_invalidate_bind_cache();  // glBindTexture above
    t.last_use = ++g_target_clock;
    return t;
}

// A buffer that is shown on screen (or is the first one the game draws into) is a main buffer: it is rendered at the
// render scale from now on. One that was created at 1x before the game first displayed it is grown, keeping its
// picture (scaled up), so no black frame shows.
static void mark_main_key(uint32_t key) {
    if (g_scale_all) return;
    bool known = false;
    for (int i = 0; i < g_main_n; i++) known |= g_main_keys[i] == key;
    if (!known) {
        if (g_main_n >= 8) return;
        g_main_keys[g_main_n++] = key;
        std::fprintf(stderr, "[DRAW] main buffer key=0x%06X (rendered at %dx)\n", key, g_rs);
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (!t.fbo || t.key != key || (t.rs == g_rs && t.w480 == g_wide_px && t.h272 == g_tall_px)) continue;
        flush_batch();
        GeTarget n = make_target(key, g_rs, g_wide_px, g_tall_px);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, n.fbo);
        const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        if (sc) glDisable(GL_SCISSOR_TEST);
        // the PSP's 480x272 band (the top rows of a tall buffer) of the old buffer into the band of the new one
        glBlitFramebuffer(0, band_b(t.h272) * t.rs, t.w480 * t.rs, (band_b(t.h272) + 272) * t.rs, 0, band_b(n.h272) * n.rs, n.w480 * n.rs, (band_b(n.h272) + 272) * n.rs,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (sc) glEnable(GL_SCISSOR_TEST);
        glDeleteFramebuffers(1, &t.fbo);
        glDeleteTextures(1, &t.color_tex);
        glDeleteRenderbuffers(1, &t.depth_rb);
        n.last_use = t.last_use;
        t = n;
        glBindFramebuffer(GL_FRAMEBUFFER, i == g_current_target ? t.fbo : (g_current_target >= 0 ? g_targets[g_current_target].fbo : 0));
        glc::invalidate();
        ge_texture_invalidate_bind_cache();
        std::fprintf(stderr, "[DRAW] target key=0x%06X grown to %dx\n", key, g_rs);
    }
}

// Find the pool slot for key, or allocate one (LRU-evicting if full).
static int acquire_target(uint32_t key) {
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) {
            g_targets[i].last_use = ++g_target_clock;
            return i;
        }
    }
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo == 0) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;  // LRU evict
        for (int i = 1; i < GE_MAX_TARGETS; ++i) {
            if (g_targets[i].last_use < g_targets[slot].last_use) slot = i;
        }
        std::fprintf(stderr,
            "[DRAW] FBO pool overflow: evicting key=0x%06X for key=0x%06X\n",
            g_targets[slot].key, key);
        glDeleteFramebuffers(1, &g_targets[slot].fbo);
        glDeleteTextures(1, &g_targets[slot].color_tex);
        glDeleteRenderbuffers(1, &g_targets[slot].depth_rb);
        if (g_targets[slot].ab_fbo) {
            glDeleteFramebuffers(1, &g_targets[slot].ab_fbo);
            glDeleteTextures(1, &g_targets[slot].ab_tex);
            glDeleteRenderbuffers(1, &g_targets[slot].ab_depth);
        }
    }
    const bool mainbuf = is_main_key(key);
    g_targets[slot] = make_target(key, mainbuf ? g_rs : 1, mainbuf ? g_wide_px : 480, mainbuf ? g_tall_px : 272);
    std::fprintf(stderr, "[DRAW] FBO target created key=0x%06X slot=%d (%dx, %d px wide)\n",
                 key, slot, g_targets[slot].rs, g_targets[slot].w480);
    return slot;
}

// Bind the FBO for the current FRAMEBUFPTR. A raw 0 (FRAMEBUFPTR not yet
// seen) keeps the current/last target so early lists still land somewhere.
static bool g_framebufptr_seen = false;   // any GE_CMD_FRAMEBUFPTR decoded yet?

void ge_draw_note_framebufptr() { g_framebufptr_seen = true; }

void ge_draw_flush();
void ge_draw_select_target(uint32_t fb_addr_raw) {
    ge_draw_flush();   // pending batch belongs to the previously bound target
    uint32_t key = ge_fb_key(fb_addr_raw);
    // Only before the first FRAMEBUFPTR command does key 0 mean "unset":
    // the command's data for VRAM base 0x04000000 is literally 0, and that
    // buffer is a real, common render target.
    if (key == 0 && !g_framebufptr_seen) {
        if (g_current_target < 0) g_current_target = acquire_target(0);
        g_targets[g_current_target].mip_dirty = true;
        glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo);
        return;
    }
    if (g_front_target < 0) mark_main_key(key);   // the first buffer the game draws into is shown
    int slot = acquire_target(key);
    static const bool seltgt_dbg = std::getenv("PSPRECOMP_MOVIE_TEXDBG") != nullptr;
    if (seltgt_dbg)
        std::fprintf(stderr, "[SELTGT] fb=%08X key=%06X slot=%d\n", fb_addr_raw, key, slot);
    g_current_target = slot;
    g_targets[slot].mip_dirty = true;   // about to be drawn into
    if (g_front_target < 0) g_front_target = slot;
    glBindFramebuffer(GL_FRAMEBUFFER, g_targets[slot].fbo);
}

void ge_draw_mark_dirty() {
    g_present_dirty = true;
}

// Apply viewport/depth-range, program + uniforms, texture and the blend /
// mask / depth / cull state for one draw (or batch). `gpu_xform` selects the
// vertex-shader transform path (the caller sets the xform uniforms itself).
// Returns whether the depth range is reversed.
static bool apply_gl_pipeline_impl(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform);
static bool g_pipe_alpha_double = false;   // last apply_gl_pipeline: GE 2*As emulated by doubling alpha
static bool apply_gl_pipeline(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform) {
    static const char* xv = std::getenv("PSPRECOMP_XP_VT");
    static unsigned xvt = xv ? (unsigned)std::strtoul(xv, nullptr, 16) : 0;
    static unsigned xm = std::getenv("PSPRECOMP_XP_MODE") ? (unsigned)std::atoi(std::getenv("PSPRECOMP_XP_MODE")) : 0;
    // PSPRECOMP_XP_FILE=<path>: file "<hexvt> <mode>" re-read every 32 draws so the
    // experiment can be changed without restarting (mode bits: 1 depth off, 2 alpha
    // test off, 4 blend off, 8 texture off, 16 cull off).
    static const char* xf = std::getenv("PSPRECOMP_XP_FILE");
    static bool xv_live = false;
    static bool xp_tex = false;
    if (xf) {
        static int xcnt = 0;
        if ((xcnt++ & 31) == 0) {
            if (FILE* f = std::fopen(xf, "r")) {
                char b1[32] = {}; int m = 0;
                if (std::fscanf(f, "%31s %d", b1, &m) == 2) {
                    // "tADDR": match on the 24-bit texture-0 address instead of the vertex type
                    xp_tex = (b1[0] == 't');
                    xvt = (unsigned)std::strtoul(b1 + (xp_tex ? 1 : 0), nullptr, 16); xm = (unsigned)m; xv_live = true; g_xp_nofilter = (m & 32) ? 1 : 0;
                }
                std::fclose(f);
            }
        }
    }
    if ((xv || xv_live) && (xp_tex ? ((state.texture_enable ? (state.tex_addr[0] & 0xFFFFFF) : 0xFFFFFFFFu) == xvt)
                                   : ((state.vertex_type & 0xFFFFFF) == xvt))) {
        GeState st2 = state;
        { static int xlog = 0; if (xlog++ < 3) std::fprintf(stderr, "[XP] applying vt=%06X mode=%u\n", xvt, xm); }
        if (xm & 16) st2.cull_enable = false;
        if (xm & 64) { st2.mask_rgb = 0xFFFFFF; st2.mask_alpha = 0xFF; }  // hide this vtype (write nothing)
        if (xm & 1) st2.depth_test_enable = false;
        if (xm & 2) st2.alpha_test_enable = false;
        if (xm & 4) st2.alpha_blend_enable = false;
        if (xm & 8) st2.texture_enable = false;
        return apply_gl_pipeline_impl(rdram, st2, prim_type, fbo_tex, gpu_xform);
    }
    return apply_gl_pipeline_impl(rdram, state, prim_type, fbo_tex, gpu_xform);
}
static bool apply_gl_pipeline_impl(uint8_t* rdram, const GeState& state, int prim_type,
                              GLuint fbo_tex, bool gpu_xform) {
    // Viewport / depth-range (issue #23). Transform-mode draws emit NDC via
    // the real MVP path, so they must map through the PSP viewport
    // scale/offset + depth range to land on screen. Through-mode (2D/sprite)
    // already bakes the full-screen 240/136 mapping into NDC in
    // ge_transform_vertices, so it keeps the full-buffer viewport set by
    // ge_draw_begin_list and an identity depth range.
    bool depth_reversed = false;
    if (!ge_vtype_through(state.vertex_type)) {
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        const int crs = cur_rs(), cw = cur_w480(), ch = cur_h272();
        // tall buffer: the PSP's rows are the band (GL y up: shifted up by band_b); a whole-screen 3D draw covers the whole height
        // (its projection is extended by draw_tall_y)
        const bool tall_full = ch > 272 && vp.y <= 0 && vp.y + vp.h >= 272;
        const int vy = tall_full ? 0 : vp.y + band_b(ch), vh = tall_full ? ch : vp.h;
        if (cw > 480 && vp.x <= 0 && vp.x + vp.w >= 480) gl_viewport(0, vy * crs, cw * crs, vh * crs);   // widescreen: whole width (x scaled by draw_wide_x)
        else gl_viewport((vp.x + (cw - 480) / 2) * crs, vy * crs, vp.w * crs, vh * crs);                  // a partial viewport: shifted to the middle
        
        gl_depth_range(vp.near_z, vp.far_z);
        depth_reversed = depth_range_reversed(vp);
    } else {
        // 2D: the whole buffer (on a tall buffer the vertex transform puts the draw in the band, see ge_set_tall)
        gl_viewport(0, 0, cur_w480() * cur_rs(), cur_h272() * cur_rs());
        gl_depth_range(0.0, 1.0);
    }


    {
        psp_prof::Scope ps(psp_prof::S_UNIFORM);
        ge_shader_use();
        ge_shader_set_uniforms(state);
        // gpu_xform: the caller sets the 3D transform uniforms afterwards.
        if (!gpu_xform) ge_shader_set_xform_passthrough();
    }
    {
        psp_prof::Scope ps(psp_prof::S_TEX);
    // Bind texture if enabled
    if (state.texture_enable) {
        gl_active_tex0();
        if (fbo_tex) {
            int dw = 1 << (state.tex_size[0] & 0xFF);
            int dh = 1 << ((state.tex_size[0] >> 8) & 0xFF);
            ge_texture_bind_external(fbo_tex, state);
            // a tall target: v 0..272 is its band (flipped: GL y up; the band's top row is band_b + 272 from the bottom)
            ge_shader_set_tex_remap(dw / 480.0f, dh / float(g_fbtex_h272), true,
                                    g_fbtex_h272 > 272 ? float(g_fbtex_h272 - 272 - band_b(g_fbtex_h272)) / float(g_fbtex_h272) : 0.0f);
        } else {
            ge_texture_bind(rdram, g_frame_counter);
            ge_shader_set_tex_remap(1.0f, 1.0f, false);
        }
    } else {
        ge_shader_set_tex_remap(1.0f, 1.0f, false);
    }
    }
    psp_prof::Scope ps_gs(psp_prof::S_GLSTATE);
    // Stencil (GE STENCILTEST 0xDC / STENCILOP 0xDD; PPSSPP GPU_GLES StateMapping).
    // The GE keeps the stencil in the framebuffer's alpha channel. The test and the
    // ops run on a real GL stencil buffer; the colour alpha channel is kept in step
    // the way PPSSPP's "stencil to alpha" does it: a zpass REPLACE writes the
    // reference value into alpha, ZERO writes 0, and ops that leave alpha alone
    // (KEEP / INVERT / INCR / DECR) mask the alpha write.
    //   STENCILTEST: bits 0-7 func, 8-15 ref, 16-23 mask
    //   STENCILOP:   bits 0-7 sfail, 8-15 zfail, 16-23 zpass
    float stencil_alpha = -1.0f;      // >= 0: fragment alpha is replaced by this
    bool stencil_alpha_keep = false;  // stencil enabled, op leaves dst alpha unchanged
    const int smode = g_stencil_mode.load(std::memory_order_relaxed);
    const bool stencil_on = state.stencil_test_enable && (smode & 1);
    if (stencil_on && (smode & 2)) {
        const int zpass = (state.stencil_op >> 16) & 0xFF;
        if (zpass == 2) stencil_alpha = (float)((state.stencil_test >> 8) & 0xFF) / 255.0f;
        else if (zpass == 1) stencil_alpha = 0.0f;
        else stencil_alpha_keep = true;
    }
    // Set GL state: alpha blend
    bool alpha_double = false;
    if (state.alpha_blend_enable) {
        gl_blend_enable(true);
        int src_factor = state.blend_mode & 0xF;
        int dst_factor = (state.blend_mode >> 4) & 0xF;
        int blend_op   = (state.blend_mode >> 8) & 0x7;

        // GE FIXA / FIXB are per-channel constant factors (0xBBGGRR) that
        // replace the source / destination factor. GL has ONE blend constant,
        // so map the common cases to ZERO / ONE / (1-)CONSTANT_COLOR and only
        // upload a constant when one is actually used. The constant is global
        // GL state: it MUST be (re)set by every draw that uses it, otherwise
        // the dst=FIXB / src=FIXA draw blends against whatever colour an
        // earlier unrelated draw left behind (0,0,0 by default).
        GLenum gl_src = map_blend_factor_src(src_factor);
        GLenum gl_dst = map_blend_factor_dst(dst_factor);

        // Doubled source-alpha factors (6: 2*As, 7: 1-2*As, on either side).
        // clamp(2*As) with SRC_ALPHA / ONE_MINUS_SRC_ALPHA reproduces both,
        // so double + clamp the fragment alpha in the shader. This is only
        // valid when every source-alpha factor in this blend is doubled
        // (the other side may use colour/dst/fixed factors); a mix of plain
        // and doubled src-alpha factors cannot share one alpha value, so
        // fall back to the single factors in that case. MIN/MAX ignore
        // factors, so no doubling there.
        auto uses_sa = [](int f) { return f == 2 || f == 3 || f == 6 || f == 7; };
        auto is_dbl  = [](int f) { return f == 6 || f == 7; };
        if (blend_op != GE_BLENDMODE_MIN && blend_op != GE_BLENDMODE_MAX
            && (is_dbl(src_factor) || is_dbl(dst_factor))) {
            const bool src_ok = !uses_sa(src_factor) || is_dbl(src_factor);
            const bool dst_ok = !uses_sa(dst_factor) || is_dbl(dst_factor);
            if (src_ok && dst_ok) {
                alpha_double = true;
            } else {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::fprintf(stderr,
                        "[GE_BLEND] mixed plain/doubled src-alpha factors "
                        "(src=%d dst=%d): doubling dropped\n",
                        src_factor, dst_factor);
                }
            }
        }
        if (dst_factor == 8 || dst_factor == 9 || src_factor == 8 || src_factor == 9) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[GE_BLEND] doubled dst-alpha factor approximated by the "
                    "single factor (src=%d dst=%d)\n", src_factor, dst_factor);
            }
        }
        const bool src_fix = (src_factor == GE_SRCBLEND_FIXA);
        const bool dst_fix = (dst_factor == GE_DSTBLEND_FIXB);
        if (src_fix || dst_fix) {
            const uint32_t fixa = state.blend_fix_a & 0xFFFFFFu;
            const uint32_t fixb = state.blend_fix_b & 0xFFFFFFu;
            bool have_const = false;
            uint32_t konst = 0;
            auto fix_factor = [&](uint32_t fix) -> GLenum {
                if (fix == 0x000000u) return GL_ZERO;
                if (fix == 0xFFFFFFu) return GL_ONE;
                if (!have_const) {
                    have_const = true;
                    konst = fix;
                    return GL_CONSTANT_COLOR;
                }
                if (konst == fix) return GL_CONSTANT_COLOR;
                if (konst == (~fix & 0xFFFFFFu)) return GL_ONE_MINUS_CONSTANT_COLOR;
                return GL_CONSTANT_COLOR;  // two distinct constants: approximate
            };
            if (src_fix) gl_src = fix_factor(fixa);
            if (dst_fix) gl_dst = fix_factor(fixb);
            if (have_const) {
                gl_blend_color((konst & 0xFF) / 255.0f,
                             ((konst >> 8) & 0xFF) / 255.0f,
                             ((konst >> 16) & 0xFF) / 255.0f,
                             1.0f);
            }
        }
        gl_blend_func(gl_src, gl_dst, stencil_alpha >= 0.0f);
        gl_blend_equation(map_blend_equation(blend_op), stencil_alpha >= 0.0f);
    } else {
        gl_blend_enable(false);
    }
    ge_shader_set_alpha_double(alpha_double);
    ge_shader_set_alpha_override(stencil_alpha);
    g_pipe_alpha_double = alpha_double;   // FX shader overrides re-apply it (psp_fx_shader.h)

    // Colour write mask (MASKRGB 0xBBGGRR / MASKALPHA): the PSP writes a
    // channel only where its mask bits are 0. GL can only mask whole
    // channels, so a channel is written iff its mask byte != 0xFF
    // (partial per-bit masks are approximated as fully writable).
    {
        const uint32_t mr = state.mask_rgb;
        const uint32_t ma = state.mask_alpha & 0xFF;
        const bool wr = (mr & 0xFF) != 0xFF;
        const bool wg = ((mr >> 8) & 0xFF) != 0xFF;
        const bool wb = ((mr >> 16) & 0xFF) != 0xFF;
        const bool wa = ma != 0xFF && !stencil_alpha_keep;
        gl_color_mask(wr, wg, wb, wa);
        static bool warned = false;
        const bool partial = (((mr & 0xFF) | ((mr >> 8) & 0xFF) | ((mr >> 16) & 0xFF) | ma) != 0)
            && (((mr & 0xFF) != 0 && (mr & 0xFF) != 0xFF)
                || (((mr >> 8) & 0xFF) != 0 && ((mr >> 8) & 0xFF) != 0xFF)
                || (((mr >> 16) & 0xFF) != 0 && ((mr >> 16) & 0xFF) != 0xFF)
                || (ma != 0 && ma != 0xFF));
        if (partial && !warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE_MASK] partial colour write mask rgb=0x%06X a=0x%02X "
                "approximated per channel\n", mr, ma);
        }
    }

    // Stencil test. The stencil write mask is the GE alpha mask (MASKALPHA: a set
    // bit protects that bit).
    if (stencil_on) {
        gl_stencil_enable(true);
        gl_stencil_func(map_stencil_func((int)(state.stencil_test & 0xFF)),
                        (int)((state.stencil_test >> 8) & 0xFF), (int)((state.stencil_test >> 16) & 0xFF));
        gl_stencil_op(map_stencil_op((int)(state.stencil_op & 0xFF)),
                      map_stencil_op((int)((state.stencil_op >> 8) & 0xFF)),
                      map_stencil_op((int)((state.stencil_op >> 16) & 0xFF)));
        gl_stencil_mask(~(unsigned)(state.mask_alpha & 0xFF) & 0xFFu);
    } else {
        gl_stencil_enable(false);
    }

    // Depth test
    if (state.depth_test_enable) {
        gl_depth_enable(true);
        gl_depth_func(map_depth_func(state.depth_func, depth_reversed));
        gl_depth_mask(
            state.depth_write_disable
            ? GL_FALSE : GL_TRUE);
    } else {
        gl_depth_enable(false);
    }

    // Cull face. The PSP never culls RECTANGLES (sprites are expanded by the
    // hardware with no winding) — a game may leave CULLFACE on while drawing
    // its 2D layer (Dissidia's movie strips).
    if (state.cull_enable && prim_type != 6) {
        gl_cull_enable(true);
        gl_cull_face(
            state.cull_face == 0
            ? GL_BACK : GL_FRONT);
    } else {
        gl_cull_enable(false);
    }

    return depth_reversed;
}


// ============================================================================
// Batched fast path
//
// ge_draw_prim no longer issues one GL draw (plus ~40 GL calls) per PRIM.
// Consecutive PRIMs whose complete GL-visible state is identical are appended
// to a CPU staging buffer and submitted as ONE draw (glDrawArrays, or
// glMultiDrawArrays for strips/fans) when the state changes or the display
// list yields (ge_draw_flush). Non-through draws are decoded in model space
// and skinned / lit / transformed in the vertex shader; through-mode draws,
// rectangles and degenerate-matrix fallbacks keep the CPU transform and use a
// passthrough vertex stage.
// ============================================================================

struct Stream {
    GLuint vbo = 0;
    GLuint vao = 0;          // 2D: CPU-transformed; 3D: skinned layout
    GLuint vao_plain = 0;    // 3D: unskinned layout (weights disabled)
    size_t stride = 0;
    size_t cap = 0;          // ring capacity in vertices
    size_t write = 0;        // next free ring slot (vertices)
    std::vector<uint8_t> stage;
    size_t stage_n = 0;      // vertices staged for the active batch
};
// Ring buffer synchronisation is by ORPHANING, not fences: vertices are
// appended at increasing offsets (a region is never rewritten while its
// storage is live), and when the ring wraps the whole buffer is re-specified
// with glBufferData(NULL), which gives the driver fresh storage while any
// in-flight draw keeps the old one. No glFenceSync / glClientWaitSync, no
// persistent mapping (GL_ARB_buffer_storage does not exist on macOS), and it
// is the idiom every GL 3.x driver (Mesa/radeonsi, Apple, NVIDIA) optimises.
// PSPRECOMP_GE_RING=sub (glBufferSubData) or map (unsynchronised
// glMapBufferRange + explicit flush; same orphan-on-wrap safety). Default:
// map on macOS, sub elsewhere. On Apple's GL-over-Metal each glBufferSubData
// into the ring the GPU is still reading costs ~40 us (Sky Fortress Bahamut at
// 4x: 713 uploads = 29 ms/frame, 19.7 fps); map took that to 0.17 ms (30 fps).
// Measured 2026-10-22 on an M2 Pro.

// PSPRECOMP_GE_SYNC: 1 = glFinish after every batch, 2 = glFinish at the end
// of every display list (FINISH), 3 = glFinish once per present, flush =
// glFlush after every batch (diagnostics / escape hatch for drivers whose
// asynchronous behaviour differs from the one this was developed on).
#if defined(__APPLE__) || defined(PSP_GLES)   // Android/Adreno: glBufferSubData costs ~0.5 ms per call (62 of 77 ms per battle frame); the mapping is far cheaper
static int g_ring_mode = 1;
#else
static int g_ring_mode = 0;
#endif
static int g_sync_mode = 0;
static bool g_sync_each = false;
static std::atomic<bool> g_fx_live{true};     // settings menu Debug page: FX shaders on/off (this session)
static std::atomic<bool> g_ping_overlay{false};   // ping to the other players, next to the fps number (settings menu Network page / PSPRECOMP_PING_OVERLAY=1)
static std::atomic<bool> g_fps_counter{false};   // simple transparent fps number (settings menu Display page / PSPRECOMP_FPS_COUNTER=1)
static std::atomic<bool> g_perf_hud{false};   // settings menu Debug page / PSPRECOMP_PERF_HUD=1
static Stream g_s2d, g_s3d;
static std::atomic<bool> g_vb_active{false};   // view blocker fade (ge_draw_set_view_blocker)
static std::atomic<bool> g_vb_linger{false};   // some draw is still (partly) faded: keep evaluating so it fades back in instead of popping
static float g_dbg_eye[3] = {0, 0, 0}, g_dbg_fwd[3] = {0, 0, 1};
// The view (4x3) and projection (4x4) matrices of the last completed main scene (the camera most of its 3D draws used, widescreen /
// tall screen included): game modules project world points onto the screen with them
// (effects drawn on the game layer at a fighter's position).
static float g_last_view[12] = {}, g_last_proj[16] = {}; static bool g_last_vp_ok = false;
static std::atomic<uint64_t> g_last_vp_tag{0};   // (gen << 32 | phase bits) of the display list the last view / projection came from
bool ge_draw_last_view_proj_tag(uint32_t* gen, float* phase) {   // interpolation tag of that list (phase -1: not interpolated)
    const uint64_t v = g_last_vp_tag.load(); const uint32_t pb = static_cast<uint32_t>(v); float ph; std::memcpy(&ph, &pb, 4);
    *gen = static_cast<uint32_t>(v >> 32); *phase = ph; return g_last_vp_ok;
}
bool ge_draw_last_view_proj(float view[12], float proj[16]) { std::memcpy(view, g_last_view, sizeof g_last_view); std::memcpy(proj, g_last_proj, sizeof g_last_proj); return g_last_vp_ok; }
void ge_draw_last_eye(float eye[3], float fwd[3]) { for (int i = 0; i < 3; i++) { eye[i] = g_dbg_eye[i]; fwd[i] = g_dbg_fwd[i]; } }   // view blocker fade (ge_draw_set_view_blocker)
static float vb_fade_factor(uint8_t* rdram, const GeState& st, int n, int prim);
static std::atomic<bool> g_cc_collect{false};   // camera collision: cache the main view's stage geometry (a battle camera is running)
static void cc_collect(uint8_t* rdram, const GeState& st, int n, int prim);
// Ring size in vertices. 3D vertices are 68 bytes, 2D 28: 8.9 MB + 3.6 MB.
// Sized for several frames of high-poly characters; small enough for an APU
// (Steam Deck) where it is carved from shared system memory. A single batch
// bigger than the ring grows it (see flush_batch).
#ifdef PSP_GLES
static constexpr size_t RING_VERTS = 1u << 19;   // see the wrap comment in flush_batch: a wrap costs a glFinish here, so wrap rarely
#else
static constexpr size_t RING_VERTS = 131072;
#endif

static bool g_fast_enabled = true;    // PSPRECOMP_GE_FAST=0 / PSPRECOMP_GE_LEGACY=1 disables
static bool g_cpu_xform = false;      // PSPRECOMP_GE_CPU3D=1: CPU transform, still batched
static bool g_nobatch = false;        // PSPRECOMP_GE_NOBATCH=1: submit every PRIM separately
static bool g_verify = false;         // PSPRECOMP_GE_VERIFY=1: compare GPU vertex stage to the CPU path
static bool g_debug_env = false;      // any per-draw diagnostic env set -> legacy path

struct KeyBuf {
    uint32_t w[512];
    int n = 0;
    uint32_t id = 0;   // nonzero: assigned when a cached key was (re)built; equal ids mean equal contents (copies keep it)
    inline void u(uint32_t v) { w[n++] = v; }
    inline void f(float v) { uint32_t b; std::memcpy(&b, &v, 4); w[n++] = b; }
    inline void a(const float* p, int c) { std::memcpy(&w[n], p, (size_t)c * 4); n += c; }
    bool operator==(const KeyBuf& o) const {
        if (id && id == o.id) return true;
        return n == o.n && std::memcmp(w, o.w, (size_t)n * 4) == 0;
    }
};

struct Xf3D {
    float world[12], view[12], proj[16], uvxf[4];
    float bones[96];
    int skin_n = 0, lit = 0, lflags = 0, nl = 0;
    float lc[4][4];
    float lt[20][4];
};

static inline float rgb_f(uint32_t c, int ch) {
    return static_cast<float>((c >> (ch * 8)) & 0xFFu) * (1.0f / 255.0f);
}

// Widescreen: the factor for the x of this draw's clip / NDC position, 480 / (target width in PSP pixels), or 1. A 3D draw
// whose PSP viewport spans the whole screen and every through-mode draw get it (the PSP's picture lands in the middle of
// the wide target); a 3D draw with a partial viewport (a model preview) keeps its own scale and is shifted instead.
static float draw_wide_x(const GeState& st) {
    const int w = cur_w480();
    if (w <= 480) return 1.0f;
    if (ge_vtype_through(st.vertex_type)) return 480.0f / float(w);
    const GeViewportDepth vp = compute_state_viewport_depth(st);
    // A projection whose aspect (y scale / x scale) is already as wide as the target (the game was given the wide aspect, so
    // it culls with it too) needs no scaling here.
    if (st.proj_matrix[0] != 0.0f && st.proj_matrix[5] / st.proj_matrix[0] >= 0.98f * float(w) / 272.0f) return 1.0f;
    return (vp.x <= 0 && vp.x + vp.w >= 480) ? 480.0f / float(w) : 1.0f;
}

// Tall buffer (g_tall_px): a 3D draw whose PSP viewport covers the whole screen gets the whole buffer as its GL viewport and its clip y
// becomes y * s + t * w, s = 272 / height, t = s - 1 + 2 * band_b / height: the PSP's picture lands on the band unchanged and the view
// goes on above and below it. Other draws: s = 1, t = 0.
struct TallY { float s = 1.0f, t = 0.0f; };
static TallY band_map(int h) { TallY r; if (h > 272) { r.s = 272.0f / float(h); r.t = r.s - 1.0f + 2.0f * float(band_b(h)) / float(h); } return r; }
static TallY draw_tall_y(const GeState& st) {
    const int h = cur_h272();
    if (h <= 272 || ge_vtype_through(st.vertex_type)) return TallY{};
    const GeViewportDepth vp = compute_state_viewport_depth(st);
    return (vp.y <= 0 && vp.y + vp.h >= 272) ? band_map(h) : TallY{};
}
static void tall_proj(float* p, TallY m) {   // column-major: row 1 (clip y) -> s * row 1 + t * row 3 (clip w)
    if (m.s == 1.0f && m.t == 0.0f) return;
    for (int c = 0; c < 4; c++) p[c * 4 + 1] = p[c * 4 + 1] * m.s + m.t * p[c * 4 + 3];
}

// Tall buffer: a 2D sprite that reads a render target and spans the PSP's whole height (the glow pass adds a blurred copy of the picture
// back in such strips) would leave the rows above and below the band without it, and they look darker. Each such sprite gets a piece
// above and one below the band: a source that is itself a tall buffer is read on (its real rows there); any other (the 480x272 glow
// buffer) is mirrored about the band's edge, a soft glow of the same kind. Set per draw in ge_draw_prim (through, render-target texture).
static bool g_tall_fb_draw = false;
static void tall_extend_strips(std::vector<DecodedVertex>& d, int prim_type) {
    if (!g_tall_fb_draw || prim_type != 6) return;
    const int H = cur_h272();
    if (H <= 272) return;
    const float below = float(band_b(H)), above = float(H - 272) - below;
    const bool mirror = g_fbtex_h272 <= 272;
    const size_t n = d.size() & ~size_t(1);
    for (size_t i = 0; i < n; i += 2) {
        const bool a_top = d[i].pos[1] <= d[i + 1].pos[1];
        const DecodedVertex top = a_top ? d[i] : d[i + 1], bot = a_top ? d[i + 1] : d[i];
        if (!top.has_uv || top.pos[1] > 0.5f || bot.pos[1] < 271.5f) continue;
        const float dv = (bot.uv[1] - top.uv[1]) / (bot.pos[1] - top.pos[1]);   // texels per PSP row
        if (above > 0.f) {
            DecodedVertex t0 = top, t1 = bot;
            t0.pos[1] = top.pos[1] - above; t0.uv[1] = mirror ? top.uv[1] + above * dv : top.uv[1] - above * dv;
            t1.pos[1] = top.pos[1]; t1.uv[1] = top.uv[1];
            d.push_back(t0); d.push_back(t1);
        }
        if (below > 0.f) {
            DecodedVertex b0 = top, b1 = bot;
            b0.pos[1] = bot.pos[1]; b0.uv[1] = bot.uv[1];
            b1.pos[1] = bot.pos[1] + below; b1.uv[1] = mirror ? bot.uv[1] - below * dv : bot.uv[1] + below * dv;
            d.push_back(b0); d.push_back(b1);
        }
    }
}

static void build_xf(const GeState& st, Xf3D& x) {
    const uint32_t vt = st.vertex_type;
    std::memcpy(x.world, st.world_matrix, sizeof x.world);
    std::memcpy(x.view, st.view_matrix, sizeof x.view);
    std::memcpy(x.proj, st.proj_matrix, sizeof x.proj);
    if (const float wx = draw_wide_x(st); wx != 1.0f) {   // clip.x = row 0 of the projection
        x.proj[0] *= wx; x.proj[4] *= wx; x.proj[8] *= wx; x.proj[12] *= wx;
    }
    tall_proj(x.proj, draw_tall_y(st));
    if (ge_vtype_tc(vt) != GE_VTYPE_TC_NONE) {
        x.uvxf[0] = st.tex_scale_u; x.uvxf[1] = st.tex_scale_v;
        x.uvxf[2] = st.tex_offset_u; x.uvxf[3] = st.tex_offset_v;
    } else {
        x.uvxf[0] = 1.0f; x.uvxf[1] = 1.0f; x.uvxf[2] = 0.0f; x.uvxf[3] = 0.0f;
    }
    if (ge_vtype_weight(vt) != 0) {
        const int wcount = ((vt >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
        x.skin_n = std::min(wcount, 8);
        std::memcpy(x.bones, st.bone_matrix, (size_t)x.skin_n * 12 * sizeof(float));
    } else {
        x.skin_n = 0;
    }
    x.lit = st.lighting_enable ? 1 : 0;
    x.lflags = 0;
    x.nl = 0;
    if (x.lit) {
        const bool has_col = ge_vtype_col(vt) != GE_VTYPE_COL_NONE;
        const bool has_nrm = ge_vtype_nrm(vt) != GE_VTYPE_NRM_NONE;
        if ((st.material_update & 1u) && has_col) x.lflags |= 1;
        if ((st.material_update & 2u) && has_col) x.lflags |= 2;
        if (has_nrm) x.lflags |= 4;
        for (int c = 0; c < 3; c++) {
            x.lc[0][c] = rgb_f(st.material_emissive, c);
            x.lc[1][c] = rgb_f(st.material_ambient, c);
            x.lc[2][c] = rgb_f(st.material_diffuse, c);
            x.lc[3][c] = rgb_f(st.ambient_color, c);
        }
        x.lc[0][3] = 0.0f;
        x.lc[1][3] = static_cast<float>(st.material_alpha & 0xFFu) / 255.0f;
        x.lc[2][3] = 0.0f;
        x.lc[3][3] = static_cast<float>(st.ambient_alpha & 0xFFu) / 255.0f;
        for (int i = 0; i < 4; i++) {
            if (!st.light_enable[i]) continue;
            float (*L)[4] = &x.lt[x.nl * 5];
            L[0][0] = st.light_pos[i][0]; L[0][1] = st.light_pos[i][1]; L[0][2] = st.light_pos[i][2];
            L[0][3] = static_cast<float>((st.light_type[i] >> 8) & 3);
            L[1][0] = st.light_dir[i][0]; L[1][1] = st.light_dir[i][1]; L[1][2] = st.light_dir[i][2];
            L[1][3] = st.light_spot_exp[i];
            L[2][0] = st.light_att[i][0]; L[2][1] = st.light_att[i][1]; L[2][2] = st.light_att[i][2];
            L[2][3] = st.light_spot_cut[i];
            for (int c = 0; c < 3; c++) {
                L[3][c] = rgb_f(st.light_amb[i], c);
                L[4][c] = rgb_f(st.light_dif[i], c);
            }
            L[3][3] = 0.0f; L[4][3] = 0.0f;
            x.nl++;
        }
    }
}

// Complete GL-visible state of a draw, as a word string. Two draws with equal
// keys render identically when merged into one GL draw call.
static void build_key(KeyBuf& k, const GeState& st, GLenum mode, int path, int prim_type,
                      GLuint fbo_tex, const Xf3D* xf) {
    const uint32_t vt_nidx = st.vertex_type & ~(uint32_t)(GE_VTYPE_IDX_MASK << GE_VTYPE_IDX_SHIFT);
    const bool through = ge_vtype_through(st.vertex_type);
    k.u(mode); k.u(path); k.u(fbo_tex); k.u(vt_nidx);
    k.u(st.texture_enable ? 1u : 0u);
    if (st.texture_enable) {
        k.u(st.tex_addr[0]); k.u(st.tex_bufw[0]); k.u(st.tex_size[0]);
        k.u(st.tex_format); k.u(st.tex_mode);
        k.u(st.clut_addr); k.u(st.clut_addr_upper); k.u(st.clut_format);
        k.u(st.tex_func | (st.tex_func_rgba ? 0x100u : 0u) | (st.tex_func_double ? 0x200u : 0u));
        k.u(st.tex_filter); k.u(st.tex_wrap); k.u(st.tex_env_color);
    }
    k.u(st.alpha_blend_enable ? 1u : 0u);
    if (st.alpha_blend_enable) { k.u(st.blend_mode); k.u(st.blend_fix_a); k.u(st.blend_fix_b); }
    k.u(st.depth_test_enable ? 1u : 0u);
    if (st.depth_test_enable) { k.u(st.depth_func); k.u(st.depth_write_disable ? 1u : 0u); }
    k.u((st.cull_enable && prim_type != GE_PRIM_RECTANGLES) ? (2u | (st.cull_face & 1u)) : 0u);
    k.u(st.mask_rgb); k.u(st.mask_alpha & 0xFFu);
    k.u(st.stencil_test_enable ? 1u : 0u);
    if (st.stencil_test_enable) { k.u(st.stencil_test & 0xFFFFFFu); k.u(st.stencil_op & 0xFFFFFFu); }
    k.u(st.alpha_test_enable ? 1u : 0u);
    if (st.alpha_test_enable) k.u(st.alpha_test);
    k.u(st.color_test_enable ? 1u : 0u);
    if (st.color_test_enable) { k.u(st.color_test & 3u); k.u(st.color_ref); k.u(st.color_test_mask); }
    if (!through) {
        k.f(st.viewport_x_scale); k.f(st.viewport_y_scale); k.f(st.viewport_z_scale);
        k.f(st.viewport_x_center); k.f(st.viewport_y_center); k.f(st.viewport_z_center);
        k.u(st.offset_x); k.u(st.offset_y);
    }
    if (xf) {
        k.a(xf->world, 12); k.a(xf->view, 12); k.a(xf->proj, 16); k.a(xf->uvxf, 4);
        k.u((uint32_t)xf->skin_n);
        if (xf->skin_n) k.a(xf->bones, xf->skin_n * 12);
        k.u((uint32_t)xf->lit);
        if (xf->lit) {
            k.u((uint32_t)xf->lflags); k.u((uint32_t)xf->nl);
            k.a(&xf->lc[0][0], 16);
            if (xf->nl) k.a(&xf->lt[0][0], xf->nl * 20);
        }
    }
}

struct Batch {
    bool active = false;
    KeyBuf key;
    GLenum mode = GL_TRIANGLES;
    Stream* s = nullptr;
    GLuint vao = 0;
    std::vector<GLint> firsts;
    std::vector<GLsizei> counts;
    bool fx_timed = false;   // FX shader override batch, GPU-timed (PSPRECOMP_FX_SHADER_LOG=2)
    bool fxl = false;        // an effects draw: also drawn into the screen-space effects layer (fxl_*)
    float refl = 0.f;        // > 0: a reflective surface (material byte / 255): its triangles go to the reflectivity mask (g_rml_tris)
    GLuint persist_vao = 0;  // != 0: one draw from a resident vertex buffer (gpu_vcache): no staging, no upload
    size_t persist_n = 0;
};
static Batch g_batch;
static GLuint g_copy_buf_bound = 0;

// ---- PSPRECOMP_GE_VERIFY: GPU vertex stage vs the CPU reference path ----
// Each GPU-transformed GL_TRIANGLES PRIM is flushed alone with transform
// feedback capturing gl_Position / v_texcoord / v_color; the capture is
// compared with ge_decode_vertices + ge_transform_vertices (the CPU path).
struct VerifyExpect { float pos[4], uv[2], col[4]; };
static std::vector<VerifyExpect> g_expect;
static bool g_expect_valid = false;
static GLuint g_tf_buf = 0;
static struct {
    uint64_t draws = 0, verts = 0, bad_pos = 0, bad_uv = 0, bad_col = 0;
    double max_pos = 0, max_uv = 0, max_col = 0;
    uint64_t lit_draws = 0, skin_draws = 0;
} g_vstat;
static bool g_expect_lit = false, g_expect_skin = false;

static void verify_report(const char* tag) {
    std::fprintf(stderr,
        "[VERIFY] %s draws=%llu (lit=%llu skinned=%llu) verts=%llu | max clip err %.3g (bad>1e-3: %llu) "
        "max uv err %.3g (bad>1e-4: %llu) max colour err %.2f LSB (bad>1.01: %llu)\n",
        tag, (unsigned long long)g_vstat.draws, (unsigned long long)g_vstat.lit_draws,
        (unsigned long long)g_vstat.skin_draws, (unsigned long long)g_vstat.verts,
        g_vstat.max_pos, (unsigned long long)g_vstat.bad_pos, g_vstat.max_uv,
        (unsigned long long)g_vstat.bad_uv, g_vstat.max_col, (unsigned long long)g_vstat.bad_col);
}

static void verify_compare(size_t n) {
    std::vector<float> got(n * 10);
    glBindBuffer(GL_COPY_READ_BUFFER, g_tf_buf);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)(n * 10 * sizeof(float)), got.data());
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    g_vstat.draws++;
    static const bool fault = std::getenv("PSPRECOMP_GE_VERIFY_FAULT") != nullptr;  // harness self-check
    if (fault && !g_expect.empty()) g_expect[0].pos[0] += 0.5f;
    if (g_vstat.draws <= 3 && !g_expect.empty())
        std::fprintf(stderr, "[VERIFY] sample v0 gpu pos=(%g,%g,%g,%g) col=(%.1f,%.1f,%.1f,%.1f) | cpu pos=(%g,%g,%g,%g) col=(%.1f,%.1f,%.1f,%.1f)\n",
            got[0], got[1], got[2], got[3], got[6] * 255, got[7] * 255, got[8] * 255, got[9] * 255,
            g_expect[0].pos[0], g_expect[0].pos[1], g_expect[0].pos[2], g_expect[0].pos[3],
            g_expect[0].col[0], g_expect[0].col[1], g_expect[0].col[2], g_expect[0].col[3]);
    if (g_expect_lit) g_vstat.lit_draws++;
    if (g_expect_skin) g_vstat.skin_draws++;
    const size_t m = std::min(n, g_expect.size());
    for (size_t i = 0; i < m; i++) {
        const float* g = &got[i * 10];
        const VerifyExpect& e = g_expect[i];
        g_vstat.verts++;
        double pe = 0;
        for (int k = 0; k < 4; k++) {
            const double d = std::fabs((double)g[k] - e.pos[k]) / std::max(1.0, std::fabs((double)e.pos[k]));
            pe = std::max(pe, d);
        }
        g_vstat.max_pos = std::max(g_vstat.max_pos, pe);
        if (pe > 1e-3) g_vstat.bad_pos++;
        double ue = 0;
        for (int k = 0; k < 2; k++)
            ue = std::max(ue, std::fabs((double)g[4 + k] - e.uv[k]) / std::max(1.0, std::fabs((double)e.uv[k])));
        g_vstat.max_uv = std::max(g_vstat.max_uv, ue);
        if (ue > 1e-4) g_vstat.bad_uv++;
        double ce = 0;
        for (int k = 0; k < 4; k++) ce = std::max(ce, std::fabs((double)g[6 + k] * 255.0 - e.col[k]));
        g_vstat.max_col = std::max(g_vstat.max_col, ce);
        if (ce > 1.01) g_vstat.bad_col++;
        if ((pe > 1e-3 || ue > 1e-4 || ce > 1.01) && (g_vstat.bad_pos + g_vstat.bad_uv + g_vstat.bad_col) <= 8) {
            std::fprintf(stderr,
                "[VERIFY] MISMATCH v%zu/%zu gpu pos=(%g,%g,%g,%g) uv=(%g,%g) col=(%.1f,%.1f,%.1f,%.1f) | cpu pos=(%g,%g,%g,%g) uv=(%g,%g) col=(%.1f,%.1f,%.1f,%.1f)\n",
                i, n, g[0], g[1], g[2], g[3], g[4], g[5], g[6] * 255, g[7] * 255, g[8] * 255, g[9] * 255,
                e.pos[0], e.pos[1], e.pos[2], e.pos[3], e.uv[0], e.uv[1], e.col[0], e.col[1], e.col[2], e.col[3]);
        }
    }
    if (g_vstat.draws % 2000 == 0) verify_report("progress");
}


// ---- Screen-space effects layer (for the SSR in psp_screenfx.cpp) ----
// The screen-space reflection marches against the scene depth, which blended effects (orbs, beams, auras, glass: depth tested,
// not depth writing) never touch. Every such draw is issued a second time into a per-target layer: RGBA8 colour (the game's own
// blend over black, so premultiplied; alpha = coverage, additive draws leave it alone) and its own depth (writes on: the nearest
// effect fragment per pixel). The pass-1 march hit-tests it next to the scene depth. Same size as the target.
struct FxLayer {
    GLuint fbo = 0, color = 0, depth = 0;
    int w = 0, h = 0;
    bool dirty = true;     // cleared before its next use (a new scene)
    bool used = false;     // holds effects of the scene being drawn
    bool failed = false;
};
// Lifetime: cleared when the screen-space pass has consumed it (lazily, by the next effects draw), NOT at a new scene: the game draws
// part of its effects (orbs, particles) after the 3D run, i.e. after the pass; those stay in the layer and are used by the next
// frame's pass (one frame late). The pass samples a single colour + depth pair (the Vulkan layer allows few samplers per program).
static FxLayer g_fxl[GE_MAX_TARGETS];
static float g_fx_clearz[GE_MAX_TARGETS];
static int g_fxl_draws = 0;
static bool g_fxl_flag = false;   // the draws being queued are effects draws (set by fx_track)
// ---- Reflectivity mask (issue #2, psp_refl_table.h) ----
// Which pixels are reflective: the triangles of every depth-writing 3D draw whose texture has a reflectivity are captured in clip
// space with the material byte (psp_refl_encode) and drawn into a mask by the screen-space pass in one go (psp_screenfx.cpp), which
// keeps a mask texel only where its depth matches the scene's: whatever was drawn over a reflective floor later hides it. One pass
// per frame however many draws reflect: re-issuing each draw into a mask framebuffer cost two render-pass breaks per draw, which
// a tiled mobile GPU pays for in full (RP6, a tower top built of many reflective pieces).
static std::vector<float> g_rml_tris[GE_MAX_TARGETS];   // per vertex: clip x y z w, material byte
static void rml_capture(const GeState& st, uint8_t* rdram, int prim_type, int count, float refl);
static void fighter_capture(const GeState& st, uint8_t* rdram, int prim_type, int count);
static bool g_rml_fighters[GE_MAX_TARGETS];   // the mask list also holds fighter triangles (material byte 1, fighter_capture)
static bool fighter_mask_wanted(const GeState& st);
static bool g_rml_flag = false;   // the draws being queued may be reflective (set by fx_track; the value comes from the bound texture)
static void fxl_free(FxLayer& L) {
    if (L.fbo) glDeleteFramebuffers(1, &L.fbo);
    if (L.color) glDeleteTextures(1, &L.color);
    if (L.depth) glDeleteTextures(1, &L.depth);
    L = FxLayer{};
}
// Binds the layer of target `ti` as the draw framebuffer (created / resized / cleared as needed); false = unavailable.
static bool fxl_bind(int ti) {
    const GeTarget& t = g_targets[ti];
    FxLayer& L = g_fxl[ti];
    const int w = t.w480 * t.rs, h = t.h272 * t.rs;
    if (L.fbo && (L.w != w || L.h != h)) { const bool f = L.failed; fxl_free(L); L.failed = f; }
    if (!L.fbo) {
        if (L.failed) return false;
        L.w = w; L.h = h;
        glGenTextures(1, &L.color);
        glBindTexture(GL_TEXTURE_2D, L.color);
        glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), w, h, 0, GL_RGBA, scene_type(), nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &L.depth);
        glBindTexture(GL_TEXTURE_2D, L.depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);   // the format the screen-space pass's own depth copy uses (the Vulkan layer has no plain DEPTH_COMPONENT24 textures)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &L.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, L.color, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, L.depth, 0);
        ge_texture_invalidate_bind_cache();   // glBindTexture above
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[SSR] effects layer framebuffer incomplete (%dx%d), off\n", w, h);
            fxl_free(L); L.failed = true;
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            return false;
        }
        L.dirty = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, L.fbo);
    if (L.dirty) {
        const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        if (sc) glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClearDepth(g_fx_clearz[ti]);   // what the scene's own depth was cleared to: the game's depth range may be reversed
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (sc) glEnable(GL_SCISSOR_TEST);
        glc::invalidate();   // masks changed behind the cache
        L.dirty = false;
    }
    L.used = true;
    return true;
}

static void flush_batch() {
    if (!g_batch.active) return;
    psp_perf::Scope perf_scope_(psp_perf::FLUSH);
    Stream& s = *g_batch.s;
    g_batch.active = false;
    const bool fx_timed = g_batch.fx_timed;
    g_batch.fx_timed = false;
    const bool fxl_draw = g_batch.fxl;
    g_batch.fxl = false;
    g_batch.refl = 0.f;
    const size_t persist_n = g_batch.persist_n;
    const GLuint persist_vao = g_batch.persist_vao;
    g_batch.persist_n = 0; g_batch.persist_vao = 0;
    const size_t n = persist_n ? persist_n : s.stage_n;
    if (n == 0) { g_batch.firsts.clear(); g_batch.counts.clear(); return; }

    if (!persist_n) {
        psp_prof::Scope ps(psp_prof::S_UPLOAD);
        if (g_copy_buf_bound != s.vbo) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, s.vbo);
            g_copy_buf_bound = s.vbo;
        }
        if (n > s.cap) {
            // Single batch larger than the ring: re-specify (orphan) at the new size.
            s.cap = n;
#ifdef PSP_GLES
            glFinish();
#endif
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(s.cap * s.stride), nullptr, GL_STREAM_DRAW);
            s.write = 0;
        } else if (s.write + n > s.cap) {
#ifdef PSP_GLES
            // Adreno does not honour orphaning: draws still pending when the buffer is re-specified read the NEW storage
            // (menus drawn with other screens' vertices, 2026-11-10, Adreno 740). Wait for the GPU instead and reuse the
            // buffer; once per ring, which is rare.
            glFinish();
#else
            // Ring wrapped: orphan so in-flight draws keep the old storage.
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(s.cap * s.stride), nullptr, GL_STREAM_DRAW);
#endif
            s.write = 0;
        }
        bool done = false;
        if (g_ring_mode == 1) {
            const GLsizeiptr len = (GLsizeiptr)(n * s.stride);
            void* dst = glMapBufferRange(GL_COPY_WRITE_BUFFER, (GLintptr)(s.write * s.stride), len,
                                         GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
            if (dst) {
                std::memcpy(dst, s.stage.data(), (size_t)len);
                glFlushMappedBufferRange(GL_COPY_WRITE_BUFFER, 0, len);
                glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                done = true;
            }
        }
        if (!done)
            glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)(s.write * s.stride),
                            (GLsizeiptr)(n * s.stride), s.stage.data());
    }
    const GLint first = persist_n ? 0 : static_cast<GLint>(s.write);
    if (!persist_n) s.write += n;

    {
        psp_prof::Scope ps(psp_prof::S_DRAW);
        gl_bind_vao(persist_n ? persist_vao : g_batch.vao);
        if (fx_timed) psp_fx_timer_begin();
        if (g_no_raster != 2) {
            const size_t nseg = g_batch.firsts.size();
            const GLenum m = g_batch.mode;
            if (g_verify && g_expect_valid && g_tf_buf
                && (m == GL_TRIANGLES || m == GL_TRIANGLE_STRIP || m == GL_TRIANGLE_FAN)) {
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, g_tf_buf);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_TRIANGLES);
                glDrawArrays(m, first, static_cast<GLsizei>(n));
                glEndTransformFeedback();
                if (g_no_raster != 1) glDisable(GL_RASTERIZER_DISCARD);
                verify_compare(g_expect.size());
            } else {
                const bool single = persist_n || nseg == 1 || m == GL_TRIANGLES || m == GL_LINES || m == GL_POINTS;
                if (!single) for (size_t i = 0; i < nseg; i++) g_batch.firsts[i] += first;
                auto issue = [&]() {
                    if (single) glDrawArrays(m, first, static_cast<GLsizei>(n));
                    else glMultiDrawArrays(m, g_batch.firsts.data(), g_batch.counts.data(), static_cast<GLsizei>(nseg));
                };
                issue();
                if (fxl_draw && g_current_target >= 0 && glc::bsrc && glc::bdst) {
                    // The same draw again into the effects layer: the game's blend for colour; alpha = coverage (additive: unchanged).
                    const GLenum s = glc::bsrc, d = glc::bdst;
                    const bool add = d == GL_ONE;
                    const bool sep = glc::bsep == 1;
                    const bool blend_was_on = glc::blend_en == 1;
                    const int ti = g_current_target;
                    if (fxl_bind(ti)) {
                        g_fxl_draws++;
                        glDepthMask(GL_TRUE);
                        if (!blend_was_on) glEnable(GL_BLEND);
                        glBlendFuncSeparate(s, d, add ? GL_ZERO : GL_ONE, add ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
                        issue();
                        glDepthMask(GL_FALSE);
                        if (sep) glBlendFuncSeparate(s, d, GL_ONE, GL_ZERO); else glBlendFunc(s, d);
                        if (!blend_was_on) glDisable(GL_BLEND);
                    }
                    glBindFramebuffer(GL_FRAMEBUFFER, g_targets[ti].fbo);
                }
            }
        }
        if (fx_timed) psp_fx_timer_end();
    }
    if (g_sync_each) glFinish();
    else if (g_sync_mode == 4) glFlush();
    s.stage_n = 0;
    g_batch.firsts.clear();
    g_batch.counts.clear();
    g_expect_valid = false;
}

void ge_draw_flush() { flush_batch(); }
void ge_draw_list_end() { flush_batch(); if (g_sync_mode == 2) glFinish(); }
void ge_draw_set_fast(bool on) { flush_batch(); g_fast_enabled = on; std::fprintf(stderr, "[DRAW] fast path %s (runtime)\n", on ? "on" : "OFF"); }

int ge_draw_sync_mode() { return g_sync_mode; }
void ge_draw_set_sync_mode(int mode) {
    flush_batch();
    g_sync_mode = (mode < 0 || mode > 4) ? 0 : mode;
    g_sync_each = g_sync_mode == 1;
    static const char* names[5] = {"off", "glFinish per batch", "glFinish per list", "glFinish per present", "glFlush per batch"};
    std::fprintf(stderr, "[DRAW] GE sync mode %d (%s) (settings menu)\n", g_sync_mode, names[g_sync_mode]);
}
int ge_draw_ring_mode() { return g_ring_mode; }
bool ge_draw_perf_overlay() { return g_perf_hud.load(); }
bool ge_draw_fps_counter() { return g_fps_counter.load(); }
bool ge_draw_ping_overlay() { return g_ping_overlay.load(); }
void ge_draw_set_ping_overlay(bool on) { g_ping_overlay = on; }
void ge_draw_set_fps_counter(bool on) {
    g_fps_counter = on;
    std::fprintf(stderr, "[DRAW] fps counter %s\n", on ? "on" : "off");
}
static void perf_overlay_apply(bool on) {
    psp_perf::g_on.store(on);
    for (auto& n : psp_perf::g_ns) n.store(0);
#if defined(PSPRECOMP_HAVE_VKGL)
    vkgl::set_profile(on);
#endif
}
void ge_draw_set_perf_overlay(bool on) {
    g_perf_hud = on;
    perf_overlay_apply(on);
    std::fprintf(stderr, "[DRAW] performance overlay %s\n", on ? "on" : "off");
}

// ---- Resident vertex buffers for cached decodes (PSPRECOMP_GPU_VCACHE=0 turns them off) ----
// One static VBO + the two 3D layouts per decode-cache entry (psp_ge_vertex.cpp), made on first use and dropped when the
// cache drops its entries (its generation changes). Same attribute layout as the 3D stream (setup_stream).
struct GpuVBuf { GLuint vbo = 0, vao = 0, vao_plain = 0; };
static std::unordered_map<uint64_t, GpuVBuf> g_vcache;
static uint64_t g_vcache_gen = 0;
static bool gpu_vcache_on() {
    static const bool on = [] { const char* e = std::getenv("PSPRECOMP_GPU_VCACHE"); return !(e && e[0] == '0'); }();
    return on;
}
static void vcache_layout(bool weights) {
    const GLsizei st = static_cast<GLsizei>(sizeof(GeGpuVertex));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(12));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(32));
    glEnableVertexAttribArray(2);
    if (weights) {
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(36));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(52));
        glEnableVertexAttribArray(5);
    }
}
static void gpu_vcache_drop_all() {
    for (auto& kv : g_vcache) {
        GLuint vaos[2] = {kv.second.vao, kv.second.vao_plain};
        glDeleteVertexArrays(2, vaos);
        glDeleteBuffers(1, &kv.second.vbo);
    }
    g_vcache.clear();
    glc::vao = ~0u;                                       // a deleted array may be the cached binding: bind afresh next time
}
static const GpuVBuf& gpu_vcache_get(uint64_t id, const GeGpuVertex* v, size_t n) {
    const uint64_t gen = ge_decode_cache_generation();
    if (gen != g_vcache_gen) { flush_batch(); gpu_vcache_drop_all(); g_vcache_gen = gen; }
    auto it = g_vcache.find(id);
    if (it != g_vcache.end()) return it->second;
    flush_batch();                                        // the buffer and layout bindings below change GL state
    GpuVBuf b;
    glGenBuffers(1, &b.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(GeGpuVertex)), v, GL_STATIC_DRAW);
    glGenVertexArrays(1, &b.vao);
    glBindVertexArray(b.vao);
    vcache_layout(true);
    glGenVertexArrays(1, &b.vao_plain);
    glBindVertexArray(b.vao_plain);
    vcache_layout(false);
    glBindVertexArray(0);
    glc::vao = 0;                                         // the raw binds above bypassed gl_bind_vao's cache
    static int made = 0;
    if (++made <= 40 || made % 100 == 0)
        std::fprintf(stderr, "[GPU_VCACHE] resident buffer %d: %zu vertices (%.1f MB)\n", made, n, n * sizeof(GeGpuVertex) / 1048576.0);
    return g_vcache.emplace(id, b).first->second;
}

static void setup_stream(Stream& s, size_t stride, bool is3d) {
    s.stride = stride;
    s.cap = RING_VERTS;
    s.write = 0;
    s.stage.resize(8192 * stride);
    glGenBuffers(1, &s.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s.cap * stride), nullptr, GL_STREAM_DRAW);
    const GLsizei st = static_cast<GLsizei>(stride);
    auto layout = [&](bool weights) {
        if (!is3d) {
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(16));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(24));
            glEnableVertexAttribArray(2);
        } else {
            // GeGpuVertex: pos3 @0, normal3 @12, uv2 @24, color4 @32, weights8 @36
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(24));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(32));
            glEnableVertexAttribArray(2);
            if (weights) {
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(36));
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(52));
                glEnableVertexAttribArray(5);
            }
        }
    };
    glGenVertexArrays(1, &s.vao);
    glBindVertexArray(s.vao);
    layout(true);
    if (is3d) {
        glGenVertexArrays(1, &s.vao_plain);
        glBindVertexArray(s.vao_plain);
        layout(false);
    }
    glBindVertexArray(0);
}

static void teardown_stream(Stream& s) {
    if (s.vbo) glDeleteBuffers(1, &s.vbo);
    if (s.vao) glDeleteVertexArrays(1, &s.vao);
    if (s.vao_plain) glDeleteVertexArrays(1, &s.vao_plain);
    s = Stream{};
}

// ---- FX shader overrides (psp_fx_shader.h, FX_SHADERS.md) ----
// g_fx_on: -1 undecided, 0 no manifest (zero cost), 1 on.
static int g_fx_on = -1;
static GLuint g_fx_screen_tex = 0, g_fx_screen_fbo = 0;
static int g_fx_screen_w = 0, g_fx_screen_h = 0;

// Copy of the bound render target for the "screen" sampler (refraction /
// distortion). The batch about to draw is still empty, so every earlier draw
// is in it. psp_fx_shader.cpp calls this at most once per frame and target.
static unsigned fx_screen_copy(int* w, int* h) {
    const int W = cur_w480() * cur_rs(), H = cur_h272() * cur_rs();
    if (!g_fx_screen_tex || g_fx_screen_w != W || g_fx_screen_h != H) {
        if (!g_fx_screen_tex) glGenTextures(1, &g_fx_screen_tex);
        glBindTexture(GL_TEXTURE_2D, g_fx_screen_tex);   // caller's unit (not unit 0)
        glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), W, H, 0, GL_RGBA, scene_type(), nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!g_fx_screen_fbo) glGenFramebuffers(1, &g_fx_screen_fbo);
        GLint prev = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fx_screen_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fx_screen_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
        g_fx_screen_w = W; g_fx_screen_h = H;
    }
    GLint cur = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &cur);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)cur);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fx_screen_fbo);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)cur);
    if (w) *w = W;
    if (h) *h = H;
    return g_fx_screen_tex;
}

bool ge_draw_fx_available() {
    if (g_fx_on < 0) g_fx_on = psp_fx_want_ids() ? 1 : 0;
    return g_fx_on == 1;
}
bool ge_draw_fx_shaders() { return ge_draw_fx_available() && g_fx_live.load(); }
void ge_draw_set_fx_shaders(bool on) {
    flush_batch();
    g_fx_live = on;
    std::fprintf(stderr, "[FXSHADER] %s (settings menu)\n", on ? "on" : "off");
}

// After the uber pipeline of a new batch: swap in the override program when
// the bound texture's pack id is in the manifest, then apply its blend/depth.
static void fx_batch(const GeState& state, GLuint fbo_tex, const GeXform3D* g) {
    if (g_fx_on < 0) g_fx_on = psp_fx_want_ids() ? 1 : 0;
    if (!g_fx_live.load(std::memory_order_relaxed)) return;
    if (!g_fx_on || !state.texture_enable || fbo_tex) return;
    const uint64_t id = ge_texture_bound_pack_id();
    if (!id) return;
    PspFxDrawIn in{};
    in.tex_id = id;
    in.state = &state;
    in.xform = g;
    in.target_w = cur_w480() * cur_rs();
    in.target_h = cur_h272() * cur_rs();
    for (int i = 0; i < 4; i++) in.vp[i] = glc::vp[i];
    in.alpha_double = g_pipe_alpha_double;
    in.screen_copy = fx_screen_copy;
    PspFxDrawOut out{};
    if (!psp_fx_begin_batch(in, &out)) return;
    using psp_fx::Blend;
    switch (out.blend) {
    case Blend::Game: break;
    case Blend::Off: gl_blend_enable(false); break;
    default: {
        GLenum s = GL_SRC_ALPHA, d = GL_ONE;
        if (out.blend == Blend::AddOne) { s = GL_ONE; d = GL_ONE; }
        else if (out.blend == Blend::Alpha) { s = GL_SRC_ALPHA; d = GL_ONE_MINUS_SRC_ALPHA; }
        else if (out.blend == Blend::Premul) { s = GL_ONE; d = GL_ONE_MINUS_SRC_ALPHA; }
        else if (out.blend == Blend::Screen) { s = GL_ONE; d = GL_ONE_MINUS_SRC_COLOR; }
        gl_blend_enable(true);
        gl_blend_func(s, d);
        gl_blend_equation(GL_FUNC_ADD);
        break;
    }
    }
    using psp_fx::Depth;
    if (out.depth == Depth::NoWrite) gl_depth_mask(false);
    else if (out.depth == Depth::Write) gl_depth_mask(true);
    else if (out.depth == Depth::Off) gl_depth_enable(false);
    g_batch.fx_timed = out.timed;
    g_batch.refl = 0.f;   // an override shader has no alpha override to write the mask with
}

// Make `k` the active batch (flushing a different one) with room for `need`
// more vertices in stream `s`, applying the GL pipeline when a new batch starts.
static void ensure_batch(const KeyBuf& k, Stream& s, GLuint vao, GLenum mode, size_t need,
                         uint8_t* rdram, const GeState& state, int prim_type,
                         GLuint fbo_tex, const Xf3D* xf) {
    if (!g_nobatch && g_batch.active && g_batch.s == &s && g_batch.key == k && s.stage_n + need <= s.cap)
        return;
    flush_batch();
    g_batch.key.n = k.n; g_batch.key.id = k.id; std::memcpy(g_batch.key.w, k.w, (size_t)k.n * 4);
    g_batch.mode = mode;
    g_batch.s = &s;
    g_batch.vao = vao;
    g_batch.active = true;
    g_batch.fxl = g_fxl_flag;
    psp_prof::batches++;
    apply_gl_pipeline(rdram, state, prim_type, fbo_tex, xf != nullptr);
    g_batch.refl = g_rml_flag && state.texture_enable && !fbo_tex ? ge_texture_bound_refl() : 0.f;
    if (g_rml_flag && state.texture_enable && !fbo_tex) {   // PSPRECOMP_REFL_LOG=1: each surface texture once, to fill reflectivity.txt
        static const bool lg = std::getenv("PSPRECOMP_REFL_LOG") != nullptr;
        static std::unordered_set<uint64_t> seen;
        const uint64_t id = ge_texture_bound_pack_id();
        if (lg && id && seen.insert(id).second)
            { const PspReflMaterial m = psp_refl_decode(uint8_t(g_batch.refl * 255.f + 0.5f));
              std::fprintf(stderr, "[REFL] stage %d texture %016llx reflectivity %.2f roughness %.2f metal %d\n", psp_stage_id(), (unsigned long long)id, m.refl, m.rough, (int)m.metal); }
    }
    GeXform3D g{};
    if (xf) {
        psp_prof::Scope ps(psp_prof::S_UNIFORM);
        g.world = xf->world; g.view = xf->view; g.proj = xf->proj;
        std::memcpy(g.uvxf, xf->uvxf, sizeof g.uvxf);
        g.skin_n = xf->skin_n; g.bones = xf->bones;
        g.lit = xf->lit; g.lflags = xf->lflags; g.nlights = xf->nl;
        std::memcpy(g.lc, xf->lc, sizeof g.lc);
        std::memcpy(g.lt, xf->lt, (size_t)xf->nl * 20 * sizeof(float));
        ge_shader_set_xform_3d(g);
    }
    if (g_fx_on != 0) fx_batch(state, fbo_tex, xf ? &g : nullptr);
}

static inline uint8_t* stage_reserve(Stream& s, size_t n) {
    const size_t need = (s.stage_n + n) * s.stride;
    if (s.stage.size() < need) s.stage.resize(std::max(need, s.stage.size() * 2));
    return s.stage.data() + s.stage_n * s.stride;
}

// RECTANGLES (sprites) in transform mode: ge_transform_vertices leaves positions
// in clip space (the GPU divides by w for triangles), but the rectangle
// expansion emits w=1 corners, so the perspective divide must happen here --
// otherwise every 3D sprite (Dissidia's glow / flash / impact particles) lands
// at clip coordinates, far outside the screen. Vertices behind the eye get
// w=0 and the expansion skips their rectangle.
extern uint32_t g_ge_cur_pc;
static void rect_clip_to_ndc(std::vector<DecodedVertex>& v, const GeState& state, int prim_type) {
    static const bool old_rect = std::getenv("PSPRECOMP_GE_RECT_OLD") != nullptr;  // A/B: pre-fix behaviour
    if (old_rect || prim_type != GE_PRIM_RECTANGLES || ge_vtype_through(state.vertex_type)) return;
    for (auto& d : v) {
        if (d.w == 1.0f) continue;  // NDC-direct fallback / already divided
        if (d.w > 1e-6f) {
            const float iw = 1.0f / d.w;
            d.pos[0] *= iw; d.pos[1] *= iw; d.pos[2] *= iw;
            d.w = 1.0f;
        } else {
            d.w = 0.0f;
        }
    }
}

static void fast_prim(uint8_t* rdram, const GeState& state, int prim_type, int count,
                      GLuint fbo_tex) {
    const bool through = ge_vtype_through(state.vertex_type);
    const GLenum mode = map_prim_type(prim_type);
    int n = count;
    if (prim_type == GE_PRIM_TRIANGLES) n -= n % 3;
    else if (prim_type == GE_PRIM_LINES || prim_type == GE_PRIM_RECTANGLES) n -= (n & 1);
    if (n <= 0) return;

    // Key cache: draws between which no key-relevant GE command ran (g_ge_state_gen, e.g. just VADDR + PRIM) share the
    // key, the transform block and the GPU-path decision, so none of it is rebuilt per draw (build_key was the top item
    // of the render thread's profile: ~100 words per 3D draw).
    static struct {
        bool valid = false;
        uint32_t gen = 0;
        GLenum mode = 0;
        int prim_type = 0, w480 = 0, fbw480 = 0;
        GLuint fbo_tex = 0;
        bool gpu = false;
        int path = 0;
        bool has2d = false;
        KeyBuf key, key2d;
        Xf3D xf;
    } kc;
    static uint32_t key_serial = 0;
    const int w480 = cur_w480();
    if (!(kc.valid && kc.gen == g_ge_state_gen && kc.mode == mode && kc.prim_type == prim_type && kc.fbo_tex == fbo_tex
          && kc.w480 == w480 && kc.fbw480 == g_fbtex_w480)) {
        kc.valid = true; kc.gen = g_ge_state_gen; kc.mode = mode; kc.prim_type = prim_type; kc.fbo_tex = fbo_tex;
        kc.w480 = w480; kc.fbw480 = g_fbtex_w480;
        kc.gpu = !through && prim_type != GE_PRIM_RECTANGLES && !g_cpu_xform && ge_gpu_transform_ok(state);
        kc.key.n = 0;
        kc.key.id = 0;
        kc.has2d = false;
        if (kc.gpu) {
            Xf3D& xf = kc.xf;
            build_xf(state, xf);
            if (fbo_tex && g_fbtex_w480 > 480 && state.texture_enable) {   // a wide render target as the texture: the PSP's u (0..1 = 480 px) -> its middle part
                const float fs = 480.0f / float(g_fbtex_w480), fo = 0.5f * float(g_fbtex_w480 - 480) / float(g_fbtex_w480);
                xf.uvxf[2] = xf.uvxf[2] * fs + fo;
                xf.uvxf[0] *= fs;
                static bool logged = false;
                if (!logged) { logged = true; std::fprintf(stderr, "[WIDE] 3D draw samples a %d px wide render target: u remapped\n", g_fbtex_w480); }
            }
            kc.path = xf.skin_n > 0 ? 2 : 1;
            build_key(kc.key, state, mode, kc.path, prim_type, fbo_tex, &xf);
            kc.key.id = ++key_serial;
        }
    }
    bool gpu = kc.gpu;
    static const bool kc_verify = std::getenv("PSPRECOMP_KEYCACHE_VERIFY") != nullptr;   // debug: cached key vs a fresh build, per draw
    if (kc_verify && gpu) {
        Xf3D xf2; KeyBuf k2;
        build_xf(state, xf2);
        if (fbo_tex && g_fbtex_w480 > 480 && state.texture_enable) {
            const float fs = 480.0f / float(g_fbtex_w480), fo = 0.5f * float(g_fbtex_w480 - 480) / float(g_fbtex_w480);
            xf2.uvxf[2] = xf2.uvxf[2] * fs + fo; xf2.uvxf[0] *= fs;
        }
        build_key(k2, state, mode, kc.path, prim_type, fbo_tex, &xf2);
        static int bad = 0, total = 0;
        total++;
        if (!(k2.n == kc.key.n && !std::memcmp(k2.w, kc.key.w, (size_t)k2.n * 4)) && bad++ < 20)
            std::fprintf(stderr, "[KEYCACHE] MISMATCH draw %d gen %u\n", total, kc.gen);
        if (total % 20000 == 0) std::fprintf(stderr, "[KEYCACHE] %d draws checked, %d mismatches\n", total, bad);
    }
    if (gpu) {
        const Xf3D& xf = kc.xf;
        const int path = kc.path;
        const KeyBuf& key = kc.key;
        float vb_f = 1.f;
        if (path == 1 && state.depth_test_enable) {   // debug: the eye of the last depth-tested 3D draw (ge_draw_last_eye)
            const float* V = state.view_matrix;
            g_dbg_eye[0] = -(V[0] * V[9] + V[1] * V[10] + V[2] * V[11]); g_dbg_eye[1] = -(V[3] * V[9] + V[4] * V[10] + V[5] * V[11]); g_dbg_eye[2] = -(V[6] * V[9] + V[7] * V[10] + V[8] * V[11]);
            g_dbg_fwd[0] = -V[2]; g_dbg_fwd[1] = -V[5]; g_dbg_fwd[2] = -V[8];

        }
        if ((g_vb_active.load(std::memory_order_relaxed) || g_vb_linger.load(std::memory_order_relaxed)) && path == 1 && state.depth_test_enable && !state.depth_write_disable)
            vb_f = vb_fade_factor(rdram, state, n, prim_type);
        if (path == 1 && state.depth_test_enable && !state.depth_write_disable && g_cc_collect.load(std::memory_order_relaxed))
            cc_collect(rdram, state, n, prim_type);
        GeState vb_state; KeyBuf vb_key;
        const bool vb_on = vb_f < 0.999f;
        if (vb_on) {   // drawn blended with no depth writes (the game's own fade does the same), so the player behind it stays visible
            // out = src * f + dst * (1 - f) with constant factors: it does not depend on how the wall gets its alpha (vertex colour, material, texture)
            const uint32_t v8 = std::min<uint32_t>(255u, (uint32_t)std::lround(vb_f * 15.f) * 17u);   // 16 steps: few state variants
            vb_state = state; vb_state.alpha_blend_enable = true; vb_state.blend_mode = GE_SRCBLEND_FIXA | (GE_DSTBLEND_FIXB << 4); vb_state.depth_write_disable = true; vb_state.alpha_test_enable = false;
            vb_state.blend_fix_a = v8 * 0x010101u; vb_state.blend_fix_b = (255u - v8) * 0x010101u;
            build_key(vb_key, vb_state, mode, path, prim_type, fbo_tex, &xf);
            vb_key.id = key.id ^ 0x40000000u;
        }
        ensure_batch(vb_on ? vb_key : key, g_s3d, path == 2 ? g_s3d.vao : g_s3d.vao_plain, mode, (size_t)n,
                     rdram, vb_on ? vb_state : state, prim_type, fbo_tex, &xf);
        // PSPRECOMP_FEP_LOG (psp_fep_log.h): the size / position this draw really has in the world; the batch's
        // texture is bound now, so its pack id is this draw's.
        static const bool fep_log = psp_fep_log_on();
        if (fep_log) psp_fep_log_draw(rdram, state, prim_type, n, ge_texture_bound_pack_id());
        if (g_batch.refl > 0.f && !vb_on && path == 1) rml_capture(state, rdram, prim_type, n, g_batch.refl);   // reflectivity mask
        if (path == 2 && fighter_mask_wanted(state)) fighter_capture(state, rdram, prim_type, n);   // the fighter mask (no self-shadowing)
        {   // PSPRECOMP_FMASK_LOG=1: which non-stage 3D draws go into the fighter mask and which do not (vertex type, path, weights)
            static const bool fl = std::getenv("PSPRECOMP_FMASK_LOG") != nullptr;
            if (fl && state.vertex_type != 0x11au && !ge_vtype_through(state.vertex_type)) {
                static std::unordered_map<uint64_t, int> seen;
                const bool in = path == 2 && fighter_mask_wanted(state);
                const uint64_t k = (uint64_t(state.vertex_type) << 8) | (uint64_t(path) << 4) | (state.depth_write_disable ? 2u : 0u) | (in ? 1u : 0u) | (uint64_t(state.alpha_blend_enable) << 3);
                if (seen[k]++ == 0) std::fprintf(stderr, "[FMASK] vt %06x path %d weights %d zwrite %d ztest %d blend %d atest %d tex %08x n %d -> %s\n", (unsigned)state.vertex_type, path, ge_vtype_weight(state.vertex_type), state.depth_write_disable ? 0 : 1, state.depth_test_enable, state.alpha_blend_enable, state.alpha_test_enable, (unsigned)state.tex_addr[0], n, in ? "MASK" : "-");
            }
        }
        if (!g_verify && n >= 512 && gpu_vcache_on()) {
            // a big draw whose decoded vertices the decode cache holds (characters: the same every frame): drawn from a
            // resident buffer made once, instead of copying ~70 bytes a vertex into the frame's ring and uploading it
            uint64_t id = 0;
            const GeGpuVertex* cv;
            {
                psp_prof::Scope ps(psp_prof::S_DECODE);
                cv = ge_decode_vertices_gpu_cached(rdram, state, n, &id);
            }
            if (cv) {
                const GpuVBuf& vb = gpu_vcache_get(id, cv, (size_t)n);
                flush_batch();                                   // its own batch: the one before it is drawn first
                ensure_batch(vb_on ? vb_key : key, g_s3d, path == 2 ? vb.vao : vb.vao_plain, mode, 0,
                             rdram, vb_on ? vb_state : state, prim_type, fbo_tex, &xf);
                g_batch.persist_vao = path == 2 ? vb.vao : vb.vao_plain;
                g_batch.persist_n = (size_t)n;
                flush_batch();
                psp_prof::verts += (uint64_t)n;
                g_has_drawn_prims = true;
                return;
            }
        }
        uint8_t* dst = stage_reserve(g_s3d, (size_t)n);
        bool ok;
        {
            psp_prof::Scope ps(psp_prof::S_DECODE);
            ok = ge_decode_vertices_gpu(rdram, state, n, reinterpret_cast<GeGpuVertex*>(dst));
        }
        if (ok && g_verify && (mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN)) {
            static thread_local std::vector<DecodedVertex> ref;
            ge_decode_vertices(rdram, state, prim_type, n, ref);
            ge_transform_vertices(ref, state);
            auto ex = [&](const DecodedVertex& v) {
                return VerifyExpect{{v.pos[0], v.pos[1], v.pos[2], v.w}, {v.uv[0], v.uv[1]},
                                    {(float)v.color[0], (float)v.color[1], (float)v.color[2], (float)v.color[3]}};
            };
            g_expect.clear();
            if (ref.size() == (size_t)n) {
                // Transform feedback in GL_TRIANGLES mode captures strips/fans as
                // separate triangles (strip: odd triangles swap the first two).
                if (mode == GL_TRIANGLES) {
                    for (const auto& v : ref) g_expect.push_back(ex(v));
                } else if (mode == GL_TRIANGLE_STRIP) {
                    for (int t = 0; t + 2 < n; t++) {
                        const int a = (t & 1) ? t + 1 : t, b = (t & 1) ? t : t + 1;
                        g_expect.push_back(ex(ref[a])); g_expect.push_back(ex(ref[b])); g_expect.push_back(ex(ref[t + 2]));
                    }
                } else {
                    for (int t = 0; t + 2 < n; t++) {
                        g_expect.push_back(ex(ref[0])); g_expect.push_back(ex(ref[t + 1])); g_expect.push_back(ex(ref[t + 2]));
                    }
                }
            }
            g_expect_valid = !g_expect.empty();
            g_expect_lit = state.lighting_enable;
            g_expect_skin = xf.skin_n > 0;
        }
        if (ok) {
            g_batch.firsts.push_back(static_cast<GLint>(g_s3d.stage_n));
            g_batch.counts.push_back(n);
            g_s3d.stage_n += (size_t)n;
            psp_prof::verts += (uint64_t)n;
            g_has_drawn_prims = true;
            return;
        }
        gpu = false;   // unreadable guest memory: CPU path reproduces NULL-page semantics
    }

    // ---- CPU-transformed path ----
    static thread_local std::vector<DecodedVertex> decoded;
    static thread_local std::vector<PackedVertex> packed;
    {
        psp_prof::Scope ps(psp_prof::S_DECODE);
        ge_decode_vertices(rdram, state, prim_type, count, decoded);
    }
    if (decoded.empty()) return;
    {
        psp_prof::Scope ps(psp_prof::S_XFORM);
        tall_extend_strips(decoded, prim_type);
        ge_transform_vertices(decoded, state);
        rect_clip_to_ndc(decoded, state, prim_type);
    }
    packed.clear();
    {
        psp_prof::Scope ps(psp_prof::S_PACK);
        if (prim_type == GE_PRIM_RECTANGLES) {
            const int rect_count = static_cast<int>(decoded.size()) / 2;   // (tall_extend_strips may have added sprites)
            packed.reserve((size_t)rect_count * 6);
            for (int r = 0; r < rect_count; r++) {
                const int i0 = r * 2, i1 = r * 2 + 1;
                if (i1 >= static_cast<int>(decoded.size())) break;
                const auto& v0 = decoded[i0];
                const auto& v1 = decoded[i1];
                if (v0.w <= 0.0f || v1.w <= 0.0f) continue;
                PackedVertex c[4];
                c[0] = {{v0.pos[0], v0.pos[1], v0.pos[2], 1.0f}, {v0.uv[0], v0.uv[1]},
                        {v0.color[0], v0.color[1], v0.color[2], v0.color[3]}};
                c[1] = {{v1.pos[0], v0.pos[1], v0.pos[2], 1.0f}, {v1.uv[0], v0.uv[1]},
                        {v1.color[0], v1.color[1], v1.color[2], v1.color[3]}};
                c[2] = {{v1.pos[0], v1.pos[1], v1.pos[2], 1.0f}, {v1.uv[0], v1.uv[1]},
                        {v1.color[0], v1.color[1], v1.color[2], v1.color[3]}};
                c[3] = {{v0.pos[0], v1.pos[1], v1.pos[2], 1.0f}, {v0.uv[0], v1.uv[1]},
                        {v0.color[0], v0.color[1], v0.color[2], v0.color[3]}};
                packed.push_back(c[0]); packed.push_back(c[1]); packed.push_back(c[2]);
                packed.push_back(c[0]); packed.push_back(c[2]); packed.push_back(c[3]);
            }
        } else {
            size_t m = decoded.size();
            if (prim_type == GE_PRIM_TRIANGLES) m -= m % 3;
            else if (prim_type == GE_PRIM_LINES) m -= (m & 1);
            packed.resize(m);
            for (size_t i = 0; i < m; i++) {
                const auto& v = decoded[i];
                packed[i] = {{v.pos[0], v.pos[1], v.pos[2], v.w}, {v.uv[0], v.uv[1]},
                             {v.color[0], v.color[1], v.color[2], v.color[3]}};
            }
        }
    }
    if (packed.empty()) return;
    const size_t pn = packed.size();
    if (!kc.has2d) {
        kc.key2d.n = 0;
        build_key(kc.key2d, state, mode, 0, prim_type, fbo_tex, nullptr);
        kc.key2d.id = ++key_serial;
        kc.has2d = true;
    }
    const KeyBuf& key = kc.key2d;
    ensure_batch(key, g_s2d, g_s2d.vao, mode, pn, rdram, state, prim_type, fbo_tex, nullptr);
    std::memcpy(stage_reserve(g_s2d, pn), packed.data(), pn * sizeof(PackedVertex));
    g_batch.firsts.push_back(static_cast<GLint>(g_s2d.stage_n));
    g_batch.counts.push_back(static_cast<GLsizei>(pn));
    g_s2d.stage_n += pn;
    psp_prof::verts += (uint64_t)pn;
    g_has_drawn_prims = true;
}


// PSPRECOMP_GE_SELFTEST=<iterations>: synthetic draws with random vertex
// formats (weights / normals / colours / texcoords / indices), random bones,
// matrices, materials and lights, run through the GPU path and compared with
// the CPU reference. Covers lighting + skinning variants the game's own
// draws never use (it has no normals).
static void verify_selftest(int iters) {
    uint8_t* ram = static_cast<uint8_t*>(psp_plat::alloc_zeroed(PSP_MEM_SIZE));
    if (!ram) { std::fprintf(stderr, "[SELFTEST] mmap failed\\n"); return; }
    std::mt19937 rng(0xC0FFEE);
    auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto I = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    const uint32_t vaddr = 0x08100000u, iaddr = 0x08300000u;
    const bool saved_nobatch = g_nobatch;
    g_nobatch = true;
    int ran = 0;
    for (int it = 0; it < iters; it++) {
        GeState st;
        st.reset();
        static const int cols[5] = {0, 4, 5, 6, 7};
        uint32_t vt = (uint32_t)I(0, 3)                      // tc
                    | ((uint32_t)cols[I(0, 4)] << 2)         // colour
                    | ((uint32_t)I(0, 3) << 5)               // normal
                    | ((uint32_t)I(1, 3) << 7)               // position
                    | ((uint32_t)I(0, 3) << 9)               // weights
                    | ((uint32_t)I(0, 3) << 11)              // index
                    | ((uint32_t)I(0, 7) << 14);             // weight count - 1
        st.vertex_type = vt;
        st.vertex_addr = vaddr;
        st.index_addr = iaddr;
        auto M = [&](float* m, int n, float lo, float hi) { for (int i = 0; i < n; i++) m[i] = U(lo, hi); };
        M(st.world_matrix, 12, -1.f, 1.f); M(st.view_matrix, 12, -1.f, 1.f);
        st.world_matrix[9] = U(-3, 3); st.world_matrix[10] = U(-3, 3); st.world_matrix[11] = U(-3, 3);
        st.view_matrix[9] = U(-3, 3); st.view_matrix[10] = U(-3, 3); st.view_matrix[11] = U(-3, 3);
        M(st.proj_matrix, 16, -0.05f, 0.05f);
        st.proj_matrix[0] = U(0.8f, 2.f); st.proj_matrix[5] = U(0.8f, 2.f);
        st.proj_matrix[10] = -1.1f; st.proj_matrix[11] = -1.f; st.proj_matrix[14] = -0.5f; st.proj_matrix[15] = 0.f;
        M(st.bone_matrix, 96, -1.f, 1.f);
        st.tex_scale_u = U(0.2f, 3.f); st.tex_scale_v = U(0.2f, 3.f);
        st.tex_offset_u = U(-1.f, 1.f); st.tex_offset_v = U(-1.f, 1.f);
        st.lighting_enable = I(0, 4) != 0;
        st.ambient_color = (uint32_t)I(0, 0xFFFFFF); st.ambient_alpha = (uint32_t)I(0, 255);
        st.material_emissive = (uint32_t)I(0, 0xFFFFFF); st.material_ambient = (uint32_t)I(0, 0xFFFFFF);
        st.material_diffuse = (uint32_t)I(0, 0xFFFFFF); st.material_alpha = (uint32_t)I(0, 255);
        st.material_update = (uint32_t)I(0, 7);
        for (int l = 0; l < 4; l++) {
            st.light_enable[l] = I(0, 3) != 0;
            st.light_type[l] = (uint32_t)(I(0, 2) << 8) | (uint32_t)I(0, 3);
            for (int k = 0; k < 3; k++) {
                st.light_pos[l][k] = U(-5.f, 5.f); st.light_dir[l][k] = U(-1.f, 1.f);
            }
            st.light_att[l][0] = U(0.3f, 2.f); st.light_att[l][1] = U(0.f, 0.5f); st.light_att[l][2] = U(0.f, 0.1f);
            st.light_spot_exp[l] = U(0.f, 8.f); st.light_spot_cut[l] = U(-1.f, 1.f);
            st.light_amb[l] = (uint32_t)I(0, 0xFFFFFF); st.light_dif[l] = (uint32_t)I(0, 0xFFFFFF);
        }
        st.viewport_x_scale = 240.f; st.viewport_y_scale = -136.f; st.viewport_z_scale = -32767.5f;
        st.viewport_x_center = 2048.f; st.viewport_y_center = 2048.f; st.viewport_z_center = 32767.5f;
        st.offset_x = 0x8000; st.offset_y = 0x8000;
        if (!ge_gpu_transform_ok(st)) continue;

        const int stride = ge_vertex_stride(vt);
        const int idx_type = ge_vtype_idx(vt);
        const int pool = I(3, 40);
        float* vf = reinterpret_cast<float*>(ram + (vaddr & PSP_ADDR_MASK));
        for (size_t i = 0; i < (size_t)pool * stride / 4 + 8; i++) vf[i] = U(-2.f, 2.f);
        const int prim_pick = I(0, 2);
        const int prim = prim_pick == 0 ? GE_PRIM_TRIANGLES : prim_pick == 1 ? GE_PRIM_TRIANGLE_STRIP : GE_PRIM_TRIANGLE_FAN;
        int n = prim == GE_PRIM_TRIANGLES ? 3 * I(1, 12) : I(3, 30);
        if (idx_type == GE_VTYPE_IDX_NONE) n = std::min(n, pool);
        if (idx_type != GE_VTYPE_IDX_NONE) {
            uint8_t* ip = ram + (iaddr & PSP_ADDR_MASK);
            for (int i = 0; i < n; i++) {
                const uint32_t v = (uint32_t)I(0, pool - 1);
                if (idx_type == GE_VTYPE_IDX_8BIT) ip[i] = (uint8_t)v;
                else if (idx_type == GE_VTYPE_IDX_16BIT) std::memcpy(ip + i * 2, &v, 2);
                else std::memcpy(ip + i * 4, &v, 4);
            }
        }
        if (n < 3) continue;
        fast_prim(ram, st, prim, n, 0);
        flush_batch();
        ran++;
    }
    g_nobatch = saved_nobatch;
    glc::invalidate();
    ge_shader_invalidate_cache();
    ge_texture_invalidate_bind_cache();
    std::fprintf(stderr, "[SELFTEST] %d synthetic draws run\\n", ran);
    verify_report("selftest");
    psp_plat::free_pages(ram, PSP_MEM_SIZE);
}

// ---- Public API ----

void ge_draw_init() {
    // Check screenshot env var
    const char* ss_env =
        std::getenv("PSPRECOMP_SCREENSHOT");
    if (ss_env && ss_env[0]) {
        g_screenshot_enabled = true;
        std::strncpy(g_screenshot_path, ss_env,
                     sizeof(g_screenshot_path) - 1);
    }

    // Default render scale: 2x (960x544). PSPRECOMP_RENDER_SCALE (or the saved menu value) overrides it.
    const char* rs = std::getenv("PSPRECOMP_RENDER_SCALE");
    if (!rs || !rs[0]) rs = "2";
    {
        int v = std::atoi(rs);
        if (!std::strcmp(rs, "auto")) {
            // PSPRECOMP_RENDER_SCALE=auto: nearest whole multiple of the
            // drawable height / 272, clamped to 1..3 (1280x800 Steam Deck
            // panel -> 3 = 1440x816; a 960x544 window -> 2). Capped at 3 so
            // a Retina/4K window does not silently allocate 8 huge targets.
            int dw = 0, dh = 0;
            if (SDL_Window* w = psp_get_sdl_window()) psp_gl_drawable_size(w, &dw, &dh);
            v = dh > 0 ? (int)((dh + 136) / 272) : 1;
            v = v < 1 ? 1 : v > 3 ? 3 : v;
            std::fprintf(stderr, "[DRAW] render scale auto: drawable %dx%d -> %d\n", dw, dh, v);
        }
        g_rs = v < 1 ? 1 : v > 16 ? 16 : v;
        GLint maxtex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
        while (g_rs > 1 && (480 * g_rs > maxtex || 272 * g_rs > maxtex)) g_rs--;
    }
    if (g_rs > 1) {
        // Keep 1-pixel PSP lines/points the same visual size (best effort:
        // core profiles may clamp wide lines to 1).
        glLineWidth(float(g_rs));
        glPointSize(float(g_rs));
        while (glGetError() != GL_NO_ERROR) {}
    }
    std::fprintf(stderr, "[DRAW] render scale %dx (targets %dx%d)\n", g_rs, 480 * g_rs, 272 * g_rs);

    // Liveness-present budget (no-flip safety net). Env-overridable.
    const char* stale_env = std::getenv("PSPRECOMP_PRESENT_STALE_MS");
    if (stale_env && stale_env[0]) {
        int v = std::atoi(stale_env);
        if (v > 0) g_present_stale_ms = v;
    }
    g_last_present = std::chrono::steady_clock::now();

    if (const char* nr = std::getenv("PSPRECOMP_NO_RASTER")) {
        g_no_raster_mode = std::atoi(nr);
        if (const char* af = std::getenv("PSPRECOMP_NO_RASTER_AFTER")) g_no_raster_after = std::atof(af);
        if (g_no_raster_after <= 0.0) {
            g_no_raster = g_no_raster_mode;
            if (g_no_raster == 1) glEnable(GL_RASTERIZER_DISCARD);
        }
        std::fprintf(stderr, "[DRAW] PSPRECOMP_NO_RASTER=%d after %.0fs\n", g_no_raster_mode, g_no_raster_after);
    }
    // FBO targets are created lazily per guest framebuffer address by
    // ge_draw_select_target. No target is pre-created here.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Create VAO + VBO
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    // Vertex attributes for PackedVertex layout
    // location 0: position (3 floats, offset 0)
    glVertexAttribPointer(
        0, 4, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // location 1: texcoord (2 floats, offset 16)
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(16));
    glEnableVertexAttribArray(1);

    // location 2: color (4 bytes normalized, offset 24)
    glVertexAttribPointer(
        2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(PackedVertex),
        reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    // Init shader
    ge_shader_init();

    // Batched fast path (see fast_prim): ON by default on every platform.
    // PSPRECOMP_GE_FAST=0 (or the older PSPRECOMP_GE_LEGACY=1) selects the
    // legacy per-PRIM path with CPU skinning. The fast path needs only GL 3.3
    // core (no persistent mapping, SSBOs, compute or GLSL > 330), so macOS's
    // 4.1 core context is supported; see DEBUGGING.md "GE fast path" for the
    // fast-vs-legacy A/B tool (PSPRECOMP_GE_AB) and GL error polling
    // (PSPRECOMP_GL_STATS=2) to run on a new driver.
    {
        const char* fe = std::getenv("PSPRECOMP_GE_FAST");
        const bool fast_off = fe && (fe[0] == '0' || fe[0] == 'n' || fe[0] == 'N' || fe[0] == 'f' || fe[0] == 'F');
        g_fast_enabled = !fast_off && !std::getenv("PSPRECOMP_GE_LEGACY");
    }
    g_ab = std::getenv("PSPRECOMP_GE_AB") != nullptr;
    if (g_ab) std::fprintf(stderr, "[AB] fast-vs-legacy dual rendering on (compare logged per present)\n");
    g_cpu_xform = std::getenv("PSPRECOMP_GE_CPU3D") != nullptr;
    if (const char* ph = std::getenv("PSPRECOMP_PERF_HUD")) { g_perf_hud = ph[0] && ph[0] != '0'; perf_overlay_apply(g_perf_hud.load()); }
    if (const char* fc = std::getenv("PSPRECOMP_FPS_COUNTER")) g_fps_counter = fc[0] && fc[0] != '0';
    if (const char* po = std::getenv("PSPRECOMP_PING_OVERLAY")) g_ping_overlay = po[0] && po[0] != '0';
    g_nobatch = std::getenv("PSPRECOMP_GE_NOBATCH") != nullptr;
    if (const char* sm = std::getenv("PSPRECOMP_GE_SYNC")) {
        g_sync_mode = !std::strcmp(sm, "flush") ? 4 : std::atoi(sm);
        if (g_sync_mode <= 0) g_sync_mode = 1;
        g_sync_each = g_sync_mode == 1;
        std::fprintf(stderr, "[DRAW] GE sync mode %d (%s)\n", g_sync_mode,
                     g_sync_mode == 1 ? "glFinish per batch" : g_sync_mode == 2 ? "glFinish per list"
                     : g_sync_mode == 3 ? "glFinish per present" : "glFlush per batch");
    }
    if (const char* rm = std::getenv("PSPRECOMP_GE_RING")) {
        if (!std::strcmp(rm, "map")) g_ring_mode = 1;
        else if (!std::strcmp(rm, "sub")) g_ring_mode = 0;

    }
    g_verify = std::getenv("PSPRECOMP_GE_VERIFY") != nullptr;
    if (g_verify) {
        g_nobatch = true;   // one PRIM per draw so each capture maps to one expectation
        glGenBuffers(1, &g_tf_buf);
        glBindBuffer(GL_COPY_WRITE_BUFFER, g_tf_buf);
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(70000 * 10 * sizeof(float)), nullptr, GL_STREAM_READ);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        std::fprintf(stderr, "[VERIFY] GPU vertex stage verification on (one PRIM per draw)\n");
    }

    g_debug_env = std::getenv("PSPRECOMP_DRAW_TRACE") || std::getenv("PSPRECOMP_3D_DIAG")
               || std::getenv("PSPRECOMP_3D_DIAG_BIG");
    setup_stream(g_s2d, sizeof(PackedVertex), false);
    setup_stream(g_s3d, sizeof(GeGpuVertex), true);
    glc::invalidate();
    ge_shader_invalidate_cache();
    std::fprintf(stderr, "[DRAW] fast path %s%s%s\n", g_fast_enabled ? "on" : "OFF (legacy)",
                 g_cpu_xform ? ", CPU 3D transform" : ", GPU 3D transform",
                 g_debug_env ? ", debug env -> legacy draws" : "");
    if (const char* stn = std::getenv("PSPRECOMP_GE_SELFTEST")) {
        if (!g_verify) {
            g_verify = true; g_nobatch = true;
            glGenBuffers(1, &g_tf_buf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g_tf_buf);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)(70000 * 10 * sizeof(float)), nullptr, GL_STREAM_READ);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }
        verify_selftest(std::max(1, std::atoi(stn)));
    }

    g_frame_counter = 0;
    g_draw_ready.store(true, std::memory_order_release);

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure initialized\n");
}

void ge_draw_shutdown() {
    g_draw_ready.store(false, std::memory_order_release);
    ge_draw_flush();
    // Auto-capture final frame if any PRIMs were rendered and no screenshot taken yet
    if (g_has_drawn_prims && !g_screenshot_done) {
        std::vector<uint8_t> flipped;
        int cw = 480, chh = 272;
        read_fbo_native(front_fbo_id(), flipped, &cw, &chh);
        const char* out_path = (g_screenshot_path[0] != '\0')
            ? g_screenshot_path : "frame.tga";
        write_tga(out_path, flipped.data(), cw, chh);
        g_screenshot_done = true;
        std::fprintf(stderr, "[DRAW] Auto-capture: %s\n", out_path);
    }

    flush_batch();
    if (g_verify) verify_report("final");
    teardown_stream(g_s2d);
    teardown_stream(g_s3d);
    psp_fx_shutdown();
    if (g_fx_screen_tex) { glDeleteTextures(1, &g_fx_screen_tex); g_fx_screen_tex = 0; }
    if (g_fx_screen_fbo) { glDeleteFramebuffers(1, &g_fx_screen_fbo); g_fx_screen_fbo = 0; }
    ge_shader_shutdown();

    if (g_vbo) {
        glDeleteBuffers(1, &g_vbo);
        g_vbo = 0;
    }
    if (g_vao) {
        glDeleteVertexArrays(1, &g_vao);
        g_vao = 0;
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (t.depth_rb) glDeleteRenderbuffers(1, &t.depth_rb);
        if (t.color_tex) glDeleteTextures(1, &t.color_tex);
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        fxl_free(g_fxl[i]);
        t = GeTarget{};
    }
    g_current_target = -1;
    g_front_target = -1;

    std::fprintf(stderr,
        "[DRAW] Draw infrastructure shutdown\n");
}

void ge_draw_begin_list() {
    // Bind the FBO for the list's current render-target address (FRAMEBUFPTR).
    // Geometry and clears in this list now target the per-address FBO, so a
    // clear to address X cannot wipe geometry on address Y (issue #59).
    ge_draw_select_target(ge_get_state().framebuf_ptr);
    if (int a = g_dyn_armed.exchange(0)) g_dyn_trace_n.store(a);
    if (g_dyn_trace_n.load() > 0)
        std::fprintf(stderr, "[DT] ---- BEGIN_LIST fb=0x%08X\n", ge_get_state().framebuf_ptr);
    // Full-buffer default; the precise PSP viewport (scale/offset + depth
    // range) is applied per-PRIM in ge_draw_prim from the live registers,
    // which may be set after begin_list (issue #23).
    gl_viewport(0, 0, cur_w480() * cur_rs(), cur_h272() * cur_rs());
    g_frame_counter++;
}

void ge_draw_end_list() {
    ge_draw_flush();
    // Mark the front buffer dirty so the liveness net can publish content
    // even for a title that draws but never calls sceDisplaySetFrameBuf.
    ge_draw_mark_dirty();
}

static void ge_draw_prim_one(uint8_t* rdram, int prim_type, int count);

// ---- Frame interpolation (ge_interp_set_frame, psp_ge_draw.h) ----
namespace {
static void ip_note_vp_tag();
struct IpMats { float w[12], v[12], p[16], b[96], tg[12], uv[4]; };   // uv: texture scale u/v, offset u/v (scrolling textures)
std::mutex g_ip_set_mu;
GeInterpTag g_ip_set;                  // the tag the game thread set for the frame it is building
uint32_t g_ip_gen = 0;                 // tag of the list being processed (render thread)
float g_ip_phase = -1.0f;
uint32_t g_ip_seq = 0;
bool g_ip_rec = false;
uint32_t g_ip_tab_gen = 0;             // gameplay frame recorded in g_ip_cur
bool g_ip_have = false;
std::unordered_map<uint64_t, IpMats> g_ip_prev, g_ip_cur;
std::unordered_map<uint64_t, uint32_t> g_ip_occ;   // occurrences of each draw key in the frame being drawn
uint32_t g_ip_occ_seq = ~0u;
int ip_mode() {
    static const int m = [] { const char* e = std::getenv("PSPRECOMP_INTERP"); return e && e[0] ? std::atoi(e) : 1; }();
    return m;
}
uint64_t ip_key(const GeState& st, int prim_type, int count, bool loose = false) {
    uint64_t h = loose ? 0x5bd1e9955bd1e995ull : 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    if (!loose) { mix(st.vertex_addr); mix(st.index_addr); }   // loose: without the addresses (the game double-buffers some vertex data, e.g. weapons)
    mix(st.vertex_type); mix(static_cast<uint64_t>(prim_type)); mix(static_cast<uint64_t>(count));
    mix(st.tex_addr[0]);   // not the render target: the game alternates two framebuffers every frame
    return h;
}
static void ip_note_vp_tag() { uint32_t pb; std::memcpy(&pb, &g_ip_phase, 4); g_last_vp_tag.store((uint64_t(g_ip_gen) << 32) | pb); }   // ge_draw_last_view_proj_tag
void ip_lerp(float* dst, const float* a, const float* b, float t, int n) { for (int i = 0; i < n; i++) dst[i] = a[i] + (b[i] - a[i]) * t; }
bool ip_far(const float* a, const float* b) {   // world positions more than 3 m apart: a teleport, do not blend
    const float dx = a[9] - b[9], dy = a[10] - b[10], dz = a[11] - b[11];
    return dx * dx + dy * dy + dz * dz > 9.0f;
}
void ip_eye(const float* m, float* e) {   // camera position of a GE view matrix (rows 0..2 the basis, row 3 = m[9..11] the translation): e = -t R^T
    for (int j = 0; j < 3; j++) e[j] = -(m[9] * m[j * 3 + 0] + m[10] * m[j * 3 + 1] + m[11] * m[j * 3 + 2]);
}
bool ip_cut(const float* a, const float* b) {   // the camera moved more than 3 m between two gameplay frames: a cut, do not blend
    float ea[3], eb[3]; ip_eye(a, ea); ip_eye(b, eb);
    const float dx = ea[0] - eb[0], dy = ea[1] - eb[1], dz = ea[2] - eb[2];
    return dx * dx + dy * dy + dz * dz > 9.0f;
}
}  // namespace

void ge_interp_set_frame(const GeInterpTag& tag) { std::lock_guard<std::mutex> lk(g_ip_set_mu); g_ip_set = tag; }
GeInterpTag ge_interp_current_tag() { std::lock_guard<std::mutex> lk(g_ip_set_mu); return g_ip_set; }
static int ip_trace_level() {   // PSPRECOMP_INTERP_TRACE: 1 = per frame the drawn (blended) position of the biggest skinned draw and the camera; 2 = also the lists' tags and one stage draw's live / previous / blended view
    static const int l = [] { const char* e = std::getenv("PSPRECOMP_INTERP_TRACE"); return e && e[0] ? std::max(1, std::atoi(e)) : 0; }();
    return l;
}
void ge_interp_list_tag(const GeInterpTag& tag) {
    if (ip_trace_level() >= 2) std::fprintf(stderr, "[IPL] list tag seq %u gen %u rec %d phase %.3f\n", tag.seq, tag.gen, tag.record ? 1 : 0, tag.phase);
    g_ip_gen = tag.gen; g_ip_phase = tag.phase; g_ip_seq = tag.seq; g_ip_rec = tag.record;
}

// Swap the draw's matrices for interpolated ones (restored by the caller); false = drawn as issued.
struct IpStat { float pw[3] = {}, eye[3] = {}; int pcount = -1; uint32_t gen = 0, seq = ~0u; float phase = -2.f; int draws = 0, applied = 0, nomatch = 0, cut_n = 0, noprev = 0, flat = 0, skinned = 0, matmove = 0, vtxonly = 0, vtxprims = 0;
    int scount = -1; float s_live[3] = {}, s_prev[3] = {}, s_blend[3] = {}; uint32_t s_key = 0; int s_how = 0; bool s_rec = false; };   // trace 2: the biggest unskinned 3D draw (stage): eye of its live / previous / blended view
std::unordered_map<uint64_t, uint64_t> g_ip_vhash_prev, g_ip_vhash_cur;   // diagnostic (PSPRECOMP_INTERP_LOG): vertex bytes per draw key on gameplay frames
uint8_t* g_ip_rdram = nullptr;
// 2D draws that follow a fighter (lock-on ring, target pointer): the game aims them at the newest gameplay state, the 3D scene is drawn
// blended. Per frame, every skinned draw's root on screen (PSP pixels) with the newest and with the blended matrices; a following 2D draw
// moves by the difference of the nearest one.
struct IpAnchor { float live[2], drawn[2]; };
std::vector<IpAnchor> g_ip_anchors;
uint32_t g_ip_anchor_seq = ~0u;
bool g_ip2d_on = false;   // the draw being decoded is such a draw (set by ip_apply, cleared after the draw)
bool ip_screen(const float* w, const float* v, const float* p, const float* b, float* out) {   // root (bone 0 origin) -> PSP screen pixels
    float o[3] = {b[9], b[10], b[11]}, wp[3], vp[3], c[4];
    for (int j = 0; j < 3; j++) wp[j] = o[0] * w[j] + o[1] * w[3 + j] + o[2] * w[6 + j] + w[9 + j];
    for (int j = 0; j < 3; j++) vp[j] = wp[0] * v[j] + wp[1] * v[3 + j] + wp[2] * v[6 + j] + v[9 + j];
    for (int j = 0; j < 4; j++) c[j] = vp[0] * p[j] + vp[1] * p[4 + j] + vp[2] * p[8 + j] + p[12 + j];
    if (c[3] <= 0.01f) return false;
    out[0] = (c[0] / c[3] + 1.0f) * 240.0f; out[1] = (1.0f - c[1] / c[3]) * 136.0f;
    return true;
}
static IpStat g_ip_stat;
static void ip_stat_flush() {
    static int left = [] { const char* e = std::getenv("PSPRECOMP_INTERP_LOG"); return e ? std::atoi(e) : 0; }();
    if (left > 0 && g_ip_stat.draws > 0) { left--; std::fprintf(stderr, "[INTERP] gen %u phase %.2f: %d 3D draws, %d blended, %d no match, %d far, %d no previous frame, %d 2D; %d skinned, %d moved by matrix, %d moved by vertices only (%d prims)\n",
        g_ip_stat.gen, g_ip_stat.phase, g_ip_stat.draws, g_ip_stat.applied, g_ip_stat.nomatch, g_ip_stat.cut_n, g_ip_stat.noprev, g_ip_stat.flat, g_ip_stat.skinned, g_ip_stat.matmove, g_ip_stat.vtxonly, g_ip_stat.vtxprims); }
    static const bool trace = std::getenv("PSPRECOMP_INTERP_TRACE") != nullptr;   // per frame: where the biggest skinned draw (a fighter) and the camera are drawn
    if (trace && g_ip_stat.pcount >= 0) std::fprintf(stderr, "[IPTRACE] seq %u gen %u phase %.3f rec %d model %.4f %.4f %.4f eye %.4f %.4f %.4f\n", g_ip_stat.seq, g_ip_stat.gen, g_ip_stat.phase,
        0, g_ip_stat.pw[0], g_ip_stat.pw[1], g_ip_stat.pw[2], g_ip_stat.eye[0], g_ip_stat.eye[1], g_ip_stat.eye[2]);
    if (ip_trace_level() >= 2 && g_ip_stat.scount >= 0) std::fprintf(stderr, "[IPSTAGE] seq %u gen %u phase %.3f rec %d key %08X n %d how %d live %.4f %.4f %.4f prev %.4f %.4f %.4f blend %.4f %.4f %.4f; %d draws %d blended %d nomatch\n",
        g_ip_stat.seq, g_ip_stat.gen, g_ip_stat.phase, g_ip_stat.s_rec ? 1 : 0, g_ip_stat.s_key, g_ip_stat.scount, g_ip_stat.s_how, g_ip_stat.s_live[0], g_ip_stat.s_live[1], g_ip_stat.s_live[2],
        g_ip_stat.s_prev[0], g_ip_stat.s_prev[1], g_ip_stat.s_prev[2], g_ip_stat.s_blend[0], g_ip_stat.s_blend[1], g_ip_stat.s_blend[2], g_ip_stat.draws, g_ip_stat.applied, g_ip_stat.nomatch);
    g_ip_stat = IpStat{}; g_ip_stat.gen = g_ip_gen; g_ip_stat.phase = g_ip_phase; g_ip_stat.seq = g_ip_seq;
}
static bool ip_apply(int prim_type, int count, IpMats* saved) {
    if (g_ip_phase <= -1.0f || ip_mode() == 0) return false;   // -1 = off; a gameplay frame shown a bit early has a small negative phase
    if (g_ip_stat.seq != g_ip_seq) ip_stat_flush();
    GeState& st = ge_get_state_mut();
    if (st.clear_mode) { g_ip_stat.flat++; return false; }
    if (g_ip_rec && (!g_ip_have || g_ip_gen != g_ip_tab_gen)) {   // a new gameplay frame: its draws become the current table
        g_ip_prev.swap(g_ip_cur); g_ip_cur.clear();
        g_ip_have = g_ip_tab_gen + 1 == g_ip_gen;                       // the previous table is the gameplay frame just before
        g_ip_tab_gen = g_ip_gen;
    }
    if (g_ip_seq != g_ip_occ_seq) { g_ip_occ.clear(); g_ip_occ_seq = g_ip_seq; }
    if (ge_vtype_through(st.vertex_type)) {   // 2D (HUD, screen copies): no matrices. The HUD that follows a fighter (lock-on ring, target
        g_ip_stat.flat++;                     // pointer: hud_fixed_tex) is placed by the game on gameplay frames only: its screen position is
        if (hud_fixed_tex(st)) g_ip2d_on = true;   // moved with the blended fighter it is on (ge_draw_through_ip, called by the vertex decoder)
        return false;
    }
    g_ip_stat.draws++;
    const uint64_t base = ip_key(st, prim_type, count), lbase = ip_key(st, prim_type, count, true);
    const uint64_t key = base ^ (static_cast<uint64_t>(g_ip_occ[base]++) * 0x9E3779B97F4A7C15ull);
    const uint64_t lkey = lbase ^ (static_cast<uint64_t>(g_ip_occ[lbase]++) * 0x9E3779B97F4A7C15ull);
    IpMats cur;
    std::memcpy(cur.w, st.world_matrix, sizeof cur.w); std::memcpy(cur.v, st.view_matrix, sizeof cur.v);
    std::memcpy(cur.p, st.proj_matrix, sizeof cur.p); std::memcpy(cur.b, st.bone_matrix, sizeof cur.b);
    std::memcpy(cur.tg, st.tgen_matrix, sizeof cur.tg);
    cur.uv[0] = st.tex_scale_u; cur.uv[1] = st.tex_scale_v; cur.uv[2] = st.tex_offset_u; cur.uv[3] = st.tex_offset_v;
    if (g_ip_rec && g_ip_gen == g_ip_tab_gen) { g_ip_cur[key] = cur; g_ip_cur[lkey] = cur; }
    static const bool diag = std::getenv("PSPRECOMP_INTERP_LOG") != nullptr;
    if (diag && g_ip_rec && g_ip_rdram) {   // what moves this draw between gameplay frames: its matrices or its vertex bytes
        if (st.vertex_type & 0x1C000u) g_ip_stat.skinned++;   // weight count bits: GE skinning
        uint64_t vh = 1469598103934665603ull; const uint32_t va = st.vertex_addr & 0x07FFFFFFu;
        for (uint32_t i = 0; i < 256 && va + i < 0x08000000u; i += 4) { uint32_t w; std::memcpy(&w, g_ip_rdram + va + i, 4); vh = (vh ^ w) * 1099511628211ull; }
        if (g_ip_gen == g_ip_tab_gen) { if (g_ip_vhash_cur.empty() || g_ip_vhash_cur.count(~0ull) == 0 || g_ip_vhash_cur[~0ull] != g_ip_gen) { g_ip_vhash_prev.swap(g_ip_vhash_cur); g_ip_vhash_cur.clear(); g_ip_vhash_cur[~0ull] = g_ip_gen; } g_ip_vhash_cur[key] = vh; }
        auto pm = g_ip_prev.find(key); auto ph = g_ip_vhash_prev.find(key);
        if (pm != g_ip_prev.end() && ph != g_ip_vhash_prev.end()) {
            const bool mat = std::memcmp(pm->second.w, cur.w, sizeof cur.w) || std::memcmp(pm->second.v, cur.v, sizeof cur.v) || std::memcmp(pm->second.b, cur.b, sizeof cur.b);
            if (mat) g_ip_stat.matmove++; else if (ph->second != vh) { g_ip_stat.vtxonly++; g_ip_stat.vtxprims += count; }
        }
    }
    const bool stage_tr = ip_trace_level() >= 2 && !(st.vertex_type & 0x1C000u) && !st.depth_write_disable && st.depth_test_enable && count > g_ip_stat.scount;
    if (stage_tr) {   // trace 2: this (so far biggest unskinned, depth-writing) draw's live view, drawn as issued unless replaced below
        g_ip_stat.scount = count; g_ip_stat.s_key = static_cast<uint32_t>(base); g_ip_stat.s_how = 0; g_ip_stat.s_rec = g_ip_rec;
        ip_eye(cur.v, g_ip_stat.s_live); ip_eye(cur.v, g_ip_stat.s_blend); std::memset(g_ip_stat.s_prev, 0, sizeof g_ip_stat.s_prev);
    }
    if (!g_ip_have) { g_ip_stat.noprev++; return false; }
    auto it = g_ip_prev.find(key);
    if (it == g_ip_prev.end()) it = g_ip_prev.find(lkey);   // the same draw from the other of two vertex buffers
    if (it == g_ip_prev.end()) { g_ip_stat.nomatch++; return false; }
    const IpMats& pv = it->second;
    if (stage_tr) { g_ip_stat.s_how = it->first == key ? 1 : 2; ip_eye(pv.v, g_ip_stat.s_prev); }
    if (ip_far(pv.w, cur.w) || ip_cut(pv.v, cur.v)) {
        static int lg = [] { const char* e = std::getenv("PSPRECOMP_INTERP_LOG"); return e ? 12 : 0; }();
        if (lg > 0) { lg--; float ea[3], eb[3]; ip_eye(pv.v, ea); ip_eye(cur.v, eb);
            std::fprintf(stderr, "[INTERPFAR] phase %.2f w %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f  v %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f  eye %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f\n", g_ip_phase,
                pv.w[9], pv.w[10], pv.w[11], cur.w[9], cur.w[10], cur.w[11], pv.v[9], pv.v[10], pv.v[11], cur.v[9], cur.v[10], cur.v[11], ea[0], ea[1], ea[2], eb[0], eb[1], eb[2]); }
        g_ip_stat.cut_n++; return false; }
    g_ip_stat.applied++;
    *saved = cur;
    if (st.vertex_type & 0x1C000u) {   // a skinned draw (fighter): its root on screen, newest and blended (for following 2D draws)
        if (g_ip_anchor_seq != g_ip_seq) { g_ip_anchors.clear(); g_ip_anchor_seq = g_ip_seq; }
        float bw[12], bv[12], bp[16], bb[12];
        ip_lerp(bw, pv.w, cur.w, ip_mode() == 2 ? 1.0f + g_ip_phase : g_ip_phase, 12); ip_lerp(bv, pv.v, cur.v, ip_mode() == 2 ? 1.0f + g_ip_phase : g_ip_phase, 12);
        ip_lerp(bp, pv.p, cur.p, ip_mode() == 2 ? 1.0f + g_ip_phase : g_ip_phase, 16); ip_lerp(bb, pv.b, cur.b, ip_mode() == 2 ? 1.0f + g_ip_phase : g_ip_phase, 12);
        IpAnchor a;
        if (g_ip_anchors.size() < 64 && ip_screen(cur.w, cur.v, cur.p, cur.b, a.live) && ip_screen(bw, bv, bp, bb, a.drawn)) g_ip_anchors.push_back(a);
    }
    g_ge_state_gen++;   // the draw-key cache (keyed by this) must not reuse matrices built for another draw: the blend changes them without a GE command
    const float t = ip_mode() == 2 ? 1.0f + g_ip_phase : g_ip_phase;   // 1: prev -> cur (one frame behind); 2: cur projected ahead
    ip_lerp(st.world_matrix, pv.w, cur.w, t, 12); ip_lerp(st.view_matrix, pv.v, cur.v, t, 12);
    ip_lerp(st.proj_matrix, pv.p, cur.p, t, 16); ip_lerp(st.bone_matrix, pv.b, cur.b, t, 96);
    ip_lerp(st.tgen_matrix, pv.tg, cur.tg, t, 12);
    float uv[4]; ip_lerp(uv, pv.uv, cur.uv, t, 4);
    for (int i = 0; i < 4; i++) if (std::fabs(cur.uv[i] - pv.uv[i]) > 0.5f) uv[i] = cur.uv[i];   // a scrolling offset that wrapped: no blend
    st.tex_scale_u = uv[0]; st.tex_scale_v = uv[1]; st.tex_offset_u = uv[2]; st.tex_offset_v = uv[3];
    if (stage_tr) ip_eye(st.view_matrix, g_ip_stat.s_blend);
    if ((st.vertex_type & 0x1C000u) && count > g_ip_stat.pcount) {   // trace: the drawn (blended) position of the biggest skinned draw = world x bone 0 translation
        g_ip_stat.pcount = count;
        const float* w = st.world_matrix; const float* b = st.bone_matrix;
        for (int j = 0; j < 3; j++) g_ip_stat.pw[j] = b[9] * w[j] + b[10] * w[3 + j] + b[11] * w[6 + j] + w[9 + j];
        ip_eye(st.view_matrix, g_ip_stat.eye);
    }
    return true;
}

// Vertex decoder (through mode, PSP pixels): the offset that moves this 2D draw from where the game put it (centre cx, cy) to the blend of
// where it was on the last two gameplay frames. false = leave it.
bool ge_draw_through_ip(float cx, float cy, float* dx, float* dy) {
    if (!g_ip2d_on || g_ip_anchor_seq != g_ip_seq || g_ip_anchors.empty()) return false;
    const IpAnchor* best = nullptr; float bd = 90.0f * 90.0f;   // the fighter whose newest root is nearest (within 90 px)
    for (const auto& a : g_ip_anchors) {
        const float d = (a.live[0] - cx) * (a.live[0] - cx) + (a.live[1] - cy) * (a.live[1] - cy);
        if (d < bd) { bd = d; best = &a; }
    }
    if (!best) return false;
    *dx = best->drawn[0] - best->live[0]; *dy = best->drawn[1] - best->live[1];
    if (*dx * *dx + *dy * *dy > 60.0f * 60.0f) return false;   // a cut: leave it
    if (ip_trace_level() >= 3) std::fprintf(stderr, "[IP2D] seq %u phase %.2f centre %.1f,%.1f anchor live %.1f,%.1f drawn %.1f,%.1f -> %+.1f,%+.1f\n", g_ip_seq, g_ip_phase,
                                            cx, cy, best->live[0], best->live[1], best->drawn[0], best->drawn[1], *dx, *dy);
    return true;
}

void ge_draw_prim(uint8_t* rdram, int prim_type, int count) {
    g_ip_rdram = rdram;
    struct Ip2dOff { ~Ip2dOff() { g_ip2d_on = false; } } ip2d_off;
    IpMats ip_saved;
    struct IpRestore { IpMats* m; bool on; ~IpRestore() { if (!on) return; GeState& st = ge_get_state_mut();
        std::memcpy(st.world_matrix, m->w, sizeof m->w); std::memcpy(st.view_matrix, m->v, sizeof m->v);
        std::memcpy(st.proj_matrix, m->p, sizeof m->p); std::memcpy(st.bone_matrix, m->b, sizeof m->b);
        std::memcpy(st.tgen_matrix, m->tg, sizeof m->tg);
        st.tex_scale_u = m->uv[0]; st.tex_scale_v = m->uv[1]; st.tex_offset_u = m->uv[2]; st.tex_offset_v = m->uv[3]; g_ge_state_gen++; } } ip_restore{&ip_saved, count > 0 && ip_apply(prim_type, count, &ip_saved)};
    psp_perf::Scope perf_scope_(psp_perf::PRIM);
    if (!g_ab || !g_fast_enabled || g_current_target < 0 || !g_targets[g_current_target].ab_fbo) {
        ge_draw_prim_one(rdram, prim_type, count);
        return;
    }
    ge_draw_prim_one(rdram, prim_type, count);          // fast path -> real target
    flush_batch();
    GeTarget& t = g_targets[g_current_target];
    glBindFramebuffer(GL_FRAMEBUFFER, t.ab_fbo);
    g_ab_shadow = true;
    static const bool fault = std::getenv("PSPRECOMP_AB_FAULT") != nullptr;   // harness self-check
    static unsigned fault_n = 0;
    if (!(fault && (++fault_n % 7) == 0))
        ge_draw_prim_one(rdram, prim_type, count);      // legacy path -> twin
    g_ab_shadow = false;
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
}

// ---- Screen-space effects (psp_screenfx.cpp) ----
// A displayed buffer's 3D scene is complete when the first through-mode (2D) draw follows a run of 3D draws: the game
// then copies the picture into its glow chain and draws the HUD. The effects run right there, on the scene alone.
static int g_fx_n[GE_MAX_TARGETS];
static bool g_fx_done[GE_MAX_TARGETS];
// The projection / view pair the scene was drawn with: the one most depth-writing draws used (a Boyer-Moore majority
// vote), not the last draw's, which can belong to a character or an effect with a matrix of its own. The light direction
// and "up" come from the view matrix, so a per-object one made shadows follow the player.
// PSPRECOMP_RT_CPU_LOG=1: render-thread time of the ray-traced scene's CPU side per scene (capture decode, persistence, merge, upload + build).
struct RtCpu { double cap = 0, note = 0, merge = 0, build = 0; int scenes = 0; long draws = 0; };
static RtCpu g_rt_cpu;
static const bool g_rt_cpu_log = std::getenv("PSPRECOMP_RT_CPU_LOG") != nullptr;
struct RtCpuT { double* acc; std::chrono::steady_clock::time_point t0; explicit RtCpuT(double* a) : acc(g_rt_cpu_log ? a : nullptr) { if (acc) t0 = std::chrono::steady_clock::now(); }
    ~RtCpuT() { if (acc) *acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } };
struct FxVote { float proj[16]; float view[12]; float near_z, far_z; int n; };
static FxVote g_fx_vote[GE_MAX_TARGETS];
// Ray-queried shadows: the opaque 3D draws of a scene as world-space triangles (psp_rt.h). Decoded a second time on the CPU
// (skinning included, then the world matrix) only while the Vulkan ray-query path is on.
static std::vector<float> g_rt_tris[GE_MAX_TARGETS];
static std::vector<uint32_t> g_rt_cols[GE_MAX_TARGETS];   // one RGBA8 per captured triangle

// The game skips draws it culls (objects outside the camera), so a shadow caster just off screen would drop out of the ray-traced
// scene and its shadow would pop away as the camera moves. Keep settled geometry (the same draw, byte for byte, in 3 frames in a
// row) while it is outside the view frustum; it is dropped as soon as it is inside the view and no longer drawn (destroyed).
struct RtPersist { std::vector<float> tris; std::vector<uint32_t> cols; uint32_t last_frame = 0; int streak = 0; bool settled = false; float bmin[3], bmax[3]; };
static std::unordered_map<uint64_t, RtPersist> g_rt_persist[GE_MAX_TARGETS];
static uint32_t g_rt_frame[GE_MAX_TARGETS];
// Some draws (the broken-block debris) use their own view matrix. Their world matrix then is not a world position: the game's position is
// view_draw * world. Re-express such triangles in the world of the frame's main view: p' = inv(view_main) * view_draw * p.
static float g_rt_main_view[GE_MAX_TARGETS][12];   // the last completed scene's main view (for effects drawn after it)
static bool g_rt_main_view_ok[GE_MAX_TARGETS];
// The main view's 3x3 is inverted for real, not transposed: under 30 Hz logic the frames between two gameplay frames are drawn with
// interpolated matrices, blended element by element, which are not pure rotations; the transpose then moved and bent the re-expressed
// triangles (a fighter's own head, slightly off, shadowed its drawn face: issue #94).
static void rt_fix_view(std::vector<float>& tr, size_t t0, const float* vd, const float* vm) {
    if (!std::memcmp(vd, vm, 12 * sizeof(float))) return;
    // vm column-major 3x3: M = [vm0 vm3 vm6; vm1 vm4 vm7; vm2 vm5 vm8]; inv(M) rows below
    const float a = vm[0], b = vm[3], c = vm[6], d = vm[1], e = vm[4], f = vm[7], g = vm[2], h = vm[5], k = vm[8];
    const float A = e * k - f * h, B = -(d * k - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-8f) return;
    const float id = 1.f / det;
    const float i00 = A * id, i01 = -(b * k - c * h) * id, i02 = (b * f - c * e) * id;
    const float i10 = B * id, i11 = (a * k - c * g) * id, i12 = -(a * f - c * d) * id;
    const float i20 = C * id, i21 = -(a * h - b * g) * id, i22 = (a * e - b * d) * id;
    for (size_t i = t0; i + 2 < tr.size(); i += 3) {
        const float x = tr[i], y = tr[i + 1], z = tr[i + 2];
        const float px = vd[0] * x + vd[3] * y + vd[6] * z + vd[9] - vm[9], py = vd[1] * x + vd[4] * y + vd[7] * z + vd[10] - vm[10], pz = vd[2] * x + vd[5] * y + vd[8] * z + vd[11] - vm[11];
        tr[i] = i00 * px + i01 * py + i02 * pz; tr[i + 1] = i10 * px + i11 * py + i12 * pz; tr[i + 2] = i20 * px + i21 * py + i22 * pz;
    }
}
// Only baked stage geometry / props (vertex type 0x11a) persist: skinned models and debris move, repeat poses and would leave ghosts.
// An entry becomes settled after 8 frames in a row and stays settled however often the game hides and shows it (walls toggle with the camera).
// The draw's identity is its source (addresses, vertex type, primitive, count, the first 64 vertex bytes), its world matrix and, when
// rt_fix_view re-expresses it, its own view matrix. Hashing every transformed float instead cost 0.5 ms of render-thread time per scene on
// the RP6. Stage geometry (0x11a) does not rewrite its vertices in place, so this is the same test.
static inline uint64_t rt_mix(uint64_t h, uint64_t w) { h ^= w; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 31); }
static uint64_t rt_persist_key(const GeState& st, uint8_t* rdram, int prim_type, int count, bool fixed) {
    uint64_t h = rt_mix(0xCBF29CE484222325ull, ((uint64_t)st.vertex_addr << 32) | st.index_addr);
    h = rt_mix(h, ((uint64_t)st.vertex_type << 32) | ((uint64_t)(uint32_t)count << 3) | (uint64_t)prim_type);
    auto mixf = [&](const float* f, int n) { for (int k = 0; k + 1 < n; k += 2) { uint64_t w; std::memcpy(&w, f + k, 8); h = rt_mix(h, w); } };
    mixf(st.world_matrix, 12);
    if (fixed) mixf(st.view_matrix, 12);
    const uint32_t va = st.vertex_addr & 0x07FFFFFFu;
    if (va + 64 <= 0x08000000u) for (int k = 0; k < 64; k += 8) { uint64_t w; std::memcpy(&w, rdram + va + k, 8); h = rt_mix(h, w); }
    return h;
}
// Static part of the ray-traced scene: every settled entry, kept in its own acceleration structure that the Vulkan layer only rebuilds
// when this list changes (an entry settles or is dropped). Rebuilding all of the stage every frame cost 1.7 ms of GPU time per frame on the
// RP6. PSPRECOMP_RT_NO_STATIC=1: everything goes into the per-frame list as before.
static std::vector<float> g_rt_st_tris[GE_MAX_TARGETS];
static std::vector<uint32_t> g_rt_st_cols[GE_MAX_TARGETS];
static bool g_rt_st_dirty[GE_MAX_TARGETS];
static uint64_t g_rt_st_ver[GE_MAX_TARGETS], g_rt_st_next = 1;
static const bool g_rt_static = std::getenv("PSPRECOMP_RT_NO_STATIC") == nullptr && std::getenv("PSPRECOMP_RT_NO_PERSIST") == nullptr;
static void rt_static_refresh(int target) {
    if (!g_rt_static || !g_rt_st_dirty[target]) return;
    g_rt_st_dirty[target] = false;
    g_rt_st_tris[target].clear(); g_rt_st_cols[target].clear();
    for (auto& kv : g_rt_persist[target]) {
        const RtPersist& e = kv.second;
        if (!e.settled) continue;
        g_rt_st_tris[target].insert(g_rt_st_tris[target].end(), e.tris.begin(), e.tris.end());
        g_rt_st_cols[target].insert(g_rt_st_cols[target].end(), e.cols.begin(), e.cols.end());
    }
    g_rt_st_ver[target] = g_rt_st_next++;
}
// A settled stage draw seen again with the same key: nothing to decode, its triangles are in the static part (or, without it, appended
// from the kept copy). True = done.
static bool rt_persist_reuse(int target, uint64_t key) {
    static const bool off = std::getenv("PSPRECOMP_RT_NO_REUSE") != nullptr;
    if (off) return false;
    auto it = g_rt_persist[target].find(key);
    if (it == g_rt_persist[target].end() || !it->second.settled || it->second.tris.empty()) return false;
    RtPersist& e = it->second;
    if (!g_rt_static) {
        g_rt_tris[target].insert(g_rt_tris[target].end(), e.tris.begin(), e.tris.end());
        g_rt_cols[target].insert(g_rt_cols[target].end(), e.cols.begin(), e.cols.end());
    }
    if (e.last_frame != g_rt_frame[target]) { e.streak = e.last_frame + 1 == g_rt_frame[target] ? e.streak + 1 : 1; e.last_frame = g_rt_frame[target]; }
    return true;
}
static void rt_persist_note(int target, size_t t0, size_t c0, uint64_t h) {
    RtCpuT rt_cpu_t_(&g_rt_cpu.note);
    const std::vector<float>& tr = g_rt_tris[target]; const std::vector<uint32_t>& co = g_rt_cols[target];
    if (tr.size() <= t0 || co.size() <= c0) return;
    RtPersist& e = g_rt_persist[target][h];
    if (e.last_frame == g_rt_frame[target]) return;                       // the same draw twice in a frame
    e.streak = e.last_frame + 1 == g_rt_frame[target] ? e.streak + 1 : 1;
    if (e.streak >= 8 && !e.settled) { e.settled = true; g_rt_st_dirty[target] = true; }
    e.last_frame = g_rt_frame[target];
    if (e.tris.empty()) {
        e.tris.assign(tr.begin() + t0, tr.end()); e.cols.assign(co.begin() + c0, co.end());
        for (int k = 0; k < 3; ++k) { e.bmin[k] = 1e30f; e.bmax[k] = -1e30f; }
        for (size_t i = 0; i + 2 < e.tris.size(); i += 3) for (int k = 0; k < 3; ++k) { e.bmin[k] = std::min(e.bmin[k], e.tris[i + k]); e.bmax[k] = std::max(e.bmax[k], e.tris[i + k]); }
    }
}
// Adds the settled draws that were not drawn this frame and are outside the frustum (proj 4x4, view 4x3 as in the GE state).
struct RtFrameTick { int t; ~RtFrameTick() { ++g_rt_frame[t]; } };   // the persistence "frame" is one completed scene (clear events come 1.5 per scene)
static void rt_persist_merge(int target, const float* proj, const float* view) {
    RtFrameTick tick{target};
    RtCpuT rt_cpu_t_(&g_rt_cpu.merge);
    auto& map = g_rt_persist[target];
    const uint32_t now = g_rt_frame[target];
    static int last_stage[GE_MAX_TARGETS] = {-1, -1, -1, -1, -1, -1, -1, -1};
    const int stage = psp_stage_id();
    if (stage >= 0 && stage != last_stage[target]) { if (last_stage[target] >= 0) { map.clear(); g_rt_st_dirty[target] = true; } last_stage[target] = stage; }   // a new stage: forget the old geometry (-1 = battle overlay briefly not loaded: ignored)
    const bool keep_all = psp_stage_keeps_geometry();
    static const bool off = std::getenv("PSPRECOMP_RT_NO_PERSIST") != nullptr;
    if (off) return;
    int n_keep = 0, n_drop = 0, n_unsettled = 0;
    for (auto it = map.begin(); it != map.end();) {
        RtPersist& e = it->second;
        if (e.last_frame == now) { ++it; continue; }
        bool keep = false;
        if (keep_all && e.settled) keep = true;
        else if (e.settled && now - e.last_frame < 100000) {
            int lo[2] = {0, 0}, hi[2] = {0, 0}, behind = 0;
            for (int c = 0; c < 8; ++c) {
                const float x = (c & 1) ? e.bmax[0] : e.bmin[0], y = (c & 2) ? e.bmax[1] : e.bmin[1], z = (c & 4) ? e.bmax[2] : e.bmin[2];
                const float vx = view[0] * x + view[3] * y + view[6] * z + view[9], vy = view[1] * x + view[4] * y + view[7] * z + view[10], vz = view[2] * x + view[5] * y + view[8] * z + view[11];
                float cl[4];
                for (int r = 0; r < 4; ++r) cl[r] = proj[r] * vx + proj[4 + r] * vy + proj[8 + r] * vz + proj[12 + r];
                if (cl[3] <= 0.f) { ++behind; continue; }
                for (int a = 0; a < 2; ++a) { if (cl[a] < -cl[3] * 1.05f) ++lo[a]; if (cl[a] > cl[3] * 1.05f) ++hi[a]; }
            }
            const int vis = 8 - behind;   // corners in front of the camera
            keep = behind == 8 || lo[0] == vis || hi[0] == vis || lo[1] == vis || hi[1] == vis;   // wholly off one side of the view
            if (behind > 0 && behind < 8) keep = false;   // straddles the camera plane: could be on screen
        }
        if (!e.settled) ++n_unsettled;
        if (!keep) { if (e.settled) { ++n_drop; g_rt_st_dirty[target] = true; } it = map.erase(it); continue; }
        ++n_keep;
        if (!g_rt_static) {   // with the static part, every settled entry is in it already
            g_rt_tris[target].insert(g_rt_tris[target].end(), e.tris.begin(), e.tris.end());
            g_rt_cols[target].insert(g_rt_cols[target].end(), e.cols.begin(), e.cols.end());
        }
        ++it;
    }
    static int lg = 0;
    if (lg++ % 240 == 0 && std::getenv("PSPRECOMP_RT_PERSIST_LOG")) { int st = 0, mx = 0; for (auto& kv : map) { st += kv.second.settled; mx = std::max(mx, kv.second.streak); } std::fprintf(stderr, "[RTP] target %d frame %u: map %zu, settled %d, max streak %d; kept %d, dropped %d, unsettled %d\n", target, now, map.size(), st, mx, n_keep, n_drop, n_unsettled); }
}
// Effects (blended, depth-tested, not depth-writing: orbs, auras, glass): a second set for the reflection's effects layer; alpha = coverage.
static std::vector<float> g_rt_fx_tris[GE_MAX_TARGETS];
static std::vector<uint32_t> g_rt_fx_cols[GE_MAX_TARGETS];
// RT scene debugging (docs/VULKAN_PORT.md): psp_rt_request_dump() (F9 in the window) logs every 3D draw of the next frame with what the RT
// scene did with it ([RTDUMP] lines on stderr), to see which draws a shadow / reflection is missing.
static std::atomic<int> g_rtdump{0};   // 0 idle, 1 armed (next frame start), 2 logging this frame
static thread_local const char* g_rt_why = "ok";
// To stderr and to rtdump.log in the working directory (appended), so a player can send the file.
static void rtdump_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void rtdump_printf(const char* fmt, ...) {
    static FILE* f = std::getenv("PSPRECOMP_RT_DUMP_EVERY") ? nullptr : std::fopen("rtdump.log", "a");
    va_list a; va_start(a, fmt);
    if (f) { va_list b; va_copy(b, a); std::vfprintf(f, fmt, b); va_end(b); std::fflush(f); }
    std::vfprintf(stderr, fmt, a); va_end(a);
}
void psp_rt_request_dump() { g_rtdump.store(1); }
static const bool g_rt_nofix = std::getenv("PSPRECOMP_RT_NO_VIEWFIX") != nullptr;   // debug: do not re-express draws that use their own view matrix
static void rt_capture(const GeState& st, uint8_t* rdram, int prim_type, int count, int target, bool fx = false) {
    RtCpuT rt_cpu_t_(&g_rt_cpu.cap);
    ++g_rt_cpu.draws;
    static const bool dbg = std::getenv("PSPRECOMP_RT_DEBUG") != nullptr;
    static int why[4] = {}, seen = 0;
    if (dbg && (++seen % 20000) == 0) std::fprintf(stderr, "[RT] capture: seen %d, rejected prim/count %d, blend %d, atest %d (blend_en=%d atest_en=%d)\n", seen, why[0], why[1], why[2], st.alpha_blend_enable, st.alpha_test_enable);
    g_rt_why = "ok";
    if (count < 3 || prim_type < GE_PRIM_TRIANGLES || prim_type > GE_PRIM_TRIANGLE_FAN) { why[0]++; g_rt_why = "prim"; return; }
    // The caller only passes depth-testing, depth-writing 3D draws, which leaves out particles / glass. The game keeps
    // GE_BLEND on for nearly every draw (opaque blend function), so blending cannot be used as a filter.
    std::vector<float>& out = fx ? g_rt_fx_tris[target] : g_rt_tris[target];
    if (out.size() > 9u * (fx ? 100000u : 400000u)) { g_rt_why = "limit"; return; }
    // What a reflection shows where it hits one of these triangles (cheap hit shade, docs/VULKAN_PORT.md): the average of the
    // texture bound for the draw times the vertex colour (or the lit material), one RGBA8 per triangle.
    const uint32_t tex = st.texture_enable ? ge_texture_bound_avg_color() : 0xFFFFFFFFu;
    // A texture that is mostly transparent is a cut-out / glow / haze card: the game draws it alpha-tested or blended, and as an opaque
    // reflector it showed up as a bright flat quad behind far ruins. Leave such draws out of the scene.
    if (!fx && st.texture_enable && (tex >> 24) < 200 && (st.tex_func_rgba || st.alpha_test_enable)) { g_rt_why = "transparent-texture"; return; }
    static thread_local std::vector<DecodedVertex> dv;
    ge_decode_vertices(rdram, st, prim_type, count, dv, true);   // positions and colours only
    if (fx && std::getenv("PSPRECOMP_RT_FX_LOG") && !dv.empty()) {   // debug: the colour state of each kind of effect draw (once per kind)
        static std::unordered_map<uint64_t, int> seen_k;
        const uint64_t k = ((uint64_t)tex << 32) ^ ((uint64_t)st.tex_func << 24) ^ ((uint64_t)st.blend_mode << 8) ^ (uint64_t)(st.tex_env_color & 0xFFFFFF) * 31u ^ (uint64_t)(st.blend_fix_a & 0xFFFFFF) * 131u;
        if (seen_k[k]++ == 0 && seen_k.size() < 400)
            std::fprintf(stderr, "[RTFX] n=%d prim=%d tex=%08X tfunc=%u rgba=%d env=%06X bm=%03X fa=%06X fb=%06X lit=%d vcol=%d v0=%02X%02X%02X%02X mat=%06X amb=%06X vt=%06X\n", count, prim_type, (unsigned)tex,
                         (unsigned)st.tex_func, (int)st.tex_func_rgba, (unsigned)(st.tex_env_color & 0xFFFFFF), (unsigned)st.blend_mode, (unsigned)(st.blend_fix_a & 0xFFFFFF), (unsigned)(st.blend_fix_b & 0xFFFFFF),
                         (int)st.lighting_enable, (int)dv[0].has_color, dv[0].color[0], dv[0].color[1], dv[0].color[2], dv[0].color[3], (unsigned)(st.material_diffuse & 0xFFFFFF), (unsigned)(st.material_ambient & 0xFFFFFF), (unsigned)st.vertex_type);
    }
    const size_t n = dv.size();
    if (n < 3) return;
    const float* m = st.world_matrix;
    // Written into a pre-sized tail of `out` (a push_back per float was 5% of the RP6 render thread)
    const size_t ntri = prim_type == GE_PRIM_TRIANGLES ? n / 3 : n - 2;
    const size_t o0 = out.size();
    out.resize(o0 + ntri * 9);
    float* op = out.data() + o0;
    auto push = [&](const DecodedVertex& v) {
        op[0] = m[0] * v.pos[0] + m[3] * v.pos[1] + m[6] * v.pos[2] + m[9];
        op[1] = m[1] * v.pos[0] + m[4] * v.pos[1] + m[7] * v.pos[2] + m[10];
        op[2] = m[2] * v.pos[0] + m[5] * v.pos[1] + m[8] * v.pos[2] + m[11];
        op += 3;
    };
    const float tr = float(tex & 255) / 255.f, tg = float((tex >> 8) & 255) / 255.f, tb = float((tex >> 16) & 255) / 255.f;
    const bool lit = st.lighting_enable != 0;
    const bool replace = st.texture_enable && st.tex_func == 3;
    auto rgb = [](uint32_t c, int k) { return float((c >> (8 * k)) & 255) / 255.f; };
    std::vector<uint32_t>& cols = fx ? g_rt_fx_cols[target] : g_rt_cols[target];
    const bool skinned = ge_vtype_weight(st.vertex_type) != 0;
    auto tri_color = [&](const DecodedVertex& a, const DecodedVertex& b, const DecodedVertex& c) {
        float col[3] = {1.f, 1.f, 1.f};
        if (!replace) {
            const DecodedVertex* vs[3] = {&a, &b, &c};
            if (lit && !(fx && a.has_color)) {   // effects: the vertex colour is what tints them (lit or not; the material is plain white)
                for (int k = 0; k < 3; k++) {
                    float d = rgb(st.material_diffuse, k);
                    if ((st.material_update & 2u) && a.has_color) { d = 0; for (const DecodedVertex* v : vs) d += float(v->color[k]) / 255.f; d /= 3.f; }
                    col[k] = d * 0.85f + 0.1f;
                }
            } else if (a.has_color) {
                for (int k = 0; k < 3; k++) { float d = 0; for (const DecodedVertex* v : vs) d += float(v->color[k]) / 255.f; col[k] = d / 3.f; }
            } else if (!st.texture_enable) { col[0] = col[1] = col[2] = 0.5f; }
        }
        const float t3[3] = {tr, tg, tb};
        // Opaque layer: a cut-out (alpha-tested / texture-alpha) draw keeps its texture's mean alpha as coverage, so a reflection
        // hit on it off screen lets what lies behind show through (k_rt_refl); a solid draw is 255.
        uint32_t alpha = (st.texture_enable && (st.tex_func_rgba || st.alpha_test_enable)) ? (tex >> 24) : 255u;
        if (fx) {   // coverage: the texture's mean alpha x the vertex alpha (the material alpha when lit)
            float al = st.texture_enable ? float(tex >> 24) / 255.f : 1.f;
            if (a.has_color) al *= (float(a.color[3]) + float(b.color[3]) + float(c.color[3])) / (3.f * 255.f);
            else if (lit) al *= float(st.material_alpha & 0xFFu) / 255.f;
            alpha = uint32_t(std::min(255.f, std::max(24.f, al * 255.f)));
        }
        if (!fx) alpha = (alpha & 0xFEu) | (skinned ? 1u : 0u);   // the low bit of the coverage: a fighter's triangle (k_rt_code skips a fighter's own body)
        uint32_t o = alpha << 24;
        for (int k = 0; k < 3; k++) o |= uint32_t(std::min(255.f, std::max(0.f, col[k] * t3[k] * 255.f + 0.5f))) << (8 * k);
        cols.push_back(o);
    };
    // Effects: big blended triangles are background sheets (cloud / sky domes, flash planes, the intro's planes), not effects; reflected
    // they covered half the floor. Spell and beam triangles are small or long and thin, so the test is the area (world units squared,
    // PSPRECOMP_RT_FX_MAX_AREA, default 30; a 100-unit beam ribbon 0.5 wide is 25).
    static const float fx_max_area = [] { const char* e = std::getenv("PSPRECOMP_RT_FX_MAX_AREA"); return e ? (float)std::atof(e) : 30.f; }();
    auto tri = [&](const DecodedVertex& a, const DecodedVertex& b, const DecodedVertex& c) {
        float* t0 = op;
        push(a); push(b); push(c);
        if (fx) {
            const float ux = t0[3] - t0[0], uy = t0[4] - t0[1], uz = t0[5] - t0[2], vx = t0[6] - t0[0], vy = t0[7] - t0[1], vz = t0[8] - t0[2];
            const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            if (0.25f * (cx * cx + cy * cy + cz * cz) > fx_max_area * fx_max_area) { op = t0; return; }   // (area = |u x v| / 2)
        }
        tri_color(a, b, c);
    };
    if (prim_type == GE_PRIM_TRIANGLES) {
        for (size_t i = 0; i + 2 < n; i += 3) tri(dv[i], dv[i + 1], dv[i + 2]);
    } else if (prim_type == GE_PRIM_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < n; i++) { const bool odd = i & 1; tri(dv[odd ? i + 1 : i], dv[odd ? i : i + 1], dv[i + 2]); }
    } else {
        for (size_t i = 1; i + 1 < n; i++) tri(dv[0], dv[i], dv[i + 1]);
    }
    out.resize(size_t(op - out.data()));   // triangles left out above
}

// The triangles of a reflective draw, in clip space (what the GPU computes: proj x view x world, widescreen x scale), with its
// material byte, for the reflectivity mask (g_rml_tris). Skinned draws are not stage surfaces and are left out.
static void rml_capture(const GeState& st, uint8_t* rdram, int prim_type, int count, float refl) {
    if (g_current_target < 0 || count < 3 || prim_type < GE_PRIM_TRIANGLES || prim_type > GE_PRIM_TRIANGLE_FAN) return;
    std::vector<float>& out = g_rml_tris[g_current_target];
    if (out.size() > 5u * 3u * 200000u) return;
    static thread_local std::vector<DecodedVertex> dv;
    ge_decode_vertices(rdram, st, prim_type, count, dv);
    const size_t n = dv.size();
    if (n < 3) return;
    const float* m = st.world_matrix;
    const float* v = st.view_matrix;
    const float* p = st.proj_matrix;
    const float wx = draw_wide_x(st);
    const float mat = std::floor(refl * 255.f + 0.5f);
    auto push = [&](const DecodedVertex& d) {
        const float x = m[0] * d.pos[0] + m[3] * d.pos[1] + m[6] * d.pos[2] + m[9];
        const float y = m[1] * d.pos[0] + m[4] * d.pos[1] + m[7] * d.pos[2] + m[10];
        const float z = m[2] * d.pos[0] + m[5] * d.pos[1] + m[8] * d.pos[2] + m[11];
        const float vx = v[0] * x + v[3] * y + v[6] * z + v[9], vy = v[1] * x + v[4] * y + v[7] * z + v[10], vz = v[2] * x + v[5] * y + v[8] * z + v[11];
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = p[r] * vx + p[4 + r] * vy + p[8 + r] * vz + p[12 + r];
        out.push_back(c[0] * wx); out.push_back(c[1]); out.push_back(c[2]); out.push_back(c[3]); out.push_back(mat);
    };
    auto tri = [&](const DecodedVertex& a, const DecodedVertex& b, const DecodedVertex& c) { push(a); push(b); push(c); };
    if (prim_type == GE_PRIM_TRIANGLES) {
        for (size_t i = 0; i + 2 < n; i += 3) tri(dv[i], dv[i + 1], dv[i + 2]);
    } else if (prim_type == GE_PRIM_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < n; i++) tri(dv[i], dv[i + 1], dv[i + 2]);
    } else {
        for (size_t i = 1; i + 1 < n; i++) tri(dv[0], dv[i], dv[i + 1]);
    }
}

// Fighters do not shadow themselves: their low-poly models (and the solid cards of hair, fur and fringes) left grey blotches all
// over the body, both with ray-traced and with screen-space shadows. The skinned draws' triangles (skinning included) go into the
// mask list with material byte 1 (psp_refl_encode never gives 1: a reflective byte is >= 32), so the mask marks the pixels where a
// fighter is visible; the shadow pass then ignores the fighters' own geometry for those pixels (psp_screenfx.cpp u_fmask).
// PSPRECOMP_SELF_SHADOW=1 keeps the old self-shadowing.
static bool self_shadow_on() { static const bool on = [] { const char* e = std::getenv("PSPRECOMP_SELF_SHADOW"); return e && e[0] == '1'; }(); return on; }
static bool fighter_mask_wanted(const GeState& st) {
    return !self_shadow_on() && g_current_target >= 0 && !g_ab_shadow && !st.clear_mode && (psp_fx_contact() > 0 || psp_rt_active())
           && ge_vtype_weight(st.vertex_type) != 0 && !ge_vtype_through(st.vertex_type) && st.depth_test_enable && !st.depth_write_disable
           && is_main_key(g_targets[g_current_target].key);
}
static void fighter_capture(const GeState& st, uint8_t* rdram, int prim_type, int count) {
    if (count < 3 || prim_type < GE_PRIM_TRIANGLES || prim_type > GE_PRIM_TRIANGLE_FAN) return;
    std::vector<float>& out = g_rml_tris[g_current_target];
    if (out.size() > 5u * 3u * 200000u) return;
    static thread_local std::vector<DecodedVertex> dv;
    ge_decode_vertices(rdram, st, prim_type, count, dv, true);   // skinned positions
    const size_t n = dv.size();
    if (n < 3) return;
    const float* m = st.world_matrix;
    const float* v = st.view_matrix;
    const float* p = st.proj_matrix;
    const float wx = draw_wide_x(st);
    auto push = [&](const DecodedVertex& d) {
        const float x = m[0] * d.pos[0] + m[3] * d.pos[1] + m[6] * d.pos[2] + m[9];
        const float y = m[1] * d.pos[0] + m[4] * d.pos[1] + m[7] * d.pos[2] + m[10];
        const float z = m[2] * d.pos[0] + m[5] * d.pos[1] + m[8] * d.pos[2] + m[11];
        const float vx = v[0] * x + v[3] * y + v[6] * z + v[9], vy = v[1] * x + v[4] * y + v[7] * z + v[10], vz = v[2] * x + v[5] * y + v[8] * z + v[11];
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = p[r] * vx + p[4 + r] * vy + p[8 + r] * vz + p[12 + r];
        out.push_back(c[0] * wx); out.push_back(c[1]); out.push_back(c[2]); out.push_back(c[3]); out.push_back(1.f);
    };
    auto tri = [&](const DecodedVertex& a, const DecodedVertex& b, const DecodedVertex& c) { push(a); push(b); push(c); };
    if (prim_type == GE_PRIM_TRIANGLES) {
        for (size_t i = 0; i + 2 < n; i += 3) tri(dv[i], dv[i + 1], dv[i + 2]);
    } else if (prim_type == GE_PRIM_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < n; i++) tri(dv[i], dv[i + 1], dv[i + 2]);
    } else {
        for (size_t i = 1; i + 1 < n; i++) tri(dv[0], dv[i], dv[i + 1]);
    }
    g_rml_fighters[g_current_target] = true;
}

// Effect lights (issue #4): glowing (additive) effect draws light the scene. Each such draw is summarised as a blob (world bbox,
// area-weighted colour x coverage); at the end of the scene nearby blobs are merged and the brightest become point lights whose colour is
// the effect's and whose reach / brightness follow its size (psp_screenfx.cpp adds them next to the stage lights). PSPRECOMP_FX_LIGHTS=0 turns them off.
struct FxBlob { float lo[3], hi[3], rgb[3], area; };
static std::vector<FxBlob> g_fx_blobs[GE_MAX_TARGETS];
static int g_fx_n_steady[GE_MAX_TARGETS], g_fx_n_nonadd[GE_MAX_TARGETS], g_fx_n_static[GE_MAX_TARGETS];   // this scene: draws left out (the effect-light log)
static std::atomic<int> g_fx_lights{-1};   // -1: from PSPRECOMP_FX_LIGHTS (on unless 0); psp_fx_lights_set overrides (test scripts: A/B in one run)
static bool fx_lights_on() {
    int v = g_fx_lights.load();
    if (v < 0) { v = (std::getenv("PSPRECOMP_FX_LIGHTS") && std::getenv("PSPRECOMP_FX_LIGHTS")[0] == '0') ? 0 : 1; g_fx_lights.store(v); }
    return v != 0;
}
// Brightness / reach multipliers of the effect lights (Settings > Debug rows, saved as env.FX_LIGHT_INTENSITY / _RADIUS = PSPRECOMP_FX_LIGHT_*)
static std::atomic<float> g_fx_scale[2] = {{-1.f}, {-1.f}};
float psp_fx_light_scale(int which) {
    which = which ? 1 : 0;
    float v = g_fx_scale[which].load();
    if (v < 0.f) { const char* e = std::getenv(which ? "PSPRECOMP_FX_LIGHT_RADIUS" : "PSPRECOMP_FX_LIGHT_INTENSITY"); v = e && e[0] ? std::max(0.f, (float)std::atof(e)) : (which ? 1.5f : 2.5f); g_fx_scale[which].store(v); }   // defaults picked in game
    return v;
}
void psp_fx_light_set_scale(int which, float v) { g_fx_scale[which ? 1 : 0].store(std::max(0.f, v)); }
// A game module can lower the number of effect lights for a while (Dissidia tag team: a member's entrance makes a burst of glowing draws that became up
// to 8 ray-traced lights and stalled Medium / High RT shadows, 18 Nov). 8 = no limit.
static std::atomic<int> g_fx_light_limit{8};
void psp_fx_lights_limit(int n) { g_fx_light_limit.store(n < 0 ? 0 : (n > 8 ? 8 : n)); }
void psp_fx_lights_set(bool on) { g_fx_lights.store(on ? 1 : 0); std::fprintf(stderr, "[FXLIGHT] effect lights %s\n", on ? "on" : "off"); }
static bool fx_additive(const GeState& st) {   // src x As (or 1) + dst x FIX with a bright FIX: the GE's additive glow
    const int dst = (st.blend_mode >> 4) & 0xF, op = (st.blend_mode >> 8) & 0x7;
    if (op != 0 || dst != 10) return false;
    const uint32_t fb = st.blend_fix_b & 0xFFFFFFu;
    return (fb & 0xFF) >= 0x80 && ((fb >> 8) & 0xFF) >= 0x80 && ((fb >> 16) & 0xFF) >= 0x80;
}
static void fx_blob_note(int cur, size_t t0, size_t c0) {
    const std::vector<float>& T = g_rt_fx_tris[cur]; const std::vector<uint32_t>& C = g_rt_fx_cols[cur];
    FxBlob b = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, {0, 0, 0}, 0};
    for (size_t k = t0, ci = c0; k + 9 <= T.size() && ci < C.size(); k += 9, ++ci) {
        const float* v = &T[k];
        for (int q = 0; q < 3; ++q) for (int a = 0; a < 3; ++a) { b.lo[a] = std::min(b.lo[a], v[q * 3 + a]); b.hi[a] = std::max(b.hi[a], v[q * 3 + a]); }
        const float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        const float ar = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
        const uint32_t c = C[ci]; const float al = float(c >> 24) / 255.f;
        for (int a = 0; a < 3; ++a) b.rgb[a] += float((c >> (8 * a)) & 255) / 255.f * al * ar;
        b.area += ar;
    }
    if (b.area <= 1e-6f) return;
    for (float& x : b.rgb) x /= b.area;
    g_fx_blobs[cur].push_back(b);
}
// Merge the scene's blobs (overlapping / near ones are one effect) and turn the brightest into lights: {x y z r g b radius intensity clear}.
static int fx_blob_lights(int cur, float (*out)[9], int max) {
    std::vector<FxBlob>& B = g_fx_blobs[cur];
    struct Cl { float lo[3], hi[3], rgb[3], area, peak; };   // peak: the brightest blob (a big soft halo must not dim the core)
    std::vector<Cl> cl;
    static const float max_ext = 40.f;
    int n_big = 0, n_dim = 0; float big_ext = 0.f;
    for (const FxBlob& b : B) {
        const float ext = 0.5f * std::sqrt((b.hi[0] - b.lo[0]) * (b.hi[0] - b.lo[0]) + (b.hi[1] - b.lo[1]) * (b.hi[1] - b.lo[1]) + (b.hi[2] - b.lo[2]) * (b.hi[2] - b.lo[2]));
        if (ext > max_ext) { ++n_big; big_ext = std::max(big_ext, ext); continue; }   // a screen-sized haze / flash card, not a light
        Cl* hit = nullptr;
        for (Cl& c : cl) {   // overlapping boxes (grown by 1 unit) are the same effect
            bool ov = true;
            for (int a = 0; a < 3; ++a) ov = ov && b.lo[a] <= c.hi[a] + 1.f && b.hi[a] >= c.lo[a] - 1.f;
            if (ov) { hit = &c; break; }
        }
        const float bpk = std::max(b.rgb[0], std::max(b.rgb[1], b.rgb[2]));
        if (!hit) { cl.push_back({{b.lo[0], b.lo[1], b.lo[2]}, {b.hi[0], b.hi[1], b.hi[2]}, {b.rgb[0] * b.area, b.rgb[1] * b.area, b.rgb[2] * b.area}, b.area, bpk}); continue; }
        hit->peak = std::max(hit->peak, bpk);
        for (int a = 0; a < 3; ++a) { hit->lo[a] = std::min(hit->lo[a], b.lo[a]); hit->hi[a] = std::max(hit->hi[a], b.hi[a]); hit->rgb[a] += b.rgb[a] * b.area; }
        hit->area += b.area;
    }
    const float k_rad = psp_fx_light_scale(1), k_int = psp_fx_light_scale(0);
    struct Cand { float score; float l[9]; };
    std::vector<Cand> cand;
    for (const Cl& c : cl) {
        float rgb[3] = {c.rgb[0] / c.area, c.rgb[1] / c.area, c.rgb[2] / c.area};
        const float avg = std::max(rgb[0], std::max(rgb[1], rgb[2]));
        if (avg < 1e-4f) { ++n_dim; continue; }
        const float mx = c.peak;   // brightness of the brightest part; the colour stays the area average (normalised below)
        for (float& x : rgb) x *= mx / avg;
        if (mx < 0.03f) { ++n_dim; continue; }
        const float ext = 0.5f * std::sqrt((c.hi[0] - c.lo[0]) * (c.hi[0] - c.lo[0]) + (c.hi[1] - c.lo[1]) * (c.hi[1] - c.lo[1]) + (c.hi[2] - c.lo[2]) * (c.hi[2] - c.lo[2]));
        const float size = std::sqrt(c.area) * 0.5f;   // from the glowing area itself: touching pieces chain into a wide box (a trail, scattered sparks), which made small attacks huge lights
        const float score = mx * std::sqrt(c.area);
        if (score < 0.05f) { ++n_dim; continue; }
        Cand d;
        d.score = score;
        for (int a = 0; a < 3; ++a) { d.l[a] = 0.5f * (c.lo[a] + c.hi[a]); d.l[3 + a] = rgb[a] / mx; }
        d.l[6] = std::min(14.f, std::max(3.f, 3.f + 2.5f * size)) * k_rad;                       // reach grows with the effect's size
        // brightness: additive glows look far brighter than their average texel (mostly soft falloff), so the average only scales a base
        // score = peak brightness x sqrt(glowing area). Measured (Kain, Ultimecia): idle 0.9, jump / land 0.3-2, dodges 0.1-3.9, small hits 3-5,
        // big attacks 10-26. Nothing below 1, then even up to full (1.2) at 16: dodges and landings stay faint, only big attacks light the scene.
        d.l[7] = 1.2f * std::min(1.f, std::max(0.f, (score - 1.f) / 15.f)) * k_int;
        if (d.l[7] < 0.02f) { ++n_dim; continue; }
        d.l[8] = std::min(ext, 2.f);                                                             // the caster (a hand / weapon inside the glow) does not block it
        cand.push_back(d);
    }
    // A glow that stays in one place for seconds is part of the stage (a torch, a crystal, the EX core's own halo), not an attack: those are
    // left to the hand-placed stage lights. Tracks: where a glow was, since when, last seen.
    {
        struct Track { float p[3]; double first, last; };
        static std::vector<Track> tracks[GE_MAX_TARGETS];
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::vector<Track>& tr = tracks[cur];
        tr.erase(std::remove_if(tr.begin(), tr.end(), [&](const Track& t) { return now - t.last > 0.5; }), tr.end());
        static const double k_static = [] { const char* e = std::getenv("PSPRECOMP_FX_LIGHT_STATIC_S"); return e ? std::atof(e) : 8.0; }();   // longer than any attack (Hell's Judgment ran past 3 s)
        std::vector<Cand> keep;
        for (const Cand& d : cand) {
            Track* t = nullptr;
            for (Track& x : tr) { const float dx = x.p[0] - d.l[0], dy = x.p[1] - d.l[1], dz = x.p[2] - d.l[2]; if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) { t = &x; break; } }
            if (!t) { tr.push_back({{d.l[0], d.l[1], d.l[2]}, now, now}); keep.push_back(d); continue; }
            t->last = now;
            if (now - t->first < k_static) keep.push_back(d); else ++g_fx_n_static[cur];
        }
        cand.swap(keep);
    }
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    { static int lg = 0; static const bool dbg = std::getenv("PSPRECOMP_FX_LIGHT_LOG") != nullptr;   // what each glow scored (the cut-off is in score units)
      if (dbg && !cand.empty() && lg++ % 5 == 0) { std::fprintf(stderr, "[FXLIGHT]   scores:"); for (size_t i = 0; i < cand.size() && i < 8; ++i) std::fprintf(stderr, " %.2f@(%.1f %.1f %.1f)", cand[i].score, cand[i].l[0], cand[i].l[1], cand[i].l[2]); std::fprintf(stderr, "\n"); } }
    // Lights live a little across frames: matched to last frame's by position, a light takes a new, brighter value at once but fades out over
    // ~0.15 s when its glow drops for a frame or two (Knight's Lance's light flickered as its pieces regrouped from frame to frame).
    int n = 0;
    {
        struct Live { float l[9]; float I; bool seen; };
        static std::vector<Live> live[GE_MAX_TARGETS];
        static double last_t[GE_MAX_TARGETS];
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float dt = last_t[cur] > 0 ? float(std::min(0.2, now - last_t[cur])) : 0.f;
        last_t[cur] = now;
        std::vector<Live>& lv = live[cur];
        for (Live& x : lv) x.seen = false;
        for (size_t i = 0; i < cand.size() && i < 16; ++i) {
            const Cand& d = cand[i];
            Live* m = nullptr; float best = 2.5f * 2.5f;
            for (Live& x : lv) { if (x.seen) continue; const float dx = x.l[0] - d.l[0], dy = x.l[1] - d.l[1], dz = x.l[2] - d.l[2], q = dx * dx + dy * dy + dz * dz; if (q < best) { best = q; m = &x; } }
            if (!m) { lv.push_back({{}, 0.f, true}); m = &lv.back(); std::memcpy(m->l, d.l, sizeof d.l); m->I = d.l[7]; continue; }
            const float I = m->I;
            std::memcpy(m->l, d.l, sizeof d.l);
            m->I = d.l[7] >= I ? d.l[7] : I + (d.l[7] - I) * std::min(1.f, dt * 7.f);
            m->seen = true;
        }
        for (Live& x : lv) if (!x.seen) x.I -= x.I * std::min(1.f, dt * 7.f);
        lv.erase(std::remove_if(lv.begin(), lv.end(), [](const Live& x) { return x.I < 0.02f; }), lv.end());
        std::sort(lv.begin(), lv.end(), [](const Live& a, const Live& b) { return a.I > b.I; });
        for (const Live& x : lv) { if (n >= max) break; std::memcpy(out[n], x.l, sizeof x.l); out[n][7] = x.I; ++n; }
    }
    {   // all effect lights together at most one full light's brightness: many small glows must not add up to a flood
        float sum = 0.f;
        for (int i = 0; i < n; ++i) sum += out[i][7];
        const float cap = 1.2f * k_int;
        if (sum > cap) for (int i = 0; i < n; ++i) out[i][7] *= cap / sum;
    }
    { static int lg = 0; static const bool dbg = std::getenv("PSPRECOMP_FX_LIGHT_LOG") != nullptr;
      if (dbg && (n || n_big || n_dim || g_fx_n_steady[cur] || g_fx_n_nonadd[cur] || g_fx_n_static[cur]) && lg++ % 5 == 0)
        std::fprintf(stderr, "[FXLIGHT] %zu blobs, %zu clusters -> %d lights | left out: %d too big (largest %.0f), %d too faint, %d steady draws, %d non-additive draws, %d staying put | first %.1f %.1f %.1f rgb %.2f %.2f %.2f radius %.1f intensity %.2f\n",
                     B.size(), cl.size(), n, n_big, big_ext, n_dim, g_fx_n_steady[cur], g_fx_n_nonadd[cur], g_fx_n_static[cur],
                     n ? out[0][0] : 0.f, n ? out[0][1] : 0.f, n ? out[0][2] : 0.f, n ? out[0][3] : 0.f, n ? out[0][4] : 0.f, n ? out[0][5] : 0.f, n ? out[0][6] : 0.f, n ? out[0][7] : 0.f); }
    { static int lg2 = 0; static const bool dbg = std::getenv("PSPRECOMP_FX_LIGHT_LOG") != nullptr;
      if (dbg && n && lg2++ % 5 == 0) { std::fprintf(stderr, "[FXLIGHT]   lights:"); for (int i = 0; i < n; ++i) std::fprintf(stderr, " (%.1f %.1f %.1f r%.1f i%.2f)", out[i][0], out[i][1], out[i][2], out[i][6], out[i][7]); std::fprintf(stderr, "\n"); } }
    g_fx_n_steady[cur] = g_fx_n_nonadd[cur] = g_fx_n_static[cur] = 0;
    return n;
}
// Glowing stage pieces (issue #4: Crystal World's and Empyreal Paradox's breakable crystals). The light editor marks a texture (+ vertex
// format) of a stage as glowing (psp_screenfx.cpp, G key, `glow` lines in stage_lights.txt); every drawn piece with it gets a light per
// connected part of its mesh (one per crystal), so a crystal that breaks and is no longer drawn stops lighting. Up to 12 per scene.
static std::vector<std::array<float, 9>> g_glow_lights[GE_MAX_TARGETS];
static float pt_tri_dist2(const float* p, const float* a, const float* b, const float* c) {   // squared distance, point to triangle (Ericson)
    const float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]}, ap[3] = {p[0]-a[0], p[1]-a[1], p[2]-a[2]};
    auto dot = [](const float* u, const float* v) { return u[0]*v[0] + u[1]*v[1] + u[2]*v[2]; };
    float q[3];
    auto set = [&](float s_, float t_) { for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k]*s_ + ac[k]*t_; };
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    const float bp[3] = {p[0]-b[0], p[1]-b[1], p[2]-b[2]}; const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    const float cp[3] = {p[0]-c[0], p[1]-c[1], p[2]-c[2]}; const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    const float vc = d1*d4 - d3*d2, vb = d5*d2 - d1*d6, va = d3*d6 - d5*d4;
    if (d1 <= 0 && d2 <= 0) set(0, 0);
    else if (d3 >= 0 && d4 <= d3) set(1, 0);
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) set(d1 / (d1 - d3), 0);
    else if (d6 >= 0 && d5 <= d6) set(0, 1);
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) set(0, d2 / (d2 - d6));
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); set(1 - w, w); }
    else { const float den = 1.f / (va + vb + vc); set(vb * den, vc * den); }
    const float dx = q[0]-p[0], dy = q[1]-p[1], dz = q[2]-p[2];
    return dx*dx + dy*dy + dz*dz;
}
struct GlowPick { float d2 = 1e30f; uint32_t hash = 0, vt = 0; };
static GlowPick g_glow_pick[GE_MAX_TARGETS];
// A drawn stage piece: its world triangles (9 floats each). Lights for a marked texture, and the G-key pick.
static void glow_draw(int cur, const GeState& st, const float* T, size_t nf) {
    if (nf < 9 || !st.texture_enable) return;
    const uint32_t th = ge_texture_bound_hash();
    if (!th) return;
    float pp[3];
    if (psp_stage_glow_pick_point(pp)) {
        GlowPick& gp = g_glow_pick[cur];
        for (size_t k = 0; k + 9 <= nf; k += 9) { const float d = pt_tri_dist2(pp, T + k, T + k + 3, T + k + 6); if (d < gp.d2) { gp.d2 = d; gp.hash = th; gp.vt = st.vertex_type; } }
    }
    float spec[5];
    if (!psp_stage_glow_find(th, st.vertex_type, spec) || g_glow_lights[cur].size() >= 12) return;
    // connected parts: triangles sharing a vertex (positions rounded to 1 cm)
    const size_t nt = nf / 9;
    std::vector<int> par(nt);
    for (size_t i = 0; i < nt; ++i) par[i] = int(i);
    auto find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
    std::unordered_map<uint64_t, int> at;
    for (size_t i = 0; i < nt; ++i) for (int v = 0; v < 3; ++v) {
        const float* q = T + i * 9 + v * 3;
        const uint64_t key = (uint64_t(int64_t(std::lround(q[0] * 100.f)) & 0x1FFFFF) << 42) ^ (uint64_t(int64_t(std::lround(q[1] * 100.f)) & 0x1FFFFF) << 21) ^ (uint64_t(int64_t(std::lround(q[2] * 100.f)) & 0x1FFFFF));
        auto it = at.find(key);
        if (it == at.end()) at.emplace(key, int(i)); else { const int a = find(int(i)), b = find(it->second); if (a != b) par[a] = b; }
    }
    std::unordered_map<int, std::array<float, 7>> box;   // lo xyz, hi xyz, triangle count
    for (size_t i = 0; i < nt; ++i) {
        auto& bx = box.try_emplace(find(int(i)), std::array<float, 7>{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f, 0.f}).first->second;
        for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a) { const float x = T[i * 9 + v * 3 + a]; bx[a] = std::min(bx[a], x); bx[3 + a] = std::max(bx[3 + a], x); }
        bx[6] += 1.f;
    }
    for (const auto& kv : box) {
        if (g_glow_lights[cur].size() >= 12) break;
        const auto& bx = kv.second;
        const float ext = 0.5f * std::sqrt((bx[3]-bx[0])*(bx[3]-bx[0]) + (bx[4]-bx[1])*(bx[4]-bx[1]) + (bx[5]-bx[2])*(bx[5]-bx[2]));
        g_glow_lights[cur].push_back({0.5f * (bx[0] + bx[3]), 0.5f * (bx[1] + bx[4]), 0.5f * (bx[2] + bx[5]), spec[0], spec[1], spec[2], spec[3], spec[4], ext});   // clear = its own size: the crystal does not shadow its light
    }
}
// The main scene's camera without the screen effects (fx_track keeps it otherwise): the view / projection most opaque 3D draws of the main
// target use, for ge_draw_last_view_proj.
static void main_camera_vote(const GeState& st) {
    static float vp[16], vv[12]; static int n = 0;
    if (g_current_target < 0 || g_ab_shadow || !is_main_key(g_targets[g_current_target].key)) return;
    if (st.clear_mode) { if (st.clear_flags == 0 || (st.clear_flags & 4)) n = 0; return; }   // a new frame of the main target: the vote starts over
    if (ge_vtype_through(st.vertex_type) || !st.depth_test_enable || st.depth_write_disable) return;
    float proj[16]; std::memcpy(proj, st.proj_matrix, sizeof proj);
    if (const float wx = draw_wide_x(st); wx != 1.0f) { proj[0] *= wx; proj[4] *= wx; proj[8] *= wx; proj[12] *= wx; }
    tall_proj(proj, draw_tall_y(st));
    const bool same = n > 0 && !std::memcmp(vp, proj, sizeof vp) && !std::memcmp(vv, st.view_matrix, sizeof vv);
    if (same) { if (n < 1000) n++; } else if (n > 0) n--; else { std::memcpy(vp, proj, sizeof vp); std::memcpy(vv, st.view_matrix, sizeof vv); n = 1; }
    if (same && n >= 8) { std::memcpy(g_last_view, vv, sizeof g_last_view); std::memcpy(g_last_proj, vp, sizeof g_last_proj); g_last_vp_ok = true; ip_note_vp_tag(); }
}
static void fx_track(const GeState& st, uint8_t* rdram, int prim_type, int count) {
    {   // effects draws (blended, depth tested, not depth writing) are also drawn into the SSR effects layer (fxl_*)
        // PSPRECOMP_SSR_FX_OFF=1: no effects layer at all (each effects draw is drawn twice, into another framebuffer: on a tiled mobile
        // GPU that is two render-pass breaks per draw, 67 draws on stage 13); before, it only stopped the pass from reading the layer.
        static const bool fxl_off = std::getenv("PSPRECOMP_SSR_FX_OFF") != nullptr;
        const bool want = !fxl_off && g_current_target >= 0 && !g_ab_shadow && !st.clear_mode && psp_fx_ssr() > 0 && !psp_rt_refl_active()
                          && !ge_vtype_through(st.vertex_type) && st.depth_test_enable && st.depth_write_disable && st.alpha_blend_enable
                          && is_main_key(g_targets[g_current_target].key);
        if (want != g_fxl_flag) { flush_batch(); g_fxl_flag = want; }
        // depth-writing 3D draws may be reflective surfaces (reflectivity mask, rml_*; the value comes from the texture)
        const bool want_m = g_current_target >= 0 && !g_ab_shadow && !st.clear_mode && (psp_fx_ssr() > 0 || psp_rt_refl_active())
                            && psp_refl_table_active() && !ge_vtype_through(st.vertex_type) && st.texture_enable && st.depth_test_enable
                            && !st.depth_write_disable && is_main_key(g_targets[g_current_target].key);
        if (want_m != g_rml_flag) { flush_batch(); g_rml_flag = want_m; }
    }
    if (g_current_target < 0 || g_ab_shadow) return;
    const int cur = g_current_target;
    if (st.clear_mode) {
        // The effects lists are not cleared here: the game draws most of its spell / beam effects after the 3D run (after the reflection pass
        // used the lists), so they are kept and traced by the next frame's pass, which clears them after use. Clearing them here left the
        // ray-traced reflection without Watera, Thundaga, ... (PSPRECOMP_RT_REFL_DEBUG=1 never showed an effect hit).
        if (st.clear_flags == 0 || (st.clear_flags & 4)) { g_fx_n[cur] = 0; g_fx_done[cur] = false; g_fx_vote[cur].n = 0; g_rt_tris[cur].clear(); g_rt_cols[cur].clear(); g_rml_tris[cur].clear(); g_rml_fighters[cur] = false; }
        { static const int every = std::getenv("PSPRECOMP_RT_DUMP_EVERY") ? std::atoi(std::getenv("PSPRECOMP_RT_DUMP_EVERY")) : 0; static int fr = 0;
          if (every > 0 && g_rtdump.load() == 0 && (st.clear_flags == 0 || (st.clear_flags & 4)) && ++fr % every == 0) g_rtdump.store(1); }   // automated runs: one dump every N frames
        if (g_rtdump.load() == 1) { g_rtdump.store(2); rtdump_printf("[RTDUMP] begin target %d\n", cur); }
        return;
    }
    if (!ge_vtype_through(st.vertex_type)) {
        if (g_rtdump.load() == 2 && !st.clear_mode) {
            g_rt_why = "not-captured";
            const bool will = st.depth_test_enable && !st.depth_write_disable;
            if (will) { /* reason is filled in by rt_capture below; log after */ }
            else rtdump_printf("[RTDUMP] draw prim %d n %d  skip: depth_test %d depth_write_off %d blend %d atest %d  pos %.1f %.1f %.1f  vt %08x\n", prim_type, count, st.depth_test_enable, st.depth_write_disable, st.alpha_blend_enable, st.alpha_test_enable, st.world_matrix[9], st.world_matrix[10], st.world_matrix[11], (unsigned)st.vertex_type);
        }
        if (st.depth_test_enable && st.alpha_blend_enable && st.depth_write_disable && !fx_additive(st)) ++g_fx_n_nonadd[cur];
        if (st.depth_test_enable && st.alpha_blend_enable && st.depth_write_disable && std::getenv("PSPRECOMP_FX_LIGHT_LOG")) {   // which blends the effects use
            static std::map<uint64_t, int> seen; const uint64_t key = (uint64_t(st.blend_mode & 0xFFF) << 32) | (st.blend_fix_b & 0xFFFFFFu) | (uint64_t(st.blend_fix_a & 0xFFFFFFu) << 44);
            if (seen[key]++ == 0) std::fprintf(stderr, "[FXLIGHT] blend %03x fixa %06x fixb %06x zwrite %d additive %d rt %d\n", (unsigned)(st.blend_mode & 0xFFF), (unsigned)(st.blend_fix_a & 0xFFFFFFu), (unsigned)(st.blend_fix_b & 0xFFFFFFu), st.depth_write_disable ? 0 : 1, (int)fx_additive(st), (int)psp_rt_active());
        }
        if (st.depth_test_enable && !st.depth_write_disable) {
            g_fx_n[cur]++;
            const size_t dump_t0 = g_rt_tris[cur].size();
            if (psp_rt_scene_wanted()) { const size_t t0 = g_rt_tris[cur].size(), c0 = g_rt_cols[cur].size(); const bool fixed = g_fx_vote[cur].n > 0 && !g_rt_nofix && std::memcmp(st.view_matrix, g_fx_vote[cur].view, 12 * sizeof(float)) != 0; const bool stage = st.vertex_type == 0x11au; const uint64_t pkey = stage ? rt_persist_key(st, rdram, prim_type, count, fixed) : 0; if (!(stage && rt_persist_reuse(cur, pkey))) { rt_capture(st, rdram, prim_type, count, cur); if (fixed) rt_fix_view(g_rt_tris[cur], t0, st.view_matrix, g_fx_vote[cur].view); if (stage) rt_persist_note(cur, t0, c0, pkey); if (g_rt_tris[cur].size() > t0) glow_draw(cur, st, g_rt_tris[cur].data() + t0, g_rt_tris[cur].size() - t0); }
                else if (psp_stage_has_glow() || psp_stage_glow_pick_point(nullptr)) { auto it = g_rt_persist[cur].find(pkey); if (it != g_rt_persist[cur].end()) glow_draw(cur, st, it->second.tris.data(), it->second.tris.size()); } }
            if (g_rtdump.load() == 2) rtdump_printf("[RTDUMP] draw prim %d n %d  %s: blend %d atest %d tex %d texalpha %u rgba %d lit %d mask %06x/%02x bm %03x fa %06x fb %06x zw %d  pos %.1f %.1f %.1f  vt %08x\n", prim_type, count, g_rt_why, st.alpha_blend_enable, st.alpha_test_enable, st.texture_enable, (unsigned)(st.texture_enable ? ge_texture_bound_avg_color() >> 24 : 255u), st.tex_func_rgba, st.lighting_enable, (unsigned)st.mask_rgb, (unsigned)(st.mask_alpha & 0xFF), (unsigned)st.blend_mode, (unsigned)st.blend_fix_a & 0xFFFFFFu, (unsigned)st.blend_fix_b & 0xFFFFFFu, st.depth_write_disable ? 0 : 1, st.world_matrix[9], st.world_matrix[10], st.world_matrix[11], (unsigned)st.vertex_type);
            if (g_rtdump.load() == 2 && g_rt_tris[cur].size() > dump_t0) { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; const std::vector<float>& tv = g_rt_tris[cur];
                for (size_t q = dump_t0; q + 2 < tv.size(); q += 3) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], tv[q + k]); hi[k] = std::max(hi[k], tv[q + k]); }
                { static thread_local std::vector<DecodedVertex> dd; ge_decode_vertices(rdram, st, prim_type, count, dd);
                  { uint32_t hv = 2166136261u; for (int k = 0; k < 12; ++k) { uint32_t b; std::memcpy(&b, &st.view_matrix[k], 4); hv = (hv ^ b) * 16777619u; } for (int k = 0; k < 16; ++k) { uint32_t b; std::memcpy(&b, &st.proj_matrix[k], 4); hv = (hv ^ b) * 16777619u; }
                  rtdump_printf("[RTDUMP]   target fb %06x/%u fmt %u vp %g,%g,%g c %g,%g,%g scis %06x %06x reg %06x %06x offs %u,%u camhash %08x\n", (unsigned)st.framebuf_ptr & 0xFFFFFFu, (unsigned)st.framebuf_width, (unsigned)st.framebuf_format, st.viewport_x_scale, st.viewport_y_scale, st.viewport_z_scale, st.viewport_x_center, st.viewport_y_center, st.viewport_z_center, (unsigned)st.scissor1, (unsigned)st.scissor2, (unsigned)st.region1, (unsigned)st.region2, (unsigned)st.offset_x, (unsigned)st.offset_y, hv); }
                  rtdump_printf("[RTDUMP]   state df %u cull %d/%u stencil %d/%x/%x ctest %d matA %02x upd %x ambA %02x tfunc %u tenv %06x vcol %d va %d,%d,%d\n", (unsigned)st.depth_func, st.cull_enable, (unsigned)st.cull_face, st.stencil_test_enable, (unsigned)st.stencil_test, (unsigned)st.stencil_op, st.color_test_enable, (unsigned)(st.material_alpha & 0xFF), (unsigned)st.material_update, (unsigned)(st.ambient_alpha & 0xFF), (unsigned)st.tex_func, (unsigned)(st.tex_env_color & 0xFFFFFF), dd.empty() ? -1 : (int)dd[0].has_color, dd.size() > 0 ? dd[0].color[3] : -1, dd.size() > 1 ? dd[1].color[3] : -1, dd.size() > 2 ? dd[2].color[3] : -1); }
                rtdump_printf("[RTDUMP]   bbox %.1f %.1f %.1f .. %.1f %.1f %.1f\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); }
            FxVote& v = g_fx_vote[cur];
            // widescreen: the x of the clip position is scaled outside the matrix (build_xf), so the effects get the same
            float proj[16];
            std::memcpy(proj, st.proj_matrix, sizeof proj);
            if (const float wx = draw_wide_x(st); wx != 1.0f) { proj[0] *= wx; proj[4] *= wx; proj[8] *= wx; proj[12] *= wx; }
            tall_proj(proj, draw_tall_y(st));
            const bool same = v.n > 0 && !std::memcmp(v.proj, proj, sizeof v.proj) && !std::memcmp(v.view, st.view_matrix, sizeof v.view);
            if (same && v.n >= 8 && is_main_key(g_targets[cur].key)) { std::memcpy(g_last_view, v.view, sizeof g_last_view); std::memcpy(g_last_proj, v.proj, sizeof g_last_proj); g_last_vp_ok = true; ip_note_vp_tag(); }   // ge_draw_last_view_proj
            if (same) v.n++;
            else if (v.n > 0) v.n--;
            else {
                std::memcpy(v.proj, proj, sizeof v.proj);
                std::memcpy(v.view, st.view_matrix, sizeof v.view);
                const GeViewportDepth vp = compute_state_viewport_depth(st);
                v.near_z = vp.near_z; v.far_z = vp.far_z;
                v.n = 1;
            }
        }
        else if (st.depth_test_enable && st.alpha_blend_enable && ((psp_rt_refl_active() && psp_rt_refl_quality() >= 2 && psp_rt_refl_fx_enabled()) || (psp_rt_active() && fx_lights_on() && fx_additive(st) && st.vertex_type != 0x11au))) {
            const bool refl_fx = psp_rt_refl_active() && psp_rt_refl_quality() >= 2 && psp_rt_refl_fx_enabled();
            const bool light_fx = psp_rt_active() && fx_lights_on() && fx_additive(st) && st.vertex_type != 0x11au;   // 0x11a: the stage's own glow layers (stage lights cover those)
            const size_t c0 = g_rt_fx_cols[cur].size();
            // Effects drawn with their own view matrix (some with an identity view: the world matrix then holds view-space positions) are
            // re-expressed in the main view's world, as the opaque draws are. Most effects come after the 3D run, when this scene's vote is
            // already reset: the last completed scene's main view is used then.
            const size_t t0 = g_rt_fx_tris[cur].size();
            rt_capture(st, rdram, prim_type, count, cur, true);
            const float* mv = g_fx_vote[cur].n > 0 ? g_fx_vote[cur].view : g_rt_main_view_ok[cur] ? g_rt_main_view[cur] : nullptr;
            if (mv && !g_rt_nofix && g_rt_fx_tris[cur].size() > t0) rt_fix_view(g_rt_fx_tris[cur], t0, st.view_matrix, mv);
            // the same glow drawn every frame for seconds (the lock-on reticle, a stage glow) is not an attack: keyed by texture, format, size
            bool steady = false;
            if (light_fx) {
                struct Seen { double first, last; bool latched; };   // first: start of the current unbroken streak; latched: the streak was logged
                static std::unordered_map<uint64_t, Seen> seen;
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const uint64_t key = (uint64_t(st.tex_addr[0]) << 32) ^ (uint64_t(st.vertex_type) << 12) ^ uint64_t(count) ^ (uint64_t(prim_type) << 28);
                Seen& sn = seen[key];
                if (sn.last == 0 || now - sn.last > 0.3) { sn.first = now; sn.latched = false; }
                sn.last = now;
                static const double k_steady = [] { const char* e = std::getenv("PSPRECOMP_FX_LIGHT_STEADY_S"); return e ? std::atof(e) : 10.0; }();   // longer than any attack (Hell's Judgment ~7 s)
                if (now - sn.first > k_steady && !sn.latched) {   // logged once per streak; a gap of 0.3 s starts a new streak (no lasting latch:
                    sn.latched = true;                            // particles share texture / format / size with every effect that uses them)
                    static const bool dbg = std::getenv("PSPRECOMP_FX_LIGHT_LOG") != nullptr;
                    if (dbg) std::fprintf(stderr, "[FXLIGHT] steady draw: tex %08x vt %08x prim %d n %d world t %.1f %.1f %.1f\n", (unsigned)st.tex_addr[0], (unsigned)st.vertex_type, prim_type, count, st.world_matrix[9], st.world_matrix[10], st.world_matrix[11]);
                }
                steady = now - sn.first > k_steady;
                if (seen.size() > 4096) seen.clear();
            }
            if (light_fx && steady) ++g_fx_n_steady[cur];
            if (light_fx && !steady && g_rt_fx_tris[cur].size() > t0) {
                fx_blob_note(cur, t0, c0);
                static const bool dbg = std::getenv("PSPRECOMP_FX_LIGHT_LOG") != nullptr;
                if (dbg && !g_fx_blobs[cur].empty()) {   // which vertex formats / matrices the effects come with (blobs at the origin = a transform not applied)
                    const FxBlob& b = g_fx_blobs[cur].back();
                    static std::map<uint32_t, int> seen;
                    const bool origin = std::fabs(0.5f * (b.lo[0] + b.hi[0])) < 0.3f && std::fabs(0.5f * (b.lo[2] + b.hi[2])) < 0.3f;
                    if (seen[st.vertex_type * 2u + (origin ? 1u : 0u)]++ < 2)
                        { float pp[3] = {0, 0, 0}; psp_player_pos(pp); std::fprintf(stderr, "[FXLIGHT] player %.1f %.1f %.1f\n", pp[0], pp[1], pp[2]); }
                        std::fprintf(stderr, "[FXLIGHT] draw vt %08x prim %d n %d centre %.1f %.1f %.1f %s world t %.1f %.1f %.1f view t %.1f %.1f %.1f bone0 t %.1f %.1f %.1f\n", (unsigned)st.vertex_type, prim_type, count,
                                     0.5f * (b.lo[0] + b.hi[0]), 0.5f * (b.lo[1] + b.hi[1]), 0.5f * (b.lo[2] + b.hi[2]), origin ? "ORIGIN" : "", st.world_matrix[9], st.world_matrix[10], st.world_matrix[11],
                                     st.view_matrix[9], st.view_matrix[10], st.view_matrix[11], st.bone_matrix[9], st.bone_matrix[10], st.bone_matrix[11]);
                }
            }
            if (!refl_fx) { g_rt_fx_tris[cur].resize(t0); g_rt_fx_cols[cur].resize(c0); }   // only wanted for the light
        }   // the effects layer is only traced from Medium up
        return;
    }
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (!t.fbo || g_fx_done[i] || g_fx_n[i] < 60 || !is_main_key(t.key)) continue;
        g_fx_done[i] = true;
        g_fx_n[i] = 0;
        flush_batch();
        ScreenFxScene s;
        std::memcpy(s.proj, g_fx_vote[i].proj, sizeof s.proj);
        std::memcpy(s.view, g_fx_vote[i].view, sizeof s.view);
        s.near_z = g_fx_vote[i].near_z; s.far_z = g_fx_vote[i].far_z;
        s.clear_z = g_fx_clearz[i];
        std::memcpy(g_last_view, s.view, sizeof g_last_view); std::memcpy(g_last_proj, s.proj, sizeof g_last_proj); g_last_vp_ok = true; ip_note_vp_tag();   // ge_draw_last_view_proj
        { static int dbg = 0; if (psp_rt_available() && dbg++ % 600 == 0) std::fprintf(stderr, "[RT] scene complete: %zu captured triangles, rt %d\n", g_rt_tris[i].size() / 9, (int)psp_rt_available()); }
        if (psp_rt_scene_wanted() && (!g_rt_tris[i].empty() || !g_rt_persist[i].empty())) rt_persist_merge(i, s.proj, s.view);
        if (psp_rt_scene_wanted()) rt_static_refresh(i);
        std::memcpy(g_rt_main_view[i], s.view, sizeof g_rt_main_view[i]); g_rt_main_view_ok[i] = true;
        { float lp[3]; if (psp_stage_light_pick_request(lp)) {   // light editor: colour of the surface the light touches
            auto closest = [&](const float* a, const float* b, const float* c, float* q) {   // closest point on a triangle to lp (Ericson)
                const float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]}, ap[3] = {lp[0]-a[0], lp[1]-a[1], lp[2]-a[2]};
                auto dot = [](const float* u, const float* v) { return u[0]*v[0] + u[1]*v[1] + u[2]*v[2]; };
                const float d1 = dot(ab, ap), d2 = dot(ac, ap);
                auto set = [&](float s, float t) { for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k]*s + ac[k]*t; };
                if (d1 <= 0 && d2 <= 0) { set(0, 0); return; }
                const float bp[3] = {lp[0]-b[0], lp[1]-b[1], lp[2]-b[2]}; const float d3 = dot(ab, bp), d4 = dot(ac, bp);
                if (d3 >= 0 && d4 <= d3) { set(1, 0); return; }
                const float vc = d1*d4 - d3*d2;
                if (vc <= 0 && d1 >= 0 && d3 <= 0) { set(d1 / (d1 - d3), 0); return; }
                const float cp[3] = {lp[0]-c[0], lp[1]-c[1], lp[2]-c[2]}; const float d5 = dot(ab, cp), d6 = dot(ac, cp);
                if (d6 >= 0 && d5 <= d6) { set(0, 1); return; }
                const float vb = d5*d2 - d1*d6;
                if (vb <= 0 && d2 >= 0 && d6 <= 0) { set(0, d2 / (d2 - d6)); return; }
                const float va = d3*d6 - d5*d4;
                if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); set(1 - w, w); return; }
                const float den = 1.f / (va + vb + vc); set(vb * den, vc * den);
            };
            float best = 1e30f; uint32_t bc = 0; bool found = false;
            auto scan = [&](const std::vector<float>& T, const std::vector<uint32_t>& C) {   // this frame's draws and the settled static part
                for (size_t k = 0; k + 9 <= T.size() && k / 9 < C.size(); k += 9) {
                    float q[3]; closest(&T[k], &T[k+3], &T[k+6], q);
                    const float dx = q[0]-lp[0], dy = q[1]-lp[1], dz = q[2]-lp[2], d = dx*dx + dy*dy + dz*dz;
                    if (d < best) { best = d; bc = C[k / 9]; found = true; }
                }
            };
            scan(g_rt_tris[i], g_rt_cols[i]);
            if (g_rt_static) scan(g_rt_st_tris[i], g_rt_st_cols[i]);
            if (found) { const float rgb[3] = {float(bc & 255) / 255.f, float((bc >> 8) & 255) / 255.f, float((bc >> 16) & 255) / 255.f}; psp_stage_light_pick_result(rgb, std::sqrt(best)); }
        } }
        if (g_rtdump.load() == 2) { g_rtdump.store(0); rtdump_printf("[RTDUMP] end: %zu scene triangles, %zu effects triangles\n", g_rt_tris[i].size() / 9, g_rt_fx_tris[i].size() / 9);
            rtdump_printf("[RTDUMP] proj"); for (int k = 0; k < 16; ++k) rtdump_printf(" %g", s.proj[k]); rtdump_printf("\n");
            rtdump_printf("[RTDUMP] view"); for (int k = 0; k < 12; ++k) rtdump_printf(" %g", s.view[k]); rtdump_printf("\n"); }
        RtCpuT* rt_cpu_b_ = new RtCpuT(&g_rt_cpu.build);
        const uint32_t st_n = g_rt_static ? (uint32_t)(g_rt_st_tris[i].size() / 9) : 0u;
        s.rt_ready = (!g_rt_tris[i].empty() || st_n) && psp_rt_scene_wanted() && g_psp_rt.set_scene(g_rt_tris[i].data(), (uint32_t)(g_rt_tris[i].size() / 9), g_rt_cols[i].size() == g_rt_tris[i].size() / 9 ? g_rt_cols[i].data() : nullptr, g_rt_fx_tris[i].data(), g_rt_fx_cols[i].size() == g_rt_fx_tris[i].size() / 9 ? (uint32_t)g_rt_fx_cols[i].size() : 0u, g_rt_fx_cols[i].data(),
                                                                                              g_rt_st_tris[i].data(), st_n, g_rt_st_cols[i].size() == (size_t)st_n ? g_rt_st_cols[i].data() : nullptr, g_rt_st_ver[i]);
        delete rt_cpu_b_;
        if (g_rt_cpu_log && psp_rt_scene_wanted() && ++g_rt_cpu.scenes == 600) {
            std::fprintf(stderr, "[RTCPU] ms/scene: capture %.3f (%.0f draws)  persist note %.3f  merge %.3f  upload+build %.3f  (%zu tris, %zu fx, %zu persisted draws)\n", g_rt_cpu.cap / 600,
                         g_rt_cpu.draws / 600.0, g_rt_cpu.note / 600, g_rt_cpu.merge / 600, g_rt_cpu.build / 600, g_rt_tris[i].size() / 9, g_rt_fx_tris[i].size() / 9, g_rt_persist[i].size());
            g_rt_cpu = RtCpu{};
        }
        s.rt_fx = s.rt_ready && !g_rt_fx_cols[i].empty();
        s.n_fx_lights = s.rt_ready ? fx_blob_lights(i, s.fx_lights, g_fx_light_limit.load()) : 0;
        s.n_glow_lights = 0;
        if (s.rt_ready) for (const auto& g : g_glow_lights[i]) { if (s.n_glow_lights >= 12) break; std::memcpy(s.glow_lights[s.n_glow_lights++], g.data(), sizeof s.glow_lights[0]); }
        g_glow_lights[i].clear();
        if (g_glow_pick[i].hash) { psp_stage_glow_pick_result(g_glow_pick[i].hash, g_glow_pick[i].vt, std::sqrt(g_glow_pick[i].d2)); g_glow_pick[i] = GlowPick{}; }
        g_fx_blobs[i].clear();
        { static int n = 0; if (std::getenv("PSPRECOMP_SSR_FX_LOG") && n++ % 120 == 0) std::fprintf(stderr, "[SSR] effects layer used=%d fbo=%u draws=%d\n", (int)g_fxl[i].used, g_fxl[i].fbo, g_fxl_draws); }
        if (g_fxl[i].used && g_fxl[i].fbo) { s.fx_color = g_fxl[i].color; s.fx_depth = g_fxl[i].depth; }
        g_fxl[i].dirty = true; g_fxl[i].used = false; g_fxl_draws = 0;
        s.refl_table = psp_refl_table_active();
        s.refl_tris = g_rml_tris[i].empty() ? nullptr : g_rml_tris[i].data();
        s.refl_nverts = (int)(g_rml_tris[i].size() / 5);
        s.fighters = g_rml_fighters[i];
        if (s.rt_ready) {
            static int logged = 0;
            if (logged++ % 600 == 0) std::fprintf(stderr, "[RT] scene %zu triangles (frame %d)\n", g_rt_tris[i].size() / 9, logged);
        }
        { static int fl = 0; if (psp_rt_refl_active() && fl++ % 600 == 0) std::fprintf(stderr, "[RT] effects layer: %zu triangles\n", g_rt_fx_tris[i].size() / 9); }
        g_rt_tris[i].clear(); g_rt_cols[i].clear(); g_rt_fx_tris[i].clear(); g_rt_fx_cols[i].clear();
        g_fx_vote[i].n = 0;
        psp_screenfx_run(t.fbo, t.color_tex, t.w480 * t.rs, t.h272 * t.rs, s);
        g_rml_tris[i].clear(); g_rml_fighters[i] = false;
        glBindFramebuffer(GL_FRAMEBUFFER, g_targets[g_current_target].fbo);
        glc::invalidate();
        ge_texture_invalidate_bind_cache();
        ge_shader_note_external_program();
    }
}

static void ge_draw_prim_one(
    uint8_t* rdram,
    int prim_type,
    int count
) {
    {
        // PSPRECOMP_SKIN_DIAG=<n>[,<from>]: for the first n skinned draws (after prim #from) log vtype, weight count,
        // index count and for each matrix slot in use a short fingerprint plus how many PRIMs ago it was last uploaded.
        extern uint32_t g_ge_bone_slot_last[8], g_ge_skin_prim_seq;
        static const char* sd = std::getenv("PSPRECOMP_SKIN_DIAG");
        static int sd_n = sd ? std::atoi(sd) : 0;
        static int sd_from = (sd && std::strchr(sd, ',')) ? std::atoi(std::strchr(sd, ',') + 1) : 0;
        const GeState& s0 = ge_get_state();
        if (sd_n > 0 && g_ge_skin_prim_seq >= (uint32_t)sd_from && ge_vtype_weight(s0.vertex_type) != 0 && !ge_vtype_through(s0.vertex_type)) {
            sd_n--;
            const int wc = ((s0.vertex_type >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
            std::fprintf(stderr, "[SKIN] prim#%u vt=%08X nw=%d wt=%d stride=%d type=%d count=%d idx=%08X bonenum=%d |", g_ge_skin_prim_seq,
                         s0.vertex_type, wc, ge_vtype_weight(s0.vertex_type), ge_vertex_stride(s0.vertex_type), prim_type, count, s0.index_addr, s0.bone_mtx_num);
            for (int k = 0; k < std::min(wc, 8); k++) {
                const float* m = &s0.bone_matrix[k * 12];
                uint32_t h = 2166136261u; for (int i = 0; i < 12; i++) { uint32_t u; std::memcpy(&u, &m[i], 4); h = (h ^ u) * 16777619u; }
                std::fprintf(stderr, " s%d:%08X(age %u,t=%.2f,%.2f,%.2f)", k, h, g_ge_skin_prim_seq - g_ge_bone_slot_last[k], m[9], m[10], m[11]);
            }
            std::fprintf(stderr, "\n");
            {   // raw first vertices (as the GE would read them)
                const int stv = ge_vertex_stride(s0.vertex_type);
                const uint32_t va = s0.vertex_addr & 0x07FFFFFFu;
                std::fprintf(stderr, "[SKIN]   va=%08X raw:", s0.vertex_addr);
                for (int v = 0; v < 3; v++) { std::fprintf(stderr, " "); for (int i = 0; i < stv; i++) std::fprintf(stderr, "%02X", rdram[va + v * stv + i]); }
                std::fprintf(stderr, "\n");
            }
        }
        g_ge_skin_prim_seq++;
    }
    // Phase 11.4 / Plan 02 Task 2 — alternate-path GE PRIM investigation.
    //
    // Even when count <= 0 we tally [GE_PRIM_DETAIL] before the early-return
    // so the totals match the GE_SUMMARY total_prims figure exactly. The
    // counters here are NEVER rate-limited (they are scalar increments, no
    // I/O per call); the dump happens once in ge_prim_detail_atexit_summary.
    //
    // Tertiary finding from Plan 01 (DIAGNOSTIC §"Raw Log Excerpt"): the
    // GE pipeline processes 243 PRIM commands but only 57 [DRAW_PRIM] log
    // lines reach stderr (all clear=1). The pre-existing [DRAW_PRIM] log
    // rate-limit (first 50 + every 25th + first 5 non-clear) hides the
    // breakdown. This block counts EVERY call by (prim_type, clear) bucket.
    // (Counter definitions moved to file scope -- g_prim_* above -- so the
    // debug socket's I command can snapshot them via ge_draw_get_stats.)
    static bool prim_detail_atexit_installed = false;
    if (!prim_detail_atexit_installed) {
        static void (*const summary)() = []() {
            std::fprintf(stderr,
                "[GE_PRIM_DETAIL_SUMMARY] total_observed=%d zero_count=%d\n",
                g_prim_total_observed, g_prim_type_zero_count);
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                int normal = g_prim_type_count[t];
                int clear = g_prim_type_count_clear[t];
                clears += clear;
                if (t == 6) sprite_nc += normal; else real_ns += normal;
                if (normal == 0 && clear == 0) continue;
                std::fprintf(stderr,
                    "[GE_PRIM_DETAIL] type=%d normal=%d clear=%d total=%d\n",
                    t, normal, clear, normal + clear);
            }
            // Canonical one-line verdict for the verification harness.
            std::fprintf(stderr,
                "[GE_GEOM_VERDICT] real_nonsprite=%d sprite_nonclear=%d "
                "clears=%d => %s\n",
                real_ns, sprite_nc, clears,
                real_ns > 0 ? "GRAPHICS" : "NO-GRAPHICS");
        };
        std::atexit(summary); psp_on_quit(summary);
        prim_detail_atexit_installed = true;
    }

    // Self-test for the verification harness (scripts/verify_geometry.sh):
    // PSPRECOMP_GEOM_SELFTEST=1 synthesizes ONE real type-4 (TRIANGLE_STRIP,
    // non-clear, non-sprite) draw through the exact counter + sentinel path, so
    // the GRAPHICS detection can be proven end-to-end without the real asset bug
    // fixed. Proves the detector is not a constant-false oracle. No GL side
    // effects; runs once.
    static bool selftest_done = false;
    static const bool selftest_wanted = std::getenv("PSPRECOMP_GEOM_SELFTEST") != nullptr;   // read once: getenv per draw cost ~17% of the render thread on Android
    if (!selftest_done && selftest_wanted) {
        selftest_done = true;
        g_prim_type_count[GE_PRIM_TRIANGLE_STRIP]++;
        std::fprintf(stderr,
            "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
            "type=%d count=%d (success-bar (b) MET) [SELFTEST]\n",
            GE_PRIM_TRIANGLE_STRIP, 3);
        std::fflush(stderr);
    }

    psp_prof::Scope prof_prim(psp_prof::S_PRIM);
    g_prim_total_observed++;
    if (count <= 0) {
        g_prim_type_zero_count++;
        return;
    }

    static int prim_call_count = 0;
    prim_call_count++;

    const GeState& state = ge_get_state();
    {
        int t_idx = (prim_type >= 0 && prim_type < 8) ? prim_type : 7;
        if (state.clear_mode) {
            g_prim_type_count_clear[t_idx]++;
        } else {
            g_prim_type_count[t_idx]++;
        }

        // [GE_GEOM_VERDICT] live verification sentinels — the definitive answer
        // to "are graphics being set up?". These print DURING the run and are
        // flushed immediately, so they survive ANY exit path (SIGTERM, SIGKILL,
        // or a hung shutdown) — unlike the atexit [GE_PRIM_DETAIL_SUMMARY],
        // which only fires on a clean return from main(). Pure diagnostic
        // output: changes no game behavior.
        //
        // Success-bar half (b) == ONE real draw with type!=6 (non-sprite) AND
        // clear==0 (non-clear) AND normal>0. The line below appears in the log
        // if and only if that condition is ever met.
        static bool geom_first_real_logged = false;
        if (!state.clear_mode && t_idx != 6 && !geom_first_real_logged) {
            geom_first_real_logged = true;
            std::fprintf(stderr,
                "[GE_GEOM_REAL_DRAW] FIRST real geometry draw: "
                "type=%d count=%d (success-bar (b) MET)\n",
                prim_type, count);
            std::fflush(stderr);
        }

        // Periodic heartbeat: a killed or hung run still leaves a current tally
        // in the log (every 256 PRIMs observed). real_nonsprite>0 == graphics.
        // Off by default since 2026-10-22 (PSPRECOMP_DRAW_LOG=1): with fflush this and the [DRAW_PRIM] lines below
        // were ~4,000 unbuffered stderr writes/s in battle (piped through run.sh's tee to the terminal), which
        // stalled the GE thread on busy frames.
        static const bool draw_log = [] { const char* v = std::getenv("PSPRECOMP_DRAW_LOG"); return v && *v && *v != '0'; }();
        if (draw_log && g_prim_total_observed % 256 == 0) {
            int real_ns = 0, sprite_nc = 0, clears = 0;
            for (int t = 0; t < 8; ++t) {
                clears += g_prim_type_count_clear[t];
                if (t == 6) sprite_nc += g_prim_type_count[t];
                else real_ns += g_prim_type_count[t];
            }
            std::fprintf(stderr,
                "[GE_GEOM_HEARTBEAT] prims=%d real_nonsprite=%d "
                "sprite_nonclear=%d clears=%d\n",
                g_prim_total_observed, real_ns, sprite_nc, clears);
            std::fflush(stderr);
        }
    }

    // Log a wider window so non-clear PRIMs in the middle of a run are
    // not silently dropped. Also log every 25th PRIM after the first 50
    // and log the first non-clear PRIM unconditionally.
    static int non_clear_logged = 0;
    bool is_non_clear_first = (!state.clear_mode && non_clear_logged < 5);
    static const bool draw_log_every = [] { const char* v = std::getenv("PSPRECOMP_DRAW_LOG"); return v && *v && *v != '0'; }();
    if (prim_call_count <= 50
        || (draw_log_every && prim_call_count % 25 == 0)
        || is_non_clear_first) {
        std::fprintf(stderr,
            "[DRAW_PRIM] #%d type=%d count=%d clear=%d "
            "vtype=0x%06X vaddr=0x%08X\n",
            prim_call_count, prim_type, count,
            state.clear_mode ? 1 : 0,
            state.vertex_type, state.vertex_addr);
        if (is_non_clear_first) non_clear_logged++;
    }
    // PSPRECOMP_3D_SKIP_VTYPE=<hex>[,<hex>...]: debug aid, drop non-clear draws
    // with these exact vertex types (isolate which layer produces an artefact).
    if (!state.clear_mode) {
        static const char* skip_env = std::getenv("PSPRECOMP_3D_SKIP_VTYPE");
        if (skip_env) {
            char buf[256]; std::snprintf(buf, sizeof buf, "%s", skip_env);
            for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ","))
                if (static_cast<uint32_t>(std::strtoul(tok, nullptr, 16)) == state.vertex_type) return;
        }
    }
    // Framebuffer-as-texture lookup (VRAM texture address == a pool target).
    GLuint fbo_tex = 0;
    if (state.texture_enable) {
        uint32_t ta = state.tex_addr[0] |
                      (((state.tex_bufw[0] >> 16) & 0xFFu) << 24);
        if ((ta & 0x07E00000u) == 0x04000000u) {
            int tw = 0, th = 0;
            fbo_tex = ge_draw_find_target_texture(ge_fb_key(ta), &tw, &th);
        }
    }
    {   // PSPRECOMP_WIDE_SKIP=glow|soft|both (diagnostic): skip the glow-chain draws that composite a 480 px buffer onto the wide picture
        static const int skip = [] { const char* e = std::getenv("PSPRECOMP_WIDE_SKIP"); if (!e) return 0;
            return !std::strcmp(e, "both") ? 3 : !std::strcmp(e, "glow") ? 1 : !std::strcmp(e, "soft") ? 2 : 0; }();
        if (skip && fbo_tex && ge_vtype_through(state.vertex_type) && cur_w480() > 480 && g_fbtex_w480 == 480 && state.alpha_blend_enable) {
            if ((skip & 1) && state.blend_mode == 0x2AA) return;   // the additive strips
            if ((skip & 2) && state.blend_mode == 0x32) return;    // the alpha-blended full-screen pass
        }
    }
    // Widescreen: tell the vertex transform how this draw is placed (psp_ge_vertex.h)
    ge_set_wide(draw_wide_x(state), fbo_tex == 0, fbo_tex != 0);
    {
        const int h = cur_h272();
        const TallY b = band_map(h), x = draw_tall_y(state);
        ge_set_tall(b.s, b.t, x.s, x.t);
        if (g_hud_live && h > 272 && !hud_fixed_tex(state)) {   // HUD zones: the same mapping with the band placed at the top pin / the bottom edge
            const float s = b.s, t_top = s - 1.0f + 2.0f * float(std::max(0, h - 272 - g_hud_top_pin)) / float(h), t_bot = s - 1.0f;
            ge_set_tall_hud(g_hud_top, t_top, g_hud_bottom, t_bot);
        } else ge_set_tall_hud(0, 0.0f, 0, 0.0f);
        ge_set_tall_cover_textured(h > 272 && g_band_off > 0);   // in battle (the picture centred)
    }
    g_tall_fb_draw = fbo_tex && ge_vtype_through(state.vertex_type) && cur_h272() > 272;
    if (fbo_tex && g_fbtex_w480 > 480) {   // the shader multiplies u by declared_width / 480 afterwards (ge_shader_set_tex_remap): pre-divide the offset
        const float dw = float(1 << (state.tex_size[0] & 0xFF));
        ge_set_fb_u(480.0f / float(g_fbtex_w480), 0.5f * float(g_fbtex_w480 - 480) / float(g_fbtex_w480) * (480.0f / dw), dw / 480.0f);
    }
    else ge_set_fb_u(1.0f, 0.0f, fbo_tex ? float(1 << (state.tex_size[0] & 0xFF)) / 480.0f : 1.0f);
    // PSPRECOMP_DRAW_TRACE=1: GE state behind each non-clear PRIM (first 64).
    // PSPRECOMP_DRAW_TRACE_FROM=<draw#>: only trace draws numbered >= N (to
    // reach the battle), PSPRECOMP_DRAW_TRACE_MAX=<n>: how many to trace.
    static int draw_trace_n = 0;
    static const int trace_from = std::getenv("PSPRECOMP_DRAW_TRACE_FROM")
        ? std::atoi(std::getenv("PSPRECOMP_DRAW_TRACE_FROM")) : 0;
    static const int trace_max = std::getenv("PSPRECOMP_DRAW_TRACE_MAX")
        ? std::atoi(std::getenv("PSPRECOMP_DRAW_TRACE_MAX")) : 100000;
    static const bool draw_trace_env = std::getenv("PSPRECOMP_DRAW_TRACE") != nullptr;
    const bool draw_trace = draw_trace_env && prim_call_count >= trace_from;
    if (draw_trace && !state.clear_mode && draw_trace_n < trace_max) {
        draw_trace_n++;
        std::fprintf(stderr,
            "[DRAW_TRACE] #%d type=%d count=%d vtype=0x%06X tex=%d fmt=%u addr=0x%08X size=0x%X clutfmt=0x%X clutaddr=0x%08X blend=%d atest=%d(0x%X) fb=0x%08X tfunc=%u rgba=%d dbl=%d mask=%06X/%02X vp=%u%s\n",
            prim_call_count, prim_type, count, state.vertex_type,
            state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0], (unsigned)state.tex_size[0],
            (unsigned)state.clut_format, (unsigned)((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)), state.alpha_blend_enable ? 1 : 0,
            state.alpha_test_enable ? 1 : 0, (unsigned)state.alpha_test, state.framebuf_ptr, (unsigned)state.tex_func, state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0,
            state.mask_rgb, state.mask_alpha & 0xFF, (unsigned)state.viewport_x_scale,
            fbo_tex ? " fbotex=1" : "");
    }

    if (g_ftrace_frames.load(std::memory_order_relaxed) > 0 && !state.clear_mode) {
        const GeViewportDepth fvp = compute_state_viewport_depth(state);
        g_ftrace_drawn = true;
        std::fprintf(stderr, "[FTRACE] #%d type=%d n=%d vtype=0x%06X thru=%d dest=0x%08X(w%d, curkey=%06X) tex=%d addr=0x%08X fbotex=%d(w480=%d) blend=%d mode=0x%X fixA=%06X vp=%d,%d,%d,%d wx=%.3f\n",
            prim_call_count, prim_type, count, state.vertex_type, ge_vtype_through(state.vertex_type) ? 1 : 0, state.framebuf_ptr,
            cur_w480(), g_current_target >= 0 ? g_targets[g_current_target].key : 0xFFFFFFu, state.texture_enable ? 1 : 0, state.tex_addr[0],
            fbo_tex ? 1 : 0, fbo_tex ? g_fbtex_w480 : 0, state.alpha_blend_enable ? 1 : 0, (unsigned)state.blend_mode, (unsigned)state.blend_fix_a,
            fvp.x, fvp.y, fvp.w, fvp.h, draw_wide_x(state));
    }
    if (psp_screenfx_active()) fx_track(state, rdram, prim_type, count);
    else { main_camera_vote(state); if (g_fxl_flag || g_rml_flag) { flush_batch(); g_fxl_flag = g_rml_flag = false; } }
    static const bool draw_trace_mat_env = std::getenv("PSPRECOMP_DRAW_TRACE_MAT") != nullptr;
    if (!state.clear_mode && prim_type == 3 && count > 30 && draw_trace_mat_env) {
        static int mat_n = 0;
        if ((mat_n++ % 3000) == 0 && mat_n < 200000) {
            std::fprintf(stderr, "[MAT] #%d W:", prim_call_count);
            for (int i = 0; i < 12; i++) std::fprintf(stderr, " %g", state.world_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   V:");
            for (int i = 0; i < 12; i++) std::fprintf(stderr, " %g", state.view_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   P:");
            for (int i = 0; i < 16; i++) std::fprintf(stderr, " %g", state.proj_matrix[i]);
            std::fprintf(stderr, "\n[MAT]   vp scale %g %g %g ctr %g %g %g\n", state.viewport_x_scale, state.viewport_y_scale, state.viewport_z_scale, state.viewport_x_center, state.viewport_y_center, state.viewport_z_center);
        }
    }
    // Clear mode: use glClear instead of drawing
    if (state.clear_mode) {
        flush_batch();
        psp_prof::Scope prof_clear(psp_prof::S_CLEAR);
        psp_prof::clears++;
        // Decode first vertex for clear color
        std::vector<DecodedVertex> clear_verts;
        ge_decode_vertices(
            rdram, state, prim_type, count, clear_verts);
        if (!clear_verts.empty()) {
            float r = clear_verts[0].color[0] / 255.0f;
            float g = clear_verts[0].color[1] / 255.0f;
            float b = clear_verts[0].color[2] / 255.0f;
            float a = clear_verts[0].color[3] / 255.0f;
            if (prim_call_count <= 20 || draw_trace) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] #%d color=(%0.2f,%0.2f,%0.2f,%0.2f) "
                    "flags=0x%X verts=%zu fb=0x%08X x[%.0f,%.0f] y[%.0f,%.0f]\n",
                    prim_call_count, r, g, b, a, state.clear_flags,
                    clear_verts.size(), state.framebuf_ptr,
                    clear_verts[0].pos[0], clear_verts.back().pos[0], clear_verts[0].pos[1], clear_verts.back().pos[1]);
            }
            glClearColor(r, g, b, a);
            if (prim_call_count <= 5) {
                std::fprintf(stderr,
                    "[DRAW_CLEAR] glClearColor(%.3f, %.3f, %.3f, %.3f) "
                    "flags=0x%X\n",
                    r, g, b, a, state.clear_flags);
            }
        }

        GLbitfield clear_bits = 0;
        if (state.clear_flags & 1)
            clear_bits |= GL_COLOR_BUFFER_BIT;
        if (state.clear_flags & 2)
            clear_bits |= GL_STENCIL_BUFFER_BIT;
        if (state.clear_flags & 4)
            clear_bits |= GL_DEPTH_BUFFER_BIT;
        if (clear_bits == 0)
            clear_bits = GL_COLOR_BUFFER_BIT
                         | GL_DEPTH_BUFFER_BIT;

        // Clear depth: the PSP writes the clear rectangle's Z into the depth
        // buffer. GL's window depth uses the same 0..65535 encoding as the
        // PSP (see map_depth_func), so clear to that Z directly. (Falls back
        // to the active range's far value if the clear verts are unavailable.)
        if (clear_bits & GL_DEPTH_BUFFER_BIT) {
            double cz;
            if (!clear_verts.empty()) {
                cz = clear_verts[0].pos[2] / 65535.0;
            } else {
                cz = compute_state_viewport_depth(state).far_z;
            }
            if (cz < 0.0) cz = 0.0;
            if (cz > 1.0) cz = 1.0;
            glClearDepth(cz);
            if (g_current_target >= 0) g_fx_clearz[g_current_target] = float(cz);
        }

        // Temporarily disable depth mask restriction. In clear mode the PSP
        // ignores MASKRGB/MASKALPHA (the clear flags pick the channels), so
        // reset the colour write mask a previous draw may have narrowed.
        gl_depth_mask(true);
        gl_color_mask(true, true, true, true);
        gl_stencil_mask(0xFF);
        gl_stencil_enable(false);
        if (clear_bits & GL_STENCIL_BUFFER_BIT) {
            // The clear writes the clear colour's alpha into the stencil/alpha plane.
            const int sv = clear_verts.empty() ? 0 : (int)clear_verts[0].color[3];
            glClearStencil(sv & 0xFF);
        }
        glClear(clear_bits);
        if (g_dyn_trace_n.load() > 0) {
            g_dyn_trace_n--;
            std::fprintf(stderr, "[DT] #%d CLEAR flags=%X bits=%X fb=0x%08X z=%.1f n=%d\n", prim_call_count,
                state.clear_flags, (unsigned)clear_bits, state.framebuf_ptr,
                clear_verts.empty() ? -1.0f : clear_verts[0].pos[2], count);
        }
        g_has_drawn_prims = true;  // Clear counts as drawing
        return;
    }

    if (g_fast_enabled && !g_ab_shadow && !g_debug_env && g_dyn_trace_n.load() <= 0) {
        psp_prof::draws++;
        fast_prim(rdram, state, prim_type, count, fbo_tex);
        return;
    }
    flush_batch();   // the immediate legacy path below must not be reordered

    // Decode vertices
    std::vector<DecodedVertex> decoded;
    {
        psp_prof::Scope ps(psp_prof::S_DECODE);
        ge_decode_vertices(
            rdram, state, prim_type, count, decoded);
    }
    if (decoded.empty()) return;

    {   // PSPRECOMP_CAM_LOG=1: log camera pose each time the view matrix changes
        static const bool cam_on = std::getenv("PSPRECOMP_CAM_LOG") != nullptr;
        static float last_v[12] = {};
        static int cam_n = 0;
        if (cam_on && !ge_vtype_through(state.vertex_type) && cam_n < 40000 &&
            std::memcmp(last_v, state.view_matrix, sizeof last_v) != 0) {
            std::memcpy(last_v, state.view_matrix, sizeof last_v);
            const float* M = state.view_matrix;
            float px = -(M[9]*M[0] + M[10]*M[1] + M[11]*M[2]);
            float py = -(M[9]*M[3] + M[10]*M[4] + M[11]*M[5]);
            float pz = -(M[9]*M[6] + M[10]*M[7] + M[11]*M[8]);
            static const auto t0 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            cam_n++;
            std::fprintf(stderr, "[CAM] t=%.0fms pos=(%.1f,%.1f,%.1f) zaxis=(%.3f,%.3f,%.3f) n=%d\n",
                ms, px, py, pz, M[2], M[5], M[8], count);
        }
    }

    // Transform
    {
        psp_prof::Scope ps(psp_prof::S_XFORM);
        tall_extend_strips(decoded, prim_type);
        ge_transform_vertices(decoded, state);
        rect_clip_to_ndc(decoded, state, prim_type);
    }
    static int vtx_trace_n = 0;
    if (draw_trace && vtx_trace_n < trace_max) {
        vtx_trace_n++;
        if (!ge_vtype_through(state.vertex_type)) {
            std::fprintf(stderr, "[MTX] #%d world=", prim_call_count);
            for (int k = 0; k < 12; k++) std::fprintf(stderr, "%g ", state.world_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d view=", prim_call_count);
            for (int k = 0; k < 12; k++) std::fprintf(stderr, "%g ", state.view_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d proj=", prim_call_count);
            for (int k = 0; k < 16; k++) std::fprintf(stderr, "%g ", state.proj_matrix[k]);
            std::fprintf(stderr, "\n[MTX] #%d vpz=%g/%g zrange=%u..%u cullface=%u\n", prim_call_count,
                state.viewport_z_scale, state.viewport_z_center, state.min_z, state.max_z, state.cull_face);
        }
        {
            uint32_t va = state.vertex_addr & 0x07FFFFFFu;
            std::fprintf(stderr, "[VTX_RAW] #%d vaddr=0x%08X stride=%d bytes=", prim_call_count, state.vertex_addr, ge_vertex_stride(state.vertex_type));
            for (int b = 0; b < 32; b++) std::fprintf(stderr, "%02X%s", rdram[va + b], (b % 4 == 3) ? " " : "");
            std::fprintf(stderr, "\n");
        }
        for (size_t i = 0; i < decoded.size() && i < 4; i++) {
            const DecodedVertex& v = decoded[i];
            std::fprintf(stderr, "[VTX_TRACE] #%d v%zu pos=(%.3f,%.3f,%.3f,w=%.3f) uv=(%.3f,%.3f) rgba=%02X%02X%02X%02X through=%d vp=(%.0f,%.0f,%.0f,%.0f) sc=(%u,%u,%u,%u) depth=%d(%u) cull=%d\n",
                prim_call_count, i, v.pos[0], v.pos[1], v.pos[2], v.w, v.uv[0], v.uv[1],
                v.color[0], v.color[1], v.color[2], v.color[3],
                (state.vertex_type & 0x800000) ? 1 : 0,
                state.viewport_x_center, state.viewport_y_center, state.viewport_x_scale, state.viewport_y_scale,
                (unsigned)(state.scissor1 & 0x3FF), (unsigned)((state.scissor1 >> 10) & 0x3FF), (unsigned)(state.scissor2 & 0x3FF), (unsigned)((state.scissor2 >> 10) & 0x3FF),
                state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func, state.cull_enable ? 1 : 0);
        }
    }

    // Handle RECTANGLES: expand 2 verts to 6 (two tris)
    std::vector<PackedVertex> packed;
    const uint64_t prof_pack_t0 = psp_prof::enabled ? psp_prof::now() : 0;
    if (prim_type == GE_PRIM_RECTANGLES) {
        // Each rectangle is 2 vertices (top-left, bottom-right)
        int rect_count = static_cast<int>(decoded.size()) / 2;   // (tall_extend_strips may have added sprites)
        packed.reserve(rect_count * 6);
        for (int r = 0; r < rect_count; r++) {
            int i0 = r * 2;
            int i1 = r * 2 + 1;
            if (i1 >= static_cast<int>(decoded.size()))
                break;

            const auto& v0 = decoded[i0];
            const auto& v1 = decoded[i1];
            if (v0.w <= 0.0f || v1.w <= 0.0f) continue;

            // Build 4 corners from 2 vertices
            PackedVertex corners[4];
            // v0 (top-left)
            corners[0] = {
                {v0.pos[0], v0.pos[1], v0.pos[2], 1.0f},
                {v0.uv[0], v0.uv[1]},
                {v0.color[0], v0.color[1],
                 v0.color[2], v0.color[3]}
            };
            // top-right
            corners[1] = {
                {v1.pos[0], v0.pos[1], v0.pos[2], 1.0f},
                {v1.uv[0], v0.uv[1]},
                {v1.color[0], v1.color[1],
                 v1.color[2], v1.color[3]}
            };
            // bottom-right (v1)
            corners[2] = {
                {v1.pos[0], v1.pos[1], v1.pos[2], 1.0f},
                {v1.uv[0], v1.uv[1]},
                {v1.color[0], v1.color[1],
                 v1.color[2], v1.color[3]}
            };
            // bottom-left
            corners[3] = {
                {v0.pos[0], v1.pos[1], v1.pos[2], 1.0f},
                {v0.uv[0], v1.uv[1]},
                {v0.color[0], v0.color[1],
                 v0.color[2], v0.color[3]}
            };

            // Two triangles: 0-1-2 and 0-2-3
            packed.push_back(corners[0]);
            packed.push_back(corners[1]);
            packed.push_back(corners[2]);
            packed.push_back(corners[0]);
            packed.push_back(corners[2]);
            packed.push_back(corners[3]);
        }
    } else {
        // Pack decoded vertices
        packed.resize(decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) {
            const auto& v = decoded[i];
            packed[i] = {
                {v.pos[0], v.pos[1], v.pos[2], v.w},
                {v.uv[0], v.uv[1]},
                {v.color[0], v.color[1],
                 v.color[2], v.color[3]}
            };
        }
    }

    if (psp_prof::enabled) psp_prof::ns[psp_prof::S_PACK] += psp_prof::now() - prof_pack_t0;
    if (packed.empty()) return;
    psp_prof::draws++;
    psp_prof::verts += packed.size();
    if (psp_prof::enabled) {
        const bool thr = ge_vtype_through(state.vertex_type);
        const bool skin = ge_vtype_weight(state.vertex_type) != 0;
        const int c = thr ? 0 : (state.lighting_enable ? (skin ? 4 : 3) : (skin ? 2 : 1));
        psp_prof::cat_draws[c]++; psp_prof::cat_verts[c] += packed.size();
        {
            static GeState prev{}; static bool have = false;
            auto d = [&](const void* a, const void* b, size_t n, int g) { if (have && std::memcmp(a, b, n)) psp_prof::grp_changes[g]++; };
            const GeState& a = state; const GeState& b = prev;
            d(a.world_matrix, b.world_matrix, sizeof a.world_matrix, psp_prof::G_WORLD);
            d(a.view_matrix, b.view_matrix, sizeof a.view_matrix, psp_prof::G_VIEW);
            d(a.proj_matrix, b.proj_matrix, sizeof a.proj_matrix, psp_prof::G_PROJ);
            if (skin) d(a.bone_matrix, b.bone_matrix, sizeof a.bone_matrix, psp_prof::G_BONES);
            { uint32_t x[8] = {a.tex_addr[0], a.tex_bufw[0], a.tex_size[0], a.tex_format, a.tex_mode, a.clut_addr, a.clut_addr_upper, a.clut_format};
              uint32_t y[8] = {b.tex_addr[0], b.tex_bufw[0], b.tex_size[0], b.tex_format, b.tex_mode, b.clut_addr, b.clut_addr_upper, b.clut_format};
              d(x, y, sizeof x, psp_prof::G_TEX); }
            { uint32_t x[6] = {a.tex_func | (a.tex_func_rgba << 8) | (a.tex_func_double << 9), a.tex_filter, a.tex_wrap, a.tex_env_color, a.texture_enable, 0};
              uint32_t y[6] = {b.tex_func | (b.tex_func_rgba << 8) | (b.tex_func_double << 9), b.tex_filter, b.tex_wrap, b.tex_env_color, b.texture_enable, 0};
              d(x, y, sizeof x, psp_prof::G_TEXFN); }
            { uint32_t x[5] = {a.alpha_blend_enable, a.blend_mode, a.blend_fix_a, a.blend_fix_b, 0};
              uint32_t y[5] = {b.alpha_blend_enable, b.blend_mode, b.blend_fix_a, b.blend_fix_b, 0};
              d(x, y, sizeof x, psp_prof::G_BLEND); }
            { uint32_t x[10] = {a.depth_test_enable, a.depth_func, a.depth_write_disable, a.cull_enable, a.cull_face, a.mask_rgb, a.mask_alpha, a.alpha_test_enable, a.alpha_test, a.color_test_enable};
              uint32_t y[10] = {b.depth_test_enable, b.depth_func, b.depth_write_disable, b.cull_enable, b.cull_face, b.mask_rgb, b.mask_alpha, b.alpha_test_enable, b.alpha_test, b.color_test_enable};
              d(x, y, sizeof x, psp_prof::G_DEPTH); }
            d(&a.lighting_enable, &b.lighting_enable, (const char*)&a.light_spc[4] - (const char*)&a.lighting_enable, psp_prof::G_LIGHT);
            d(&a.vertex_type, &b.vertex_type, 4, psp_prof::G_VTYPE);
            d(&a.viewport_x_scale, &b.viewport_x_scale, 24, psp_prof::G_VP);
            d(&a.tex_scale_u, &b.tex_scale_u, 16, psp_prof::G_UVX);
            prev = a; have = true;
        }
        static uint64_t last_tex_key = ~0ull;
        const uint64_t k = ((uint64_t)state.tex_addr[0] << 32) ^ state.tex_size[0] ^ ((uint64_t)state.clut_addr << 8) ^ state.tex_format;
        if (k == last_tex_key) psp_prof::same_tex_draws++;
        last_tex_key = k;
    }

    // Upload to VBO
    const uint64_t prof_up_t0 = psp_prof::enabled ? psp_prof::now() : 0;
    gl_bind_vao(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 packed.size() * sizeof(PackedVertex),
                 packed.data(),
                 GL_STREAM_DRAW);
    if (psp_prof::enabled) psp_prof::ns[psp_prof::S_UPLOAD] += psp_prof::now() - prof_up_t0;

    // Pipeline state (program/uniforms/texture/blend/depth/cull) via the
    // cached setters shared with the batched path.
    apply_gl_pipeline(rdram, state, prim_type, fbo_tex, false);
    // Draw
    GLenum gl_prim = map_prim_type(prim_type);

    // PSPRECOMP_3D_DIAG=<period>: for every <period>-th non-through draw,
    // log the full pipeline state, the first vertices' clip/NDC position and
    // (via an occlusion query) how many fragments actually passed.
    static const int diag_period = std::getenv("PSPRECOMP_3D_DIAG")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG")) : 0;
    static const int diag_from = std::getenv("PSPRECOMP_3D_DIAG_FROM")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_FROM")) : 0;
    static const int diag_minn = std::getenv("PSPRECOMP_3D_DIAG_MINN")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_MINN")) : 0;
    static int diag_n = 0, diag_logged = 0;
    const bool diag = diag_period > 0 && !ge_vtype_through(state.vertex_type)
        && prim_call_count >= diag_from && count >= diag_minn
        && (diag_n++ % diag_period) == 0 && diag_logged < 600;
    GLuint diag_q = 0;
    if (diag) {
        diag_logged++;
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        std::fprintf(stderr,
            "[D3] #%d type=%d n=%d vtype=0x%06X fb=0x%08X glvp=(%d,%d,%d,%d) z=%.3f..%.3f off=(0x%X,0x%X) "
            "zt=%d zf=%u zwd=%d cull=%d/%u tex=%d fmt=%u addr=0x%08X fbotex=%d light=%d blend=%d atest=%d "
            "mask=%06X/%02X bm=0x%X atest=0x%X tfunc=%u rgba=%d dbl=%d tsz=0x%X lmode?\n",
            prim_call_count, prim_type, count, state.vertex_type, state.framebuf_ptr,
            vp.x, vp.y, vp.w, vp.h, vp.near_z, vp.far_z, state.offset_x, state.offset_y,
            state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func,
            state.depth_write_disable ? 1 : 0, state.cull_enable ? 1 : 0, state.cull_face,
            state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0], fbo_tex ? 1 : 0,
            state.lighting_enable ? 1 : 0, state.alpha_blend_enable ? 1 : 0,
            state.alpha_test_enable ? 1 : 0, state.mask_rgb, state.mask_alpha & 0xFF,
            (unsigned)state.blend_mode, (unsigned)state.alpha_test, (unsigned)state.tex_func,
            state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0, (unsigned)state.tex_size[0]);
        for (size_t i = 0; i < packed.size() && i < 3; i++) {
            const PackedVertex& p = packed[i];
            const float w = p.pos[3];
            std::fprintf(stderr,
                "[D3]   v%zu clip=(%.3f,%.3f,%.3f,%.3f) ndc=(%.3f,%.3f,%.3f) uv=(%.3f,%.3f) rgba=%d,%d,%d,%d\n",
                i, p.pos[0], p.pos[1], p.pos[2], w,
                p.pos[0] / w, p.pos[1] / w, p.pos[2] / w, p.uv[0], p.uv[1],
                (int)p.color[0], (int)p.color[1], (int)p.color[2], (int)p.color[3]);
        }
        if (std::getenv("PSPRECOMP_3D_DIAG_MAT")) {
            auto pm = [&](const char* n, const float* m, int c) {
                std::fprintf(stderr, "[D3]   %s:", n);
                for (int k = 0; k < c; k++) std::fprintf(stderr, " %g", m[k]);
                std::fprintf(stderr, "\n");
            };
            pm("W", state.world_matrix, 12); pm("V", state.view_matrix, 12); pm("P", state.proj_matrix, 16);
            for (int b = 0; b < 3; b++) { char nm[8]; std::snprintf(nm, 8, "B%d", b); pm(nm, &state.bone_matrix[b * 12], 12); }
        }
        glGenQueries(1, &diag_q);
        glBeginQuery(GL_SAMPLES_PASSED, diag_q);
    }
    static const int big_thresh = std::getenv("PSPRECOMP_3D_DIAG_BIG")
        ? std::atoi(std::getenv("PSPRECOMP_3D_DIAG_BIG")) : 0;
    static int big_logged = 0;
    GLuint big_q = 0;
    const bool big = big_thresh > 0 && !diag && !ge_vtype_through(state.vertex_type)
        && prim_call_count >= diag_from && big_logged < 300;
    if (big) { glGenQueries(1, &big_q); glBeginQuery(GL_SAMPLES_PASSED, big_q); }
    GLuint dt_q = 0;
    static const char* dt_only_env = std::getenv("PSPRECOMP_DT_ONLY_VT");
    static const uint32_t dt_only_vt = dt_only_env ? (uint32_t)std::strtoul(dt_only_env, nullptr, 16) : 0;
    static const uint32_t dt_only_vt2 = (dt_only_env && std::strchr(dt_only_env, ',')) ? (uint32_t)std::strtoul(std::strchr(dt_only_env, ',') + 1, nullptr, 16) : dt_only_vt;
    const bool dt = g_dyn_trace_n.load() > 0 && (!dt_only_env || g_xp_nofilter.load() || state.vertex_type == dt_only_vt || state.vertex_type == dt_only_vt2);
    if (dt) { g_dyn_trace_n--; glGenQueries(1, &dt_q); glBeginQuery(GL_SAMPLES_PASSED, dt_q); }
    {
        psp_prof::Scope ps(psp_prof::S_DRAW);
        if (g_no_raster != 2)
            glDrawArrays(gl_prim, 0,
                         static_cast<GLsizei>(packed.size()));
    }
    if (dt) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(dt_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &dt_q);
        const GeViewportDepth vp = compute_state_viewport_depth(state);
        const PackedVertex& p0 = packed[0];
        if (state.lighting_enable != 0 || true) {
            static uint64_t last_hash = 0;
            uint64_t h = state.ambient_color * 31u + state.ambient_alpha * 7u + state.material_emissive * 13u + state.material_ambient * 17u + state.material_diffuse * 19u + state.material_alpha * 23u + state.material_update * 29u + (state.lighting_enable?1:0) + (state.light_enable[0]?2:0) + (state.light_enable[1]?4:0)+ (state.light_enable[2]?8:0)+ (state.light_enable[3]?16:0) + state.light_amb[0] * 37u + state.light_dif[0] * 41u;
            if (h != last_hash) {
                last_hash = h;
                std::fprintf(stderr, "[DT]   LIGHT lit=%d en=%d%d%d%d amb=%06X/%02X emis=%06X mamb=%06X mdif=%06X malpha=%02X mupd=%u l0amb=%06X l0dif=%06X l0pos=%g,%g,%g type=%X\n",
                    state.lighting_enable?1:0, state.light_enable[0]?1:0, state.light_enable[1]?1:0, state.light_enable[2]?1:0, state.light_enable[3]?1:0,
                    state.ambient_color&0xFFFFFF, state.ambient_alpha&0xFF, state.material_emissive&0xFFFFFF, state.material_ambient&0xFFFFFF,
                    state.material_diffuse&0xFFFFFF, state.material_alpha&0xFF, state.material_update, state.light_amb[0]&0xFFFFFF, state.light_dif[0]&0xFFFFFF,
                    state.light_pos[0][0], state.light_pos[0][1], state.light_pos[0][2], (unsigned)state.light_type[0]);
            }
        }
        {
            static uint64_t last_m = 0;
            uint64_t mh = 1469598103934665603ull;
            auto mix = [&](const float* f, int n) { for (int i = 0; i < n; i++) { uint32_t u; std::memcpy(&u, &f[i], 4); mh = (mh ^ u) * 1099511628211ull; } };
            mix(state.world_matrix, 12); mix(state.view_matrix, 12); mix(state.proj_matrix, 16);
            if (mh != last_m) {
                last_m = mh;
                std::fprintf(stderr, "[DT]   MTX world=[");
                for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.world_matrix[i]);
                std::fprintf(stderr, "] view=[");
                for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.view_matrix[i]);
                std::fprintf(stderr, "] proj=[");
                for (int i = 0; i < 16; i++) std::fprintf(stderr, "%g ", state.proj_matrix[i]);
                std::fprintf(stderr, "]\n");
            }
        }
        if (ge_vtype_weight(state.vertex_type) != 0) {
            static uint64_t last_b = 0;
            const int nb = std::min<int>(((state.vertex_type >> GE_VTYPE_WEIGHTCOUNT_SHIFT) & GE_VTYPE_WEIGHTCOUNT_MASK) + 1, 8);
            uint64_t bh = 1469598103934665603ull;
            for (int i = 0; i < nb * 12; i++) { uint32_t u; std::memcpy(&u, &state.bone_matrix[i], 4); bh = (bh ^ u) * 1099511628211ull; }
            if (bh != last_b) {
                last_b = bh;
                for (int b = 0; b < nb; b++) {
                    std::fprintf(stderr, "[DT]   BONE%d=[", b);
                    for (int i = 0; i < 12; i++) std::fprintf(stderr, "%g ", state.bone_matrix[b * 12 + i]);
                    std::fprintf(stderr, "]\n");
                }
            }
        }
        if (state.texture_enable && (state.tex_format == 4 || state.tex_format == 5)) {
            static uint64_t seen[64]; static int nseen = 0;
            uint32_t tad = (state.tex_addr[0] | (((state.tex_bufw[0] >> 16) & 0xFFu) << 24)) & 0x07FFFFFFu;
            uint32_t cad = ((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)) & 0x07FFFFFFu;
            uint64_t key = ((uint64_t)tad << 32) | cad;
            bool is_new = true;
            for (int i = 0; i < nseen; i++) if (seen[i] == key) is_new = false;
            if (is_new && nseen < 64) {
                seen[nseen++] = key;
                std::fprintf(stderr, "[DT]   TEX addr=%08X fmt=%u size=%X bufw=%X mode=%X clut=%08X clf=%X texdata:", tad, (unsigned)state.tex_format,
                             (unsigned)state.tex_size[0], (unsigned)state.tex_bufw[0] & 0x7FF, (unsigned)state.tex_mode, cad, (unsigned)state.clut_format);
                for (int i = 0; i < 32; i++) std::fprintf(stderr, "%02X", rdram[tad + i]);
                std::fprintf(stderr, " clutdata:");
                for (int i = 0; i < 32; i++) std::fprintf(stderr, "%02X", rdram[cad + i]);
                unsigned nz = 0; for (int i = 0; i < 4096; i++) nz += rdram[tad + i] != 0;
                unsigned cz = 0; for (int i = 0; i < 1024; i++) cz += rdram[cad + i] != 0;
                std::fprintf(stderr, " nz_tex4k=%u nz_clut1k=%u atest=%X ablend=%d bmode=%X wrap=%X filt=%X tfunc=%u/%d/%d\n", nz, cz,
                             (unsigned)state.alpha_test, state.alpha_blend_enable ? 1 : 0, (unsigned)state.blend_mode, (unsigned)state.tex_wrap, (unsigned)state.tex_filter,
                             (unsigned)state.tex_func, state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0);
            }
        }
        std::fprintf(stderr, "[DT]   FB fbfmt=%u fbw=%u tex=%d texfmt=%u texbufw=0x%X texsize=0x%X\n", (unsigned)state.framebuf_format,
                     (unsigned)state.framebuf_width, state.texture_enable ? 1 : 0, (unsigned)state.tex_format,
                     (unsigned)state.tex_bufw[0], (unsigned)state.tex_size[0]);
        if (state.tex_map_mode != 0 || state.tex_shade_ls != 0)
            std::fprintf(stderr, "[DT]   TMAP mode=%06X shadels=%06X tgen=[%g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                         (unsigned)state.tex_map_mode, (unsigned)state.tex_shade_ls,
                         state.tgen_matrix[0], state.tgen_matrix[1], state.tgen_matrix[2], state.tgen_matrix[3],
                         state.tgen_matrix[4], state.tgen_matrix[5], state.tgen_matrix[6], state.tgen_matrix[7],
                         state.tgen_matrix[8], state.tgen_matrix[9], state.tgen_matrix[10], state.tgen_matrix[11]);
        if (packed.size() <= 6) {
            std::fprintf(stderr, "[DT]   VERTS");
            for (size_t i = 0; i < packed.size(); i++) {
                const PackedVertex& q = packed[i];
                std::fprintf(stderr, " (%.2f,%.2f,%.2f,%.2f|uv %.2f,%.2f|c %02X%02X%02X%02X)", q.pos[0], q.pos[1], q.pos[2], q.pos[3], q.uv[0], q.uv[1], q.color[0], q.color[1], q.color[2], q.color[3]);
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "[DT] #%d pc=%08X pass=%u t=%d n=%d vt=%06X fb=%08X vp=(%d,%d,%d,%d) z=%.3f..%.3f zt=%d zf=%u zwd=%d cull=%d/%u tex=%d fmt=%u ta=%08X cl=%08X/%X fbotex=%d bl=%d bm=%X at=%d mask=%06X/%02X v0=(%.1f,%.1f,%.4f,%.2f) col=%02X%02X%02X%02X va=%08X ia=%08X clf=%X ts=%X raw=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X st=%d/%06X/%06X tf=%u/%d/%d bfa=%06X bfb=%06X\n",
            prim_call_count, g_ge_cur_pc, passed, prim_type, count, state.vertex_type, state.framebuf_ptr,
            vp.x, vp.y, vp.w, vp.h, vp.near_z, vp.far_z, state.depth_test_enable?1:0, (unsigned)state.depth_func,
            state.depth_write_disable?1:0, state.cull_enable?1:0, state.cull_face, state.texture_enable?1:0,
            (unsigned)state.tex_format, state.tex_addr[0], (unsigned)((state.clut_addr & 0xFFFFFF) | ((state.clut_addr_upper << 8) & 0x0F000000)), (unsigned)state.clut_format, fbo_tex?1:0, state.alpha_blend_enable?1:0,
            (unsigned)state.blend_mode, state.alpha_test_enable?1:0, state.mask_rgb, state.mask_alpha&0xFF,
            p0.pos[0], p0.pos[1], p0.pos[2], p0.pos[3], p0.color[0], p0.color[1], p0.color[2], p0.color[3],
            state.vertex_addr, state.index_addr, (unsigned)state.clut_format, (unsigned)state.tex_size[0],
            rdram[(state.vertex_addr&0x07FFFFFF)+0], rdram[(state.vertex_addr&0x07FFFFFF)+1], rdram[(state.vertex_addr&0x07FFFFFF)+2], rdram[(state.vertex_addr&0x07FFFFFF)+3],
            rdram[(state.vertex_addr&0x07FFFFFF)+4], rdram[(state.vertex_addr&0x07FFFFFF)+5], rdram[(state.vertex_addr&0x07FFFFFF)+6], rdram[(state.vertex_addr&0x07FFFFFF)+7],
            rdram[(state.vertex_addr&0x07FFFFFF)+8], rdram[(state.vertex_addr&0x07FFFFFF)+9], rdram[(state.vertex_addr&0x07FFFFFF)+10], rdram[(state.vertex_addr&0x07FFFFFF)+11],
            state.stencil_test_enable?1:0, (unsigned)state.stencil_test&0xFFFFFF, (unsigned)state.stencil_op&0xFFFFFF, (unsigned)state.tex_func, state.tex_func_rgba?1:0, state.tex_func_double?1:0, (unsigned)state.blend_fix_a&0xFFFFFF, (unsigned)state.blend_fix_b&0xFFFFFF);
    }
    if (big) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(big_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &big_q);
        if (passed >= (GLuint)big_thresh) {
            big_logged++;
            const PackedVertex& p0 = packed[0];
            std::fprintf(stderr,
                "[BIG] #%d passed=%u type=%d n=%zu vtype=0x%06X fb=0x%08X tex=%d fmt=%u addr=0x%08X tsz=0x%X "
                "bm=0x%X blend=%d zt=%d zf=%u zwd=%d atest=%d/0x%X tfunc=%u rgba=%d dbl=%d light=%d "
                "v0 clip=(%.2f,%.2f,%.2f,%.2f) uv=(%.3f,%.3f) col=%d,%d,%d,%d amb=%06X/%02X mem=%06X mamb=%06X mdif=%06X malpha=%02X\n",
                prim_call_count, passed, prim_type, packed.size(), state.vertex_type, state.framebuf_ptr,
                state.texture_enable ? 1 : 0, (unsigned)state.tex_format, state.tex_addr[0],
                (unsigned)state.tex_size[0], (unsigned)state.blend_mode, state.alpha_blend_enable ? 1 : 0,
                state.depth_test_enable ? 1 : 0, (unsigned)state.depth_func, state.depth_write_disable ? 1 : 0,
                state.alpha_test_enable ? 1 : 0, (unsigned)state.alpha_test, (unsigned)state.tex_func,
                state.tex_func_rgba ? 1 : 0, state.tex_func_double ? 1 : 0, state.lighting_enable ? 1 : 0,
                p0.pos[0], p0.pos[1], p0.pos[2], p0.pos[3], p0.uv[0], p0.uv[1],
                (int)p0.color[0], (int)p0.color[1], (int)p0.color[2], (int)p0.color[3],
                state.ambient_color & 0xFFFFFF, state.ambient_alpha & 0xFF, state.material_emissive & 0xFFFFFF,
                state.material_ambient & 0xFFFFFF, state.material_diffuse & 0xFFFFFF, state.material_alpha & 0xFF);
        }
    }
    if (diag) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint passed = 0;
        glGetQueryObjectuiv(diag_q, GL_QUERY_RESULT, &passed);
        glDeleteQueries(1, &diag_q);
        std::fprintf(stderr, "[D3]   samples_passed=%u\n", passed);
    }
    g_has_drawn_prims = true;
}

// Service the pending screenshot/auto-capture from the given front FBO.
static void present_service_screenshot(GLuint fbo) {
    if (!g_screenshot_enabled || g_screenshot_done || !g_has_drawn_prims) {
        return;
    }
    std::vector<uint8_t> flipped;
    int cw = 480, chh = 272;
    read_fbo_native(fbo, flipped, &cw, &chh);
    // An active savedata dialog is composited over the frame at present time;
    // include it in the capture so screenshots match what the window shows.
    composite_dialog_overlay(flipped, cw, chh);
    composite_game_overlay(flipped, cw, chh);
    write_tga(g_screenshot_path, flipped.data(), cw, chh);
    g_screenshot_done = true;
}

struct PresentRect { int x0, y0, x1, y1; GLenum filter; bool bars; };

// ---- Window presentation: aspect fit / integer scale / stretch ------------
// PSPRECOMP_STRETCH=1 fills the window; PSPRECOMP_FILTER=nearest|linear|integer.
// PSPRECOMP_INTEGER_SCALE=1 (generic "retro" presentation): the picture is the
// largest whole multiple k of 480x272 that fits the drawable (k=2 -> 960x544
// centred with black borders on a 1280x800 Steam Deck panel), and the blit is
// nearest-neighbour whenever the render target is a whole multiple of the
// output (or vice versa), bilinear otherwise. PSPRECOMP_FILTER=integer gives the
// same layout but always nearest. Default: 480:272 aspect-fit with black bars;
// nearest when the fit is an exact integer multiple, bilinear otherwise. Computed on the *drawable* size
// (SDL_GL_GetDrawableSize: 2x the window size on Retina) and cached per size.
static bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

static int g_present_w480 = 480;   // width (PSP pixels) of the picture being shown: 480, or wider with widescreen
static int g_present_h272 = 272;   // height (PSP pixels): 272, or taller on a tall window (g_tall_px)
static PresentRect present_layout(SDL_Window* window) {
    static int cw = -1, ch = -1, cpw = -1, ctall = -1, cph = -1;
    const int pw = g_present_w480, ph = g_present_h272;
    static PresentRect cached{0, 0, 480, 272, GL_NEAREST, false};
    int dw = 0, dh = 0;
    psp_gl_drawable_size(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) { dw = pw; dh = ph; }
    const int tall = psp_touch_tall_layout(dw, dh) ? 1 : 0;   // touch controls on a tall window: the picture goes to the top
    if (dw == cw && dh == ch && pw == cpw && tall == ctall && ph == cph) return cached;
    cw = dw; ch = dh; cpw = pw; ctall = tall; cph = ph;
    const char* st = std::getenv("PSPRECOMP_STRETCH");
    const bool stretch = st && st[0] && st[0] != '0';
    const char* fl = std::getenv("PSPRECOMP_FILTER");
    std::string filter = fl ? fl : "";
    if (filter.empty() && env_on("PSPRECOMP_INTEGER_SCALE")) filter = "integer-scale";
    PresentRect r{0, 0, dw, dh, GL_NEAREST, false};
    bool exact_int = false;
    if (filter == "integer" || filter == "integer-scale") {
        int k = std::min(dw / pw, dh / ph);
        if (k >= 1) {
            int w = pw * k, h = ph * k;
            r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
            exact_int = true;
        } else {  // window smaller than native: fall back to a linear fit
            double s = std::min(dw / double(pw), dh / double(ph));
            int w = std::max(1, int(pw * s + 0.5)), h = std::max(1, int(ph * s + 0.5));
            r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
        }
    } else if (!stretch) {
        double s = std::min(dw / double(pw), dh / double(ph));
        int w = std::min(dw, std::max(1, int(pw * s + 0.5)));
        int h = std::min(dh, std::max(1, int(ph * s + 0.5)));
        r.x0 = (dw - w) / 2; r.y0 = (dh - h) / 2; r.x1 = r.x0 + w; r.y1 = r.y0 + h;
        exact_int = (w % pw == 0 && h % ph == 0 && w / pw == h / ph);
    } else {
        exact_int = (dw % pw == 0 && dh % ph == 0 && dw / pw == dh / ph);
    }
    if (tall && !stretch) {   // (GL coordinates: y1 is the top edge)
        const int ph = r.y1 - r.y0;
        r.y1 = dh; r.y0 = dh - ph;
    }
    if (filter == "nearest" || exact_int) r.filter = GL_NEAREST;
    else if (filter == "integer" && exact_int) r.filter = GL_NEAREST;
    else r.filter = GL_LINEAR;   // (integer-scale: present_blit decides from the render-scale ratio)
    r.bars = (r.x0 != 0 || r.y0 != 0 || r.x1 != dw || r.y1 != dh);
    cached = r;
    std::fprintf(stderr, "[PRESENT] drawable %dx%d -> dst (%d,%d)-(%d,%d) %s%s%s\n", dw, dh,
                 r.x0, r.y0, r.x1, r.y1, r.filter == GL_LINEAR ? "linear" : "nearest",
                 r.bars ? " +bars" : "", tall ? " top (tall window, touch controls under it)" : "");
    return r;
}

// Debug socket 'V <path.tga>': capture the whole window (drawable size).
static std::mutex g_wc_mutex;
static std::condition_variable g_wc_cv;
static int g_wc_state = 0;  // 0 idle, 1 pending, 2 done, 3 failed
static char g_wc_path[256];

bool ge_draw_capture_window(const char* path, int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_wc_mutex);
    if (g_wc_state == 1) return false;
    std::strncpy(g_wc_path, path, sizeof(g_wc_path) - 1);
    g_wc_path[sizeof(g_wc_path) - 1] = '\0';
    g_wc_state = 1;
    bool fin = g_wc_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [] { return g_wc_state >= 2; });
    bool ok = fin && g_wc_state == 2;
    g_wc_state = 0;
    return ok;
}

static void present_service_window_capture() {
    char path[sizeof(g_wc_path)];
    {
        std::lock_guard<std::mutex> lock(g_wc_mutex);
        if (g_wc_state != 1) return;
        std::memcpy(path, g_wc_path, sizeof(path));
    }
    SDL_Window* window = psp_get_sdl_window();
    int dw = 0, dh = 0;
    psp_gl_drawable_size(window, &dw, &dh);
    std::vector<uint8_t> px(size_t(dw) * dh * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < dh; y++)
        std::memcpy(fl.data() + size_t(y) * dw * 4, px.data() + size_t(dh - 1 - y) * dw * 4, size_t(dw) * 4);
    write_tga(path, fl.data(), dw, dh);
    std::lock_guard<std::mutex> lock(g_wc_mutex);
    if (g_wc_state == 1) { g_wc_state = 2; g_wc_cv.notify_all(); }
}

// Debug socket 'REC <n> <dir>': write each of the next n presented frames (whole window) as <dir>/f<NNNNN>_<ms>.tga, numbered from 0
// (<ms> = steady-clock milliseconds), for frame-by-frame checks of menu transitions. Test aid only; costs a read-back per frame while armed.
static std::atomic<int> g_rec_left{0};
static int g_rec_n = 0;
static char g_rec_dir[200];
void ge_draw_record_frames(int n, const char* dir) {
    std::lock_guard<std::mutex> lock(g_wc_mutex);
    std::strncpy(g_rec_dir, dir, sizeof(g_rec_dir) - 1); g_rec_dir[sizeof(g_rec_dir) - 1] = '\0';
    g_rec_n = 0; g_rec_left.store(n);
}
static void present_service_record() {
    if (g_rec_left.load(std::memory_order_relaxed) <= 0) return;
    SDL_Window* window = psp_get_sdl_window();
    int dw = 0, dh = 0;
    psp_gl_drawable_size(window, &dw, &dh);
    std::vector<uint8_t> px(size_t(dw) * dh * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < dh; y++)
        std::memcpy(fl.data() + size_t(y) * dw * 4, px.data() + size_t(dh - 1 - y) * dw * 4, size_t(dw) * 4);
    char path[260];
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::snprintf(path, sizeof path, "%s/f%05d_%lld.tga", g_rec_dir, g_rec_n++, ms);
    write_tga(path, fl.data(), dw, dh);
    g_rec_left.fetch_sub(1);
}

// Utility-dialog overlay (sceUtilitySavedata list): the HLE renders the
// dialog into an RGBA canvas at an integer multiple (1..4) of 480x272 chosen
// from the output size (psp_sddialog::set_output_scale, updated each present)
// and publishes it; while one is active it is blitted opaquely over the game
// frame, filtered linearly when it is not a 1:1 blit. GL texture/pixel-store
// state is saved and restored so the GE's own state tracking is not disturbed.
// The overlay texture drawn with its own alpha over the window (the settings screen's opening circle): a textured quad over the
// picture rectangle instead of the opaque blit.
// (The Vulkan layer's GLSL translator reads one in / out / uniform declaration per line: keep them on separate lines.)
static void draw_overlay_quad(GLuint tex, const PresentRect& pr, float u0, float v_top, float u1, float v_bot, bool blend);
static void draw_overlay_alpha(GLuint tex, const PresentRect& pr) { draw_overlay_quad(tex, pr, 0.f, 0.f, 1.f, 1.f, true); }
// A textured quad over the window rectangle `pr` (GL y up: y1 is the top): texture coordinates u0..u1 left to right, v_top at the
// top edge and v_bot at the bottom; `blend` = straight alpha, otherwise opaque.
static void draw_overlay_quad(GLuint tex, const PresentRect& pr, float u0, float v_top, float u1, float v_bot, bool blend) {
    static GLuint prog = 0, vao = 0, vbo = 0;
    static GLint u_tex = -1;
    static bool failed = false;
    if (failed) return;
    if (!prog) {
        const char* vs_body = "in vec2 a_pos;\nin vec2 a_uv;\nout vec2 v_uv;\n"
                              "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        const char* fs_body = "in vec2 v_uv;\nout vec4 o_col;\nuniform sampler2D u_tex;\n"
                              "void main() { o_col = texture(u_tex, v_uv); }\n";
        auto compile = [](GLenum type, const char* body) {
            const char* src[2] = {PSP_GLSL_VERSION, body};
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 2, src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[512] = {}; glGetShaderInfoLog(sh, sizeof log, nullptr, log); std::fprintf(stderr, "[OVERLAY] alpha shader compile failed: %s\n", log); glDeleteShader(sh); return GLuint(0); }
            return sh;
        };
        GLuint vs = compile(GL_VERTEX_SHADER, vs_body), fs = compile(GL_FRAGMENT_SHADER, fs_body);
        if (!vs || !fs) { failed = true; return; }
        prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "a_pos"); glBindAttribLocation(prog, 1, "a_uv");
        glLinkProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { { char log[1024] = {}; glGetProgramInfoLog(prog, sizeof log, nullptr, log); std::fprintf(stderr, "[OVERLAY] alpha program link failed: %s\n", log); } glDeleteProgram(prog); prog = 0; failed = true; return; }
        u_tex = glGetUniformLocation(prog, "u_tex");
        GLint prev_vao = 0, prev_abo = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindBuffer(GL_ARRAY_BUFFER, GLuint(prev_abo));
        glBindVertexArray(GLuint(prev_vao));
    }
    int dw = 0, dh = 0;
    psp_gl_drawable_size(psp_get_sdl_window(), &dw, &dh);
    if (dw <= 0 || dh <= 0) return;
    auto cx = [&](float x) { return x / float(dw) * 2.f - 1.f; };
    auto cy = [&](float y) { return y / float(dh) * 2.f - 1.f; };
    const float x0 = float(std::min(pr.x0, pr.x1)), x1 = float(std::max(pr.x0, pr.x1));
    const float yb = float(std::min(pr.y0, pr.y1)), yt = float(std::max(pr.y0, pr.y1));
    const float verts[] = {   // (canvas images: texture row 0 = canvas top = the top edge of the quad)
        cx(x0), cy(yt), u0, v_top,
        cx(x1), cy(yt), u1, v_top,
        cx(x0), cy(yb), u0, v_bot,
        cx(x1), cy(yb), u1, v_bot,
    };
    GLint prev_prog = 0, prev_vao = 0, prev_abo = 0, prev_vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, prev_fbo = 0, prev_tex = 0, prev_active = 0;
    GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST), prev_cull = glIsEnabled(GL_CULL_FACE),
              prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean cmask[4];
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUseProgram(prog);
    glUniform1i(u_tex, 0);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(prev_abo));
    glBindVertexArray(GLuint(prev_vao));
    glUseProgram(GLuint(prev_prog));
    glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prev_fbo));
    glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
    glActiveTexture(GLenum(prev_active));
    glc::invalidate();
    ge_shader_note_external_program();
}

// ---- Game layer (ge_draw_set_game_layer) ----
static std::mutex g_gl_mu;
static std::vector<uint32_t> g_gl_px;
static int g_gl_w = 0, g_gl_h = 0;
static bool g_gl_dirty = false;
static bool g_gl_full = true;                 // the whole image must go to the texture (new size, new life); else only g_gl_dr
static int g_gl_dr[4] = {0, 0, -1, -1};      // dirty rectangle (inclusive x0, y0, x1, y1): what differs from the texture's content
static const bool g_gl_old_upload = std::getenv("PSPRECOMP_MENUFPS_OLD") != nullptr;   // 26 Nov: upload the whole image every time, as before
static std::chrono::steady_clock::time_point g_gl_time;
static int g_gl_next_ttl = 150, g_gl_ttl = 150;   // ms the layer stays without a new publish (ge_draw_set_game_layer_ttl: for the next publish only)
void ge_draw_set_game_layer_ttl(int ms) { std::lock_guard<std::mutex> lk(g_gl_mu); g_gl_next_ttl = std::clamp(ms, 50, 2000); }
static std::vector<GeLayerCopy> g_gl_copies;
static uint64_t g_gl_serial = 0;
uint64_t ge_draw_game_layer_serial() { std::lock_guard<std::mutex> lk(g_gl_mu); return g_gl_serial; }
bool ge_draw_game_layer_touch() {
    std::lock_guard<std::mutex> lk(g_gl_mu);
    const auto now = std::chrono::steady_clock::now();
    if (g_gl_w <= 0 || now - g_gl_time > std::chrono::milliseconds(g_gl_ttl)) return false;
    g_gl_time = now; g_gl_ttl = g_gl_next_ttl; g_gl_next_ttl = 150;
    return true;
}
void ge_draw_set_game_layer(const uint32_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) { ge_draw_clear_game_layer(); return; }
    std::lock_guard<std::mutex> lk(g_gl_mu);
    // 26 Nov (menus at 10 fps on the Deck): when the image has the shape of the one held, only the rows / columns that differ are copied and uploaded
    if (!g_gl_old_upload && !g_gl_full && g_gl_w == w && g_gl_h == h && g_gl_px.size() == size_t(w) * size_t(h)) {
        int y0 = -1, y1 = -1, x0 = w, x1 = -1;
        for (int y = 0; y < h; y++) {
            const uint32_t* a = rgba + size_t(y) * size_t(w); uint32_t* b = g_gl_px.data() + size_t(y) * size_t(w);
            if (std::memcmp(a, b, size_t(w) * 4) == 0) continue;
            int lo = 0, hi = w - 1; while (a[lo] == b[lo]) lo++; while (a[hi] == b[hi]) hi--;
            std::memcpy(b + lo, a + lo, size_t(hi - lo + 1) * 4);
            if (y0 < 0) y0 = y; y1 = y; x0 = std::min(x0, lo); x1 = std::max(x1, hi);
        }
        if (y0 >= 0) {
            if (g_gl_dirty && g_gl_dr[2] >= g_gl_dr[0]) { x0 = std::min(x0, g_gl_dr[0]); y0 = std::min(y0, g_gl_dr[1]); x1 = std::max(x1, g_gl_dr[2]); y1 = std::max(y1, g_gl_dr[3]); }
            g_gl_dr[0] = x0; g_gl_dr[1] = y0; g_gl_dr[2] = x1; g_gl_dr[3] = y1; g_gl_dirty = true;
        }
        g_gl_serial++;
        g_gl_time = std::chrono::steady_clock::now(); g_gl_ttl = g_gl_next_ttl; g_gl_next_ttl = 150;
        return;
    }
    g_gl_px.assign(rgba, rgba + size_t(w) * size_t(h));
    g_gl_full = false; g_gl_dr[0] = 0; g_gl_dr[1] = 0; g_gl_dr[2] = w - 1; g_gl_dr[3] = h - 1;
    g_gl_w = w; g_gl_h = h; g_gl_dirty = true; g_gl_serial++;
    g_gl_time = std::chrono::steady_clock::now(); g_gl_ttl = g_gl_next_ttl; g_gl_next_ttl = 150;
}
void ge_draw_clear_game_layer() {
    std::lock_guard<std::mutex> lk(g_gl_mu);
    g_gl_px.clear(); g_gl_w = g_gl_h = 0; g_gl_dirty = true; g_gl_full = true; g_gl_serial++;
    g_gl_copies.clear();
}
void ge_draw_set_game_layer_copies(const GeLayerCopy* c, int n) {
    std::lock_guard<std::mutex> lk(g_gl_mu);
    g_gl_copies.assign(c, c + std::max(0, n));
}
// Redraws parts of the shown picture elsewhere (ge_draw_set_game_layer_copies), straight from the game's colour target `src_tex`
// (sw x sh texels, GL rows bottom up, the PSP's 480x272 part where the band and wide extension put it), into the PSP area `band`.
static void present_game_layer_copies(GLuint src_tex, int spw, int h272, const PresentRect& band) {
    std::vector<GeLayerCopy> copies;
    {
        std::lock_guard<std::mutex> lk(g_gl_mu);
        if (g_gl_w <= 0 || std::chrono::steady_clock::now() - g_gl_time > std::chrono::milliseconds(g_gl_ttl)) return;
        copies = g_gl_copies;
    }
    if (copies.empty() || !src_tex || spw <= 0 || h272 <= 0) return;
    const float ox = (spw - 480) / 2.f, bb = float(band_b(h272));
    const float bw = float(band.x1 - band.x0), bh = float(band.y1 - band.y0);
    for (const GeLayerCopy& c : copies) {
        PresentRect d = band;
        d.x0 = band.x0 + int(c.dx0 / 480.f * bw + 0.5f); d.x1 = band.x0 + int(c.dx1 / 480.f * bw + 0.5f);
        d.y1 = band.y1 - int(c.dy0 / 272.f * bh + 0.5f); d.y0 = band.y1 - int(c.dy1 / 272.f * bh + 0.5f);
        if (d.x1 <= d.x0 || d.y1 <= d.y0) continue;
        const float u0 = (c.sx0 + ox) / spw, u1 = (c.sx1 + ox) / spw;
        const float vt = (bb + 272.f - c.sy0) / h272, vb = (bb + 272.f - c.sy1) / h272;
        draw_overlay_quad(src_tex, d, u0, vt, u1, vb, false);
    }
}
static void draw_overlay_alpha(GLuint tex, const PresentRect& pr);
// Draws the game layer over the PSP area `band` (render thread, inside a present).
static void present_game_layer(const PresentRect& band) {
    static GLuint tex = 0;
    static int tw = 0, th = 0;
    static bool shown = false;
    {
        std::lock_guard<std::mutex> lk(g_gl_mu);
        if (g_gl_w > 0 && std::chrono::steady_clock::now() - g_gl_time > std::chrono::milliseconds(g_gl_ttl)) { g_gl_px.clear(); g_gl_w = g_gl_h = 0; g_gl_dirty = true; g_gl_full = true; }
        if (g_gl_dirty) {
            g_gl_dirty = false;
            shown = g_gl_w > 0;
            if (shown) {
                GLint prev_tex = 0, prev_align = 4, prev_row = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
                glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                if (!tex) glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                bool resized = false;
                if (tw != g_gl_w || th != g_gl_h) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_gl_w, g_gl_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    tw = g_gl_w; th = g_gl_h; resized = true;
                }
                if (!resized && !g_gl_old_upload && g_gl_dr[2] >= g_gl_dr[0] && g_gl_dr[3] >= g_gl_dr[1] && !(g_gl_dr[0] == 0 && g_gl_dr[1] == 0 && g_gl_dr[2] == g_gl_w - 1 && g_gl_dr[3] == g_gl_h - 1)) {
                    // only the part that changed (the texture already holds the rest)
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, g_gl_w);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, g_gl_dr[0], g_gl_dr[1], g_gl_dr[2] - g_gl_dr[0] + 1, g_gl_dr[3] - g_gl_dr[1] + 1, GL_RGBA, GL_UNSIGNED_BYTE, g_gl_px.data() + size_t(g_gl_dr[1]) * size_t(g_gl_w) + size_t(g_gl_dr[0]));
                } else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_gl_w, g_gl_h, GL_RGBA, GL_UNSIGNED_BYTE, g_gl_px.data());
                g_gl_dr[0] = g_gl_dr[1] = 0; g_gl_dr[2] = g_gl_dr[3] = -1;
                glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
                glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row);
            }
        }
    }
    if (shown && tex) draw_overlay_alpha(tex, band);
}

static void note_overlay_rect(SDL_Window* window, const PresentRect& r);
// The PSP's 480x272 part of the picture `pr`: the middle of a wide one, the band of a tall one (GL y up: y1 is the top).
static PresentRect band_rect(const PresentRect& pr) {
    PresentRect cpr = pr;
    if (g_present_w480 > 480) {
        const int fw = pr.x1 - pr.x0, cw = int(fw * 480.0 / g_present_w480 + 0.5);
        cpr.x0 = pr.x0 + (fw - cw) / 2;
        cpr.x1 = cpr.x0 + cw;
    }
    if (g_present_h272 > 272) {
        const double k = double(pr.y1 - pr.y0) / g_present_h272;
        cpr.y0 = pr.y0 + int(band_b(g_present_h272) * k + 0.5);
        cpr.y1 = cpr.y0 + int(272 * k + 0.5);
    }
    return cpr;
}
// pr: where a 480x272 canvas goes (the PSP's band); full: the whole picture, for a taller canvas (the settings screen on a tall window)
static void present_dialog_overlay(const PresentRect& band, const PresentRect& full) {
    static GLuint ovl_tex = 0, ovl_fbo = 0;
    static int tex_w = 0, tex_h = 0;
    static std::vector<uint32_t> px;
    int ow = 0, oh = 0;
    bool translucent = false;
    if (!psp_sddialog::overlay_fetch(px, &ow, &oh, &translucent) || ow <= 0 || oh <= 0 ||
        px.size() != size_t(ow) * size_t(oh))
        return;
    PresentRect pr = band;
    if (int64_t(oh) * 480 > int64_t(ow) * 272 + ow) {   // taller than 480:272: the whole picture's width, from its top (GL y up)
        pr = full;
        pr.y0 = pr.y1 - int(int64_t(pr.x1 - pr.x0) * oh / ow);
        note_overlay_rect(psp_get_sdl_window(), pr);   // pointer mapping (the settings screen) follows
    }
    GLint prev_tex = 0, prev_align = 4, prev_row_len = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_len);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (!ovl_tex) {
        glGenTextures(1, &ovl_tex);
        glBindTexture(GL_TEXTURE_2D, ovl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &ovl_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, ovl_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, ovl_tex, 0);
    }
    glBindTexture(GL_TEXTURE_2D, ovl_tex);
    if (tex_w != ow || tex_h != oh) {   // (re)allocate when the overlay scale changes
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        tex_w = ow; tex_h = oh;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (translucent) {   // draw with alpha so the game's picture shows where the canvas is transparent
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        draw_overlay_alpha(ovl_tex, pr);
        glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len);
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ovl_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const bool one_to_one = (pr.x1 - pr.x0) == ow && (pr.y1 - pr.y0) == oh;
    // Canvas row 0 is the top; GL texture row 0 is the bottom => flip Y.
    glBlitFramebuffer(0, 0, ow, oh, pr.x0, pr.y1, pr.x1, pr.y0, GL_COLOR_BUFFER_BIT,
                      one_to_one ? GL_NEAREST : GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len);
}

// Frame statistics behind both on-screen counters. Page flips (what the game calls a frame) are timed; presents are
// counted separately, so a gap between the two shows the presenter running without the game producing frames.
//  - fps counter: just the flips per second, a transparent number in the top-right corner.
//  - performance overlay (debug): flips per second, average / worst / 1%-low-ish frame time, presents per second,
//    render scale, output size and process memory, in a box in the top-left corner.
// Text is rendered (FreeType, the dialog's font) only when it changes; every present is then a single small draw.
struct PerfHud {
    std::chrono::steady_clock::time_point last{}, win0{};
    int frames = 0, presents = 0;
    double sum_ms = 0, worst_ms = 0;
    static constexpr int kGraph = 120;   // frame times shown in the debug overlay's graph (oldest first)
    float graph[kGraph] = {};
    int graph_n = 0;
    std::chrono::steady_clock::time_point graph_drawn{};
    std::string text;       // performance overlay line
    std::string fps_text;   // fps counter
    bool dirty = false, fps_dirty = false;
    // Session statistics (average fps, 1% lows): only frames of "gameplay" count (ge_draw_perf_set_gameplay); they restart when
    // gameplay starts again, so menus and loading screens never pull the numbers down.
    static constexpr int kHist = 1200;   // frame-time histogram, 0.25 ms buckets up to 300 ms (the last one also takes anything slower)
    uint32_t hist[kHist] = {};
    uint64_t sess_frames = 0;
    double sess_ms = 0;
    bool sess_seen = false;   // a session has run (so there is something to show while a menu is up)
    bool was_gameplay = false;
    double ph_ms[psp_perf::N_IDS] = {};   // psp_perf scope time per frame, last window
    double gpu_ms = -1;                    // GPU time per frame, last window (Vulkan timestamp queries)
    std::vector<std::pair<std::string, double>> gpu_passes;   // most expensive GPU passes, ms per frame
};
static PerfHud g_perf;
// Whether the game is in "gameplay" (a battle) right now: only then do the average / 1% low frame rates count. A game module sets it every
// frame (games/dissidia/runtime/framerate.cpp); a game that never does leaves it true, so every frame counts.
static std::atomic<bool> g_perf_gameplay{true};
void ge_draw_perf_set_gameplay(bool on) { g_perf_gameplay.store(on, std::memory_order_relaxed); }

static void perf_session_reset() {
    std::memset(g_perf.hist, 0, sizeof g_perf.hist);
    g_perf.sess_frames = 0;
    g_perf.sess_ms = 0;
}
static void perf_session_add(double ms) {
    int b = int(ms * 4.0);
    if (b < 0) b = 0;
    if (b >= PerfHud::kHist) b = PerfHud::kHist - 1;
    g_perf.hist[b]++;
    g_perf.sess_frames++;
    g_perf.sess_ms += ms;
    g_perf.sess_seen = true;
}
// Frame time (ms) that the slowest `frac` of the frames reach or exceed, from the histogram (bucket upper edge).
static double perf_session_percentile_ms(double frac) {
    if (g_perf.sess_frames == 0) return 0;
    const uint64_t want = std::max<uint64_t>(1, uint64_t(std::ceil(double(g_perf.sess_frames) * frac)));
    uint64_t seen = 0;
    for (int b = PerfHud::kHist - 1; b >= 0; --b) {
        seen += g_perf.hist[b];
        if (seen >= want) return double(b + 1) / 4.0;
    }
    return 0;
}

static bool perf_stats_wanted() { return g_perf_hud.load(std::memory_order_relaxed) || g_fps_counter.load(std::memory_order_relaxed); }

// Resident memory of the process in MB, or -1 where it isn't cheap to read.
static int perf_rss_mb() {
#ifdef __linux__
    static FILE* f = nullptr;
    if (!f) f = std::fopen("/proc/self/statm", "r");
    if (!f) return -1;
    std::rewind(f);
    long total = 0, rss = 0;
    if (std::fscanf(f, "%ld %ld", &total, &rss) != 2) return -1;
    std::fflush(f);
    return int(rss * 4096L / (1024L * 1024L));
#else
    return -1;
#endif
}

#ifndef PSPRECOMP_RELEASE_VERSION
#define PSPRECOMP_RELEASE_VERSION "dev"
#endif
std::string psp_release_version() { return PSPRECOMP_RELEASE_VERSION; }   // also read by games/dissidia/runtime/discord_presence.cpp

// ---- system information for the performance overlay (Linux; elsewhere the fields read "n/a") ----
struct SysInfo {
    std::string cpu_model, gpu_model, gl_version, api_name, release;   // api_name: "OpenGL" / "Vulkan 1.2.x"
    int cpu_threads = 0;
    bool have_nvx = false, have_ati = false, init = false, vulkan = false;
    unsigned long long last_idle = 0, last_total = 0, last_proc = 0;   // /proc/stat jiffies, process utime+stime
    std::chrono::steady_clock::time_point last_t{};
    double cpu_sys = -1, cpu_proc = -1;          // percent; proc is of one core (can exceed 100)
    double ram_used_gb = -1, ram_total_gb = -1;
    double vram_used_gb = -1, vram_total_gb = -1;
    int gpu_busy = -1;
};
static SysInfo g_sys;

static std::string sys_read_line(const char* path, const char* key) {   // first line starting with `key`, value after ':'
    FILE* f = std::fopen(path, "r");
    if (!f) return {};
    char buf[512];
    std::string r;
    while (std::fgets(buf, sizeof buf, f)) {
        if (std::strncmp(buf, key, std::strlen(key)) == 0) {
            const char* c = std::strchr(buf, ':');
            if (c) { r = c + 1; while (!r.empty() && (r[0] == ' ' || r[0] == '\t')) r.erase(0, 1); while (!r.empty() && (r.back() == '\n' || r.back() == ' ')) r.pop_back(); }
            break;
        }
    }
    std::fclose(f);
    return r;
}
static long long sys_read_ll(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    long long v = -1;
    if (std::fscanf(f, "%lld", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}
// amdgpu exposes VRAM and load in sysfs; returns the first card that has them.
static std::string sys_amdgpu_dir() {
    static std::string dir = [] {
        for (int i = 0; i < 8; ++i) {
            std::string d = "/sys/class/drm/card" + std::to_string(i) + "/device/";
            if (sys_read_ll(d + "mem_info_vram_total") > 0) return d;
        }
        return std::string();
    }();
    return dir;
}
static bool gl_has_ext(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (e && !std::strcmp(e, name)) return true;
    }
    return false;
}
static void sys_info_init() {
    SysInfo& s = g_sys;
    s.init = true;
    s.release = psp_release_version();
#if defined(PSPRECOMP_HAVE_VKGL)
    {
        vkgl::DeviceInfo di;
        if (vkgl::device_info(&di)) {   // GL-on-Vulkan: the GL strings are placeholders, ask the Vulkan device
            s.api_name = "Vulkan (" + di.api + ")";
            s.gpu_model = di.name;
            s.gl_version = di.driver;
            s.vram_total_gb = di.vram_total_gb;   // used: filled in by sysfs below where available
            s.vulkan = true;
        }
    }
#endif
    if (!s.vulkan) s.api_name = "OpenGL";
    if (!s.vulkan) {
        auto gs = [](GLenum e) { const GLubyte* p = glGetString(e); return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); };
        s.gpu_model = gs(GL_RENDERER);
        s.gl_version = gs(GL_VERSION);
        const std::string vendor = gs(GL_VENDOR);
        if (!vendor.empty() && s.gpu_model.find(vendor) == std::string::npos && vendor.size() < 24) s.gpu_model = vendor + " " + s.gpu_model;
        s.have_nvx = gl_has_ext("GL_NVX_gpu_memory_info");
        s.have_ati = gl_has_ext("GL_ATI_meminfo");
        glGetError();
    }
#ifdef __linux__
    s.cpu_model = sys_read_line("/proc/cpuinfo", "model name");
    if (s.cpu_model.empty()) s.cpu_model = sys_read_line("/proc/cpuinfo", "Hardware");
    FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (f) { char b[256]; while (std::fgets(b, sizeof b, f)) if (!std::strncmp(b, "processor", 9)) s.cpu_threads++; std::fclose(f); }
#endif
}
// Called about twice a second from the overlay's window; every read is a small /proc or sysfs file.
static void sys_info_sample() {
    SysInfo& s = g_sys;
    if (!s.init) sys_info_init();
#ifdef __linux__
    using namespace std::chrono;
    unsigned long long user = 0, nice = 0, sysm = 0, idle = 0, iow = 0, irq = 0, sirq = 0, steal = 0;
    if (FILE* f = std::fopen("/proc/stat", "r")) {
        if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sysm, &idle, &iow, &irq, &sirq, &steal) >= 4) {
            const unsigned long long idle_all = idle + iow, total = user + nice + sysm + idle + iow + irq + sirq + steal;
            if (s.last_total && total > s.last_total) s.cpu_sys = 100.0 * double((total - s.last_total) - (idle_all - s.last_idle)) / double(total - s.last_total);
            s.last_idle = idle_all; s.last_total = total;
        }
        std::fclose(f);
    }
    if (FILE* f = std::fopen("/proc/self/stat", "r")) {
        char buf[1024];
        size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        std::fclose(f);
        const char* p = std::strrchr(buf, ')');   // fields after the command name; utime / stime are the 12th / 13th
        unsigned long long ut = 0, st = 0;
        if (p && std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) {
            const auto now = steady_clock::now();
            const double dt = duration<double>(now - s.last_t).count();
            const unsigned long long proc = ut + st;
            if (s.last_proc && dt > 0.05 && proc >= s.last_proc) s.cpu_proc = 100.0 * double(proc - s.last_proc) / (double(sysconf(_SC_CLK_TCK)) * dt);
            s.last_proc = proc; s.last_t = now;
        }
    }
    {
        const std::string tot = sys_read_line("/proc/meminfo", "MemTotal"), av = sys_read_line("/proc/meminfo", "MemAvailable");
        const double t = std::atof(tot.c_str()) / (1024.0 * 1024.0), a = std::atof(av.c_str()) / (1024.0 * 1024.0);   // kB -> GB
        if (t > 0) { s.ram_total_gb = t; s.ram_used_gb = t - a; }
    }
    const std::string d = sys_amdgpu_dir();
    if (!d.empty()) {
        const long long used = sys_read_ll(d + "mem_info_vram_used"), tot = sys_read_ll(d + "mem_info_vram_total");
        if (tot > 0 && used >= 0) { s.vram_total_gb = double(tot) / 1073741824.0; s.vram_used_gb = double(used) / 1073741824.0; }
        s.gpu_busy = int(sys_read_ll(d + "gpu_busy_percent"));
    }
#endif
    if (s.vram_total_gb < 0 && s.have_nvx) {
        GLint total_kb = 0, avail_kb = 0;
        glGetIntegerv(0x9048, &total_kb);   // GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
        glGetIntegerv(0x9049, &avail_kb);   // GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
        if (total_kb > 0) { s.vram_total_gb = total_kb / 1048576.0; s.vram_used_gb = (total_kb - avail_kb) / 1048576.0; }
        glGetError();
    } else if (s.vram_total_gb < 0 && s.have_ati) {
        GLint v[4] = {0, 0, 0, 0};
        glGetIntegerv(0x87FB, v);   // TEXTURE_FREE_MEMORY_ATI: free KB in the pool (total is not exposed)
        if (v[0] > 0) s.vram_used_gb = -2, s.vram_total_gb = v[0] / 1048576.0;   // -2 = "free" only
        glGetError();
    }
}
static std::string sys_info_text() {
    const SysInfo& s = g_sys;
    char b[256];
    std::string r;
    auto clip = [](std::string v, size_t n) { if (v.size() > n) v.resize(n); return v; };
    auto pct = [&](double v) { if (v < 0) return std::string("n/a"); std::snprintf(b, sizeof b, "%.0f%%", v); return std::string(b); };
    r += "Release: " + (s.release.empty() ? std::string("dev") : s.release) + "  |  API: " + s.api_name + "\n";
    r += "CPU: " + clip(s.cpu_model.empty() ? "n/a" : s.cpu_model, 48);
    if (s.cpu_threads) r += " (" + std::to_string(s.cpu_threads) + " threads)";
    r += "  |  system " + pct(s.cpu_sys) + "  game " + pct(s.cpu_proc) + " of one core\n";
    r += "GPU: " + clip(s.gpu_model.empty() ? "n/a" : s.gpu_model, 56) + "  |  load " + (s.gpu_busy >= 0 ? pct(double(s.gpu_busy)) : std::string("n/a"));
    if (s.vram_total_gb > 0 && s.vram_used_gb >= 0) { std::snprintf(b, sizeof b, "  |  VRAM %.1f / %.1f GB", s.vram_used_gb, s.vram_total_gb); r += b; }
    else if (s.vram_used_gb == -2) { std::snprintf(b, sizeof b, "  |  VRAM %.1f GB free", s.vram_total_gb); r += b; }
    else if (s.vram_total_gb > 0) { std::snprintf(b, sizeof b, "  |  VRAM %.1f GB", s.vram_total_gb); r += b; }
    else r += "  |  VRAM n/a";
    r += std::string("\n") + (s.vulkan ? "Driver: " : "GL driver: ") + clip(s.gl_version.empty() ? "n/a" : s.gl_version, 80) + "\n";
    const int mb = perf_rss_mb();
    if (mb >= 0) r += "RAM: game " + std::to_string(mb) + " MB";
    else r += "RAM: game n/a";
    if (s.ram_total_gb > 0) { std::snprintf(b, sizeof b, "  |  system %.1f / %.1f GB", s.ram_used_gb, s.ram_total_gb); r += b; }
    return r;
}

// The overlay's extra lines: session average / 1% low, GPU usage, and where the frame time goes. Each ends with a newline.
static std::string perf_detail_text(double frame_ms) {
    char b[320];
    std::string r;
    // ---- session fps (battle only) ----
    if (g_perf.sess_frames >= 30 || g_perf.sess_seen) {
        const double avg = g_perf.sess_frames ? 1000.0 * double(g_perf.sess_frames) / g_perf.sess_ms : 0.0;
        const double low1 = perf_session_percentile_ms(0.01), low01 = perf_session_percentile_ms(0.001);
        std::snprintf(b, sizeof b, "%s: avg %.1f fps  |  1%% low %.0f fps  |  0.1%% low %.0f fps  |  %llu frames\n",
                      g_perf_gameplay.load(std::memory_order_relaxed) ? "Battle" : "Last battle (paused in menu)", avg,
                      low1 > 0 ? 1000.0 / low1 : 0.0, low01 > 0 ? 1000.0 / low01 : 0.0, (unsigned long long)g_perf.sess_frames);
        r += b;
    } else {
        r += "Battle: avg / 1% low start when a battle begins (menus are not counted)\n";
    }
    // ---- GPU usage ----
    if (g_perf.gpu_ms >= 0 && frame_ms > 0) {
        std::snprintf(b, sizeof b, "GPU usage: %.0f%%  (%.1f ms of %.1f ms per frame)\n", std::min(100.0, 100.0 * g_perf.gpu_ms / frame_ms), g_perf.gpu_ms, frame_ms);
        r += b;
    } else if (g_sys.gpu_busy >= 0) {
        std::snprintf(b, sizeof b, "GPU usage: %d%%\n", g_sys.gpu_busy);
        r += b;
    } else {
        r += "GPU usage: n/a on this system\n";
    }
    // ---- where the frame time goes: the biggest parts of the render thread's work ----
    {
        struct Part { const char* name; double ms; };
        std::vector<Part> parts;
        double used = 0;
        for (int i = 0; i < psp_perf::N_IDS; ++i) { parts.push_back({psp_perf::name(i), g_perf.ph_ms[i]}); used += g_perf.ph_ms[i]; }
        parts.push_back({"Waiting for the game / other", std::max(0.0, frame_ms - used)});
        std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& c) { return a.ms > c.ms; });
        r += "Frame time:";
        for (size_t i = 0; i < parts.size() && i < 4; ++i) {
            if (parts[i].ms < 0.05) break;
            std::snprintf(b, sizeof b, "%s %s %.1f ms (%.0f%%)", i ? "  |" : "", parts[i].name, parts[i].ms, frame_ms > 0 ? 100.0 * parts[i].ms / frame_ms : 0.0);
            r += b;
        }
        r += "\n";
    }
    // ---- the most expensive GPU passes (Vulkan) ----
    if (!g_perf.gpu_passes.empty()) {
        r += "GPU passes:";
        for (size_t i = 0; i < g_perf.gpu_passes.size() && i < 3; ++i) {
            std::string l = g_perf.gpu_passes[i].first;
            const size_t arrow = l.find(" <-");
            if (arrow != std::string::npos) l.resize(arrow);
            const size_t fbo = l.find("fbo");
            if (fbo != std::string::npos) { size_t e = l.find(' ', fbo); if (e != std::string::npos) l.erase(fbo, e - fbo + 1); }
            std::snprintf(b, sizeof b, "%s %s %.1f ms", i ? "  |" : "", l.c_str(), g_perf.gpu_passes[i].second);
            r += b;
        }
        r += "\n";
    }
    return r;
}

static void perf_note_present() {
    if (perf_stats_wanted()) g_perf.presents++;
}

static void perf_note_flip() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (!perf_stats_wanted()) { g_perf.last = {}; return; }
    if (g_perf.last.time_since_epoch().count() == 0) {   // first flip after turning it on / a pause
        g_perf = PerfHud{};
        g_perf.last = g_perf.win0 = now;
        g_perf.text = "-- fps";
        g_perf.fps_text = "--";
        g_perf.dirty = g_perf.fps_dirty = true;
        return;
    }
    const double ms = duration<double, std::milli>(now - g_perf.last).count();
    g_perf.last = now;
    if (ms > 1000.0) { g_perf.win0 = now; g_perf.frames = g_perf.presents = 0; g_perf.sum_ms = g_perf.worst_ms = 0; return; }   // paused
    g_perf.frames++;
    g_perf.sum_ms += ms;
    g_perf.worst_ms = std::max(g_perf.worst_ms, ms);
    {
        const bool gp = g_perf_gameplay.load(std::memory_order_relaxed);
        if (gp && !g_perf.was_gameplay) perf_session_reset();   // a new battle: start the averages again
        g_perf.was_gameplay = gp;
        if (gp) perf_session_add(ms);
    }
    if (g_perf_hud.load(std::memory_order_relaxed)) {
        if (g_perf.graph_n == PerfHud::kGraph) std::memmove(g_perf.graph, g_perf.graph + 1, (PerfHud::kGraph - 1) * sizeof(float));
        else g_perf.graph_n++;
        g_perf.graph[g_perf.graph_n - 1] = float(ms);
        if (now - g_perf.graph_drawn >= milliseconds(50)) { g_perf.graph_drawn = now; g_perf.dirty = true; }   // ~20 redraws/s
    }
    const double win = duration<double, std::milli>(now - g_perf.win0).count();
    if (win >= 500.0 && g_perf.frames > 0) {
        const double fps = g_perf.frames * 1000.0 / win;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%.0f", fps);
        if (g_perf.fps_text != buf) { g_perf.fps_text = buf; g_perf.fps_dirty = true; }
        if (g_perf_hud.load(std::memory_order_relaxed)) {
            for (int i = 0; i < psp_perf::N_IDS; ++i)
                g_perf.ph_ms[i] = double(psp_perf::g_ns[i].exchange(0, std::memory_order_relaxed)) * 1e-6 / double(g_perf.frames);
#if defined(PSPRECOMP_HAVE_VKGL)
            {
                vkgl::PerfSample ps;
                g_perf.gpu_ms = -1;
                g_perf.gpu_passes.clear();
                if (vkgl::take_perf(&ps)) {
                    g_perf.gpu_ms = ps.gpu_ms;
                    for (auto& p : ps.passes) g_perf.gpu_passes.push_back({p.label, p.ms});
                }
            }
#endif
            int n = std::snprintf(buf, sizeof buf, "%.0f fps  %.1f ms  worst %.1f ms  |  present %.0f/s  |  %dx",
                                  fps, g_perf.sum_ms / g_perf.frames, g_perf.worst_ms, g_perf.presents * 1000.0 / win,
                                  ge_draw_render_scale());
            if (ge_draw_widescreen() && n > 0 && n < int(sizeof buf)) std::snprintf(buf + n, sizeof buf - n, " wide");
            sys_info_sample();
            g_perf.text = std::string(buf) + "\n" + perf_detail_text(g_perf.sum_ms / g_perf.frames) + sys_info_text();
            g_perf.dirty = true;
        }
        g_perf.win0 = now;
        g_perf.frames = g_perf.presents = 0;
        g_perf.sum_ms = g_perf.worst_ms = 0;
    }
}

// Toast: a message box over the picture for a few seconds, same look as the performance overlay. The text
// is rendered when it changes; each present is then one small blit.
static std::mutex g_toast_mu;
static std::string g_toast_text;
static std::chrono::steady_clock::time_point g_toast_until{};
static bool g_toast_dirty = false;
static int g_toast_y = 10;

void ge_draw_toast(const char* text, int ms, int y_px) {
    std::lock_guard<std::mutex> lk(g_toast_mu);
    g_toast_y = y_px;
    g_toast_text = text ? text : "";
    g_toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    g_toast_dirty = true;
}


// ---- View blocker fade (ge_draw_set_view_blocker) ----------------------------------------------------------------------------------
static float g_vb_target[3] = {0, 0, 0};
static float g_vb_eye[3] = {0, 0, 0};   // the game camera's eye (the draws of the main view have this eye; screen-space effects use another view and are left alone)
void ge_draw_set_view_blocker(bool active, const float target[3], const float eye[3]) {
    for (int i = 0; i < 3; i++) g_vb_eye[i] = eye[i];   // always: a draw still fading back in must keep being matched to the main view
    if (active) for (int i = 0; i < 3; i++) g_vb_target[i] = target[i];
    g_vb_active.store(active, std::memory_order_relaxed);
}
// Fade state of one drawn instance (1 = opaque), eased in time so blocks fade in and out instead of popping. Keyed by the vertex
// data address and count AND the instance's world position: one mesh drawn at several places (a row of pillars, repeated wall
// pieces) used to share one entry, so one hit pillar faded all of them, and the un-hit copies drawn after it pulled it back.
struct VbFade { float f = 1.f; double t = 0.0; };
static std::unordered_map<uint64_t, VbFade> g_vb_fade;
static double g_vb_last_faded_t = 0.0;
static double vb_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool vb_main_view(const GeState& st, float eye[3]) {
    const float* V = st.view_matrix;
    eye[0] = -(V[0] * V[9] + V[1] * V[10] + V[2] * V[11]); eye[1] = -(V[3] * V[9] + V[4] * V[10] + V[5] * V[11]); eye[2] = -(V[6] * V[9] + V[7] * V[10] + V[8] * V[11]);
    const float dx = eye[0] - g_vb_eye[0], dy = eye[1] - g_vb_eye[1], dz = eye[2] - g_vb_eye[2];
    return dx * dx + dy * dy + dz * dz < 0.25f;
}
struct VbBox { float lo[3], hi[3]; };
static std::unordered_map<uint64_t, VbBox> g_vb_box;   // model-space box of a draw's vertices, by vertex address and count (static geometry: decoded once)
static float vb_fade_factor(uint8_t* rdram, const GeState& st, int n, int prim) {
    if (n < 3) return 1.f;
    static const float k_alpha = [] { const char* e = std::getenv("PSPRECOMP_CAM_BLOCKER_ALPHA"); return e ? std::clamp((float)std::atof(e), 0.f, 1.f) : 0.3f; }();
    const bool want = g_vb_active.load(std::memory_order_relaxed);
    const float* m = st.world_matrix;
    static thread_local std::vector<GeGpuVertex> tmp;
    const uint64_t key = (uint64_t)st.vertex_addr << 20 ^ (uint64_t)n;
    // the instance: its world position, quantised to 25 cm (static pieces keep their entry; moving ones just do not fade)
    auto q = [](float v) { return (uint64_t)(uint32_t)(int32_t)std::lround(v * 4.f); };
    const uint64_t ikey = key * 0x9E3779B97F4A7C15ull ^ (q(m[9]) * 0xC2B2AE3D27D4EB4Full) ^ (q(m[10]) * 0x165667B19E3779F9ull) ^ (q(m[11]) * 0x27D4EB2F165667C5ull);
    float eye[3];
    if (!vb_main_view(st, eye)) return 1.f;   // not drawn from the game camera (screen-space effects, shadow maps, reflections, ...)
    const double now = vb_now();
    auto fi = g_vb_fade.find(ikey);
    if (!want && fi == g_vb_fade.end()) return 1.f;   // lingering: only draws that are still faded need work
    bool hit = false;
    if (want) {
    auto bi = g_vb_box.find(key);
    if (bi == g_vb_box.end()) {
        VbBox bx{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        tmp.resize((size_t)n);
        if (ge_decode_vertices_gpu(rdram, st, n, tmp.data()))
            for (int i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) { bx.lo[k] = std::min(bx.lo[k], tmp[i].pos[k]); bx.hi[k] = std::max(bx.hi[k], tmp[i].pos[k]); }
        bi = g_vb_box.emplace(key, bx).first;
    }
    const float* lo = bi->second.lo; const float* hi = bi->second.hi;
    if (lo[0] > hi[0]) return 1.f;
    float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float x = (c & 1) ? hi[0] : lo[0], y = (c & 2) ? hi[1] : lo[1], z = (c & 4) ? hi[2] : lo[2];
        const float w[3] = {m[0] * x + m[3] * y + m[6] * z + m[9], m[1] * x + m[4] * y + m[7] * z + m[10], m[2] * x + m[5] * y + m[8] * z + m[11]};
        for (int k = 0; k < 3; ++k) { wlo[k] = std::min(wlo[k], w[k]); whi[k] = std::max(whi[k], w[k]); }
    }
    const float* T = g_vb_target;   // the player's chest
    float hx = T[0] - eye[0], hz = T[2] - eye[2];
    const float hl = std::sqrt(hx * hx + hz * hz);
    if (hl > 1e-3f) { hx /= hl; hz /= hl; } else { hx = 0; hz = 1; }
    // Rays to the body (knee, chest, head, both shoulders), each starting a little behind the eye so a wall the camera touches
    // still counts. Not to the feet: a ray grazing the floor at the fighter's feet faded floor pieces and kerbs. Floors and
    // anything lower than the fighter's knees is never faded (the box top test), and the start is only 35 cm behind the eye
    // (it was 2 m: a long wall the camera stood beside, out of the picture's way, faded as a whole).
    const float aim[5][3] = {{0, -0.5f, 0}, {0, 0, 0}, {0, 0.75f, 0}, {-hz * 0.4f, 0.25f, hx * 0.4f}, {hz * 0.4f, 0.25f, -hx * 0.4f}};
    bool decoded = false;
    const bool low = whi[1] < T[1] - 0.45f;   // top below the knees (feet are ~1 m below the chest)
    if (n <= 1500 && !low) {   // (a block or a wall, not the whole stage: a single mesh of the floor or the sky would fade as one; Pandaemonium's walls are long slabs of a few triangles)
        for (int ri = 0; ri < 5 && !hit; ++ri) {
            const float tg[3] = {T[0] + aim[ri][0], T[1] + aim[ri][1], T[2] + aim[ri][2]};
            float dir[3] = {tg[0] - eye[0], tg[1] - eye[1], tg[2] - eye[2]};
            const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (dl < 1e-3f) continue;
            const float back = 0.35f;   // start behind the eye
            const float o[3] = {eye[0] - dir[0] / dl * back, eye[1] - dir[1] / dl * back, eye[2] - dir[2] / dl * back};
            const float d[3] = {tg[0] - o[0], tg[1] - o[1], tg[2] - o[2]};
            const float len = dl + back;
            const float tmax = std::max(0.f, 1.f - 0.6f / len);   // stop a body's width short of the player: only what he is touching does not fade (a wall a step in front of him must)
            float t0 = 0.f, t1 = tmax;
            for (int k = 0; k < 3 && t0 <= t1; ++k) {
                const bool thin = whi[k] - wlo[k] < 0.3f;   // a wall is often a flat sheet: shrinking it would invert the box, so widen it a little instead
                const float a2 = wlo[k] + (thin ? -0.05f : 0.05f), b2 = whi[k] - (thin ? -0.05f : 0.05f);
                if (std::fabs(d[k]) < 1e-6f) { if (o[k] < a2 || o[k] > b2) t0 = 2.f; }
                else { float u0 = (a2 - o[k]) / d[k], u1 = (b2 - o[k]) / d[k]; if (u0 > u1) std::swap(u0, u1); t0 = std::max(t0, u0); t1 = std::min(t1, u1); }
            }
            if (t0 > t1) continue;
            if (!decoded) {
                tmp.resize((size_t)n);
                if (!ge_decode_vertices_gpu(rdram, st, n, tmp.data())) return 1.f;
                decoded = true;
            }
            const GeGpuVertex* v = tmp.data();
            auto W = [&](int i, float out[3]) { const float* qv = v[i].pos; for (int r = 0; r < 3; ++r) out[r] = m[r] * qv[0] + m[3 + r] * qv[1] + m[6 + r] * qv[2] + m[9 + r]; };
            auto tri = [&](int ia, int ib, int ic) {
                float a[3], b[3], c[3]; W(ia, a); W(ib, b); W(ic, c);
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (std::fabs(det) < 1e-9f) return false;
                const float inv = 1.f / det, tv[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
                const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
                if (u < 0.f || u > 1.f) return false;
                const float qq[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                const float w = (d[0] * qq[0] + d[1] * qq[1] + d[2] * qq[2]) * inv;
                if (w < 0.f || u + w > 1.f) return false;
                const float t = (e2[0] * qq[0] + e2[1] * qq[1] + e2[2] * qq[2]) * inv;
                return t > 0.f && t < tmax;
            };
            if (prim == GE_PRIM_TRIANGLES) { for (int i = 0; i + 2 < n && !hit; i += 3) hit = tri(i, i + 1, i + 2); }
            else if (prim == GE_PRIM_TRIANGLE_STRIP) { for (int i = 0; i + 2 < n && !hit; ++i) hit = tri(i, i + 1, i + 2); }
            else if (prim == GE_PRIM_TRIANGLE_FAN) { for (int i = 1; i + 1 < n && !hit; ++i) hit = tri(0, i, i + 1); }
        }
    }
    }
    if (fi == g_vb_fade.end()) {
        if (!hit) return 1.f;   // only blocking instances get an entry
        if (g_vb_fade.size() > 4096) g_vb_fade.clear();   // (moving pieces make new keys)
        fi = g_vb_fade.emplace(ikey, VbFade{1.f, now}).first;
    }
    VbFade& e = fi->second;
    const float dt = (float)std::clamp(now - e.t, 0.0, 0.1);   // per frame, not per draw: an instance drawn twice must not ease twice as fast
    e.t = now;
    const float target = hit ? k_alpha : 1.f;
    const float tc = hit ? 0.08f : 0.3f;   // fades out fast (the fighter must not stay hidden), back in gently
    e.f += (target - e.f) * (1.f - std::exp(-dt / tc));
    if (std::fabs(e.f - target) < 0.01f) e.f = target;
    const float f = e.f;
    if (f >= 1.f) { g_vb_fade.erase(fi); return 1.f; }
    g_vb_last_faded_t = now;
    return f;
}
// ---- Camera collision (ge_draw_camera_probe_*) ----
// The stage geometry the main view draws, in world space, one entry per drawn instance (same key as the fade). Static pieces
// are decoded once; an entry not drawn for 30 s is dropped, and everything is dropped when the total gets large (a new stage).
// The game's own camera pulls in for walls behind ITS eye; a turned or held view needs its own test, or it ends up outside the
// room looking at the back of a wall.
struct CcMesh { float lo[3], hi[3]; std::vector<float> tri; double seen; int frames = 1; };   // frames: how many frames it was drawn in at this spot
static std::unordered_map<uint64_t, CcMesh> g_cc;
static size_t g_cc_tris = 0;
static double g_cc_last_on = 0;
static std::mutex g_cc_mu;
static float g_cc_from[3], g_cc_to[3], g_cc_result = 1.f;
static uint32_t g_cc_req_id = 0, g_cc_res_id = 0;
static bool g_cc_pending = false;
void ge_draw_camera_probe_request(const float from[3], const float to[3], uint32_t id) {
    std::lock_guard<std::mutex> lk(g_cc_mu);
    for (int i = 0; i < 3; i++) { g_cc_from[i] = from[i]; g_cc_to[i] = to[i]; }
    g_cc_req_id = id; g_cc_pending = true;
    g_cc_collect.store(true, std::memory_order_relaxed);
    g_cc_last_on = vb_now();
}
float ge_draw_camera_probe_result(uint32_t* id) {
    std::lock_guard<std::mutex> lk(g_cc_mu);
    if (id) *id = g_cc_res_id;
    return g_cc_result;
}
static void cc_collect(uint8_t* rdram, const GeState& st, int n, int prim) {
    if (n < 3 || n > 12000) return;
    if (prim != GE_PRIM_TRIANGLES && prim != GE_PRIM_TRIANGLE_STRIP && prim != GE_PRIM_TRIANGLE_FAN) return;
    float eye[3];
    if (!vb_main_view(st, eye)) return;
    const float* m = st.world_matrix;
    auto q = [](float v) { return (uint64_t)(uint32_t)(int32_t)std::lround(v * 4.f); };
    const uint64_t key = (uint64_t)st.vertex_addr << 20 ^ (uint64_t)n;
    const uint64_t ikey = key * 0x9E3779B97F4A7C15ull ^ (q(m[9]) * 0xC2B2AE3D27D4EB4Full) ^ (q(m[10]) * 0x165667B19E3779F9ull) ^ (q(m[11]) * 0x27D4EB2F165667C5ull);
    const double now = vb_now();
    auto it = g_cc.find(ikey);
    if (it != g_cc.end()) { if (now - it->second.seen > 0.005) it->second.frames++; it->second.seen = now; return; }
    if (g_cc_tris > 400000) { g_cc.clear(); g_cc_tris = 0; }
    static thread_local std::vector<GeGpuVertex> tmp;
    tmp.resize((size_t)n);
    if (!ge_decode_vertices_gpu(rdram, st, n, tmp.data())) return;
    CcMesh cm{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, {}, now};
    auto add = [&](int ia, int ib, int ic) {
        for (int j : {ia, ib, ic}) {
            const float* p = tmp[j].pos;
            for (int r = 0; r < 3; ++r) {
                const float w = m[r] * p[0] + m[3 + r] * p[1] + m[6 + r] * p[2] + m[9 + r];
                cm.tri.push_back(w); cm.lo[r] = std::min(cm.lo[r], w); cm.hi[r] = std::max(cm.hi[r], w);
            }
        }
    };
    if (prim == GE_PRIM_TRIANGLES) for (int i = 0; i + 2 < n; i += 3) add(i, i + 1, i + 2);
    else if (prim == GE_PRIM_TRIANGLE_STRIP) for (int i = 0; i + 2 < n; ++i) add(i, i + 1, i + 2);
    else for (int i = 1; i + 1 < n; ++i) add(0, i, i + 1);
    if (!std::isfinite(cm.lo[0]) || cm.lo[0] > cm.hi[0]) return;
    g_cc_tris += cm.tri.size() / 9;
    g_cc.emplace(ikey, std::move(cm));
}
// First hit of the segment o -> o + d (t in 0..1) against the cached geometry; 1 when clear.
static float cc_trace(const float o[3], const float d[3]) {
    float best = 1.f;
    const double now = vb_now();
    for (const auto& kv : g_cc) {
        const CcMesh& cm = kv.second;
        if (cm.frames < 30 && now - cm.seen > 0.25) continue;   // something that moved (a prop, a weapon): only where it is now
        float t0 = 0.f, t1 = best;
        for (int k = 0; k < 3 && t0 <= t1; ++k) {
            const float a2 = cm.lo[k] - 0.01f, b2 = cm.hi[k] + 0.01f;
            if (std::fabs(d[k]) < 1e-7f) { if (o[k] < a2 || o[k] > b2) t0 = 2.f; }
            else { float u0 = (a2 - o[k]) / d[k], u1 = (b2 - o[k]) / d[k]; if (u0 > u1) std::swap(u0, u1); t0 = std::max(t0, u0); t1 = std::min(t1, u1); }
        }
        if (t0 > t1) continue;
        const float* v = cm.tri.data();
        for (size_t i = 0; i + 8 < cm.tri.size(); i += 9) {
            const float* a = v + i; const float* b = v + i + 3; const float* c = v + i + 6;
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-9f) continue;
            const float inv = 1.f / det, tv[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < 0.f || u > 1.f) continue;
            const float qq[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
            const float w = (d[0] * qq[0] + d[1] * qq[1] + d[2] * qq[2]) * inv;
            if (w < 0.f || u + w > 1.f) continue;
            const float t = (e2[0] * qq[0] + e2[1] * qq[1] + e2[2] * qq[2]) * inv;
            if (t > 0.f && t < best) best = t;
        }
    }
    return best;
}
static void cc_frame_end() {
    const double now = vb_now();
    if (g_cc_collect.load(std::memory_order_relaxed) && now - g_cc_last_on > 2.0) {   // no battle camera any more (menus): drop it all
        g_cc_collect.store(false, std::memory_order_relaxed);
        g_cc.clear(); g_cc_tris = 0;
        return;
    }
    static double last_sweep = 0;
    if (now - last_sweep > 5.0) {
        last_sweep = now;
        for (auto it = g_cc.begin(); it != g_cc.end();) {
            if (now - it->second.seen > 30.0) { g_cc_tris -= it->second.tri.size() / 9; it = g_cc.erase(it); } else ++it;
        }
    }
    float o[3], d[3]; uint32_t id;
    {
        std::lock_guard<std::mutex> lk(g_cc_mu);
        if (!g_cc_pending) return;
        g_cc_pending = false;
        for (int i = 0; i < 3; i++) { o[i] = g_cc_from[i]; d[i] = g_cc_to[i] - g_cc_from[i]; }
        id = g_cc_req_id;
    }
    const float r = cc_trace(o, d);
    std::lock_guard<std::mutex> lk(g_cc_mu);
    g_cc_result = r; g_cc_res_id = id;
}

// Called once per frame (present): keep evaluating draws after the custom camera is back on the game's while some are still faded.
static void vb_frame_end() {
    cc_frame_end();
    g_vb_linger.store(!g_vb_fade.empty() && vb_now() - g_vb_last_faded_t < 1.0, std::memory_order_relaxed);
    if (!g_vb_linger.load(std::memory_order_relaxed) && !g_vb_active.load(std::memory_order_relaxed)) g_vb_fade.clear();
}


// Flash: four bars along the edges of the picture, cleared to one colour through the scissor box (no shader needed), for a short time.
static std::atomic<int64_t> g_flash_until_ms{0};
static std::atomic<uint32_t> g_flash_rgb{0};
static int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void ge_draw_flash(uint32_t rgb, int ms) {
    g_flash_rgb.store(rgb, std::memory_order_relaxed);
    g_flash_until_ms.store(steady_ms() + ms, std::memory_order_relaxed);
}

static void present_flash(const PresentRect& pr) {
    static const bool test_flash = std::getenv("PSPRECOMP_TEST_FLASH") != nullptr;   // tests: a green flash for 400 ms every 2 s
    if (test_flash && steady_ms() % 2000 < 40) ge_draw_flash(0x40FF70, 400);
    if (steady_ms() >= g_flash_until_ms.load(std::memory_order_relaxed)) return;
    const uint32_t c = g_flash_rgb.load(std::memory_order_relaxed);
    const int left = std::min(pr.x0, pr.x1), right = std::max(pr.x0, pr.x1), bottom = std::min(pr.y0, pr.y1), top = std::max(pr.y0, pr.y1);
    const int w = right - left, h = top - bottom;
    if (w <= 0 || h <= 0) return;
    const int t = std::max(2, int(std::lround(0.03 * double(h))));
    GLint box[4]; GLfloat clear[4];
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f, 1.f);
    const int bars[4][4] = {{left, top - t, w, t}, {left, bottom, w, t}, {left, bottom, t, h}, {right - t, bottom, t, h}};
    for (const auto& b : bars) { glScissor(b[0], b[1], b[2], b[3]); glClear(GL_COLOR_BUFFER_BIT); }
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!sc) glDisable(GL_SCISSOR_TEST);
}

static void present_toast(const PresentRect& pr) {
    std::string text;
    bool dirty;
    {
        std::lock_guard<std::mutex> lk(g_toast_mu);
        if (g_toast_text.empty() || std::chrono::steady_clock::now() >= g_toast_until) return;
        text = g_toast_text;
        dirty = g_toast_dirty;
        g_toast_dirty = false;
    }
    static GLuint tex = 0, fbo = 0;
    static int tw = 0, th = 0, tscale = 0, alloc_w = 0, alloc_h = 0;
    static std::vector<uint32_t> px;
    const int scale = psp_sddialog::output_scale();
    if (dirty || scale != tscale || !tex) {
        int w = 0, h = 0;
        if (!psp_sddialog::render_hud_box(text, scale, px, &w, &h) || w <= 0 || h <= 0) return;
        GLint prev_tex = 0, prev_align = 4, prev_row_len = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_len);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenFramebuffers(1, &fbo);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        if (w > alloc_w || h > alloc_h) {
            alloc_w = std::max(w, alloc_w); alloc_h = std::max(h, alloc_h);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, alloc_w, alloc_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len);
        tw = w; th = h; tscale = scale;
    }
    const float sx = float(pr.x1 - pr.x0) / 480.f, sy = float(std::abs(pr.y1 - pr.y0)) / float(g_present_h272);   // a tall picture: the PSP band is its top 272 rows
    const int top = std::max(pr.y0, pr.y1), left = std::min(pr.x0, pr.x1);
    const int bw = int(std::lround(float(tw) / float(tscale) * sx));
    const int x0 = left + (int(std::lround(480.f * sx)) - bw) / 2;     // centred
    const int x1 = x0 + bw;
    int ty;
    { std::lock_guard<std::mutex> lk(g_toast_mu); ty = g_toast_y; }
    const int y_top = top - int(std::lround(float(ty) * sy));
    const int y_bot = y_top - int(std::lround(float(th) / float(tscale) * sy));
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, tw, th, x0, y_top, x1, y_bot, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

// Translucent HUD text (the fps counter and the performance overlay): a textured quad blended over the picture with its
// own tiny program (GL state saved and restored, like the touch controls). `box` = the performance overlay's dark box
// (top-left), otherwise the bare number with a soft shadow (top-right). `alpha` is the overall opacity.
struct HudBlend { GLuint tex = 0; int tw = 0, th = 0, tscale = 0, alloc_w = 0, alloc_h = 0; std::vector<uint32_t> px; };
static void present_blend_hud(HudBlend& st, const std::string& text, bool& text_dirty, bool box, float alpha, const PresentRect& pr,
                              const float* graph = nullptr, int graph_n = 0, bool canvas = false) {
    if (!canvas && text.empty()) return;
    static GLuint prog = 0, vao = 0, vbo = 0;
    static GLint u_tex = -1, u_alpha = -1;
    static bool failed = false;
    GLuint& tex = st.tex;
    int &tw = st.tw, &th = st.th, &tscale = st.tscale, &alloc_w = st.alloc_w, &alloc_h = st.alloc_h;
    std::vector<uint32_t>& px = st.px;
    if (failed) return;
    if (!prog) {
        const char* vs_body = "in vec2 a_pos;\nin vec2 a_uv;\nout vec2 v_uv;\n"
                              "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        const char* fs_body = "in vec2 v_uv;\nout vec4 o_col;\nuniform sampler2D u_tex;\nuniform float u_alpha;\n"
                              "void main() { vec4 c = texture(u_tex, v_uv); o_col = vec4(c.rgb, c.a * u_alpha); }\n";
        auto compile = [](GLenum type, const char* body) {
            const char* src[2] = {PSP_GLSL_VERSION, body};
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 2, src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) { glDeleteShader(sh); return GLuint(0); }
            return sh;
        };
        GLuint vs = compile(GL_VERTEX_SHADER, vs_body), fs = compile(GL_FRAGMENT_SHADER, fs_body);
        if (!vs || !fs) { failed = true; return; }
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "a_pos");
        glBindAttribLocation(prog, 1, "a_uv");
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(prog); prog = 0; failed = true; return; }
        u_tex = glGetUniformLocation(prog, "u_tex");
        u_alpha = glGetUniformLocation(prog, "u_alpha");
        GLint prev_vao = 0, prev_abo = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindBuffer(GL_ARRAY_BUFFER, GLuint(prev_abo));
        glBindVertexArray(GLuint(prev_vao));
    }
    const int scale = canvas ? 1 : psp_sddialog::output_scale();
    GLint prev_tex = 0, prev_active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    if (text_dirty || scale != tscale || !tex) {
        int w = 0, h = 0;
        bool ok = false;
        if (canvas) {
            std::lock_guard<std::mutex> lk(g_game_canvas.m);
            w = g_game_canvas.w; h = g_game_canvas.h;
            if (w > 0 && h > 0 && g_game_canvas.px.size() >= size_t(w) * size_t(h)) { px = g_game_canvas.px; ok = true; }
        } else {
            ok = box ? psp_sddialog::render_hud_box(text, scale, px, &w, &h, graph, graph_n)
                     : psp_sddialog::render_hud_text(text, scale, px, &w, &h);
        }
        if (!ok || w <= 0 || h <= 0) {
            glActiveTexture(GLenum(prev_active));
            return;
        }
        GLint prev_align = 4, prev_row_len = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_len);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (!tex) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        if (w > alloc_w || h > alloc_h) {   // grow only; the text is drawn from the texture's corner
            alloc_w = std::max(w, alloc_w); alloc_h = std::max(h, alloc_h);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, alloc_w, alloc_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_len);
        tw = w; th = h; tscale = scale;
        text_dirty = false;
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    // Quad in window pixels, top-right of the picture, then to clip space.
    int dw = 0, dh = 0;
    psp_gl_drawable_size(psp_get_sdl_window(), &dw, &dh);
    if (dw <= 0 || dh <= 0) { glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex)); glActiveTexture(GLenum(prev_active)); return; }
    const float sx = float(pr.x1 - pr.x0) / 480.f, sy = float(std::abs(pr.y1 - pr.y0)) / float(g_present_h272);   // a tall picture: the PSP band is its top 272 rows
    const float qw = canvas ? float(std::abs(pr.x1 - pr.x0)) : float(tw) / float(tscale) * sx;
    const float right = canvas ? float(std::max(pr.x0, pr.x1)) : box ? float(std::min(pr.x0, pr.x1)) + 4.f * sx + qw : float(std::max(pr.x0, pr.x1)) - 4.f * sx;
    const float top = canvas ? float(std::max(pr.y0, pr.y1)) : float(std::max(pr.y0, pr.y1)) - 4.f * sy;   // GL origin is bottom-left
    const float qh = canvas ? float(std::abs(pr.y1 - pr.y0)) * 272.f / float(g_present_h272) : float(th) / float(tscale) * sy;
    auto cx = [&](float x) { return x / float(dw) * 2.f - 1.f; };
    auto cy = [&](float y) { return y / float(dh) * 2.f - 1.f; };
    const float u1 = float(tw) / float(alloc_w), v1 = float(th) / float(alloc_h);
    // texture row 0 = canvas top, so v = 0 is the top edge of the quad
    const float verts[] = {
        cx(right - qw), cy(top),      0.f, 0.f,
        cx(right),      cy(top),      u1,  0.f,
        cx(right - qw), cy(top - qh), 0.f, v1,
        cx(right),      cy(top - qh), u1,  v1,
    };
    GLint prev_prog = 0, prev_vao = 0, prev_abo = 0, prev_vp[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, prev_fbo = 0;
    GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST), prev_cull = glIsEnabled(GL_CULL_FACE),
              prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean cmask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glUniform1i(u_tex, 0);
    glUniform1f(u_alpha, alpha);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(prev_abo));
    glBindVertexArray(GLuint(prev_vao));
    glUseProgram(GLuint(prev_prog));
    glBlendFuncSeparate(bsrc_rgb, bdst_rgb, bsrc_a, bdst_a);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prev_fbo));
    glBindTexture(GL_TEXTURE_2D, GLuint(prev_tex));
    glActiveTexture(GLenum(prev_active));
    glc::invalidate();
    ge_shader_note_external_program();
}

static void present_fps_counter(const PresentRect& pr) {
    static HudBlend st;
    static std::string shown, ping_text;
    static bool dirty = false;
    static std::chrono::steady_clock::time_point ping_next{};
    const bool fps = g_fps_counter.load(std::memory_order_relaxed), ping = g_ping_overlay.load(std::memory_order_relaxed);
    if (!fps && !ping) return;
    if (ping) {   // read four times a second (it takes the ad-hoc lock); only while in an online group
        const auto now = std::chrono::steady_clock::now();
        if (now >= ping_next) {
            ping_next = now + std::chrono::milliseconds(250);
            const int ms = psp_adhoc_connected() ? psp_adhoc_ping_ms() : -2;
            ping_text = ms == -2 ? "" : ms < 0 ? "Ping --" : "Ping " + std::to_string(ms) + " ms";
        }
    }
    std::string text = fps ? g_perf.fps_text : std::string();
    if (ping && !ping_text.empty()) text += (text.empty() ? "" : "  ") + ping_text;
    if (text != shown) { shown = text; dirty = true; }
    if (g_perf.fps_dirty) { g_perf.fps_dirty = false; dirty = true; }
    present_blend_hud(st, shown, dirty, false, 0.95f, pr);
}
// A game module's overlay canvas (ge_draw_set_game_overlay), over the 480-pixel-wide picture area.
static void present_game_overlay(const PresentRect& pr) {
    static HudBlend st;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(g_game_canvas.m);
        if (g_game_canvas.w <= 0 || g_game_canvas.h <= 0) return;
        dirty = g_game_canvas.dirty; g_game_canvas.dirty = false;
    }
    present_blend_hud(st, std::string(), dirty, false, 1.0f, pr, nullptr, 0, true);
}
void ge_draw_set_game_overlay(const uint32_t* rgba, int w, int h) {
    std::lock_guard<std::mutex> lk(g_game_canvas.m);
    if (!rgba || w <= 0 || h <= 0) { g_game_canvas.w = g_game_canvas.h = 0; return; }
    g_game_canvas.px.assign(rgba, rgba + size_t(w) * size_t(h));
    g_game_canvas.w = w; g_game_canvas.h = h; g_game_canvas.dirty = true;
}
static void present_perf_hud(const PresentRect& pr) {
    static HudBlend st;
    if (!g_perf_hud.load(std::memory_order_relaxed)) return;
    present_blend_hud(st, g_perf.text, g_perf.dirty, true, 0.65f, pr, g_perf.graph, g_perf.graph_n);
}


// PSPRECOMP_FRAME_DUMP=<dir>,<n>: write the next n presented frames (every
// present_blit, including liveness-net presents) as <dir>/f_<k>.tga, with the
// present reason in the log, to inspect frame-to-frame alternation. Capture
// starts once the file <dir>/go exists.
static void present_dump_frame(GLuint fbo, const char* why) {
    static const char* spec = std::getenv("PSPRECOMP_FRAME_DUMP");
    static std::string dir; static int remaining = -1, k = 0;
    if (!spec) return;
    if (remaining < 0) {
        std::string sp(spec); size_t c = sp.rfind(',');
        dir = c == std::string::npos ? sp : sp.substr(0, c);
        remaining = c == std::string::npos ? 100 : std::atoi(sp.c_str() + c + 1);
    }
    if (remaining <= 0) return;
    // Capture starts when <dir>/go exists (so a script can arm it mid-run).
    static bool started = false;
    if (!started) {
        std::string go = dir + "/go";
        if (access(go.c_str(), F_OK) != 0) return;
        started = true;
    }
    remaining--;
    std::vector<uint8_t> fl;
    int cw = 480, chh = 272;
    read_fbo_native(fbo, fl, &cw, &chh);
    char path[600];
    std::snprintf(path, sizeof path, "%s/f_%04d.tga", dir.c_str(), k);
    write_tga(path, fl.data(), cw, chh);
    long sum = 0; for (size_t i = 0; i < fl.size(); i += 4 * 61) sum += fl[i] + fl[i + 1] + fl[i + 2];
    std::fprintf(stderr, "[FDUMP] %d %s fbo=%u t=%.1fms meansum=%ld\n", k, why, fbo,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(), sum);
    k++;
}

// Where the 480x272 overlay (settings menu, dialogs) was last drawn, as fractions of the drawable; the settings menu maps mouse and
// touch positions through it (ge_draw_overlay_rect).
static float g_ovl_fx0 = 0.f, g_ovl_fy0 = 0.f, g_ovl_fx1 = 1.f, g_ovl_fy1 = 1.f;
static void note_overlay_rect(SDL_Window* window, const PresentRect& r) {
    int dw = 0, dh = 0;
    psp_gl_drawable_size(window, &dw, &dh);
    if (dw <= 0 || dh <= 0) return;
    g_ovl_fx0 = float(r.x0) / float(dw); g_ovl_fx1 = float(r.x1) / float(dw);
    g_ovl_fy0 = float(dh - r.y1) / float(dh); g_ovl_fy1 = float(dh - r.y0) / float(dh);   // GL y is up, window fractions go down
}
void ge_draw_overlay_rect(float* fx0, float* fy0, float* fx1, float* fy1) {
    *fx0 = g_ovl_fx0; *fy0 = g_ovl_fy0; *fx1 = g_ovl_fx1; *fy1 = g_ovl_fy1;
}

// Blit the given front FBO to the window and swap. Common to the routine
// (page-flip) present and the liveness net.
static void present_blit(GLuint fbo, const char* why = "flip") {
    psp_perf::Scope perf_scope_(psp_perf::PRESENT);
    SDL_Window* window = psp_get_sdl_window();
    if (!window) return;
    present_dump_frame(fbo, why);
    present_service_screenshot(fbo);
    g_present_w480 = w480_of_fbo(fbo);
    g_present_h272 = h272_of_fbo(fbo);
    const PresentRect pr = present_layout(window);
    // Widescreen: the dialog / settings overlays are 480x272 canvases; they go in the middle of the picture
    const PresentRect cpr = band_rect(pr);
    // Tell the savedata-dialog renderer how big the output is so the overlay
    // is rendered at (at least) output resolution rather than upscaled.
    psp_sddialog::set_output_scale(std::max({g_rs, (cpr.x1 - cpr.x0 + 479) / 480, 1}));
    note_overlay_rect(window, cpr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const int srs = rs_of_fbo(fbo), spw = w480_of_fbo(fbo);   // the shown buffer's scale and width
    if (pr.bars) {
        GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
        GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        GLboolean cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        if (sc) glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(cm[0], cm[1], cm[2], cm[3]);
        if (sc) glEnable(GL_SCISSOR_TEST);
    }
    // Source is the 480n x 272n target. A nearest blit that is not 1:1 would
    // alias the supersampled image, so use linear unless nearest is forced.
    GLenum pf = pr.filter;
    static const char* fl = std::getenv("PSPRECOMP_FILTER");
    if (!(fl && fl[0]) && env_on("PSPRECOMP_INTEGER_SCALE")) {
        // Integer-scale presentation: crisp only when source and output differ by a whole factor
        // (render scale 2 shown at 2x = 1:1; render scale 1 shown at 2x; render scale 2 at 1x is a 2:1
        // downscale, also whole). Otherwise (e.g. 2x target on a 3x output) bilinear avoids uneven pixels.
        const int sw = spw * srs, ow = pr.x1 - pr.x0;
        const bool whole = (ow >= sw) ? (ow % sw == 0) : (sw % ow == 0);
        pf = whole ? GL_NEAREST : GL_LINEAR;
    } else if (srs > 1 && pf == GL_NEAREST && (pr.x1 - pr.x0) != spw * srs) {
        if (!(fl && (!std::strcmp(fl, "nearest") || !std::strcmp(fl, "integer")))) pf = GL_LINEAR;
    }
    {   // How much of the scene is above 1.0 while HDR Scene is on: one [HDRSTATS] line every 15 presents with PSPRECOMP_HDR_STATS=1,
        // every 120 otherwise (cheap; lets a plain play session on a device without env access be checked from the log).
        static const char* e = std::getenv("PSPRECOMP_HDR_STATS");
        static const int every = (e && e[0] == '1') ? 15 : (e && e[0] == '0') ? 0 : 120;
        static int n = 0;
        if (every && g_scene_fp16 && (++n % every) == 0) {
            const int fw = spw * srs, fh = h272_of_fbo(fbo) * srs;
            std::vector<float> px(size_t(fw) * fh * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_FLOAT, px.data());
            float mx = 0.f; size_t above = 0, bright = 0;
            for (size_t i = 0; i < size_t(fw) * fh; i++) {
                const float m = std::max(px[i * 4], std::max(px[i * 4 + 1], px[i * 4 + 2]));
                if (m > mx) mx = m;
                if (m > 1.001f) above++;
                if (m > 1.5f) bright++;
            }
            std::fprintf(stderr, "[HDRSTATS] max=%.3f above1=%zu above1.5=%zu of %zu\n", mx, above, bright, size_t(fw) * fh);
        }
    }
    bool post_done = false;
    if (psp_post_active()) {   // FXAA / sharpen / smooth upscale (psp_post.cpp) instead of the plain blit
        GLuint src_tex = 0;
        for (int i = 0; i < GE_MAX_TARGETS && !src_tex; ++i) {
            if (g_targets[i].fbo == fbo) src_tex = g_targets[i].color_tex;
            else if (g_targets[i].ab_fbo && g_targets[i].ab_fbo == fbo) src_tex = g_targets[i].ab_tex;
        }
        if (src_tex) {
            post_done = psp_post_draw(src_tex, spw * srs, h272_of_fbo(fbo) * srs, pr.x0, pr.y0, pr.x1, pr.y1, pf == GL_NEAREST);
            if (post_done) {
                int dw = 0, dh = 0;
                psp_gl_drawable_size(window, &dw, &dh);
                glViewport(0, 0, dw, dh);
                glc::invalidate();
                ge_texture_invalidate_bind_cache();   // the pass bound / unbound texture unit 0 behind the texture module's back
                ge_shader_note_external_program();
            }
        }
    }
    if (!post_done)
    glBlitFramebuffer(0, 0, spw * srs, h272_of_fbo(fbo) * srs, pr.x0, pr.y0, pr.x1, pr.y1,
                      GL_COLOR_BUFFER_BIT, pf);
    perf_note_present();
    vb_frame_end();
    if (!std::strcmp(why, "flip")) perf_note_flip();
    if (psp_post_final_active()) {   // copy of the scene for the final pass (grade / HDR), taken before the HUD and overlays
        int sdw = 0, sdh = 0;
        psp_gl_drawable_size(window, &sdw, &sdh);
        psp_post_scene_snapshot(sdw, sdh);
    }
    {   // the game layer's copies of the picture, then its image
        GLuint src_tex = 0;
        for (int i = 0; i < GE_MAX_TARGETS && !src_tex; ++i) {
            if (g_targets[i].fbo == fbo) src_tex = g_targets[i].color_tex;
            else if (g_targets[i].ab_fbo && g_targets[i].ab_fbo == fbo) src_tex = g_targets[i].ab_tex;
        }
        present_game_layer_copies(src_tex, spw, h272_of_fbo(fbo), cpr);
    }
    present_game_layer(cpr);
    if (std::strcmp(why, "menu") != 0) { present_game_overlay(cpr); present_perf_hud(pr); present_fps_counter(pr); present_toast(pr); present_flash(pr); }
    present_dialog_overlay(cpr, pr);
    if (psp_touch_enabled() && (!psp_settings_menu_open() || psp_touch_editing() || psp_settings_test_open())) {   // on-screen controls over the whole window (hidden while the settings menu is open: it takes touches itself) (own program/VAO: the GE's cached GL state is reset after)
        int tw = 0, th = 0;
        psp_gl_drawable_size(window, &tw, &th);
        psp_touch_draw(tw, th);
        glc::invalidate();
        ge_shader_note_external_program();
    }
    if (psp_post_final_active()) {   // colour grade / HDR over the whole window (psp_post.cpp)
        int fdw = 0, fdh = 0;
        psp_gl_drawable_size(window, &fdw, &fdh);
        if (psp_post_final(fdw, fdh)) {
            glViewport(0, 0, fdw, fdh);
            glc::invalidate();
            ge_texture_invalidate_bind_cache();
            ge_shader_note_external_program();
        }
    }
    present_service_window_capture();
    present_service_record();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (g_sync_mode == 3) glFinish();
    {
        psp_perf::Scope swap_scope_(psp_perf::SWAP);
        psp_gl_swap(window);
    }
    g_present_dirty = false;
    g_last_present = std::chrono::steady_clock::now();
}


// PSPRECOMP_GE_AB: compare every live target with its legacy twin (native res).
static void ab_compare_all() {
    static uint64_t frame = 0, bad_frames = 0, cmp = 0;
    static const char* dir = std::getenv("PSPRECOMP_AB_DIR") ? std::getenv("PSPRECOMP_AB_DIR") : "/tmp";
    static const int thr = std::getenv("PSPRECOMP_AB_THR") ? std::atoi(std::getenv("PSPRECOMP_AB_THR")) : 4;
    frame++;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        const GeTarget& t = g_targets[i];
        if (!t.fbo || !t.ab_fbo) continue;
        std::vector<uint8_t> a, b;
        int w = 0, h = 0;
        read_fbo_native(t.fbo, a, &w, &h);
        read_fbo_native(t.ab_fbo, b, &w, &h);
        size_t nbad = 0; int maxd = 0;
        std::vector<uint8_t> d(a.size(), 255);
        for (size_t k = 0; k < a.size(); k += 4) {
            int m = 0;
            for (int c = 0; c < 3; c++) m = std::max(m, std::abs((int)a[k + c] - (int)b[k + c]));
            if (m > thr) { nbad++; d[k] = 255; d[k + 1] = 0; d[k + 2] = 0; }
            else { d[k] = d[k + 1] = d[k + 2] = (uint8_t)(a[k] / 2 + 127); }
            maxd = std::max(maxd, m);
        }
        cmp++;
        if (nbad) {
            bad_frames++;
            std::fprintf(stderr, "[AB] present#%llu target key=%06X: %zu px differ (>%d), max channel delta %d\n",
                         (unsigned long long)frame, t.key, nbad, thr, maxd);
            if (nbad >= 20 && g_ab_dumped < 24) {
                char path[512];
                const int n = g_ab_dumped++;
                std::snprintf(path, sizeof path, "%s/ab_%02d_fast.tga", dir, n); write_tga(path, a.data(), w, h);
                std::snprintf(path, sizeof path, "%s/ab_%02d_legacy.tga", dir, n); write_tga(path, b.data(), w, h);
                std::snprintf(path, sizeof path, "%s/ab_%02d_diff.tga", dir, n); write_tga(path, d.data(), w, h);
                std::fprintf(stderr, "[AB] dumped %s/ab_%02d_{fast,legacy,diff}.tga\n", dir, n);
            }
        }
    }
    if (frame % 120 == 0)
        std::fprintf(stderr, "[AB] summary: %llu presents, %llu target comparisons, %llu with differences\n",
                     (unsigned long long)frame, (unsigned long long)cmp, (unsigned long long)bad_frames);
}

// Widescreen: the width the displayed buffers should have for the current window (480 when it is off or the window is
// not wider than 16:9), and the (re)allocation of the displayed buffers when that changes.
static int wide_px_for_window() {
    if (!wide_on()) return 480;
    SDL_Window* w = psp_get_sdl_window();
    if (!w) return g_wide_px;
    int dw = 0, dh = 0;
    psp_gl_drawable_size(w, &dw, &dh);
    if (dw <= 0 || dh <= 0) return 480;
    int px = int(272.0 * dw / dh + 0.5) & ~1;
    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    int cap = 960;
    while (cap > 480 && cap * g_rs > maxtex) cap -= 2;
    return std::max(480, std::min(px, cap));
}
static void wide_update() {
    const int px = wide_px_for_window();
    if (px == g_wide_px) return;
    g_wide_px = px;
    std::fprintf(stderr, "[DRAW] widescreen: displayed buffers %d x 272 PSP pixels%s\n", px, px == 480 ? " (off)" : "");
    for (int i = 0; i < g_main_n; i++) mark_main_key(g_main_keys[i]);   // grows / shrinks them, keeping the picture
    delete_scratch();
}
// Tall windows: the height the displayed buffers should have (272 unless the touch controls use the tall layout, see g_tall_px).
static int tall_px_for_window() {
    static const bool off = [] { const char* e = std::getenv("PSPRECOMP_TALL_FILL"); return e && e[0] == '0'; }();
    SDL_Window* w = psp_get_sdl_window();
    if (off || !w) return 272;
    int dw = 0, dh = 0;
    psp_gl_drawable_size(w, &dw, &dh);
    if (dw <= 0 || dh <= 0 || !psp_touch_tall_layout(dw, dh)) return 272;
    int px = int(480.0 * dh / dw + 0.5) & ~1;
    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    int cap = 960;
    while (cap > 272 && cap * g_rs > maxtex) cap -= 2;
    return std::max(272, std::min(px, cap));
}
static void tall_update() {
    g_present_count++;
    {   // the band: centred while the game reports a battle view (within the last 10 presents), kept above the controls' bottom row
        int off = 0;
        const uint32_t note = g_scene_note.load(std::memory_order_relaxed);
        SDL_Window* w = psp_get_sdl_window();
        int dw = 0, dh = 0;
        if (w) psp_gl_drawable_size(w, &dw, &dh);
        if (note && g_present_count - note < 10 && g_tall_px > 272 && dh > 0) {
            const int room = g_tall_px - 272;
            const int below = int(std::ceil(double(psp_touch_tall_bottom(dw, dh)) * g_tall_px / dh));   // PSP pixels under the band kept clear
            // with the game's HUD pinned to the edges (ge_draw_set_hud_zones) nothing in the band needs to clear the controls: centred
            off = (g_hud_top || g_hud_bottom) ? room / 2 : std::max(0, std::min(room / 2, room - below));
        }
        g_hud_live = off > 0 && (g_hud_top || g_hud_bottom);
        g_hud_top_pin = (g_hud_live && dh > 0) ? int(std::ceil(double(psp_touch_tall_top(dw, dh)) * g_tall_px / dh)) : 0;
        if (off != g_band_off) {
            std::fprintf(stderr, "[DRAW] tall window: the picture %s (band at %d of %d PSP rows)\n", off ? "centred (battle view)" : "at the top", off, g_tall_px);
            g_band_off = off;
        }
    }
    const int px = tall_px_for_window();
    if (px == g_tall_px) return;
    g_tall_px = px;
    std::fprintf(stderr, "[DRAW] tall window: displayed buffers 480 x %d PSP pixels%s\n", px, px == 272 ? " (off)" : " (the PSP's picture in the top 272 rows)");
    for (int i = 0; i < g_main_n; i++) mark_main_key(g_main_keys[i]);   // grows / shrinks them, keeping the picture
    delete_scratch();
}
void ge_draw_set_widescreen(bool on) { g_wide_state.store(on ? 1 : 0); wide_update(); }
bool ge_draw_widescreen() { return wide_on(); }
int ge_draw_widescreen_width() { return g_wide_px; }

void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
) {
    (void)rdram;
    (void)fb_stride;
    (void)fb_format;
    ge_draw_flush();
    if (g_fx_on == 1 && g_fx_live.load(std::memory_order_relaxed)) psp_fx_end_frame();
    if (g_ab) ab_compare_all();
    if (g_ftrace_frames.load() > 0 && g_ftrace_drawn) { std::fprintf(stderr, "[FTRACE] ---- end of frame ----\n"); g_ftrace_drawn = false; g_ftrace_frames.fetch_sub(1); }
    if (g_dump_armed.exchange(false)) {
        std::string dir;
        { std::lock_guard<std::mutex> lk(g_dump_mu); dir = g_dump_dir; }
        for (int i = 0; i < GE_MAX_TARGETS; ++i) {
            if (!g_targets[i].fbo) continue;
            std::vector<uint8_t> a; int w = 0, h = 0;
            read_fbo_native(g_targets[i].fbo, a, &w, &h);
            char path[512];
            std::snprintf(path, sizeof path, "%s/tgt_%06X.tga", dir.c_str(), g_targets[i].key);
            std::vector<uint8_t> rgb = a;
            for (size_t k = 3; k < rgb.size(); k += 4) rgb[k] = 255;
            write_tga(path, rgb.data(), w, h);
            for (size_t k = 0; k < a.size(); k += 4) { const uint8_t v = a[k + 3]; rgb[k] = rgb[k + 1] = rgb[k + 2] = v; rgb[k + 3] = 255; }
            std::snprintf(path, sizeof path, "%s/tgt_%06X_alpha.tga", dir.c_str(), g_targets[i].key);
            write_tga(path, rgb.data(), w, h);
            std::fprintf(stderr, "[DUMP] target key=%06X -> %s\n", g_targets[i].key, path);
        }
    }
    psp_prof::Scope prof_present(psp_prof::S_PRESENT);
    if (const int req = g_no_raster_request.exchange(-1); req >= 0 && req != g_no_raster) {   // ge_draw_request_no_raster (render thread applies it)
        if (g_no_raster == 1) glDisable(GL_RASTERIZER_DISCARD);
        g_no_raster = req;
        if (g_no_raster == 1) glEnable(GL_RASTERIZER_DISCARD);
        std::fprintf(stderr, "[DRAW] raster mode %d (requested)\n", g_no_raster);
    }
    if (g_no_raster_mode && g_no_raster == 0 && g_no_raster_after > 0.0
        && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t_start).count()
               >= g_no_raster_after) {
        g_no_raster = g_no_raster_mode;
        if (g_no_raster == 1) glEnable(GL_RASTERIZER_DISCARD);
        std::fprintf(stderr, "[DRAW] raster discard mode %d now active\n", g_no_raster);
    }

    // [FPS] GE frames (display flips) per second, logged every 5 s.
    {
        static auto t0 = std::chrono::steady_clock::now();
        static uint32_t n = 0;
        n++;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t0).count();
        if (dt >= 5.0) {
            std::fprintf(stderr, "[FPS] %.1f GE frames/s (%u in %.1fs)\n", n / dt, n, dt);
            psp_prof::report(dt, n);
            n = 0;
            t0 = now;
        }
    }
    gl_stats_frame();

    // Select the FBO matching the displayed (scanned-out) VRAM address — the
    // front buffer. On key miss for a real (non-zero) address the guest is
    // scanning out a VRAM buffer the GE has not rendered into yet (e.g. the
    // first flip to the back buffer before any list targets it). Create that
    // buffer's own FBO — cleared to black — and present THAT, rather than
    // blitting an unrelated sibling slot (which would publish a different
    // buffer's stale/cleared contents as if they were this frame). This keeps
    // present strictly per-address: a fresh flip shows a clean buffer, and any
    // geometry the GE later renders into this address lands in the same FBO.
    wide_update();   // the window may have changed shape
    tall_update();
    uint32_t key = ge_fb_key(fb_addr);
    mark_main_key(key);   // a buffer that is displayed is rendered at the render scale
    if (g_dyn_trace_n.load() > 0)
        std::fprintf(stderr, "[DT] ---- PRESENT fb=0x%08X\n", fb_addr);
    int slot = -1;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        if (g_targets[i].fbo != 0 && g_targets[i].key == key) { slot = i; break; }
    }
    if (slot < 0) {
        if (key != 0) {
            // Materialize the displayed buffer's own (cleared) FBO.
            slot = acquire_target(key);
        } else {
            // key==0 (FRAMEBUFPTR/displayed addr not yet meaningful): keep the
            // most-recently-rendered target so present never blanks.
            slot = (g_front_target >= 0) ? g_front_target : g_current_target;
            static int fallback_count = 0;
            if (++fallback_count <= 20) {
                std::fprintf(stderr,
                    "[PRESENT] fallback: no FBO for fb=0x%08X key=0x%06X "
                    "(using slot=%d)\n", fb_addr, key, slot);
            }
        }
    }
    if (slot < 0) return;  // nothing rendered yet

    g_front_target = slot;
    g_targets[slot].last_use = ++g_target_clock;
    if (std::getenv("PSPRECOMP_MOVIE_TEXDBG"))
        std::fprintf(stderr, "[DISPSET] fb=%08X key=%06X slot=%d\n", fb_addr, key, slot);

    static int present_count = 0;
    if (++present_count <= 5) {
        std::fprintf(stderr,
            "[PRESENT] Frame %d (fb=0x%08X key=0x%06X stride=%u fmt=%u)\n",
            present_count, fb_addr, key, fb_stride, fb_format);
    }
    present_blit(g_targets[slot].fbo);
}

// Liveness net (§2.1): if lists were processed but no real page-flip arrived
// within the stale budget, present the current front buffer once. Preserves
// "content becomes visible even without a flip" without making every list a
// frame boundary. Main (GL) thread only — same thread as the routine present.
void ge_draw_present_if_stale(uint8_t* rdram) {
    (void)rdram;
    if (!g_present_dirty) return;
    auto age = std::chrono::steady_clock::now() - g_last_present;
    if (age < std::chrono::milliseconds(g_present_stale_ms)) return;
    GLuint fbo = front_fbo_id();
    if (fbo == 0) { g_present_dirty = false; return; }
    present_blit(fbo, "stale");
}

// ---- Settings menu support (psp_settings.cpp, main/GL thread) ----

// Change the render scale live: every pool target gets new colour/depth storage
// at the new size with its picture carried over (linear resample), keeping its
// FBO object, so a target the game samples later (a captured background) is
// not lost. Scratch copies are rebuilt lazily. Call while the game is paused
// (no display list mid-way). Refused when the A/B shadow targets are on.
bool ge_draw_set_render_scale(int n) {
    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    n = n < 1 ? 1 : n > 16 ? 16 : n;
    while (n > 1 && (480 * n > maxtex || 272 * n > maxtex)) n--;
    if (n == g_rs) return true;
    if (g_ab) return false;
    flush_batch();
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    GLuint tmp = 0;
    glGenFramebuffers(1, &tmp);
    int resized = 0;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (!t.fbo || t.rs != g_rs) continue;   // the 1x buffers of the blur chain stay as they are
        const int ow = t.w480 * t.rs, oh = t.h272 * t.rs, nw = t.w480 * n, nh = t.h272 * n;
        GLuint tex = 0, rb = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), nw, nh, 0, GL_RGBA, scene_type(), nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, tmp);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp);
        glBlitFramebuffer(0, 0, ow, oh, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, nw, nh);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDeleteTextures(1, &t.color_tex);
        glDeleteRenderbuffers(1, &t.depth_rb);
        t.color_tex = tex;
        t.depth_rb = rb;
        t.rs = n;
        resized++;
    }
    glDeleteFramebuffers(1, &tmp);
    delete_scratch();
    g_rs = n;
    glLineWidth(float(g_rs));
    glPointSize(float(g_rs));
    while (glGetError() != GL_NO_ERROR) {}
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_current_target >= 0 ? g_targets[g_current_target].fbo : 0);
    glc::invalidate();
    ge_texture_invalidate_bind_cache();
    std::fprintf(stderr, "[DRAW] render scale %dx (targets %dx%d), %d target(s) resized\n", g_rs, g_wide_px * g_rs, g_tall_px * g_rs, resized);
    return true;
}

// HDR Scene toggle (or the HDR output going live / off): give every scene target the right colour format, carrying the
// picture over. Same constraints as ge_draw_set_render_scale (game parked, no display list in flight). Cheap no-op when
// nothing changed. Returns true when the targets were reallocated.
bool ge_draw_scene_format_stale() { return !g_ab && psp_hdr_scene_live() != g_scene_fp16; }

bool ge_draw_sync_scene_format() {
    const bool want = psp_hdr_scene_live();
    if (want == g_scene_fp16 || g_ab) return false;
    flush_batch();
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_scene_fp16 = want;
    GLuint tmp = 0;
    glGenFramebuffers(1, &tmp);
    int n = 0;
    for (int i = 0; i < GE_MAX_TARGETS; ++i) {
        GeTarget& t = g_targets[i];
        if (!t.fbo) continue;
        const int w = t.w480 * t.rs, h = t.h272 * t.rs;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, scene_ifmt(), w, h, 0, GL_RGBA, scene_type(), nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, tmp);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glDeleteTextures(1, &t.color_tex);
        t.color_tex = tex;
        t.mip_dirty = true;
        n++;
    }
    glDeleteFramebuffers(1, &tmp);
    delete_scratch();
    for (int i = 0; i < GE_MAX_TARGETS; ++i) fxl_free(g_fxl[i]);
    g_fx_screen_w = 0;   // recreated in the new format on next use
    while (glGetError() != GL_NO_ERROR) {}
    if (sc) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_current_target >= 0 ? g_targets[g_current_target].fbo : 0);
    glc::invalidate();
    ge_texture_invalidate_bind_cache();
    std::fprintf(stderr, "[DRAW] HDR Scene %s: %d target(s) now %s\n", want ? "on" : "off", n, want ? "RGBA16F" : "RGBA8");
    return true;
}

// Present now (the game may be paused): the last game frame with the dialog /
// menu overlay over it, or black under the overlay before the first frame.
void ge_draw_present_overlay_now() {
    if (GLuint fbo = front_fbo_id()) { present_blit(fbo, "menu"); return; }
    SDL_Window* window = psp_get_sdl_window();
    if (!window) return;
    const PresentRect pr = present_layout(window), cpr = band_rect(pr);
    psp_sddialog::set_output_scale(std::max({g_rs, (cpr.x1 - cpr.x0 + 479) / 480, 1}));
    note_overlay_rect(window, cpr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    present_dialog_overlay(cpr, pr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    { psp_perf::Scope swap_scope_(psp_perf::SWAP); psp_gl_swap(window); }
    if (sc) glEnable(GL_SCISSOR_TEST);
    glc::invalidate();
}

// ---- Debug socket accessors (issue #35) ----

void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
) {
    // Racy reads of render-thread counters -- diagnostics only.
    if (frames) *frames = g_frame_counter;
    if (prims_total) *prims_total = g_prim_total_observed;
    int real_ns = 0, sprite_nc = 0, clr = 0;
    for (int t = 0; t < 8; ++t) {
        clr += g_prim_type_count_clear[t];
        if (t == 6) sprite_nc += g_prim_type_count[t];
        else real_ns += g_prim_type_count[t];
    }
    if (real_nonsprite) *real_nonsprite = real_ns;
    if (sprite_nonclear) *sprite_nonclear = sprite_nc;
    if (clears) *clears = clr;
}

bool ge_draw_capture_screenshot(const char* path, int timeout_ms) {
    if (!g_draw_ready.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
            "[DRAW] Screenshot request before GL init: %s\n", path);
        return false;
    }
    std::unique_lock<std::mutex> lock(g_ss_req_mutex);
    if (g_ss_req_state == SS_PENDING) {
        // Another client's capture is in flight -- refuse instead of
        // overwriting its path.
        return false;
    }
    std::strncpy(g_ss_req_path, path, sizeof(g_ss_req_path) - 1);
    g_ss_req_path[sizeof(g_ss_req_path) - 1] = '\0';
    g_ss_req_state = SS_PENDING;
    bool finished = g_ss_req_cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [] { return g_ss_req_state == SS_DONE
                    || g_ss_req_state == SS_FAILED; });
    bool ok = finished && g_ss_req_state == SS_DONE;
    g_ss_req_state = SS_IDLE;   // reset (also cancels on timeout)
    return ok;
}

void ge_draw_service_screenshot_request() {
    char path[sizeof(g_ss_req_path)];
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        if (g_ss_req_state != SS_PENDING) return;
        std::memcpy(path, g_ss_req_path, sizeof(path));
    }
    // GL work outside the request lock (requester only waits on the cv).
    bool ok = g_draw_ready.load(std::memory_order_acquire)
              && capture_fbo_to_tga(path);
    {
        std::lock_guard<std::mutex> lock(g_ss_req_mutex);
        // Only publish if the requester hasn't timed out and reset state.
        if (g_ss_req_state == SS_PENDING) {
            g_ss_req_state = ok ? SS_DONE : SS_FAILED;
        }
    }
    g_ss_req_cv.notify_all();
}
