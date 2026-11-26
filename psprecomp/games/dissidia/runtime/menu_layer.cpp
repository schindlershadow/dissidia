// games/dissidia/runtime/menu_layer.cpp — see menu_layer.h.
#include "menu_layer.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <tuple>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

extern const unsigned char psp_ui_font_bodyb[];   // runtime/assets/embed_fonts.cmake (Crimson Text SemiBold)
extern const unsigned long psp_ui_font_bodyb_len;
extern const unsigned char psp_sans_regular[], psp_sans_bold[];   // Noto Sans: the game text's face where the host has none
extern const unsigned long psp_sans_regular_len, psp_sans_bold_len;

namespace dissidia {

namespace {

struct Faces {
    FT_Library lib = nullptr;
    FT_Face reg = nullptr, bold = nullptr, serif = nullptr;
    std::vector<uint8_t> reg_bytes, bold_bytes;
};
bool read_file(const std::string& p, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}
// The game's text is drawn by sceFont from these host files (psp_hle_font): the same faces make an added label match.
Faces& faces() {
    static Faces f;
    static std::once_flag once;
    std::call_once(once, [] {
        if (FT_Init_FreeType(&f.lib)) { f.lib = nullptr; return; }
        FT_New_Memory_Face(f.lib, psp_ui_font_bodyb, FT_Long(psp_ui_font_bodyb_len), 0, &f.serif);
        // The same faces sceFont uses (psp_hle_font.cpp ft_init): the embedded Noto Sans on Android / Windows and wherever the host
        // has no font, the host file elsewhere; PSPRECOMP_FONT (a file) wins, PSPRECOMP_FONT=embedded forces Noto.
        std::string reg, bold;
        const char* fenv = std::getenv("PSPRECOMP_FONT");
        bool embedded = fenv && !std::strcmp(fenv, "embedded");
#if defined(__ANDROID__) || defined(_WIN32)
        if (!(fenv && *fenv)) embedded = true;
#endif
        if (!embedded && !psp_font_host_files(&reg, &bold)) embedded = true;
        if (embedded) reg.clear(), bold.clear();
        if (!reg.empty() && read_file(reg, &f.reg_bytes)) FT_New_Memory_Face(f.lib, f.reg_bytes.data(), FT_Long(f.reg_bytes.size()), 0, &f.reg);
        if (!bold.empty() && read_file(bold, &f.bold_bytes)) FT_New_Memory_Face(f.lib, f.bold_bytes.data(), FT_Long(f.bold_bytes.size()), 0, &f.bold);
        if (!f.reg) FT_New_Memory_Face(f.lib, psp_sans_regular, FT_Long(psp_sans_regular_len), 0, &f.reg);
        if (!f.bold) FT_New_Memory_Face(f.lib, psp_sans_bold, FT_Long(psp_sans_bold_len), 0, &f.bold);
        if (!f.bold) f.bold = f.reg;
    });
    return f;
}

}  // namespace

void MenuLayer::begin(int scale) {
    s_ = std::max(1, std::min(4, scale));
    w_ = 480 * s_; h_ = 272 * s_;
    px_.assign(size_t(w_) * size_t(h_) * 4, 0.f);
    copies_.clear();
    zk_ = 1.f; zx_ = zy_ = 0.f;
}

void MenuLayer::blend(int x, int y, float r, float g, float b, float a) {
    a *= opacity;
    if (a <= 0.f || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (a > 1.f) a = 1.f;
    float* p = &px_[(size_t(y) * size_t(w_) + size_t(x)) * 4];
    if (p[3] <= 0.f) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; return; }   // empty pixel: the same result without the divisions
    const float da = p[3] * (1.f - a), oa = a + da;
    if (oa <= 0.f) return;
    const float inv = 1.f / oa;
    p[0] = (r * a + p[0] * da) * inv; p[1] = (g * a + p[1] * da) * inv; p[2] = (b * a + p[2] * da) * inv; p[3] = oa;
}

void MenuLayer::line(float x0, float x1, float y, float thick, LayerColor c) {
    band(x0, y - thick / 2, x1, y + thick / 2, [&](float, float) { return c; });
}

void MenuLayer::glow(float cx, float cy, float rad, LayerColor c) {
    const int x0 = std::max(0, int(PX(cx - rad))), x1 = std::min(w_, int(PX(cx + rad)) + 1);
    const int y0 = std::max(0, int(PY(cy - rad))), y1 = std::min(h_, int(PY(cy + rad)) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = IX(x + 0.5f) - cx, dy = IY(y + 0.5f) - cy, d2 = (dx * dx + dy * dy) / (rad * rad);
            if (d2 < 1.f) { const float k = 1.f - std::sqrt(d2); blend(x, y, c.r, c.g, c.b, c.a * k * k); }
        }
}

void MenuLayer::image(const uint32_t* px, int iw, int ih, int sx, int sy, int sw, int sh, float x, float y, float w, float h, float alpha) {
    if (!px || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, int(PX(x))), x1 = std::min(w_, int(PX(x + w) + 0.999f));
    const int y0 = std::max(0, int(PY(y))), y1 = std::min(h_, int(PY(y + h) + 0.999f));
    auto texel = [&](int tx, int ty, float* out) {
        tx = std::min(std::max(tx, sx), sx + sw - 1); ty = std::min(std::max(ty, sy), sy + sh - 1);
        tx = std::min(std::max(tx, 0), iw - 1); ty = std::min(std::max(ty, 0), ih - 1);
        const uint32_t p = px[size_t(ty) * size_t(iw) + size_t(tx)];
        const float a = float(p >> 24) / 255.f;
        out[0] = float(p & 255) * a; out[1] = float((p >> 8) & 255) * a; out[2] = float((p >> 16) & 255) * a; out[3] = a;   // premultiplied for filtering
    };
    for (int py = y0; py < y1; ++py)
        for (int pxx = x0; pxx < x1; ++pxx) {
            const float u = sx + (IX(pxx + 0.5f) - x) / w * sw - 0.5f, v = sy + (IY(py + 0.5f) - y) / h * sh - 0.5f;
            const int iu = int(std::floor(u)), iv = int(std::floor(v));
            const float fu = u - iu, fv = v - iv;
            float a[4], b[4], c[4], d[4], o[4];
            texel(iu, iv, a); texel(iu + 1, iv, b); texel(iu, iv + 1, c); texel(iu + 1, iv + 1, d);
            for (int k = 0; k < 4; k++) o[k] = (a[k] * (1 - fu) + b[k] * fu) * (1 - fv) + (c[k] * (1 - fu) + d[k] * fu) * fv;
            if (o[3] <= 0.001f) continue;
            blend(pxx, py, o[0] / o[3], o[1] / o[3], o[2] / o[3], o[3] * alpha);
        }
}

// Rendered glyph cache: the HUD layer draws every string several times a frame (shadow passes, measuring) and used to render each glyph again with
// FreeType every time (a banner / notice frame cost 40 ms: the stutter after a call-in). Key: face, pixel size, glyph index. Caller holds the text mutex.
// PSPRECOMP_GLYPH_CACHE_OFF=1 renders as before.
struct CGlyph { std::vector<uint8_t> buf; int rows = 0, width = 0, pitch = 0, left = 0, top = 0; float advance = 0.f; bool ok = false; };
const CGlyph& cached_glyph(FT_Face face, unsigned px, FT_UInt gi) {
    static std::map<std::tuple<FT_Face, unsigned, FT_UInt>, CGlyph> cache; static const bool off = std::getenv("PSPRECOMP_GLYPH_CACHE_OFF") != nullptr; static CGlyph tmp;
    const auto key = std::make_tuple(face, px, gi);
    if (!off) { auto it = cache.find(key); if (it != cache.end()) return it->second; }
    CGlyph& c = off ? tmp : cache[key];
    c = CGlyph{};
    if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
        const FT_GlyphSlot g = face->glyph;
        c.ok = true; c.rows = int(g->bitmap.rows); c.width = int(g->bitmap.width); c.pitch = g->bitmap.pitch; c.left = g->bitmap_left; c.top = g->bitmap_top; c.advance = g->advance.x / 64.f;
        if (g->bitmap.buffer && c.rows > 0 && c.pitch > 0) c.buf.assign(g->bitmap.buffer, g->bitmap.buffer + size_t(c.rows) * size_t(c.pitch));
    }
    return c;
}

float MenuLayer::text(const std::string& str, bool bold, float em, float x, float baseline, LayerColor c, bool measure_only) {
    Faces& f = faces();
    FT_Face face = bold ? f.bold : f.reg;
    if (!face) return 0.f;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const unsigned px = unsigned(std::lround(em * S()));
    FT_Set_Pixel_Sizes(face, 0, FT_UInt(px));   // (kerning below reads the current size)
    const float pen0 = PX(x); float pen = pen0;
    const int by = int(std::lround(PY(baseline)));
    FT_UInt prev = 0;
    for (unsigned char ch : str) {
        const FT_UInt gi = FT_Get_Char_Index(face, ch);
        if (prev && gi && FT_HAS_KERNING(face)) { FT_Vector k; FT_Get_Kerning(face, prev, gi, FT_KERNING_DEFAULT, &k); pen += k.x / 64.f; }
        const CGlyph& g = cached_glyph(face, px, gi);
        if (g.ok) {
            if (!measure_only) {
                const int gx = int(std::lround(pen)) + g.left, gy = by - g.top;
                for (int r = 0; r < g.rows; r++)
                    for (int q = 0; q < g.width; q++) {
                        const uint8_t cov = g.buf[size_t(r) * size_t(g.pitch) + size_t(q)];
                        if (cov) blend(gx + q, gy + r, c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g.advance;
        }
        prev = gi;
    }
    return (pen - pen0) / S();
}

float MenuLayer::serif_caps(const std::string& str, float em, float small, float track, float x, float baseline, LayerColor c, bool measure_only) {
    FT_Face face = faces().serif;
    if (!face) return 0.f;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const float pen0 = PX(x); float pen = pen0;
    const int by = int(std::lround(PY(baseline)));
    for (unsigned char ch : str) {
        const bool lower = ch >= 'a' && ch <= 'z';
        const unsigned px = unsigned(std::lround(em * (lower ? small : 1.f) * S()));
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(px));
        const FT_UInt gi = FT_Get_Char_Index(face, lower ? ch - 32 : ch);
        const CGlyph& g = cached_glyph(face, px, gi);
        if (g.ok) {
            if (!measure_only) {
                const int gx = int(std::lround(pen)) + g.left, gy = by - g.top;
                for (int r = 0; r < g.rows; r++)
                    for (int q = 0; q < g.width; q++) {
                        const uint8_t cov = g.buf[size_t(r) * size_t(g.pitch) + size_t(q)];
                        if (cov) blend(gx + q, gy + r, c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g.advance + track * S();
        }
    }
    return (pen - pen0) / S() - track;
}

std::shared_ptr<const std::vector<uint32_t>> MenuLayer::bake() const {
    auto out = std::make_shared<std::vector<uint32_t>>(size_t(w_) * size_t(h_));
    for (size_t i = 0; i < out->size(); i++) {
        const float* p = &px_[i * 4];
        auto ch = [](float v) { return uint32_t(std::min(255.f, std::max(0.f, v + 0.5f))); };
        (*out)[i] = ch(p[0]) | ch(p[1]) << 8 | ch(p[2]) << 16 | uint32_t(std::min(255.f, p[3] * 255.f + 0.5f)) << 24;
    }
    return out;
}

void MenuLayer::publish_baked(const std::vector<uint32_t>& img, int w, int h) {
    ge_draw_set_game_layer_copies(nullptr, 0);
    ge_draw_set_game_layer(img.data(), w, h);
}

void MenuLayer::publish() {
    const auto out = bake();
    ge_draw_set_game_layer_copies(copies_.data(), int(copies_.size()));
    ge_draw_set_game_layer(out->data(), w_, h_);
}

void MenuLayer::clear() { ge_draw_clear_game_layer(); }

}  // namespace dissidia
