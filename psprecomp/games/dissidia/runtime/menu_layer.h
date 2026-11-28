// games/dissidia/runtime/menu_layer.h — a small straight-alpha painter for ge_draw_set_game_layer: elements the runtime adds to
// the game's own screens (menu_settings.cpp), drawn with the game's text font (the sceFont host file) and its menu textures.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "psp_ge_draw.h"

namespace dissidia {

struct LayerColor { float r, g, b, a; };   // 0..255 for r, g, b; 0..1 for a

class MenuLayer {
public:
    // Screens that opt in (frame_cache = true) get the 26 Nov frame cache (docs/tag-team.md "Batch fifty-six"): every primitive also folds its
    // parameters into a hash; a frame whose hash is the one already shown is not rasterised (a "dry" pass: the draw code runs, the layer calls only
    // hash), a state seen before is published from a cache of finished images, and only a changed state is rasterised (the frame after the
    // change is seen). PSPRECOMP_MENUFPS_OLD=1: every frame rasterised as before. PSPRECOMP_MENUFPS_LOG=1: one line a second per screen.
    MenuLayer() = default;
    MenuLayer(const char* n, bool cache) : frame_cache(cache), name(n) {}
    bool frame_cache = false;
    bool incremental = true;     // may keep the unchanged start of an animated screen (off for the battle HUD: its first parts move every frame)
    const char* name = "layer";   // log label (the layer); tag() names the screen drawn this frame
    void tag(const char* t) { tag_ = t; }
    // A screen that writes pixels itself (blend): note() its parameters, then draw only if want_raster() (false in a hash-only pass and for the unchanged
    // leading part of an animated screen, whose pixels are already there).
    bool want_raster() { return gate(); }
    void note(uint64_t v) { hh_ = (hh_ ^ v) * 0x100000001b3ull; hh_ ^= hh_ >> 31; }
    void notef(float v) { uint32_t u; std::memcpy(&u, &v, 4); note(u); }
    // A 480x272 design-unit layer at `scale` pixels per unit.
    void begin(int scale);
    void begin_raw(int scale);   // always rasterised, never cached (a caller that bakes the image itself)
    int scale() const { return s_; }
    // Horizontal gradient band: colour(x) for x in design units, rows [y0, y1) with soft vertical edges.
    template <class F> void band(float x0, float y0, float x1, float y1, F color_at);
    void line(float x0, float x1, float y, float thick, LayerColor c);
    void glow(float cx, float cy, float rad, LayerColor c);   // soft round dot
    // RGBA8 image (straight alpha, w x h), source rect (sx, sy, sw, sh) drawn to (x, y, w, h) design units.
    void image(const uint32_t* px, int iw, int ih, int sx, int sy, int sw, int sh, float x, float y, float w, float h, float alpha);
    // Text in the game's font (bold or regular), em size in design units, left x and baseline y; returns the advance width.
    float text(const std::string& s, bool bold, float em, float x, float baseline, LayerColor c, bool measure_only = false);
    // Serif small capitals (Crimson Text SemiBold, shipped with the runtime): capitals at `em`, lower case as capitals at
    // `small` x em, `track` extra units between letters; the title screen's lettering.
    float serif_caps(const std::string& s, float em, float small, float track, float x, float baseline, LayerColor c, bool measure_only = false);
    // Part of the game's own picture drawn again under the layer (ge_draw_set_game_layer_copies), PSP units.
    void copy(float sx0, float sy0, float sx1, float sy1, float dx0, float dy0, float dx1, float dy1) { if (cache_on_) { note(6); notef(sx0); notef(sy0); notef(sx1); notef(sy1); notef(dx0); notef(dy0); notef(dx1); notef(dy1); } copies_.push_back({sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1}); }
    // publish() in two steps, for a screen that caches its finished image: bake() converts, publish_baked() hands it over (no copies).
    std::shared_ptr<const std::vector<uint32_t>> bake() const;
    int width() const { return w_; }
    int height() const { return h_; }
    static void publish_baked(const std::vector<uint32_t>& img, int w, int h);
    void publish();   // hand the image to the runtime (ge_draw_set_game_layer)
    void clear();     // remove it

    void blend(int x, int y, float r, float g, float b, float a);   // pixel space, straight alpha "over" (a cached screen's own loops: note() their parameters, skip them when dry())
    float opacity = 1.f;   // multiplies every element drawn from now on (a faded group: the overhead mini panels)
    // Zoom about a design-unit point: everything drawn from now on is scaled by k around (ox, oy) (a smaller group: the tag team side panels).
    // Measured widths stay in unzoomed design units, so a layout written for k = 1 shrinks as a whole. set_zoom(1, 0, 0) turns it off.
    void set_zoom(float k, float ox, float oy) { zk_ = k > 0.f ? k : 1.f; zx_ = ox; zy_ = oy; }
    float PX(float x) const { return (zx_ + (x - zx_) * zk_) * float(s_); }   // design unit -> pixel
    float PY(float y) const { return (zy_ + (y - zy_) * zk_) * float(s_); }
    float IX(float p) const { return zx_ + (p / float(s_) - zx_) / zk_; }     // pixel -> design unit
    float IY(float p) const { return zy_ + (p / float(s_) - zy_) / zk_; }
    float S() const { return float(s_) * zk_; }                              // pixels per design unit
private:
    void note_state() { notef(opacity); notef(zk_); notef(zx_); notef(zy_); }
    // blend with the opacity already applied, inside the canvas, the bounding boxes already widened
    void put_nb(int x, int y, float r, float g, float b, float a) {
        if (a > 1.f) a = 1.f;
        float* p = &px_[(size_t(y) * size_t(w_) + size_t(x)) * 4];
        if (p[3] <= 0.f || a >= 1.f) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; return; }
        const float da = p[3] * (1.f - a), oa = a + da, inv = 1.f / oa;
        p[0] = (r * a + p[0] * da) * inv; p[1] = (g * a + p[1] * da) * inv; p[2] = (b * a + p[2] * da) * inv; p[3] = oa;
    }
    bool gate();                       // after a primitive's parameters are hashed: true = rasterise it
    void take_snapshot(size_t k);
    void restore_rows(const int bb[4]);
    void put(int x, int y, float r, float g, float b, float a);   // blend() without the dry check
    void convert(int x0, int y0, int x1, int y1) const;           // px_ -> out_ for the rectangle
    void clear_px();
    struct Cached { std::shared_ptr<const std::vector<uint32_t>> img; int w = 0, h = 0; std::vector<GeLayerCopy> copies; uint64_t used = 0; };
    // Incremental frames: once a screen animates, px_ is kept at the state after its first snap_n_ primitives (snap_, the part that did not change in the
    // last two frames); the next frame skips those primitives (checked by their running hash) and draws only the rest.
    bool inc_ = false, snap_ok_ = false, aborted_ = false, fd_done_ = false, inframe_ = false;
    int streak_ = 0, snap_cool_ = 0, drops_ = 0; size_t snap_n_ = 0; double last_area_ = 0.0;
    std::vector<uint64_t> cum_, last_cum_, snap_cum_;
    std::vector<float> snap_;
    int sb_[4] = {0, 0, -1, -1};      // bounding box of what was written since the snapshot
    bool cache_on_ = false, dry_ = false, hot_ = true, out_ok_ = false, px_ok_ = false;
    uint64_t hh_ = 0, pub_key_ = 0, wet_key_ = 0, pub_serial_ = 0, tick_ = 0;
    const char* tag_ = nullptr;
    std::chrono::steady_clock::time_point t_begin_{};
    int px_bb_[4] = {0, 0, -1, -1}, out_bb_[4] = {0, 0, -1, -1};   // x0, y0, x1, y1 (inclusive) of what px_ / out_ hold
    mutable std::vector<uint32_t> out_;
    std::map<uint64_t, Cached> cache_;
    std::shared_ptr<const std::vector<uint32_t>> pub_img_;
    int s_ = 2, w_ = 0, h_ = 0;
    float zk_ = 1.f, zx_ = 0.f, zy_ = 0.f;
    std::vector<float> px_;   // r, g, b (0..255, not premultiplied), a (0..1)
    std::vector<GeLayerCopy> copies_;
};

template <class F> void MenuLayer::band(float x0, float y0, float x1, float y1, F color_at) {
    bool flat = false; LayerColor c0{0, 0, 0, 0};
    if (cache_on_) {   // the frame cache's hash: the rectangle, the layer state, the colour at a 9 x 9 grid
        note(1); notef(x0); notef(y0); notef(x1); notef(y1); note_state();
        flat = true;
        for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) {
            const LayerColor c = color_at(x0 + (x1 - x0) * float(i) * 0.125f, y0 + (y1 - y0) * float(j) * 0.125f);
            notef(c.r); notef(c.g); notef(c.b); notef(c.a);
            if (i == 0 && j == 0) c0 = c; else if (c.r != c0.r || c.g != c0.g || c.b != c0.b || c.a != c0.a) flat = false;
        }
        if (!gate()) return;
    }
    const float px0 = PX(x0), px1 = PX(x1), py0 = PY(y0), py1 = PY(y1);
    const int ix0 = std::max(0, int(px0)), ix1 = std::min(w_, int(px1 + 0.999f));
    const int iy0 = std::max(0, int(py0)), iy1 = std::min(h_, int(py1 + 0.999f));
    if (flat && zk_ == 1.f && ix1 > ix0 && iy1 > iy0) {   // one colour all over (a dim, a base): no per-pixel colour, one bounding box update
        const float ca = c0.a * opacity;
        if (ca > 0.f) {
            px_bb_[0] = std::min(px_bb_[0], ix0); px_bb_[1] = std::min(px_bb_[1], iy0); px_bb_[2] = std::max(px_bb_[2], ix1 - 1); px_bb_[3] = std::max(px_bb_[3], iy1 - 1);
            sb_[0] = std::min(sb_[0], ix0); sb_[1] = std::min(sb_[1], iy0); sb_[2] = std::max(sb_[2], ix1 - 1); sb_[3] = std::max(sb_[3], iy1 - 1);
            for (int y = iy0; y < iy1; ++y) {
                const float top = std::min(1.f, std::max(0.f, (y + 1) - py0)), bot = std::min(1.f, std::max(0.f, py1 - y));
                const float a = ca * std::min(top, bot);
                if (a <= 0.f) continue;
                for (int x = ix0; x < ix1; ++x) put_nb(x, y, c0.r, c0.g, c0.b, a);
            }
        }
        return;
    }
    for (int y = iy0; y < iy1; ++y) {
        const float top = std::min(1.f, std::max(0.f, (y + 1) - py0)), bot = std::min(1.f, std::max(0.f, py1 - y));
        const float cov = std::min(top, bot);
        for (int x = ix0; x < ix1; ++x) {
            const LayerColor c = color_at(IX(x + 0.5f), IY(y + 0.5f));
            put(x, y, c.r, c.g, c.b, c.a * cov);
        }
    }
}

}  // namespace dissidia
