// games/dissidia/runtime/tagteam_hud.cpp - the tag team battle's own HUD, drawn in the game's style on the game layer (menu_layer.h,
// ge_draw_set_game_layer) at the output resolution:
//   - a compact panel for each fighter who is not on the game's big panel: the character's face strip (the menus' "face in name" picture,
//     menu/jp/common/chara_face_in_name/p_<code>_face_in_name.gim, as on the Customization screen), the name in the game's bold face, a thin
//     HP bar in the retail HUD's blues and the HP / BRV numbers;
//   - the reserve of each side: the face strips of the members still to come, the next one in front;
//   - the "NAME JOINS THE BATTLE" banner.
// The data is handed in by tagteam.cpp (tagteam_hud_draw, once per tick) and drawn by tagteam_layer_frame (once per frame, with the
// effects of tagteam_fx.cpp on the same layer).
#include "tagteam_hud.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include "gim_decode.h"
#include "menu_layer.h"
#include "package_mods.h"
#include "psp_ge_draw.h"
#include "hle/psp_hle_io.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"

std::string dissidia_mods_dir();   // hooks_io.cpp
int dissidia_ttnet_pause_menu(int* sel);   // tagteam_net.cpp: the online pause menu (0 closed, 1 menu, 2 forfeit confirm, 3 forfeit sent)
void dissidia_menu_draw_hand(dissidia::MenuLayer& L, float x, float y, float a);   // menu_settings.cpp

namespace {

using dissidia::LayerColor;
dissidia::MenuLayer g_layer;
bool g_layer_shown = false;
std::vector<TagHudFighter> g_fighters;
std::vector<TagHudReserve> g_reserves;
std::vector<TagHudOverhead> g_overhead; int g_overhead_style = 0; bool g_hide_panels = false;
const uint8_t* g_hud_ram = nullptr;
float (*g_top_fn)(uint32_t) = nullptr;
float top_of(uint32_t a) { return g_top_fn ? g_top_fn(a) : 2.05f; }
double g_vig_t0 = -10.0, g_vig_dur = 0.4; float g_vig_str = 0.f; int g_vig_side = 0;   // the lock-on screen pulse (red-orange edges)
bool g_intro_running = false;
TagHudLockInd g_lockind; std::map<uint32_t, double> g_lock_new; double g_lock_me_new = -10.0;   // actor -> wall-clock time it newly locked on
std::string g_banner;
struct Notice { std::string text; int kind; double until, start; };   // wall-clock seconds
std::vector<Notice> g_notices;

// ---- HP damage indicator (PSPRECOMP_TAGTEAM_HPDMG: 0 off, 1 panel number + trail + feed (default), 2 + a floating number at the fighter;
// PSPRECOMP_TAGTEAM_HPFEED=0 hides the feed line). Presentation only (wall clock, nothing in the simulation reads it). ----
struct HitRec { TagHudHit h; double t0; };
std::vector<HitRec> g_hits;
uint32_t g_big[2] = {0, 0};
int hpdmg_mode() { static const int m = [] { const char* e = std::getenv("PSPRECOMP_TAGTEAM_HPDMG"); return e ? std::atoi(e) : 1; }(); return m; }
bool hpfeed_on() { static const bool b = [] { const char* e = std::getenv("PSPRECOMP_TAGTEAM_HPFEED"); return !(e && std::atoi(e) == 0); }(); return b; }
constexpr double kNumLife = 1.2, kFeedLife = 2.3, kTrailLife = 1.0;
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- the characters' face strips (128 x 32 GIM; the picture is the top 26 rows) ----
struct Face { bool tried = false; dissidia::GimImage img; };
std::map<std::string, Face> g_faces;
const dissidia::GimImage* face(const std::string& code) {
    if (code.empty()) return nullptr;
    Face& f = g_faces[code];
    if (!f.tried) {
        f.tried = true;
        std::vector<uint8_t> b;
        const std::string path = "menu/jp/common/chara_face_in_name/p_" + code + "_face_in_name.gim";
        if (dissidia::read_asset(psp_path_to_host("disc0:/PSP_GAME/USRDIR/DATA"), dissidia_mods_dir(), path, &b) && !b.empty()) {
            std::string err;
            if (!dissidia::gim_decode(b.data(), b.size(), 0, &f.img, &err)) std::fprintf(stderr, "[TAGHUD] %s: %s\n", path.c_str(), err.c_str());
        } else std::fprintf(stderr, "[TAGHUD] %s: not read\n", path.c_str());
    }
    return f.img.w > 0 ? &f.img : nullptr;
}
// the face strip with its right end fading out (the strip's own art fades to the menu's plate; over the battle it ends softly)
void draw_face(const std::string& code, float x, float y, float w, float h, float alpha, bool mirror) {
    const dissidia::GimImage* g = face(code);
    if (!g) { g_layer.band(x, y, x + w, y + h, [&](float, float) { return LayerColor{40, 44, 70, 0.7f * alpha}; }); return; }
    const int sh = std::min(g->h, 26);
    (void)mirror;   // (the strip is not mirrored: negative sizes draw nothing; on the right side it simply sits at the panel's right end)
    g_layer.image(g->rgba.data(), g->w, g->h, 0, 0, g->w, sh, x, y, w, h, alpha);
}

// ---- pieces in the retail HUD's colours ----
const LayerColor kPlate{14, 16, 40, 0.62f}, kEdgeL{120, 170, 255, 0.95f}, kEdgeR{255, 120, 110, 0.95f};
void plate(float x0, float y0, float x1, float y1, float a, bool right) {   // a dark plate fading towards the screen centre
    g_layer.band(x0, y0, x1, y1, [&](float x, float) {
        const float t = right ? (x1 - x) / (x1 - x0) : (x - x0) / (x1 - x0);
        return LayerColor{kPlate.r, kPlate.g, kPlate.b, kPlate.a * a * (1.f - 0.65f * t)};
    });
}
void hp_bar(float x, float y, float w, float frac, bool right, float a) {   // the retail HP bar: cyan to deep blue, a light top line
    frac = std::clamp(frac, 0.f, 1.f);
    g_layer.band(x, y, x + w, y + 3.2f, [&](float, float) { return LayerColor{20, 24, 44, 0.85f * a}; });
    const float fx0 = right ? x + w * (1.f - frac) : x, fx1 = right ? x + w : x + w * frac;
    const bool low = frac < 0.25f;
    g_layer.band(fx0, y, fx1, y + 3.2f, [&](float xx, float yy) {
        const float t = (xx - x) / std::max(1.f, w), top = yy < y + 1.f ? 1.f : 0.f;
        if (low) return LayerColor{255.f, 150.f + 40.f * top, 60.f + 60.f * top, a};
        return LayerColor{40.f + 60.f * t + 80.f * top, 170.f + 50.f * t + 40.f * top, 255.f, a};
    });
}
void shadow_text(const std::string& s, bool bold, float em, float x, float base, LayerColor c) {
    for (const auto& o : {std::pair<float, float>{0.5f, 0.5f}, {0.5f, 0.f}, {0.f, 0.5f}, {-0.4f, 0.f}, {0.f, -0.4f}})
        g_layer.text(s, bold, em, x + o.first, base + o.second, LayerColor{10, 10, 30, 0.8f * c.a});
    g_layer.text(s, bold, em, x, base, c);
}
float text_w(const std::string& s, bool bold, float em) { return g_layer.text(s, bold, em, 0, 0, LayerColor{0, 0, 0, 0}, true); }

// the EX gauge as the retail HUD's vertical tube (purple, filling from the bottom), at the panel's outer edge. Full: gold with a glow (as retail);
// EX Mode (the gauge drains): pulsing magenta.
void ex_tube(float x, float y0, float y1, float frac, bool full, bool exmode, float a, double t) {
    frac = std::clamp(frac, 0.f, 1.f);
    const float w = 3.4f, h = y1 - y0, fy = y1 - h * frac;
    g_layer.band(x, y0, x + w, y1, [&](float xx, float) { const float e = std::fabs(xx - (x + w / 2.f)) / (w / 2.f); return LayerColor{26, 18, 52, (0.85f - 0.25f * e) * a}; });
    const float pulse = exmode ? 0.5f + 0.5f * static_cast<float>(std::sin(t * 9.0)) : 0.f;
    g_layer.band(x + 0.4f, fy, x + w - 0.4f, y1 - 0.3f, [&](float xx, float yy) {
        const float e = std::fabs(xx - (x + w / 2.f)) / (w / 2.f), k = (yy - fy) / std::max(1.f, y1 - fy), hi = 1.f - e;
        if (exmode) return LayerColor{230.f + 25.f * hi, 90.f + 120.f * pulse * hi, 230.f + 25.f * hi, a};
        if (full) return LayerColor{255.f, 170.f + 70.f * hi, 40.f + 120.f * hi, a};   // full: gold, as the retail tube
        return LayerColor{120.f + 60.f * hi - 30.f * k, 60.f + 60.f * hi - 20.f * k, 210.f + 45.f * hi, a};
    });
    if (full || exmode) {   // glow around the full tube
        const float ga = exmode ? 0.25f + 0.35f * pulse : 0.35f;
        g_layer.band(x - 1.2f, fy - 1.f, x + w + 1.2f, y1 + 0.5f, [&](float xx, float) { const float d = std::min(std::fabs(xx - x), std::fabs(xx - (x + w))); return LayerColor{255.f, exmode ? 120.f : 200.f, exmode ? 255.f : 80.f, (xx >= x && xx <= x + w) ? 0.f : ga * a * (1.f - std::min(1.f, d / 1.2f))}; });
    }
}
// the assist gauge as the retail HUD's two chevrons under the HP bar (each a level of 5000), dark red to orange; a full level brightens
void assist_chevrons(float x, float y, float w, float frac, bool right, bool has, float a) {
    frac = std::clamp(frac, 0.f, 1.f);
    const float h = 2.8f, gap = 1.6f, sw = (w - gap) / 2.f, sl = 1.6f;   // slant
    for (int s = 0; s < 2; s++) {
        const float sx = x + s * (sw + gap);
        const int lev = right ? 1 - s : s;   // levels fill from the panel's outer edge
        const float lf = std::clamp(frac * 2.f - lev, 0.f, 1.f);
        auto inside = [&](float xx, float yy) { const float d = (yy - y) / h * sl; const float l = right ? sx + sl - d : sx + d, r = l + sw - sl; return xx >= l && xx <= r; };
        g_layer.band(sx, y, sx + sw, y + h, [&](float xx, float yy) { return inside(xx, yy) ? LayerColor{18, 18, 36, 0.85f * a} : LayerColor{0, 0, 0, 0}; });
        if (lf <= 0.f) continue;
        const float fx0 = right ? sx + sw * (1.f - lf) : sx, fx1 = right ? sx + sw : sx + sw * lf, lit = lf >= 1.f ? 1.f : 0.f;
        g_layer.band(fx0, y, fx1, y + h, [&](float xx, float yy) {
            if (!inside(xx, yy)) return LayerColor{0, 0, 0, 0};
            const float t = (xx - sx) / sw, top = yy < y + 0.9f ? 1.f : 0.f, k = right ? 1.f - t : t;
            const float aa = (has ? 1.f : 0.45f) * a;
            return LayerColor{190.f + 50.f * k + 15.f * top + 10.f * lit, 40.f + 70.f * k + 60.f * top + 70.f * lit, 30.f + 20.f * top + 40.f * lit, aa};
        });
    }
}


// "-1234": pops in (scale 1.25 -> 1.0 in 0.15 s), holds 0.6 s, drifts up and fades (0.4 s). White with a red-orange edge (the retail HP numbers' look).
// anchor x: the number's near edge (left edge, or right edge when `right`); y: baseline at rest.
void dmg_number(const TagHudHit& h, double age, float ax, float y, bool right, float em0, float rise_px) {
    const float t = static_cast<float>(age);
    const float pop = t < 0.15f ? 1.25f - 0.25f * (t / 0.15f) : 1.f;
    const float fin = std::min(1.f, t / 0.05f), fout = t > 0.75f ? std::max(0.f, 1.f - (t - 0.75f) / 0.4f) : 1.f, a = fin * fout;
    if (a <= 0.01f) return;
    const float dy = -rise_px * (t > 0.75f ? (t - 0.75f) / 0.4f : 0.f) - 1.5f * std::min(1.f, t / 0.15f);
    const float em = em0 * pop;
    const std::string s = "-" + std::to_string(h.dmg);
    const float w = text_w(s, true, em), x = right ? ax - w : ax, by = y + dy;
    const LayerColor edge = h.ko ? LayerColor{255, 60, 40, 0.95f * a} : LayerColor{255, 110, 40, 0.9f * a};
    for (const auto& o : {std::pair<float, float>{0.8f, 0.f}, {-0.8f, 0.f}, {0.f, 0.8f}, {0.f, -0.8f}, {0.6f, 0.6f}, {-0.6f, 0.6f}, {0.6f, -0.6f}, {-0.6f, -0.6f}})
        g_layer.text(s, true, em, x + o.first, by + o.second, edge);
    g_layer.text(s, true, em, x, by, h.ko ? LayerColor{255, 236, 200, a} : LayerColor{255, 255, 255, a});
}
// the hits of one victim that are still showing a number, oldest first; k = index for stacking
void hit_numbers_at(uint32_t victim, double tnow, float ax, float y, bool right, float em, float rise) {
    if (!victim || hpdmg_mode() <= 0) return;
    int k = 0;
    for (const HitRec& r : g_hits) if (r.h.victim == victim && tnow - r.t0 < kNumLife) { dmg_number(r.h, tnow - r.t0, ax, y - 0.75f * em * k, right, em, rise); k++; }
}
// the lost chunk as a lighter trail on a panel's HP bar: waits 0.35 s, then shrinks away (retail's delayed bar)
void hp_trail(uint32_t victim, double tnow, float x, float y, float w, float cur_frac, bool right, float a) {
    if (!victim || hpdmg_mode() <= 0) return;
    const HitRec* last = nullptr; int from = 0;
    for (const HitRec& r : g_hits) if (r.h.victim == victim && tnow - r.t0 < kTrailLife) { if (!last) from = r.h.hp_before; from = std::max(from, r.h.hp_before); last = &r; }
    if (!last) return;
    const float age = static_cast<float>(tnow - last->t0), p = std::clamp((age - 0.35f) / 0.55f, 0.f, 1.f), e = p * p * (3.f - 2.f * p);
    const float hi = std::clamp((float(from) + (float(last->h.hp_after) - float(from)) * e) / float(std::max(1, last->h.hp_max)), 0.f, 1.f), lo = std::clamp(cur_frac, 0.f, 1.f);
    if (hi <= lo) return;
    const float x0 = right ? x + w * (1.f - hi) : x + w * lo, x1 = right ? x + w * (1.f - lo) : x + w * hi;
    const float fa = a * (age < 0.9f ? 1.f : (1.f - (age - 0.9f) / 0.1f));
    g_layer.band(x0, y, x1, y + 3.2f, [&](float, float yy) { return LayerColor{255.f, 236.f - 30.f * (yy - y) / 3.2f, 205.f, 0.95f * fa}; });
}
// a short red wash over a panel at the moment of the hit
void hit_wash(uint32_t victim, double tnow, float x0, float y0, float x1, float y1, float a) {
    if (!victim || hpdmg_mode() <= 0) return;
    for (const HitRec& r : g_hits) if (r.h.victim == victim && tnow - r.t0 < 0.3) {
        const float k = 1.f - static_cast<float>(tnow - r.t0) / 0.3f;
        g_layer.band(x0, y0, x1, y1, [&](float, float) { return LayerColor{255, 70, 50, 0.3f * k * a}; });
    }
}
// the feed: the newest hits as small lines "ATT > VICTIM  -1234" (side coloured names), top centre, 3 lines at most, 2.3 s each
void hit_feed(double tnow, float top) {   // top: below the job-power bars shown along the top edge (they used to cover the feed)
    if (hpdmg_mode() <= 0 || !hpfeed_on()) return;
    std::vector<const HitRec*> live;
    for (const HitRec& r : g_hits) if (tnow - r.t0 < kFeedLife) live.push_back(&r);
    if (live.size() > 3) live.erase(live.begin(), live.end() - 3);
    const float em = 9.4f, cx = 240.f, step = 10.5f; float y = top + 22.f + step * (static_cast<int>(live.size()) - 1);   // (20 Nov: bigger, was 7.4 / 8 px)
    // newest at the bottom, older lines above, top centre under the retail top bar
    for (size_t i = live.size(); i-- > 0; y -= step) {
        const HitRec& r = *live[i]; const float age = static_cast<float>(tnow - r.t0);
        const float a = std::min(std::min(1.f, age / 0.12f), std::clamp((static_cast<float>(kFeedLife) - age) / 0.5f, 0.f, 1.f));
        if (a <= 0.01f) continue;
        const LayerColor ca = r.h.aside == 0 ? kEdgeL : kEdgeR, cv = r.h.vside == 0 ? kEdgeL : kEdgeR;
        const std::string an = r.h.aname, vn = r.h.vname, arrow = " > ", num = "  -" + std::to_string(r.h.dmg), ko = r.h.ko ? "  KO" : "";
        const float wa = an.empty() ? 0.f : text_w(an, true, em) + text_w(arrow, true, em), wv = text_w(vn, true, em), wn = text_w(num, true, em), wk = ko.empty() ? 0.f : text_w(ko, true, em);
        const float tw = wa + wv + wn + wk, x0 = cx - tw / 2.f + (1.f - a) * 6.f;
        g_layer.band(x0 - 8.f, y - 0.84f * em, x0 + tw + 8.f, y + 0.27f * em, [&](float x, float) { const float d = std::fabs(x - (x0 + tw / 2.f)) / (tw / 2.f + 8.f); return LayerColor{14, 16, 40, 0.55f * a * (1.f - std::pow(d, 3.f))}; });
        float x = x0;
        if (!an.empty()) { shadow_text(an, true, em, x, y, LayerColor{ca.r, ca.g, ca.b, a}); x += text_w(an, true, em); shadow_text(arrow, true, em, x, y, LayerColor{190, 195, 215, a}); x += text_w(arrow, true, em); }
        shadow_text(vn, true, em, x, y, LayerColor{cv.r, cv.g, cv.b, a}); x += wv;
        shadow_text(num, true, em, x, y, LayerColor{255, 214, 150, a}); x += wn;
        if (!ko.empty()) shadow_text(ko, true, em, x, y, LayerColor{255, 90, 80, a});
    }
}
// the optional floating number at the fighter in the 3D view (HPDMG=2)
void hit_floating(double tnow) {
    if (hpdmg_mode() < 2) return;
    for (const HitRec& r : g_hits) {
        const double age = tnow - r.t0; if (age >= 1.1 || !r.h.have_pos) continue;
        const float p[3] = {r.h.pos[0], r.h.pos[1] + (r.h.victim ? top_of(r.h.victim) : 2.05f) + 0.05f, r.h.pos[2]}; float sx, sy; if (!tagteam_fx_project(p, sx, sy)) continue;
        if (sx < 10.f || sx > 470.f || sy < 8.f || sy > 262.f) continue;
        TagHudHit h = r.h; const float w = text_w("-" + std::to_string(h.dmg), true, 8.f);
        dmg_number(h, age, sx - w / 2.f, sy - static_cast<float>(age) * 14.f, false, 8.f, 0.f);
    }
}

// the summon orb as the retail HUD draws it (a red jewel with a small label: "R+O" yellow for a manual stone, a purple "A" badge for an AUTO one); greyed when the use is spent
void stone_orb(float cx, float cy, bool autoStone, bool used, float a) {
    const float r = 3.6f;
    g_layer.band(cx - r - 0.6f, cy - r - 0.6f, cx + r + 0.6f, cy + r + 0.6f, [&](float xx, float yy) {
        const float dx = xx - cx, dy = yy - cy, d = std::sqrt(dx * dx + dy * dy); if (d > r + 0.5f) return LayerColor{0, 0, 0, 0};
        if (d > r - 0.5f) return LayerColor{used ? 70.f : 40.f, used ? 70.f : 20.f, used ? 78.f : 30.f, 0.95f * a};
        const float hl = std::clamp(1.f - std::sqrt((dx + 1.1f) * (dx + 1.1f) + (dy + 1.1f) * (dy + 1.1f)) / 3.f, 0.f, 1.f);
        if (used) return LayerColor{90.f + 40.f * hl, 90.f + 40.f * hl, 100.f + 40.f * hl, 0.8f * a};
        return LayerColor{200.f + 55.f * hl, 40.f + 110.f * hl, 40.f + 110.f * hl, a};
    });
    if (autoStone) { g_layer.band(cx + 1.6f, cy + 1.6f, cx + 5.4f, cy + 5.4f, [&](float, float) { return LayerColor{used ? 90.f : 150.f, used ? 90.f : 70.f, used ? 100.f : 235.f, 0.95f * a}; });
        shadow_text("A", true, 3.4f, cx + 2.3f, cy + 4.9f, LayerColor{255, 255, 255, 0.95f * a}); }
    else shadow_text("R+O", true, 3.0f, cx - 3.6f, cy + 7.2f, used ? LayerColor{150, 150, 160, 0.9f * a} : LayerColor{255, 226, 90, a});
}

constexpr double kLethalPulse = 4.65;   // rad/s of the lethal-BRV glow (retail's shimmer, period 1.35 s, measured from window shots of the big panel)
// one compact panel: 112 x 24 design units (31 with the gauges), plus the EX tube at its outer edge
void panel(const TagHudFighter& f, float x, float y, double tnow) {
    const bool right = f.side != 0, ko = f.hp <= 0, gauges = f.ex >= 0.f || f.assist >= 0.f;
    const float w = 112.f, a = ko ? 0.55f : 1.f, H = gauges ? 31.f : 24.f;
    plate(x, y, x + w, y + H, 1.f, right);
    g_layer.band(right ? x + w - 1.2f : x, y, right ? x + w : x + 1.2f, y + H, [&](float, float) { const LayerColor c = right ? kEdgeR : kEdgeL; return LayerColor{c.r, c.g, c.b, c.a * a}; });
    if (f.ex >= 0.f) ex_tube(right ? x + w + 1.f : x - 4.4f, y + 0.5f, y + H - 0.5f, ko ? 0.f : f.ex, !ko && f.ex >= 0.999f, !ko && f.ex_mode, a, tnow);
    const float fw = 52.f, fh = 13.f, fx = right ? x + w - 2.f - fw : x + 2.f;
    draw_face(f.code, fx, y + 1.5f, fw, fh, a, right);
    std::string nm = f.name; for (char& ch : nm) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const float tx = right ? fx - 3.f - text_w(nm, true, 7.f) : fx + fw + 3.f;
    shadow_text(nm, true, 7.f, tx, y + 8.f, ko ? LayerColor{160, 160, 170, 1.f} : LayerColor{255, 255, 255, 1.f});
    // BRV under the name, HP number at the bar's end
    if (ko) { shadow_text("KO", true, 7.f, right ? fx - 3.f - text_w("KO", true, 7.f) : fx + fw + 3.f, y + 15.6f, LayerColor{255, 110, 100, 1.f}); }
    else if (f.brv >= 0) {
        const std::string b = std::to_string(f.brv);
        const float bw = g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        const float bx = right ? fx - 3.f - bw : fx + fw + 3.f;
        g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx + 0.5f, y + 16.5f, LayerColor{10, 10, 30, 0.8f});
        if (f.lethal && f.brv > 0) {   // lethal BRV (>= the HP of this fighter's own target, an HP attack would KO): the number glows purple and shimmers, as the retail big panel's
            const float ph = 0.5f + 0.5f * static_cast<float>(std::sin(tnow * kLethalPulse)), r = 0.9f + 0.7f * ph;   // soft halo radius breathes with the pulse
            for (int ring = 0; ring < 2; ring++) {
                const float rr = r * (ring ? 1.9f : 1.f), al = (ring ? 0.16f : 0.34f) * (0.55f + 0.45f * ph);
                for (int k = 0; k < 8; k++) { const float an = k * 0.7853982f; g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx + rr * std::cos(an), y + 16.f + rr * std::sin(an), LayerColor{120, 70, 235, al}); }
            }
            g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx, y + 16.f, LayerColor{180.f + 30.f * ph, 145.f + 60.f * ph, 255.f, 1.f});
        } else
        g_layer.serif_caps(b, 7.6f, 1.f, 0.2f, bx, y + 16.f, f.brv == 0 ? LayerColor{255, 120, 110, 1.f} : LayerColor{255, 236, 190, 1.f});
    }
    // HP bar over the whole panel width, filling from the outer edge (as the retail slot panels). With the gauges the HP number sits on the row under it beside the
    // half-length assist chevrons (native layout: our side: assist at the left, HP number at the right of the box; the enemy's panel mirrored); without them it ends the bar row.
    const float bx0 = x + 2.f, bw0 = w - 4.f;
    hp_bar(bx0, y + 18.6f, bw0, ko ? 0.f : float(f.hp) / float(std::max(1, f.hp_max)), right, a);
    hp_trail(f.id, tnow, bx0, y + 18.6f, bw0, ko ? 0.f : float(f.hp) / float(std::max(1, f.hp_max)), right, a);
    hit_wash(f.id, tnow, x, y, x + w, y + H, a);
    // the damage number beside the panel's inner edge (the EX tube sits at the outer one), level with the HP bar
    hit_numbers_at(f.id, tnow, right ? x - 5.f : x + w + 5.f, y + 12.f, right, 13.f, 8.f);   // (20 Nov: bigger, was 10)
    const std::string hp = ko ? "0" : std::to_string(std::max(0, f.hp));
    const float hw = text_w(hp, true, 6.2f);
    if (!gauges) shadow_text(hp, true, 6.2f, right ? bx0 : x + w - 2.f - hw, y + 23.f, LayerColor{230, 238, 255, a});
    else {
        const float aw = bw0 * 0.5f - 6.f;   // half the former chevron length (the old gauge was the panel's width less 26)
        const float ax = right ? x + w - 2.f - aw : bx0;
        if (f.assist >= 0.f) assist_chevrons(ax, y + 24.6f, aw, ko ? 0.f : f.assist, right, f.has_assist, a);
        shadow_text(hp, true, 6.2f, right ? bx0 : x + w - 2.f - hw, y + 29.3f, LayerColor{230, 238, 255, a});
    }
    if (f.stone >= 0 && !ko) stone_orb(right ? x + 5.5f : x + w - 5.5f, y + 4.6f, f.stone_auto, f.stone_used, a);   // the member's summon orb at the panel's inner top corner
    if (!ko && (f.ex_mode || f.ex >= 0.999f)) {   // "EX" at the panel's far end of the BRV row: gold = the gauge is full (EX Mode ready), pulsing magenta = in EX Mode
        const float pulse = f.ex_mode ? 0.85f + 0.15f * static_cast<float>(std::sin(tnow * 9.0)) : 1.f;
        const std::string t = f.ex_mode ? "EX MODE" : "EX";
        const float em = f.ex_mode ? 5.0f : 6.0f, tw = text_w(t, true, em);
        shadow_text(t, true, em, right ? x + 3.f : x + w - 3.f - tw, y + 15.6f, f.ex_mode ? LayerColor{255, 150, 245, pulse} : LayerColor{255, 214, 110, 1.f});
    }
    if (!ko && f.break_flash >= 0.f && f.break_flash < 1.f) {   // this fighter's own break (never on the big panel of the player: user, 18 + 20 Nov): the panel flashes red and
        // shakes, "BREAK" pops over its BRV number, shards fly off it (the retail break animation, scaled down to the small panel)
        const float t = f.break_flash, al = t < 0.1f ? t / 0.1f : (t > 0.75f ? (1.f - t) / 0.25f : 1.f), em = 9.6f + 3.f * (1.f - std::min(1.f, t * 5.f));
        const float fl = std::max(0.f, 1.f - t * 3.5f);
        g_layer.band(x, y, x + w, y + H, [&](float, float) { return LayerColor{255, 70, 50, 0.55f * fl * a}; });
        const std::string bt = "BREAK";
        const float bwid = text_w(bt, true, em), shk = t < 0.4f ? 0.9f * std::sin(t * 140.f) * (1.f - t / 0.4f) : 0.f;
        const float bx = (right ? fx - 3.f - bwid : fx + fw + 3.f) + shk, by = y + 16.2f;
        g_layer.band(bx - 2.f, by - em * 0.78f, bx + bwid + 2.f, by + 2.f, [&](float, float) { return LayerColor{20, 8, 18, 0.7f * al}; });
        g_layer.text(bt, true, em, bx + 0.6f, by + 0.6f, LayerColor{10, 10, 30, 0.85f * al});
        g_layer.text(bt, true, em, bx, by, LayerColor{255, 96, 76, al});
        for (int i = 0; i < 7; i++) {   // shards
            const float ang = i * 0.9f + 0.4f, d = 3.f + t * 26.f * (0.6f + 0.1f * (i % 4)), sa = std::max(0.f, 1.f - t * 1.3f);
            const float cx = bx + bwid * 0.5f + std::cos(ang) * d * 1.6f, cy = by - 3.f + std::sin(ang) * d * 0.7f;
            g_layer.band(cx - 1.2f, cy - 0.7f, cx + 1.2f, cy + 0.7f, [&](float, float) { return LayerColor{255, 190.f - 12.f * i, 150.f, 0.9f * sa * a}; });
        }
    }
}

// a side's reserve: face strips of the members still to come (next one first), small, under the x1.0 counters
void reserve(const TagHudReserve& r) {
    const int n = static_cast<int>(std::min(r.names.size(), static_cast<size_t>(5)));
    if (n <= 0) return;
    const bool right = r.side != 0;
    const float fw = 40.f, fh = 10.f, gap = 2.f, y = 22.f;
    const float label_w = text_w("RESERVE", true, 5.6f);
    const float total = label_w + 3.f + n * (fw + gap);
    float x = right ? 480.f - 4.f - total : 4.f;
    plate(x - 2.f, y - 1.5f, x + total + 1.f, y + fh + 1.5f, 0.9f, right);
    shadow_text("RESERVE", true, 5.6f, x, y + 7.2f, LayerColor{200, 210, 240, 0.95f});
    x += label_w + 3.f;
    for (int i = 0; i < n; i++) {
        const std::string code = i < static_cast<int>(r.codes.size()) ? r.codes[i] : std::string();
        draw_face(code, x, y, fw, fh, i == 0 ? 1.f : 0.6f, right);
        if (i == 0) g_layer.band(x, y + fh, x + fw, y + fh + 0.8f, [&](float, float) { const LayerColor c = right ? kEdgeR : kEdgeL; return c; });
        x += fw + gap;
    }
    if (r.countdown >= 0.f) {   // the next member's call-in wait, under the strip
        char b[32]; std::snprintf(b, sizeof b, "NEXT IN %d", static_cast<int>(std::ceil(r.countdown)));
        const std::string t = b; const float tw = text_w(t, true, 6.4f), tx = right ? 480.f - 4.f - tw : 4.f + 2.f, ty = y + fh + 9.f;
        plate(right ? tx - 4.f : tx - 3.f, ty - 7.f, right ? tx + tw + 3.f : tx + tw + 5.f, ty + 2.2f, 0.9f, right);
        shadow_text(t, true, 6.4f, tx, ty, r.countdown < 5.f ? LayerColor{255, 214, 120, 1.f} : LayerColor{235, 240, 255, 1.f});
    }
}

// The online pause menu (Start in a Communications tag battle; the battle goes on): a card in the game's menu look with Resume / Forfeit, then the YES / NO confirm.
void pause_menu(int state, int sel) {
    g_layer.band(0.f, 0.f, 480.f, 272.f, [&](float, float) { return LayerColor{8, 10, 36, 0.38f}; });
    const float x0 = 140.f, x1 = 340.f, y0 = 78.f, y1 = state == 3 ? 150.f : 190.f;
    g_layer.band(x0 + 1.5f, y0 + 2.f, x1 + 2.5f, y1 + 3.f, [&](float, float) { return LayerColor{20, 20, 70, 0.30f}; });
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float e = std::min({x - x0, x1 - x, y - y0, y1 - y}), t = (y - y0) / (y1 - y0);
        if (e < 1.f) return LayerColor{120, 124, 188, 1.f};
        if (e < 2.2f) return LayerColor{255, 255, 255, 1.f};
        return LayerColor{244 - 12 * t, 246 - 12 * t, 255 - 6 * t, 0.97f};
    });
    g_layer.band(x0 + 2.2f, y0 + 2.2f, x1 - 2.2f, y0 + 20.f, [&](float x, float y) {   // the navy title ribbon
        const float t = (y - y0 - 2.2f) / 17.8f, fade = x > x1 - 50.f ? std::max(0.f, 1.f - (x - (x1 - 50.f)) / 47.8f) : 1.f;
        return LayerColor{20 + 40 * (1 - t), 22 + 38 * (1 - t), 96 + 56 * (1 - t), fade};
    });
    const std::string title = state == 2 ? "FORFEIT THE MATCH?" : state == 3 ? "FORFEIT" : "PAUSE";
    shadow_text(title, true, 9.f, x0 + 12.f, y0 + 15.f, LayerColor{255, 255, 255, 1.f});
    if (state == 3) {
        const std::string t = "Leaving the match...";
        g_layer.text(t, true, 7.6f, (x0 + x1) / 2.f - text_w(t, true, 7.6f) / 2.f, y0 + 46.f, LayerColor{40, 42, 65, 1.f});
        return;
    }
    const std::string a = state == 2 ? "NO" : "Resume", b = state == 2 ? "YES" : "Forfeit";
    for (int i = 0; i < 2; i++) {
        const float ry = y0 + 28.f + 26.f * static_cast<float>(i), rh = 20.f, rx0 = x0 + 22.f, rx1 = x1 - 12.f; const bool on = sel == i;
        const float pt = 8.f;
        g_layer.band(rx0, ry, rx1, ry + rh, [&](float x, float yy) {
            const float dx = std::min(x - rx0, rx1 - x), half = rh / 2.f * (dx >= pt ? 1.f : 0.45f + 0.55f * dx / pt), dy = std::fabs(yy - (ry + rh / 2.f));
            const float m = std::clamp((half - dy) * 2.f, 0.f, 1.f), v = (yy - ry) / rh, t = std::clamp((x - rx0) / (rx1 - rx0), 0.f, 1.f);
            if (on) { float r = 63 + 71 * t * t, g = 53 + 83 * t * t, bl = 174 + 33 * t * t; if (v < 0.22f) { r += 34; g += 36; bl += 14; } return LayerColor{r, g, bl, m}; }
            if (std::min(dy / std::max(half, 0.01f), 1.f) > 0.86f) return LayerColor{128, 126, 164, m};
            return LayerColor{248 - 40 * v, 249 - 40 * v, 255 - 26 * v, m};
        });
        const std::string& t = i == 0 ? a : b;
        shadow_text(t, true, 8.4f, rx0 + 16.f, ry + rh / 2.f + 3.f, on ? LayerColor{255, 255, 255, 1.f} : LayerColor{40, 42, 65, 1.f});
        if (on) dissidia_menu_draw_hand(g_layer, rx0 - 20.f, ry + rh / 2.f - 12.f, 1.f);
    }
    const std::string hint = state == 2 ? "Cross: select   Circle: back" : "Cross: select   Start: resume";
    g_layer.text(hint, true, 6.2f, (x0 + x1) / 2.f - text_w(hint, true, 6.2f) / 2.f, y1 - 8.f, LayerColor{70, 74, 130, 1.f});
}

void banner(const std::string& t) {
    const float em = 10.f, tw = text_w(t, true, em), cx = 240.f, y = 50.f;
    g_layer.band(cx - tw / 2.f - 40.f, y - 9.f, cx + tw / 2.f + 40.f, y + 5.f, [&](float x, float yy) {
        const float d = std::fabs(x - cx) / (tw / 2.f + 40.f), fade = 1.f - std::pow(d, 3.f);
        float r = 63, g = 53, b = 174; if (yy < y - 8.f) { r += 40; g += 45; b += 30; }
        return LayerColor{r, g, b, 0.85f * fade};
    });
    shadow_text(t, true, em, cx - tw / 2.f, y + 2.5f, LayerColor{255, 255, 255, 1.f});
}

// a notice. A fall ("NAME IS DOWN"): a dark plate with a red edge under the join banner. The job powers: the game's own result-screen bar ("Party
// received strong powers of the Warrior." across the top: a navy bar, white text, the |marked| words in cyan), fading in and out.
void notice(const Notice& n, float y, double t) {
    const float fin = static_cast<float>(std::clamp((t - n.start) / 0.25, 0.0, 1.0)), fout = static_cast<float>(std::clamp((n.until - t) / 0.5, 0.0, 1.0)), a = std::min(fin, fout);
    if (n.kind == 0) {
        const float em = 8.4f, tw = text_w(n.text, true, em), cx = 240.f, hw = tw / 2.f + 26.f;
        g_layer.band(cx - hw, y - 7.f, cx + hw, y + 4.2f, [&](float x, float) { const float d = std::fabs(x - cx) / hw; return LayerColor{14, 16, 40, 0.72f * a * (1.f - std::pow(d, 3.f))}; });
        g_layer.band(cx - hw, y - 7.f, cx + hw, y - 6.2f, [&](float x, float) { const float d = std::fabs(x - cx) / hw; return LayerColor{255, 110, 100, 0.9f * a * (1.f - std::pow(d, 2.f))}; });
        shadow_text(n.text, true, em, cx - tw / 2.f, y + 2.f, LayerColor{255, 255, 255, a});
        return;
    }
    const float em = 9.f, h = 19.f;
    std::vector<std::pair<std::string, bool>> seg; { bool hi = false; std::string cur; for (char c : n.text) { if (c == '|') { if (!cur.empty()) seg.push_back({cur, hi}); cur.clear(); hi = !hi; } else cur += c; } if (!cur.empty()) seg.push_back({cur, hi}); }
    float tw = 0.f; for (const auto& sg : seg) tw += text_w(sg.first, true, em);
    g_layer.band(0.f, y, 480.f, y + h, [&](float, float yy) { const float k = (yy - y) / h; return LayerColor{18.f + 8.f * (1.f - k), 28.f + 14.f * (1.f - k), 96.f - 30.f * k, 0.92f * a}; });
    g_layer.band(0.f, y + h - 1.f, 480.f, y + h, [&](float, float) { return LayerColor{110, 150, 255, 0.85f * a}; });
    g_layer.band(0.f, y, 480.f, y + 0.8f, [&](float, float) { return LayerColor{150, 180, 255, 0.55f * a}; });
    float x = 240.f - tw / 2.f;
    for (const auto& sg : seg) { shadow_text(sg.first, true, em, x, y + 13.f, sg.second ? LayerColor{90, 215, 255, a} : LayerColor{255, 255, 255, a}); x += text_w(sg.first, true, em); }
}

}  // namespace

void tagteam_hud_notice(const std::string& text, int kind, float secs) {
    const double t = now_s();
    for (const Notice& n : g_notices) if (n.text == text && n.until > t) return;   // the same message is not stacked twice
    g_notices.push_back({text, kind, t + secs, t});
    if (g_notices.size() > 6) g_notices.erase(g_notices.begin());
}
void tagteam_hud_hp_hit(const TagHudHit& h) {
    if (hpdmg_mode() <= 0 || h.dmg <= 0) return;
    g_hits.push_back({h, now_s()});
    if (g_hits.size() > 24) g_hits.erase(g_hits.begin());
}
void tagteam_hud_big_panels(uint32_t l, uint32_t r) { g_big[0] = l; g_big[1] = r; }
void tagteam_hud_clear() { g_hits.clear(); g_notices.clear(); g_banner.clear(); }

void tagteam_hud_set_banner(const std::string& t) { g_banner = t; }
void tagteam_hud_overhead(const std::vector<TagHudOverhead>& bars, int style, bool hide_panels) { g_overhead = bars; g_overhead_style = style; g_hide_panels = hide_panels; }
void tagteam_hud_set_ram(const uint8_t* rdram) { g_hud_ram = rdram; }
void tagteam_hud_set_intro(bool r) { g_intro_running = r; }
void tagteam_hud_set_top_fn(float (*fn)(uint32_t)) { g_top_fn = fn; }
void tagteam_hud_lockind(const TagHudLockInd& li) {
    const double t = now_s();
    for (uint32_t a : li.fresh) { g_lock_new[a] = t; g_lock_me_new = t; }
    if (!li.fresh.empty() && !std::getenv("PSPRECOMP_TAGTEAM_LOCKFX_OFF") && g_hud_ram) {   // a new lock-on: a short pulse, stronger / longer for the 2nd and later enemy, brighter on the side the enemy is on
        g_vig_t0 = t; g_vig_str = li.by.size() >= 2 ? 1.f : 0.65f; g_vig_dur = (li.by.size() >= 2 ? 0.65 : 0.45) * (std::getenv("PSPRECOMP_TAGTEAM_LOCKFX_SLOW") ? std::atof(std::getenv("PSPRECOMP_TAGTEAM_LOCKFX_SLOW")) : 1.0); g_vig_side = 0;   // (_SLOW=N: test aid, N times longer)
        float p[3]; std::memcpy(p, g_hud_ram + ((li.fresh[0] + 0x80u) & 0x07FFFFFFu), 12); p[1] += 1.0f; float sx, sy;
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) { if (!tagteam_fx_project(p, sx, sy)) g_vig_side = 2; else if (sx < 100.f) g_vig_side = -1; else if (sx > 380.f) g_vig_side = 1; }   // 2 = behind the camera: the bottom edge
    }
    g_lockind = li;
    if (li.by.empty() || !li.me) g_lock_new.clear();
}


namespace {
// One bar over a fighter's head (2.05 m above the feet): the retail HP bar's colours on a dark plate, the target's bravery above it.
void overhead_bar(const TagHudOverhead& o) {
    if (!g_hud_ram || !o.actor) return;
    float pos[3]; std::memcpy(pos, g_hud_ram + ((o.actor + 0x80u) & 0x07FFFFFFu), 12);
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || std::fabs(pos[0]) > 1000.f || std::fabs(pos[2]) > 1000.f) return;   // (a fighter parked away from the stage)
    const float head[3] = {pos[0], pos[1] + top_of(o.actor), pos[2]};
    float x, y; if (!tagteam_fx_project(head, x, y)) return;
    if (g_overhead_style == 3) return;   // the game's own panels, half size (tagteam.cpp npanel)
    if (g_overhead_style == 2) {   // the mini status panel: 64 x 19 above the head, the target's at 150 / 256 of full strength, the others at 70 / 256 (plugin v0.61)
        const float w = 64.f, h = 19.f, x0 = x - w * 0.5f, y0 = y - h;
        if (x0 + w < 0.f || x0 > 480.f || y0 + h < 0.f || y0 > 272.f) return;
        g_layer.opacity = o.target ? 150.f / 256.f : 70.f / 256.f;
        plate(x0, y0, x0 + w, y0 + h, 1.f, true);
        g_layer.band(x0 + w - 1.f, y0, x0 + w, y0 + h, [&](float, float) { return kEdgeR; });
        std::string nm = o.name; for (char& ch : nm) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        shadow_text(nm, true, 5.6f, x0 + 2.f, y0 + 6.2f, LayerColor{255, 255, 255, 1.f});
        if (o.brv >= 0) {
            const std::string b = std::to_string(o.brv);
            const float bw = g_layer.serif_caps(b, 9.f, 1.f, 0.2f, 0, 0, LayerColor{0, 0, 0, 0}, true), bx = x0 + w - 2.5f - bw;
            g_layer.serif_caps(b, 9.f, 1.f, 0.2f, bx + 0.5f, y0 + 11.5f, LayerColor{10, 10, 30, 0.8f});
            g_layer.serif_caps(b, 9.f, 1.f, 0.2f, bx, y0 + 11.f, o.brv == 0 ? LayerColor{255, 120, 110, 1.f} : LayerColor{255, 236, 190, 1.f});
        }
        hp_bar(x0 + 2.f, y0 + 12.6f, w - 4.f, static_cast<float>(o.hp) / static_cast<float>(std::max(1, o.hp_max)), true, 1.f);
        const std::string hp = std::to_string(std::max(0, o.hp));
        shadow_text(hp, true, 4.8f, x0 + 2.f, y0 + 18.6f, LayerColor{230, 238, 255, 1.f});
        g_layer.opacity = 1.f;
        return;
    }
    // Every enemy gets the same full bar and its bravery (user: "every enemy should have it"); the lock target's is a little brighter and drawn last.
    const float w = 36.f, a = o.target ? 1.f : 0.55f;   // the other enemies more see-through (user: "the sub target should be more transparent")
    if (x < -w || x > 480.f + w || y < 0.f || y > 272.f) return;
    const float x0 = x - w * 0.5f;
    g_layer.band(x0 - 1.f, y - 1.f, x0 + w + 1.f, y + 4.2f, [&](float, float) { return LayerColor{14, 16, 40, 0.7f * a}; });
    hp_bar(x0, y, w, static_cast<float>(o.hp) / static_cast<float>(std::max(1, o.hp_max)), false, a);
    if (o.brv >= 0) {
        const std::string b = std::to_string(o.brv);
        shadow_text(b, true, 7.f, x - text_w(b, true, 7.f) * 0.5f, y - 2.f, o.target ? LayerColor{255, 236, 170, a} : LayerColor{235, 225, 200, a});
    }
}
}  // namespace


namespace {
bool vig_active(double tnow) { return tnow - g_vig_t0 < g_vig_dur; }
void vig_draw(double tnow) {
    const float k = static_cast<float>((tnow - g_vig_t0) / g_vig_dur);
    if (k < 0.f || k >= 1.f) return;
    { static int n = 0; if (std::getenv("PSPRECOMP_TAGTEAM_LOG") && n++ % 60 == 0) std::fprintf(stderr, "[LOCKFX] pulse k=%.3f str=%.2f side=%d\n", k, g_vig_str, g_vig_side); }
    const float a0 = 0.55f * g_vig_str * std::pow(1.f - k, 1.6f) * std::min(1.f, static_cast<float>(tnow - g_vig_t0) / 0.05f + 0.3f);   // quick rise, slow fade
    const float wv = 58.f;
    auto strip = [&](float x0, float y0, float x1, float y1, int dir, float boost) {   // dir: 0 left edge, 1 right, 2 top, 3 bottom
        g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
            float d = dir == 0 ? (x - x0) : dir == 1 ? (x1 - x) : dir == 2 ? (y - y0) : (y1 - y);
            d = 1.f - d / (dir < 2 ? wv : wv * 0.6f); if (d <= 0.f) return LayerColor{0, 0, 0, 0};
            return LayerColor{255, 70.f + 50.f * d, 30.f, a0 * boost * d * d};
        });
    };
    const float bl = g_vig_side == -1 ? 1.f : (g_vig_side == 0 ? 0.7f : 0.45f), br = g_vig_side == 1 ? 1.f : (g_vig_side == 0 ? 0.7f : 0.45f);
    strip(0, 0, wv, 272, 0, bl); strip(480 - wv, 0, 480, 272, 1, br);
    strip(0, 0, 480, wv * 0.6f, 2, 0.5f); strip(0, 272 - wv * 0.6f, 480, 272, 3, g_vig_side == 2 ? 1.f : 0.5f);
}
}  // namespace

namespace {
// ---- lock-on indicator: who has the player as their target ----
bool world_pos(uint32_t actor, float p[3]) {
    if (!g_hud_ram || !actor) return false;
    std::memcpy(p, g_hud_ram + ((actor + 0x80u) & 0x07FFFFFFu), 12);
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::fabs(p[0]) < 1000.f && std::fabs(p[2]) < 1000.f;
}
// An eye (almond with an iris) in the game's red-orange: cx, cy = centre, w = width. a = opacity, pulse 0..1 brightens the iris, flash 0..1 a bloom behind.
void eye_icon(float cx, float cy, float w, float a, float pulse, float flash) {
    const float h = w * 0.52f, hw = w * 0.5f, hh = h * 0.5f;
    if (flash > 0.01f) g_layer.glow(cx, cy, w * (0.7f + 1.1f * (1.f - flash)), LayerColor{255, 150, 90, 0.75f * flash * a});
    // dark plate (a slightly larger almond) so the eye reads on any stage
    g_layer.band(cx - hw - 1.5f, cy - hh - 1.5f, cx + hw + 1.5f, cy + hh + 1.5f, [&](float x, float y) {
        const float u = (x - cx) / (hw + 1.2f), cov = (1.f - u * u) * (hh + 1.2f) - std::fabs(y - cy);
        if (std::fabs(u) >= 1.f || cov <= 0.f) return LayerColor{0, 0, 0, 0};
        return LayerColor{14, 10, 24, 0.78f * a * std::min(1.f, cov * 1.5f)};
    });
    g_layer.band(cx - hw, cy - hh, cx + hw, cy + hh, [&](float x, float y) {
        const float u = (x - cx) / hw, dy = y - cy, edge = (1.f - u * u) * hh - std::fabs(dy);
        if (std::fabs(u) >= 1.f || edge <= 0.f) return LayerColor{0, 0, 0, 0};
        const float ca = std::min(1.f, edge * 1.6f) * a;
        const float r = std::sqrt((x - cx) * (x - cx) + dy * dy);
        const float ir = hh * 0.95f, pr = hh * 0.42f;
        if (r < pr) return LayerColor{20, 6, 10, ca};                                       // pupil
        if (r < ir) return LayerColor{255.f, 80.f + 60.f * pulse, 40.f + 30.f * pulse, ca};   // iris: red-orange
        return LayerColor{250, 232, 218, ca};                                                // white of the eye
    });
}
float lock_age(uint32_t a, double tnow) { auto it = g_lock_new.find(a); return it == g_lock_new.end() ? 99.f : static_cast<float>(tnow - it->second); }
void lockind_draw(double tnow) {
    const TagHudLockInd& li = g_lockind;
    if (li.by.empty() || !li.me) return;
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(tnow * 6.0));
    // -- on each enemy that is locked on: an eye over its head (above the overhead bar when there is one); off to the side of the screen: at the edge
    for (uint32_t a : li.by) {
        float pos[3]; if (!world_pos(a, pos)) continue;
        const float head[3] = {pos[0], pos[1] + top_of(a), pos[2]};
        float x, y; if (!tagteam_fx_project(head, x, y)) continue;
        static const float up[4] = {7.f, 19.f, 30.f, 23.f};   // above: nothing / the HP bar + BRV / the mini panel / the PPSSPP panel
        const float dy = up[std::clamp(li.oh_style, 0, 3)];
        float ex = x, ey = y - dy; bool edge = false;
        if (ex < 12.f) { ex = 12.f; edge = true; } else if (ex > 468.f) { ex = 468.f; edge = true; }
        if (ey < 30.f) ey = 30.f;
        if (ey > 252.f) { ey = 252.f; edge = true; }
        const float age = lock_age(a, tnow), fl = age < 0.6f ? 1.f - age / 0.6f : 0.f;
        const float pop = age < 0.18f ? 1.35f - 0.35f * (age / 0.18f) : 1.f;
        if (std::getenv("PSPRECOMP_TAGTEAM_LOCKIND_DBG")) {   // test aid: a small green cross where the enemy's body centre (0.9 m up) projects: the retail lock-on ring should be centred on it
            const float bc[3] = {pos[0], pos[1] + 0.9f, pos[2]}; float bx, by; if (tagteam_fx_project(bc, bx, by)) { g_layer.band(bx - 4.f, by - 0.5f, bx + 4.f, by + 0.5f, [&](float, float) { return LayerColor{80, 255, 80, 1.f}; }); g_layer.band(bx - 0.5f, by - 4.f, bx + 0.5f, by + 4.f, [&](float, float) { return LayerColor{80, 255, 80, 1.f}; }); }
        }
        eye_icon(ex, ey, (edge ? 12.f : 14.f) * pop, edge ? 0.8f : 0.95f, pulse, fl);
    }
    // -- on the player: a pulsing red-orange chevron over its head, "xN" with more than one (no floor ring: removed 25 Nov, user)
    float me[3]; if (!world_pos(li.me, me)) return;
    const float head[3] = {me[0], me[1] + top_of(li.me) + 0.3f, me[2]};
    float hx, hy; const bool ok_head = tagteam_fx_project(head, hx, hy);
    if (ok_head && hx > -20.f && hx < 500.f && hy > -20.f && hy < 292.f) {
        const float bob = 1.5f * static_cast<float>(std::sin(tnow * 5.0)), cx = hx, tipy = hy + bob, wd = 10.f, ht = 8.f, th = 4.8f;   // a V (pointing down at the player): its centre is the lowest
        const float a = 0.95f;
        g_layer.band(cx - wd - 1.5f, tipy - ht - 1.5f, cx + wd + 1.5f, tipy + th + 1.5f, [&](float x, float y) {   // dark outline of the chevron
            const float yt = tipy - std::fabs(x - cx) * (ht / wd);
            return (std::fabs(x - cx) <= wd + 1.4f && y >= yt - 1.4f && y <= yt + th + 1.4f) ? LayerColor{14, 10, 24, 0.75f * a} : LayerColor{0, 0, 0, 0};
        });
        g_layer.band(cx - wd, tipy - ht, cx + wd, tipy + th, [&](float x, float y) {
            const float yt = tipy - std::fabs(x - cx) * (ht / wd);
            if (std::fabs(x - cx) > wd || y < yt || y > yt + th) return LayerColor{0, 0, 0, 0};
            const float t = (y - yt) / th;
            return LayerColor{255.f, 90.f + 60.f * pulse - 30.f * t, 45.f + 30.f * pulse, a};
        });
        if (li.by.size() > 1) {
            const std::string s = "x" + std::to_string(li.by.size());
            shadow_text(s, true, 9.f, cx + wd + 4.f, tipy - 3.f, LayerColor{255, 214, 150, 1.f});
        }
    }
}
}  // namespace

void tagteam_hud_draw(const std::vector<TagHudFighter>& fighters, const std::vector<TagHudReserve>& reserves) {
    g_fighters = fighters; g_reserves = reserves;
}
void tagteam_hud_hide() { g_hits.clear(); g_fighters.clear(); g_reserves.clear(); g_overhead.clear(); g_banner.clear(); g_notices.clear(); g_lockind = TagHudLockInd(); g_lock_new.clear(); g_vig_t0 = -10.0; }

bool tagteam_fx_draw(dissidia::MenuLayer& layer);   // tagteam_fx.cpp

// once per frame: the HUD and the effects on the game layer
namespace {
struct Lap {   // PSPRECOMP_TAGTEAM_HITCH: where a slow layer frame spends its time
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now(); bool on = std::getenv("PSPRECOMP_TAGTEAM_HITCH") != nullptr; std::string acc;
    void lap(const char* n) { if (!on) return; const auto now = std::chrono::steady_clock::now(); const double ms = std::chrono::duration<double, std::milli>(now - t).count(); t = now; if (ms >= 3.0) { char b[64]; std::snprintf(b, sizeof b, " %s=%.1f", n, ms); acc += b; } }
    ~Lap() { if (on && !acc.empty()) std::fprintf(stderr, "[LAYERLAP]%s\n", acc.c_str()); }
};
}
void tagteam_layer_frame(bool in_battle) {
    Lap L;
    if (!in_battle) { tagteam_hud_hide(); }
    const double tnow = now_s();
    g_notices.erase(std::remove_if(g_notices.begin(), g_notices.end(), [&](const Notice& n) { return n.until <= tnow; }), g_notices.end());
    g_hits.erase(std::remove_if(g_hits.begin(), g_hits.end(), [&](const HitRec& r) { return tnow - r.t0 >= kFeedLife + 0.1; }), g_hits.end());
    int pm_sel = 0; const int pm = dissidia_ttnet_pause_menu(&pm_sel);
    const bool any = pm != 0 || !g_fighters.empty() || !g_hits.empty() || !g_reserves.empty() || !g_overhead.empty() || !g_lockind.by.empty() || vig_active(tnow) || !g_banner.empty() || !g_notices.empty();
    L.lap("prep"); g_layer.begin(std::max(2, psp_sddialog::output_scale())); L.lap("begin");
    if (any) {
        for (const TagHudOverhead& o : g_overhead) if (!o.target) overhead_bar(o);   // (under the panels; the target's bar last, on top of the others)
        for (const TagHudOverhead& o : g_overhead) if (o.target) overhead_bar(o);
        int row[2] = {0, 0};
        for (const TagHudFighter& f : g_fighters) {
            if (g_hide_panels || g_intro_running) break;   // (not in the banters / intro) Settings > Debug > Hide Tag Team UI
            const int s = f.side != 0;
            const bool g = f.ex >= 0.f || f.assist >= 0.f;   // with the gauges a panel is 31 high (rows 34 apart) and its EX tube sits outside it
            // Smaller and a little see-through (user, 22 Nov: "the side HUD is too big and distracting"): the stack is zoomed to kSideZoom about the
            // panels' outer bottom corner (above the retail big panel), so it keeps its place at the screen edge.
            constexpr float kSideZoom = 0.68f;
            g_layer.set_zoom(kSideZoom, s == 0 ? 0.f : 480.f, 173.f); g_layer.opacity = 0.85f;
            panel(f, s == 0 ? (g ? 9.f : 4.f) : 480.f - (g ? 9.f : 4.f) - 112.f, 142.f - (g ? 34.f : 27.f) * row[s]++, tnow);
            g_layer.set_zoom(1.f, 0.f, 0.f); g_layer.opacity = 1.f;
        }
        vig_draw(tnow); lockind_draw(tnow); L.lap("lockind");
        L.lap("panels"); for (const TagHudReserve& r : g_reserves) reserve(r); L.lap("reserves");
        if (hpdmg_mode() > 0 && !g_hits.empty()) {   // fighters on the retail big panels (bottom corners, number beside the BRV): ours sits next to the retail pulse
            for (int s = 0; s < 2; s++) {
                bool small = false; for (const TagHudFighter& f : g_fighters) if (f.id == g_big[s]) small = true;
                if (!small) hit_numbers_at(g_big[s], tnow, s == 0 ? 148.f : 332.f, 243.f, s != 0, 14.f, 9.f);   // (was 11)
            }
            hit_floating(tnow);
            { float top = 0.f; for (const Notice& n : g_notices) if (n.kind != 0) top += 19.f; hit_feed(tnow, top); }
        }
        L.lap("hits"); if (!g_banner.empty()) banner(g_banner); L.lap("banner");
        { float yd = 70.f, yj = 0.f;   // falls under the join banner, job powers along the top edge
          for (const Notice& n : g_notices) { if (n.kind == 0) { notice(n, yd, tnow); yd += 13.f; } else { notice(n, yj, tnow); yj += 19.f; } } }
    }
    if (pm) pause_menu(pm, pm_sel);
    L.lap("notices"); const bool fx = tagteam_fx_draw(g_layer); L.lap("fx");
    if (!any && !fx) { if (g_layer_shown) { g_layer.clear(); g_layer_shown = false; } return; }
    g_layer.publish(); L.lap("publish");
    g_layer_shown = true;
}

const uint32_t* dissidia_tagteam_face_px(const std::string& code, int* w, int* h) {   // the Free-for-all selector's tiles (the menu layer draws them itself)
    const dissidia::GimImage* g = face(code); if (!g) return nullptr; *w = g->w; *h = g->h; return g->rgba.data();
}
