// games/dissidia/runtime/menu_layer.h — a small straight-alpha painter for ge_draw_set_game_layer: elements the runtime adds to
// the game's own screens (menu_settings.cpp), drawn with the game's text font (the sceFont host file) and its menu textures.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "psp_ge_draw.h"

namespace dissidia {

struct LayerColor { float r, g, b, a; };   // 0..255 for r, g, b; 0..1 for a

class MenuLayer {
public:
    // A 480x272 design-unit layer at `scale` pixels per unit.
    void begin(int scale);
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
    void copy(float sx0, float sy0, float sx1, float sy1, float dx0, float dy0, float dx1, float dy1) { copies_.push_back({sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1}); }
    // publish() in two steps, for a screen that caches its finished image: bake() converts, publish_baked() hands it over (no copies).
    std::shared_ptr<const std::vector<uint32_t>> bake() const;
    int width() const { return w_; }
    int height() const { return h_; }
    static void publish_baked(const std::vector<uint32_t>& img, int w, int h);
    void publish();   // hand the image to the runtime (ge_draw_set_game_layer)
    void clear();     // remove it

    void blend(int x, int y, float r, float g, float b, float a);   // pixel space, straight alpha "over"
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
    int s_ = 2, w_ = 0, h_ = 0;
    float zk_ = 1.f, zx_ = 0.f, zy_ = 0.f;
    std::vector<float> px_;   // r, g, b (0..255, not premultiplied), a (0..1)
    std::vector<GeLayerCopy> copies_;
};

template <class F> void MenuLayer::band(float x0, float y0, float x1, float y1, F color_at) {
    const float px0 = PX(x0), px1 = PX(x1), py0 = PY(y0), py1 = PY(y1);
    const int ix0 = std::max(0, int(px0)), ix1 = std::min(w_, int(px1 + 0.999f));
    const int iy0 = std::max(0, int(py0)), iy1 = std::min(h_, int(py1 + 0.999f));
    for (int y = iy0; y < iy1; ++y) {
        const float top = std::min(1.f, std::max(0.f, (y + 1) - py0)), bot = std::min(1.f, std::max(0.f, py1 - y));
        const float cov = std::min(top, bot);
        for (int x = ix0; x < ix1; ++x) {
            const LayerColor c = color_at(IX(x + 0.5f), IY(y + 0.5f));
            blend(x, y, c.r, c.g, c.b, c.a * cov);
        }
    }
}

}  // namespace dissidia
