// games/dissidia/runtime/tagteam_result.cpp - the Free-for-all result screen (docs/tag-team.md "Batch forty-three").
// Retail's Tournament round result is built for two parties (Player Party 3 - 0 Enemy Party), so a Free-for-all was bent into it. This screen is our own, made from the
// retail result scene's own parts: its brown swirl ground, its ornate "1st / 2nd / 3rd / 4th", the medals, the glowing digits, the "Lv" numerals and the
// VICTORY! / DEFEATED... / DRAW banners (menu/menu_pk_party_battle_result_gim_0.bin entries 0 and 2, menu/en/menu_lpk_party_battle_result_gim_0.bin entry 1), the
// faces of the menus, bands in the party colours. Its cues are retail's: the row triplet (menu SE 28, 25, 29) per fighter, the final jingle (request 400005)
// and the Cross SE; the retail scene's own requests are muted while it is up (its background music plays on).
// PSPRECOMP_TAGTEAM_FFA_RESULT_OLD=1: the old bent retail result (tagteam.cpp ffa_result_tick).
#include "tagteam_result.h"
#include "gim_decode.h"
#include "menu_layer.h"
#include "package_mods.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_io.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include "recomp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

std::string dissidia_mods_dir();   // hooks_io.cpp
std::string dissidia_tagteam_char_name(int id);
std::string dissidia_tagteam_char_code(int id);
const uint32_t* dissidia_tagteam_face_px(const std::string& code, int* w, int* h);
int dissidia_tagteam_result_phase();       // tagteam.cpp: 0 none, 1 the retail result scene is up (menu overlay), 2 the retail end-of-battle list is up (battle overlay)
void dissidia_tagteam_result_consumed();   // tagteam.cpp: shown, do not show it again
bool dissidia_tagteam_post_flow();         // tagteam_menu.cpp: the way back to the players screen has begun

namespace {
using dissidia::LayerColor;
using dissidia::GimImage;
dissidia::MenuLayer g_L("ffa_result", true);
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the retail result scene's picture parts ----
struct Img { bool tried = false; GimImage g; };
Img g_p0, g_p2, g_l1;
const GimImage* load_img(Img& im, const char* pack, int entry) {
    if (!im.tried) {
        im.tried = true;
        std::vector<uint8_t> b;
        if (dissidia::read_asset(psp_path_to_host("disc0:/PSP_GAME/USRDIR/DATA"), dissidia_mods_dir(), pack, &b) && !b.empty()) {
            const uint8_t* p = nullptr; size_t len = 0; std::string err;
            if (!dissidia::menu_pak_entry(b.data(), b.size(), entry, &p, &len) || !len || !dissidia::gim_decode(p, len, 0, &im.g, &err)) std::fprintf(stderr, "[FFARES] %s entry %d: not decoded (%s)\n", pack, entry, err.c_str());
        } else std::fprintf(stderr, "[FFARES] %s: not read\n", pack);
    }
    return im.g.w > 0 ? &im.g : nullptr;
}
const GimImage* p0() { return load_img(g_p0, "menu/menu_pk_party_battle_result_gim_0.bin", 0); }   // WIN, digits, 1st..5th, Lv, medals, flares (256 x 256)
const GimImage* p2() { return load_img(g_p2, "menu/menu_pk_party_battle_result_gim_0.bin", 2); }   // the brown swirl ground (512 x 512)
const GimImage* l1() { return load_img(g_l1, "menu/en/menu_lpk_party_battle_result_gim_0.bin", 1); }   // VICTORY! / DEFEATED... / DRAW (256 x 256)
// a part of an image drawn into a box: source x0..x1, y0..y1 inclusive (pixels), destination box
void spr(const GimImage* g, int x0, int y0, int x1, int y1, float dx, float dy, float dw, float dh, float a) {
    if (!g || a <= 0.f) return;
    g_L.image(g->rgba.data(), g->w, g->h, x0, y0, x1 - x0 + 1, y1 - y0 + 1, dx, dy, dw, dh, a);
}
struct Rect { int x0, x1; };
const Rect kBig[10] = {{7, 24}, {36, 45}, {57, 73}, {82, 98}, {106, 122}, {131, 148}, {155, 172}, {181, 196}, {205, 222}, {229, 247}};   // glowing digits, rows 48..77
const Rect kSmall[10] = {{8, 21}, {29, 36}, {45, 56}, {63, 75}, {81, 92}, {99, 111}, {116, 129}, {136, 146}, {153, 165}, {170, 184}};     // smaller ones, rows 81..105
const Rect kLv[10] = {{119, 129}, {145, 151}, {168, 177}, {192, 201}, {216, 224}, {120, 128}, {143, 153}, {168, 176}, {191, 201}, {215, 225}};   // white "Lv" numerals (0..4 row 175..189, 5..9 row 198..213)
// the digits of a number, left x, top y, digit height h (the digit's source height is kept in proportion); returns the width
float digits(int v, int kind, float x, float y, float h, float a, bool measure = false) {
    const GimImage* g = p0(); if (!g) return 0.f;
    const std::string s = std::to_string(std::max(0, v)); float cx = x;
    for (char c : s) {
        const int d = c - '0'; int sy0, sy1; Rect r;
        if (kind == 0) { r = kBig[d]; sy0 = 48; sy1 = 77; }
        else if (kind == 1) { r = kSmall[d]; sy0 = 81; sy1 = 105; }
        else { r = kLv[d]; if (d < 5) { sy0 = 174; sy1 = 190; } else { sy0 = 197; sy1 = 214; } }
        const int pad = kind == 2 ? 1 : 3;   // (the glow around a digit)
        const int sx0 = r.x0 - pad, sx1 = r.x1 + pad;
        const float k = h / static_cast<float>(sy1 - sy0 + 1), w = static_cast<float>(sx1 - sx0 + 1) * k;
        if (!measure) spr(g, sx0, sy0, sx1, sy1, cx, y, w, h, a);
        cx += w - (kind == 2 ? 2.f * k : 3.f * k);
    }
    return cx - x;
}
float tw(const std::string& s, float em, bool bold = true) { return g_L.text(s, bold, em, 0, 0, LayerColor{0, 0, 0, 0}, true); }
// lettering with the dark edge the retail result's names carry, shrunk to fit maxw
float label(const std::string& s, float em, float x, float base, LayerColor c, float maxw = 1e9f, bool right = false) {
    float e = em; const float w0 = tw(s, e); if (w0 > maxw) e = em * maxw / w0;
    const float w = tw(s, e), x0 = right ? x - w : x;
    for (const auto& o : {std::pair<float, float>{0.6f, 0.7f}, {-0.5f, 0.6f}, {0.6f, -0.3f}}) g_L.text(s, true, e, x0 + o.first, base + o.second, LayerColor{12, 10, 40, 0.7f * c.a});
    g_L.text(s, true, e, x0, base, c);
    return w;
}
float ease_out(float t) { t = std::clamp(t, 0.f, 1.f); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
LayerColor slot_col(int slot) { static const LayerColor c[4] = {{66, 110, 235, 1.f}, {222, 62, 74, 1.f}, {44, 168, 96, 1.f}, {232, 168, 36, 1.f}}; return c[std::clamp(slot, 0, 3)]; }
const char* look_name(int lk) { switch (lk) { case 1: return "Normal"; case 2: return "Alt 1"; case 3: return "Alt 2"; case 4: return "DLC 1"; case 5: return "DLC 2"; default: return lk > 0 ? "DLC" : ""; } }

// ---- the timeline: rows come in from the last place to the first (each: its band, then its counts run up), then the banner ----
constexpr float kRow0 = 450.f, kRowStep = 1000.f, kSlide = 280.f, kCount = 620.f;
float row_t(int n, int i) { return kRow0 + static_cast<float>(n - 1 - i) * kRowStep; }   // the moment row i (0 = 1st) starts
float banner_t(int n) { return kRow0 + static_cast<float>(n) * kRowStep + 250.f; }
float ready_t(int n) { return banner_t(n) + 950.f; }

void face_crop(int ch, float x, float y, float w, float h, float a, bool strip) {   // the menus' face: the strip (wide) or its square face part
    int iw = 0, ih = 0; const uint32_t* px = ch > 0 ? dissidia_tagteam_face_px(dissidia_tagteam_char_code(ch), &iw, &ih) : nullptr;
    if (!px) { g_L.band(x, y, x + w, y + h, [&](float, float) { return LayerColor{30, 34, 70, 0.6f * a}; }); return; }
    const int sh = std::min(ih, 26), sw = strip ? std::min(iw, std::max(8, static_cast<int>(static_cast<float>(sh) * w / h + 0.5f))) : std::min(iw, 60);   // (small boxes: the left 60 px of the strip, where every character's face is; callers give them that 60:26 shape)
    g_L.image(px, iw, ih, 0, 0, sw, sh, x, y, w, h, a);
}

void draw_row(const FfaResRow& r, int n, float t, float a_all) {
    const int i = r.place - 1;
    const float t0 = row_t(n, i), k = (t - t0) / kSlide;
    if (k <= 0.f) return;
    const float e = ease_out(k), slide = (1.f - e) * (i % 2 ? 150.f : -150.f), a = std::min(1.f, k * 2.2f) * a_all;
    const float y = 44.f + 40.f * static_cast<float>(i), h = 37.f, x0 = 4.f + slide, x1 = 476.f + slide;
    const LayerColor sc = slot_col(r.slot);
    const bool first = r.place == 1;
    // the band: the retail result's party bands (navy for you, crimson for the others), fading out at its inner end, a pale line over and under
    g_L.band(x0, y, x1, y + h, [&](float x, float yy) {
        const float u = (x - x0) / (x1 - x0), v = (yy - y) / h;
        const bool me = r.you; const float fade = u < 0.86f ? 1.f : std::max(0.15f, 1.f - (u - 0.86f) / 0.14f * 0.85f);
        const float r0 = me ? 28.f + 46.f * (1.f - v) : 92.f + 40.f * (1.f - v), g0 = me ? 36.f + 36.f * (1.f - v) : 26.f + 22.f * (1.f - v), b0 = me ? 126.f + 62.f * (1.f - v) : 76.f + 34.f * (1.f - v);
        if (v < 0.07f || v > 0.93f) return LayerColor{first ? 255.f : 214.f, first ? 224.f : 218.f, first ? 120.f : 246.f, 0.95f * a * fade};
        return LayerColor{r0, g0, b0, 0.93f * a * fade};
    });
    // the slot's colour tab with its number, left of the band
    g_L.band(x0, y + 4.f, x0 + 11.f, y + h - 4.f, [&](float, float) { return LayerColor{sc.r, sc.g, sc.b, 0.95f * a}; });
    { const std::string pn = "P" + std::to_string(r.slot + 1); g_L.text(pn, true, 5.6f, x0 + 1.4f, y + h * 0.5f + 2.f, LayerColor{255, 255, 255, a}); }
    // the placing: the retail ornate numerals, the medal
    static const int rk[4][2] = {{65, 89}, {97, 127}, {136, 163}, {172, 200}};
    const int pl = std::clamp(r.place, 1, 4) - 1;
    spr(p0(), rk[pl][0] - 3, 141, rk[pl][1] + 3, 163, x0 + 14.f, y + 8.f, static_cast<float>(rk[pl][1] - rk[pl][0] + 7) * 1.0f, 23.f, a);
    if (r.place <= 3) { static const int md[3][2] = {{5, 20}, {23, 38}, {42, 57}}; spr(p0(), md[pl][0], 131, md[pl][1], 152, x0 + 14.f + 31.f, y + 7.f, 12.f, 17.f, a); }
    // the portrait (and the tag-in who came in)
    const float fx = x0 + 62.f;
    face_crop(r.ch, fx, y + 4.f, r.tag ? 62.f : 84.f, 22.f, a, true);
    if (r.tag) { face_crop(r.tag, fx + 64.f, y + 4.f, 48.f, 22.f, a, false); g_L.band(fx + 63.f, y + 4.f, fx + 64.f, y + 26.f, [&](float, float) { return LayerColor{230, 230, 255, 0.8f * a}; }); }
    g_L.band(fx, y + 3.f, fx + (r.tag ? 86.f : 84.f), y + 4.f, [&](float, float) { return LayerColor{214, 218, 246, 0.8f * a}; });
    // name, level, who, outfit
    const float nx = fx + (r.tag ? 118.f : 94.f);
    std::string nm = dissidia_tagteam_char_name(r.ch); if (r.tag) nm += " > " + dissidia_tagteam_char_name(r.tag);
    label(nm, 9.4f, nx, y + 14.f, LayerColor{255, 255, 255, a}, r.tag ? 130.f : 150.f);
    float lx = nx;
    spr(p0(), 85, 174, 105, 190, lx, y + 20.f, 11.f, 9.f, a); lx += 12.f;
    if (r.level > 0) lx += digits(r.level, 2, lx, y + 20.f, 9.f, a) + 5.f;
    const std::string who = r.you ? "You" : r.who.empty() ? (r.cpu ? "CPU" : "Player") : r.who;
    lx += label(who, 6.0f, lx, y + 28.f, r.you ? LayerColor{255, 226, 120, a} : LayerColor{214, 220, 250, a}, 60.f) + 6.f;
    if (r.look > 0) label(look_name(r.look), 5.6f, lx, y + 28.f, LayerColor{170, 214, 255, a}, 40.f);
    // KOs (count up), damage, who knocked whom out
    const float cu = std::clamp((t - t0 - kSlide) / kCount, 0.f, 1.f), ce = ease_out(cu);
    const float kx = x0 + 318.f;
    label("KOs", 5.0f, kx + 12.f, y + 10.f, LayerColor{200, 206, 255, a}, 30.f);
    { const int kv = static_cast<int>(std::lround(static_cast<float>(r.kos) * ce)); const float w = digits(kv, 0, 0, 0, 15.f, 0.f, true); digits(kv, 0, kx + 14.f - w / 2.f, y + 13.f, 15.f, a); }
    const float dxr = x0 + 396.f;
    label("Damage", 5.0f, dxr - 20.f, y + 10.f, LayerColor{200, 206, 255, a}, 40.f);
    { const int dv = static_cast<int>(std::lround(static_cast<float>(r.dmg) * ce)); const float w = digits(dv, 1, 0, 0, 11.f, 0.f, true); digits(dv, 1, dxr - w, y + 14.f, 11.f, a); }
    const float fx2 = x0 + 410.f;   // the victims' faces, "by" the one who got him
    int shown = 0;
    for (size_t q = 0; q < r.victims.size() && shown < 3; q++, shown++) face_crop(r.victims[q], fx2 - 6.f + 25.f * static_cast<float>(shown), y + 5.f, 23.f, 10.f, a * (cu >= 1.f || q * 3 < static_cast<size_t>(cu * 9) ? 1.f : 0.f), false);
    if (r.alive) label(first ? "WINNER" : "In the fight", 5.2f, x0 + 470.f - 4.f, y + 31.f, first ? LayerColor{255, 226, 120, a} : LayerColor{214, 220, 250, a}, 56.f, true);
    else if (r.forfeit) label("Forfeited", 5.6f, x0 + 470.f - 4.f, y + 31.f, LayerColor{255, 190, 150, a}, 56.f, true);
    else if (r.by > 0) { label("KO'd by", 4.8f, x0 + 444.f, y + 29.f, LayerColor{200, 206, 255, a}, 36.f, true); face_crop(r.by, x0 + 447.f, y + 22.f, 25.f, 11.f, a, false); }
    else label("Knocked out", 5.2f, x0 + 466.f, y + 31.f, LayerColor{214, 220, 250, a}, 56.f, true);
}

void draw_screen(const FfaResult& R, float t, float white) {
    g_L.begin(std::max(2, psp_sddialog::output_scale()));
    const int n = R.n;
    // the ground: an opaque dark base (the swirl is partly transparent: the retail scene under it must not show), the retail result's swirl, a dark vignette
    g_L.band(0.f, 0.f, 480.f, 272.f, [&](float, float) { return LayerColor{14.f, 10.f, 9.f, 1.f}; });
    if (const GimImage* g = p2()) spr(g, 0, 110, 511, 110 + 290, 0.f, 0.f, 480.f, 272.f, 1.f);
    else g_L.band(0.f, 0.f, 480.f, 272.f, [&](float x, float y) { return LayerColor{30.f + 14.f * (x / 480.f), 20.f + 8.f * (y / 272.f), 16.f, 1.f}; });
    g_L.band(0.f, 0.f, 480.f, 272.f, [&](float x, float y) { const float dx = (x - 240.f) / 240.f, dy = (y - 136.f) / 136.f; return LayerColor{0.f, 0.f, 0.f, std::clamp((dx * dx + dy * dy - 0.35f) * 0.45f, 0.f, 0.55f)}; });
    // the heading: the pale "Tournament" label of the retail result, ours says what this is; below it the band across the screen (retail: Player Party | score | Enemy Party)
    { const std::string h = "Free-for-all"; const float w = tw(h, 7.4f); label(h, 7.4f, 240.f - w / 2.f, 13.f, LayerColor{236, 240, 255, 1.f}); }
    const float ba = std::min(1.f, t / 300.f);
    g_L.band(0.f, 20.f, 480.f, 36.f, [&](float x, float y) {
        const float u = x / 480.f, v = (y - 20.f) / 16.f, edge = std::min(u, 1.f - u) < 0.05f ? std::min(u, 1.f - u) / 0.05f : 1.f;
        const bool l = u < 0.5f; const float c = 1.f - 0.45f * v;
        return LayerColor{(l ? 20.f : 120.f) * c + 20.f, (l ? 28.f : 24.f) * c + 10.f, (l ? 120.f : 76.f) * c + 20.f, 0.92f * ba * edge};
    });
    { std::string head;
      if (n > 0) { const FfaResRow& w = R.rows[0]; head = dissidia_tagteam_char_name(w.ch) + " (P" + std::to_string(w.slot + 1) + ")" + (w.you ? "  (you)" : "") + "  wins"; }
      const float w = tw(head, 8.2f); label(head, 8.2f, 240.f - w / 2.f, 32.f, LayerColor{255, 255, 255, ba}); }
    for (int i = n - 1; i >= 0; i--) draw_row(R.rows[i], n, t, 1.f);
    // the banner: VICTORY! / DEFEATED... / DRAW, popping in (retail: it glows in over the rows)
    const float tb = banner_t(n);
    if (t >= tb) {
        const float k = std::clamp((t - tb) / 380.f, 0.f, 1.f), e = ease_out(k), sc = 0.7f + 0.3f * e + 0.06f * std::sin(std::min(1.f, k) * 3.14159f);
        const bool win = R.you_won; const GimImage* g = l1();
        const int bx0 = 5, bx1 = win ? 184 : 216, by0 = win ? 3 : 60, by1 = win ? 42 : 98;
        const float bw = static_cast<float>(bx1 - bx0 + 1) * 0.95f * sc, bh = static_cast<float>(by1 - by0 + 1) * 0.95f * sc, cx = 240.f, cy = 226.f;
        // the light streak behind it
        spr(p0(), 100, 236, 252, 254, 240.f - 150.f * e, cy - 5.f, 300.f * e, 12.f, 0.85f * e);
        if (g) spr(g, bx0, by0, bx1, by1, cx - bw / 2.f, cy - bh / 2.f, bw, bh, e);
        else label(win ? "VICTORY!" : "DEFEATED...", 24.f, cx - tw(win ? "VICTORY!" : "DEFEATED...", 24.f) / 2.f, cy + 8.f, LayerColor{255, 255, 255, e});
        const float fl = std::clamp(1.f - (t - tb) / 700.f, 0.f, 1.f);   // a flare as it lands
        if (fl > 0.f) spr(p0(), 4, 213, 50, 252, cx - 30.f, cy - 26.f, 60.f, 52.f, fl);
    }
    // the prompt, when it is over
    if (t >= ready_t(n)) {
        const float pa = 0.65f + 0.35f * std::sin(t / 260.f);
        const std::string k = "Cross:", a = " Continue"; const float w = tw(k + a, 6.6f);
        g_L.band(240.f - w / 2.f - 12.f, 255.f, 240.f + w / 2.f + 12.f, 269.f, [&](float x, float y) { const float rr = 7.f, cx2 = std::clamp(x, 240.f - w / 2.f - 5.f, 240.f + w / 2.f + 5.f), d = std::sqrt((x - cx2) * (x - cx2) + (y - 262.f) * (y - 262.f)); return LayerColor{22.f, 22.f, 70.f, 0.8f * std::clamp((rr - d) * 2.f, 0.f, 1.f)}; });
        const float kw = g_L.text(k, true, 6.6f, 240.f - w / 2.f, 264.6f, LayerColor{110, 232, 240, pa});
        g_L.text(a, true, 6.6f, 240.f - w / 2.f + kw, 264.6f, LayerColor{255, 255, 255, pa});
    }
    if (white > 0.f) g_L.band(0.f, 0.f, 480.f, 272.f, [&](float, float) { return LayerColor{255, 255, 255, std::min(1.f, white)}; });
    ge_draw_set_game_layer_ttl(150);
    g_L.publish();
}

// ---- the state machine ----
struct State {
    int stage = 0;               // 0 off, 1 the screen (pad swallowed), 2 leaving (the player's Cross is handed to the retail screen under it)
    int phase = 0;
    int64_t t0 = 0, texit = 0;
    float skip_to = 0.f;         // the timeline jumped to this (after a Cross during it)
    FfaResult R;
    std::vector<std::pair<float, uint32_t>> cues;   // (time, request id): played when the timeline passes them
    size_t next_cue = 0;
    std::vector<uint32_t> pending;                  // requests for the next frame hook (it has a CPU context)
    uint32_t prev = 0; int pulse = 0;
} g;
FuncPtr g_orig_req = nullptr;
float timeline() { return static_cast<float>(now_ms() - g.t0); }
void make_cues(const FfaResult& R) {
    g.cues.clear(); g.next_cue = 0; const int n = R.n;
    for (int i = n - 1; i >= 0; i--) { const float t = row_t(n, i); g.cues.push_back({t, 10028u}); g.cues.push_back({t + 467.f, 10025u}); g.cues.push_back({t + 933.f, 10029u}); }
    if (R.you_won) g.cues.push_back({banner_t(n), 400005u});   // (retail's jingle of a win; a defeat has the defeat music only)
}
}  // namespace

bool dissidia_tagteam_result_active() { return g.stage != 0; }
bool dissidia_tagteam_result_mute_scene(uint32_t ra) { return g.stage != 0 && ra >= 0x08B543D0u && ra < 0x08B57700u; }

void dissidia_tagteam_result_install(FuncPtr orig_request) { g_orig_req = orig_request; }

uint32_t dissidia_tagteam_result_pad(uint32_t raw) {
    constexpr uint32_t CROSS = 0x4000u;
    const uint32_t edge = raw & ~g.prev; g.prev = raw;
    if (g.stage == 0) {
        const int ph = dissidia_tagteam_result_phase();
        if (!ph || !dissidia_tagteam_ffa_result(&g.R) || g.R.n < 2) return raw;
        g.stage = 1; g.phase = ph; g.t0 = now_ms(); g.skip_to = 0.f; make_cues(g.R); g.pending.clear();
        std::fprintf(stderr, "[FFARES] the result screen is up (%s), %d fighters, you %s\n", ph == 1 ? "over the retail result scene" : "over the retail end-of-battle list", g.R.n, g.R.you_won ? "won" : "did not win");
        return 0u;
    }
    const int n = g.R.n;
    if (g.stage == 1) {
        float t = timeline();
        if (edge & CROSS) {
            if (t < ready_t(n) - 1.f) {   // the first Cross runs the counting out (its jingle still plays once)
                g.t0 -= static_cast<int64_t>(ready_t(n) - t);
                t = timeline(); while (g.next_cue < g.cues.size() && g.cues[g.next_cue].first <= t) { if (g.cues[g.next_cue].second == 400005u) g.pending.push_back(400005u); g.next_cue++; }
                std::fprintf(stderr, "[FFARES] Cross: the counting is shown at once\n");
            } else { g.stage = 2; g.texit = now_ms(); g.pulse = 0; g.pending.push_back(10001u); std::fprintf(stderr, "[FFARES] Cross: the result screen is left (the retail screen under it goes on)\n"); }
        }
        while (g.next_cue < g.cues.size() && g.cues[g.next_cue].first <= t) { g.pending.push_back(g.cues[g.next_cue].second); g.next_cue++; }
        draw_screen(g.R, t, 0.f);
        return 0u;
    }
    // stage 2: white fades over the last picture while the player's Cross reaches the retail screen under it; the way back to the players screen takes the layer over (white to white)
    const int64_t dt = now_ms() - g.texit;
    draw_screen(g.R, timeline(), std::min(1.f, static_cast<float>(dt) / 380.f));
    if (dissidia_tagteam_post_flow() || dt > 7000) { g.stage = 0; dissidia_tagteam_result_consumed(); std::fprintf(stderr, "[FFARES] result screen closed after %lld ms\n", static_cast<long long>(dt)); return raw; }
    g.pulse++; return (g.pulse % 10) < 3 ? (raw | CROSS) : (raw & ~CROSS);   // (the player's own press, kept going until the retail screen takes it)
}

void dissidia_tagteam_result_frame(uint8_t* rdram, void* vctx) {
    if (g.pending.empty() || !g_orig_req) return;
    recomp_context* ctx = static_cast<recomp_context*>(vctx);
    for (uint32_t id : g.pending) {
        const recomp_context save = *ctx;
        ctx->r[4] = 0; ctx->r[5] = static_cast<int32_t>(id); ctx->r[6] = 1;
        g_orig_req(rdram, ctx);
        *ctx = save;
        std::fprintf(stderr, "[FFARES] cue: sound %u\n", id);
    }
    g.pending.clear();
}
