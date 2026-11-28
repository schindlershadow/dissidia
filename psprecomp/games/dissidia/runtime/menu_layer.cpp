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
#include <unordered_map>
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

namespace {
const bool g_old = std::getenv("PSPRECOMP_MENUFPS_OLD") != nullptr;   // every frame rasterised and uploaded in full, as before 26 Nov
const bool g_verify = std::getenv("PSPRECOMP_MENUFPS_VERIFY") != nullptr;   // test aid: always rasterise, report a frame whose hash matched the last one but whose pixels did not
const bool g_log = std::getenv("PSPRECOMP_MENUFPS_LOG") != nullptr;
const bool g_inc_off = std::getenv("PSPRECOMP_MENUFPS_INC_OFF") != nullptr;   // no incremental frames (an animated screen is rasterised whole)
void stat_flush(std::chrono::steady_clock::time_point now);
struct FpsStat { int calls = 0, wet = 0, same = 0, cached = 0, inc = 0; double ms = 0, mx = 0, wet_ms = 0, wet_mx = 0, prims = 0, skipped = 0, area = 0, pub = 0; };
std::map<std::string, FpsStat> g_stats;
std::chrono::steady_clock::time_point g_stat_t{};
void stat_note(const std::string& key, double ms, int kind, int prims = 0, int skipped = 0, double area = 0, double pub = 0) {   // kind 0 rasterised, 1 unchanged, 2 from the cache
    const auto now = std::chrono::steady_clock::now();
    if (g_stat_t.time_since_epoch().count() == 0) g_stat_t = now;
    FpsStat& st = g_stats[key];
    st.calls++; st.ms += ms; st.mx = std::max(st.mx, ms);
    st.pub += pub;
    if (kind == 0) { st.prims += prims; st.skipped += skipped; st.area += area; if (skipped) st.inc++; st.wet++; st.wet_ms += ms; st.wet_mx = std::max(st.wet_mx, ms); } else if (kind == 1) st.same++; else st.cached++;
    stat_flush(now);
}
void stat_flush(std::chrono::steady_clock::time_point now) {
    const double dt = std::chrono::duration<double>(now - g_stat_t).count();
    if (dt < 1.0) return;
    for (auto& kv : g_stats) {
        const FpsStat& s = kv.second; if (!s.calls) continue;
        std::fprintf(stderr, "[MENUFPS] %-22s ticks/s %5.1f  raster %3d (avg %.2f max %.2f ms)  unchanged %3d  cached %3d  layer cost avg %.2f max %.2f ms%s\n", kv.first.c_str(), s.calls / dt, s.wet,
                     s.wet ? s.wet_ms / s.wet : 0.0, s.wet_mx, s.same, s.cached, s.ms / s.calls, s.mx,
                     s.wet ? (std::string("  [prims ") + std::to_string(int(s.prims / s.wet)) + ", skipped " + std::to_string(int(s.skipped / s.wet)) + ", drawn area " + std::to_string(int(s.area * 100 / s.wet)) + "%, publish " + std::to_string(int(s.pub * 100 / s.calls) / 100.0).substr(0, 4) + " ms]").c_str() : "");
    }
    g_stats.clear(); g_stat_t = now;
}
}  // namespace

void MenuLayer::clear_px() {   // only what the last frame wrote
    const size_t need = size_t(w_) * size_t(h_) * 4;
    if (g_old || !px_ok_ || px_.size() != need) {
        px_.assign(need, 0.f); out_.assign(size_t(w_) * size_t(h_), 0u); out_bb_[0] = out_bb_[1] = 0; out_bb_[2] = out_bb_[3] = -1; px_ok_ = true; snap_ok_ = false;
    } else if (px_bb_[2] >= px_bb_[0] && px_bb_[3] >= px_bb_[1])
        for (int y = px_bb_[1]; y <= px_bb_[3]; y++) std::memset(&px_[(size_t(y) * size_t(w_) + size_t(px_bb_[0])) * 4], 0, size_t(px_bb_[2] - px_bb_[0] + 1) * 16);
    px_bb_[0] = sb_[0] = w_; px_bb_[1] = sb_[1] = h_; px_bb_[2] = px_bb_[3] = sb_[2] = sb_[3] = -1;
}

void MenuLayer::restore_rows(const int bb[4]) {   // px_ back to the snapshot inside a rectangle
    if (bb[2] < bb[0] || bb[3] < bb[1] || snap_.size() != px_.size()) return;
    for (int y = bb[1]; y <= bb[3]; y++) { const size_t o = (size_t(y) * size_t(w_) + size_t(bb[0])) * 4; std::memcpy(&px_[o], &snap_[o], size_t(bb[2] - bb[0] + 1) * 16); }
}

void MenuLayer::take_snapshot(size_t k) {   // px_ now holds the state after the first k primitives
    snap_ = px_;
    snap_n_ = k; snap_cum_.assign(cum_.begin(), cum_.begin() + std::ptrdiff_t(k)); snap_ok_ = true;
    sb_[0] = w_; sb_[1] = h_; sb_[2] = sb_[3] = -1;
}

bool MenuLayer::gate() {
    if (dry_) return false;
    const size_t k = cum_.size();
    cum_.push_back(hh_);
    if (aborted_) return false;
    if (inc_ && k < snap_n_) { if (hh_ != snap_cum_[k]) aborted_ = true; return false; }   // the unchanged start: its pixels are in px_ already
    if (streak_ >= 1 && !fd_done_ && (k >= last_cum_.size() || last_cum_[k] != hh_)) {   // the first primitive that differs from the last frame: from here on it animates
        fd_done_ = true;
        if (k > 0 && !g_inc_off && incremental && snap_cool_ <= 0 && (!snap_ok_ || k >= snap_n_ + 4)) take_snapshot(k);
    }
    return true;
}

void MenuLayer::begin_raw(int scale) {
    cache_on_ = dry_ = false;
    s_ = std::max(1, std::min(4, scale));
    const int nw = 480 * s_, nh = 272 * s_;
    if (nw != w_ || nh != h_) px_ok_ = false;
    w_ = nw; h_ = nh;
    px_ok_ = false; snap_ok_ = false; inc_ = false; inframe_ = false;   // (px_ may be at the snapshot state: a full clear)
    clear_px();
    copies_.clear();
    zk_ = 1.f; zx_ = zy_ = 0.f; opacity = 1.f;
    hh_ = 0; tag_ = nullptr; t_begin_ = std::chrono::steady_clock::now();
}

void MenuLayer::begin(int scale) {
    if (!frame_cache || g_old) { begin_raw(scale); return; }
    const int sc = std::max(1, std::min(4, scale));
    const bool dry = !hot_ && !g_verify;
    cache_on_ = true; dry_ = dry;
    if (sc != s_ || 480 * sc != w_) { px_ok_ = false; snap_ok_ = false; hot_ = true; pub_key_ = 0; cache_.clear(); pub_img_.reset(); out_ok_ = false; dry_ = false; streak_ = 0; }
    s_ = sc; w_ = 480 * s_; h_ = 272 * s_;
    if (!dry_) {
        if (inframe_) {   // begun again without a publish: what was drawn goes
            if (snap_ok_ && inc_) { restore_rows(px_bb_); px_bb_[0] = sb_[0] = w_; px_bb_[1] = sb_[1] = h_; px_bb_[2] = px_bb_[3] = sb_[2] = sb_[3] = -1; }
            else { px_ok_ = false; snap_ok_ = false; }
        }
        inc_ = snap_ok_ && px_ok_ && px_.size() == size_t(w_) * size_t(h_) * 4;
        if (!inc_) { snap_ok_ = false; clear_px(); }
        inframe_ = true;
    }
    aborted_ = false; fd_done_ = false; cum_.clear(); if (snap_cool_ > 0 && !dry_) snap_cool_--;
    copies_.clear();
    zk_ = 1.f; zx_ = zy_ = 0.f; opacity = 1.f;
    hh_ = 0xcbf29ce484222325ull ^ uint64_t(s_); tag_ = nullptr; t_begin_ = std::chrono::steady_clock::now();
}

void MenuLayer::put(int x, int y, float r, float g, float b, float a) {
    a *= opacity;
    if (a <= 0.f || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (a > 1.f) a = 1.f;
    if (x < px_bb_[0]) px_bb_[0] = x;
    if (x > px_bb_[2]) px_bb_[2] = x;
    if (y < px_bb_[1]) px_bb_[1] = y;
    if (y > px_bb_[3]) px_bb_[3] = y;
    if (x < sb_[0]) sb_[0] = x;
    if (x > sb_[2]) sb_[2] = x;
    if (y < sb_[1]) sb_[1] = y;
    if (y > sb_[3]) sb_[3] = y;
    float* p = &px_[(size_t(y) * size_t(w_) + size_t(x)) * 4];
    if (p[3] <= 0.f || a >= 1.f) { p[0] = r; p[1] = g; p[2] = b; p[3] = a; return; }   // empty pixel / opaque: the same result without the divisions
    const float da = p[3] * (1.f - a), oa = a + da;
    if (oa <= 0.f) return;
    const float inv = 1.f / oa;
    p[0] = (r * a + p[0] * da) * inv; p[1] = (g * a + p[1] * da) * inv; p[2] = (b * a + p[2] * da) * inv; p[3] = oa;
}

void MenuLayer::blend(int x, int y, float r, float g, float b, float a) { if (!dry_) put(x, y, r, g, b, a); }

void MenuLayer::line(float x0, float x1, float y, float thick, LayerColor c) {
    band(x0, y - thick / 2, x1, y + thick / 2, [&](float, float) { return c; });
}

void MenuLayer::glow(float cx, float cy, float rad, LayerColor c) {
    if (cache_on_) { note(2); notef(cx); notef(cy); notef(rad); notef(c.r); notef(c.g); notef(c.b); notef(c.a); note_state(); if (!gate()) return; }
    const int x0 = std::max(0, int(PX(cx - rad))), x1 = std::min(w_, int(PX(cx + rad)) + 1);
    const int y0 = std::max(0, int(PY(cy - rad))), y1 = std::min(h_, int(PY(cy + rad)) + 1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = IX(x + 0.5f) - cx, dy = IY(y + 0.5f) - cy, d2 = (dx * dx + dy * dy) / (rad * rad);
            if (d2 < 1.f) { const float k = 1.f - std::sqrt(d2); put(x, y, c.r, c.g, c.b, c.a * k * k); }
        }
}

void MenuLayer::image(const uint32_t* px, int iw, int ih, int sx, int sy, int sw, int sh, float x, float y, float w, float h, float alpha) {
    if (!px || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    if (cache_on_) {   // (the source's address, its rectangle, five of its texels: a picture redrawn in place is seen)
        note(3); note(uint64_t(reinterpret_cast<uintptr_t>(px))); note(uint64_t(iw) << 32 | uint32_t(ih)); note(uint64_t(sx) << 48 | uint64_t(sy) << 32 | uint64_t(sw) << 16 | uint64_t(sh));
        notef(x); notef(y); notef(w); notef(h); notef(alpha); note_state();
        for (int k = 0; k < 5; k++) { const int tx = std::min(std::max(sx + (k & 1 ? sw - 1 : 0) + (k == 4 ? -sw / 2 : 0), 0), iw - 1), ty = std::min(std::max(sy + (k & 2 ? sh - 1 : 0) + (k == 4 ? -sh / 2 : 0), 0), ih - 1); note(px[size_t(ty) * size_t(iw) + size_t(tx)]); }
        if (!gate()) return;
    }
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
            put(pxx, py, o[0] / o[3], o[1] / o[3], o[2] / o[3], o[3] * alpha);
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
    if (cache_on_ && !measure_only) { note(4); note(bold); notef(em); notef(x); notef(baseline); notef(c.r); notef(c.g); notef(c.b); notef(c.a); note_state(); uint64_t sh = 1469598103934665603ull; for (unsigned char ch : str) sh = (sh ^ ch) * 1099511628211ull; note(sh); note(str.size()); }
    const bool draw = !measure_only && (!cache_on_ || gate());
    const float pen0 = PX(x); float pen = pen0;
    const int by = int(std::lround(PY(baseline)));
    // A string that is only measured (or skipped): its width is cached on the exact inputs, so the result is bit for bit what the loop gives (a layout that
    // feeds on it, and the frame hash, must not change between a drawn and a skipped call).
    static std::unordered_map<uint64_t, float> wcache; uint64_t wkey = 0;
    if (!draw && !g_old) {
        uint32_t pb; std::memcpy(&pb, &pen0, 4);
        wkey = 1469598103934665603ull; for (unsigned char ch : str) wkey = (wkey ^ ch) * 1099511628211ull;
        wkey = (wkey ^ (uint64_t(px) << 33 | uint64_t(bold) << 32 | pb)) * 1099511628211ull; wkey ^= uint64_t(reinterpret_cast<uintptr_t>(face)) * 0x9E3779B97F4A7C15ull;
        if (auto it = wcache.find(wkey); it != wcache.end()) return it->second;
    }
    FT_Set_Pixel_Sizes(face, 0, FT_UInt(px));   // (kerning below reads the current size)
    FT_UInt prev = 0;
    for (unsigned char ch : str) {
        const FT_UInt gi = FT_Get_Char_Index(face, ch);
        if (prev && gi && FT_HAS_KERNING(face)) { FT_Vector k; FT_Get_Kerning(face, prev, gi, FT_KERNING_DEFAULT, &k); pen += k.x / 64.f; }
        const CGlyph& g = cached_glyph(face, px, gi);
        if (g.ok) {
            if (draw) {
                const int gx = int(std::lround(pen)) + g.left, gy = by - g.top;
                for (int r = 0; r < g.rows; r++)
                    for (int q = 0; q < g.width; q++) {
                        const uint8_t cov = g.buf[size_t(r) * size_t(g.pitch) + size_t(q)];
                        if (cov) put(gx + q, gy + r, c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g.advance;
        }
        prev = gi;
    }
    const float width = (pen - pen0) / S();
    if (!draw && !g_old) { if (wcache.size() > 16384) wcache.clear(); wcache[wkey] = width; }
    return width;
}

float MenuLayer::serif_caps(const std::string& str, float em, float small, float track, float x, float baseline, LayerColor c, bool measure_only) {
    FT_Face face = faces().serif;
    if (!face) return 0.f;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const float pen0 = PX(x); float pen = pen0;
    const int by = int(std::lround(PY(baseline)));
    if (cache_on_ && !measure_only) { note(5); notef(em); notef(small); notef(track); notef(x); notef(baseline); notef(c.r); notef(c.g); notef(c.b); notef(c.a); note_state(); uint64_t sh = 1469598103934665603ull; for (unsigned char ch : str) sh = (sh ^ ch) * 1099511628211ull; note(sh); }
    const bool draw = !measure_only && (!cache_on_ || gate());
    for (unsigned char ch : str) {
        const bool lower = ch >= 'a' && ch <= 'z';
        const unsigned px = unsigned(std::lround(em * (lower ? small : 1.f) * S()));
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(px));
        const FT_UInt gi = FT_Get_Char_Index(face, lower ? ch - 32 : ch);
        const CGlyph& g = cached_glyph(face, px, gi);
        if (g.ok) {
            if (draw) {
                const int gx = int(std::lround(pen)) + g.left, gy = by - g.top;
                for (int r = 0; r < g.rows; r++)
                    for (int q = 0; q < g.width; q++) {
                        const uint8_t cov = g.buf[size_t(r) * size_t(g.pitch) + size_t(q)];
                        if (cov) put(gx + q, gy + r, c.r, c.g, c.b, c.a * cov / 255.f);
                    }
            }
            pen += g.advance + track * S();
        }
    }
    return (pen - pen0) / S() - track;
}

// px_ -> out_ (RGBA8) for a rectangle (inclusive pixels). A pixel nothing touched stays 0.
void MenuLayer::convert(int x0, int y0, int x1, int y1) const {
    if (out_.size() != size_t(w_) * size_t(h_)) return;
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w_ - 1, x1); y1 = std::min(h_ - 1, y1);
    for (int y = y0; y <= y1; y++) {
        const float* p = &px_[(size_t(y) * size_t(w_) + size_t(x0)) * 4];
        uint32_t* o = &out_[size_t(y) * size_t(w_) + size_t(x0)];
        for (int x = x0; x <= x1; x++, p += 4, o++) {
            if (p[3] <= 0.f) { *o = 0; continue; }
            const uint32_t r = uint32_t(std::min(255.f, std::max(0.f, p[0] + 0.5f))), g = uint32_t(std::min(255.f, std::max(0.f, p[1] + 0.5f))), b = uint32_t(std::min(255.f, std::max(0.f, p[2] + 0.5f)));
            *o = r | g << 8 | b << 16 | uint32_t(std::min(255.f, p[3] * 255.f + 0.5f)) << 24;
        }
    }
}

std::shared_ptr<const std::vector<uint32_t>> MenuLayer::bake() const {
    if (g_old) {   // the old full conversion
        auto out = std::make_shared<std::vector<uint32_t>>(size_t(w_) * size_t(h_));
        for (size_t i = 0; i < out->size(); i++) {
            const float* p = &px_[i * 4];
            auto ch = [](float v) { return uint32_t(std::min(255.f, std::max(0.f, v + 0.5f))); };
            (*out)[i] = ch(p[0]) | ch(p[1]) << 8 | ch(p[2]) << 16 | uint32_t(std::min(255.f, p[3] * 255.f + 0.5f)) << 24;
        }
        return out;
    }
    // only the rows / columns that were written now or into the last image
    out_.resize(size_t(w_) * size_t(h_), 0u);
    const int ux0 = std::min(px_bb_[0], out_bb_[0] <= out_bb_[2] ? out_bb_[0] : px_bb_[0]), uy0 = std::min(px_bb_[1], out_bb_[1] <= out_bb_[3] ? out_bb_[1] : px_bb_[1]);
    const int ux1 = std::max(px_bb_[2], out_bb_[2]), uy1 = std::max(px_bb_[3], out_bb_[3]);
    if (ux1 >= ux0 && uy1 >= uy0) convert(ux0, uy0, ux1, uy1);
    auto* self = const_cast<MenuLayer*>(this);
    self->out_bb_[0] = px_bb_[0]; self->out_bb_[1] = px_bb_[1]; self->out_bb_[2] = px_bb_[2]; self->out_bb_[3] = px_bb_[3];
    self->out_ok_ = true;
    return std::make_shared<std::vector<uint32_t>>(out_);
}

void MenuLayer::publish_baked(const std::vector<uint32_t>& img, int w, int h) {
    ge_draw_set_game_layer_copies(nullptr, 0);
    ge_draw_set_game_layer(img.data(), w, h);
}

void MenuLayer::publish() {
    if (!cache_on_) {   // not cached (raw / opted out): rasterised and handed over
        if (g_old) { const auto out = bake(); ge_draw_set_game_layer_copies(copies_.data(), int(copies_.size())); ge_draw_set_game_layer(out->data(), w_, h_); }
        else {
            bake();   // (into out_, no copy)
            ge_draw_set_game_layer_copies(copies_.data(), int(copies_.size()));
            ge_draw_set_game_layer(out_.data(), w_, h_);
        }
        if (g_log) stat_note(std::string(name) + ":" + (tag_ ? tag_ : "-"), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin_).count(), 0);
        return;
    }
    using clk = std::chrono::steady_clock;
    const auto t_pub_ = clk::now();
    const uint64_t key = hh_;
    const bool dry = dry_;
    bool rastered = false, same = false, from_cache = false;
    auto republish = [&](const Cached& c) {
        ge_draw_set_game_layer_copies(c.copies.data(), int(c.copies.size()));
        ge_draw_set_game_layer(c.img->data(), c.w, c.h);
        pub_serial_ = ge_draw_game_layer_serial(); pub_key_ = key; out_ok_ = false; pub_img_ = c.img;
    };
    if (dry) {   // nothing was rasterised: the same state as is shown, a state seen before, or a new one (rasterised on the next frame)
        if (key == pub_key_ && ge_draw_game_layer_serial() == pub_serial_ && ge_draw_game_layer_touch()) same = true;
        else if (auto it = cache_.find(key); it != cache_.end()) { it->second.used = ++tick_; republish(it->second); from_cache = true; }
        else { ge_draw_game_layer_touch(); hot_ = true; wet_key_ = key; }
    } else if (aborted_ || (inc_ && cum_.size() < snap_n_)) {   // the part taken to be unchanged was not: this frame is lost, the next one is drawn whole
        snap_ok_ = false; px_ok_ = false; streak_ = 0; hot_ = true; wet_key_ = 0; ge_draw_game_layer_touch();
        drops_ = std::min(drops_ + 1, 6); snap_cool_ = 30 << drops_;   // (a screen whose start keeps changing: no snapshot for a while, longer each time)
        if (g_log) std::fprintf(stderr, "[MENUFPS] %s: incremental frame dropped (the unchanged start changed)\n", tag_ ? tag_ : name);
    } else {
        rastered = true;
        {   // px_ -> out_: where px_ or the last image differ from the state they are measured against (zero, or the snapshot)
            out_.resize(size_t(w_) * size_t(h_), 0u);
            const bool e1 = px_bb_[2] >= px_bb_[0] && px_bb_[3] >= px_bb_[1], e2 = out_bb_[2] >= out_bb_[0] && out_bb_[3] >= out_bb_[1];
            last_area_ = e1 ? double(px_bb_[2] - px_bb_[0] + 1) * double(px_bb_[3] - px_bb_[1] + 1) / (double(w_) * double(h_)) : 0.0;
            if (e1 || e2) convert(e1 && e2 ? std::min(px_bb_[0], out_bb_[0]) : e1 ? px_bb_[0] : out_bb_[0], e1 && e2 ? std::min(px_bb_[1], out_bb_[1]) : e1 ? px_bb_[1] : out_bb_[1],
                                  std::max(px_bb_[2], out_bb_[2]), std::max(px_bb_[3], out_bb_[3]));
            if (snap_ok_) {   // (restoring the rows costs far less than rasterising the start again, whatever the size of the changing part)
                std::memcpy(out_bb_, sb_, sizeof out_bb_); restore_rows(sb_); px_bb_[0] = sb_[0] = w_; px_bb_[1] = sb_[1] = h_; px_bb_[2] = px_bb_[3] = sb_[2] = sb_[3] = -1;
            } else std::memcpy(out_bb_, px_bb_, sizeof out_bb_);
        }
        ge_draw_set_game_layer_copies(copies_.data(), int(copies_.size()));
        ge_draw_set_game_layer(out_.data(), w_, h_);
        pub_serial_ = ge_draw_game_layer_serial(); out_ok_ = true;
        if (g_verify && key == wet_key_ && key == pub_key_ && pub_img_ && *pub_img_ != out_) std::fprintf(stderr, "[MENUFPS] VERIFY MISMATCH %s: the hash did not change but the pixels did\n", tag_ ? tag_ : name);
        const bool stable = key == wet_key_;   // the same state twice in a row: settled, so it is kept and the next frames only hash
        streak_ = stable ? 0 : streak_ + 1;
        wet_key_ = key; pub_key_ = key; last_cum_ = cum_;
        if (stable) {
            hot_ = false;
            Cached c; c.img = std::make_shared<std::vector<uint32_t>>(out_); c.w = w_; c.h = h_; c.copies = copies_; c.used = ++tick_;
            pub_img_ = c.img;
            cache_[key] = std::move(c);
            size_t bytes = 0; for (auto& kv : cache_) bytes += kv.second.img->size() * 4;
            while (cache_.size() > 24 || bytes > (size_t(64) << 20)) {   // least recently used goes
                auto old = cache_.end(); for (auto it = cache_.begin(); it != cache_.end(); ++it) if (it->first != key && (old == cache_.end() || it->second.used < old->second.used)) old = it;
                if (old == cache_.end()) break;
                bytes -= old->second.img->size() * 4; cache_.erase(old);
            }
        } else { hot_ = true; if (g_verify) pub_img_ = std::make_shared<std::vector<uint32_t>>(out_); }
    }
    inframe_ = false;
    if (g_log) stat_note(std::string(name) + ":" + (tag_ ? tag_ : "-"), std::chrono::duration<double, std::milli>(clk::now() - t_begin_).count(), rastered ? 0 : from_cache ? 2 : 1, int(cum_.size()), inc_ ? int(snap_n_) : 0, double(last_area_), std::chrono::duration<double, std::milli>(clk::now() - t_pub_).count());
}

void MenuLayer::clear() { pub_key_ = 0; inframe_ = false; ge_draw_clear_game_layer(); }

}  // namespace dissidia
