// games/dissidia/runtime/tagteam_vs.cpp - the pre-battle "Round N / VS" page in tag modes (docs/tag-team.md "Batch thirty-six").
// Retail's page shows one fighter a side (the retail slot fighter, the party's first member). In a tag battle the real start is different (partners on the
// stage, a reserve behind them, free-for-all without parties), so the page's fighter panel is repainted on the game layer in the page's own look: dark blue
// band for this player's side, crimson for the opponents, serif names, the face strips retail uses for "Assist", job tags with pointed ends, and the orb rows
// get a gold ring on every member that starts on the stage. STAGE / JUDGMENT, the Round pill and the sides' band titles stay retail's.
// PSPRECOMP_TAGTEAM_VSPAGE_OLD=1: the retail page unchanged.
#include "menu_layer.h"
#include "psp_ge_draw.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include "tagteam_vs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

std::string dissidia_tagteam_char_name(int id);
std::string dissidia_tagteam_char_code(int id);
const uint32_t* dissidia_tagteam_face_px(const std::string& code, int* w, int* h);

namespace {
using dissidia::LayerColor;
dissidia::MenuLayer g_vl("vs", true);
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double g_t0 = -1, g_last = -1;
const char* kJob[16] = {"Freelancer", "Warrior", "Monk", "Thief", "Dark Knight", "Dragoon", "White Mage", "Black Mage", "Summoner", "Mime", "Knight", "Samurai", "Berserker", "Mystic Knight", "Ninja", "Sage"};
float tw(const std::string& s, float em, bool bold = true) { return g_vl.text(s, bold, em, 0, 0, LayerColor{0, 0, 0, 0}, true); }
// text with a dark edge (the page's lettering), shrunk to fit maxw
float label(const std::string& s, float em, float x, float base, LayerColor c, float maxw = 1e9f, bool serif = false, bool right = false) {
    float e = em; const float w0 = tw(s, e); if (w0 > maxw) e = em * maxw / w0;
    const float w = tw(s, e); const float x0 = right ? x - w : x;
    for (const auto& o : {std::pair<float, float>{0.5f, 0.6f}, {-0.4f, 0.5f}}) g_vl.text(s, true, e, x0 + o.first, base + o.second, LayerColor{12, 14, 48, 0.55f * c.a});
    (void)serif; g_vl.text(s, true, e, x0, base, c);
    return w;
}
void job_tag(int job, float x, float y, float w, float h, bool right_end, float a) {   // the pointed grey tag (retail's "Monk" / "White Mage")
    if (job < 1 || job > 15) return;
    const std::string s = kJob[job];
    g_vl.band(x, y, x + w, y + h, [&](float xx, float yy) {
        const float dx = right_end ? x + w - xx : xx - x, half = h / 2.f * (dx > h * 0.5f ? 1.f : 0.3f + 1.4f * dx / h), dy = std::fabs(yy - (y + h / 2.f));
        const float m = std::clamp((half - dy) * 2.f, 0.f, 1.f), t = (yy - y) / h;
        return LayerColor{92 - 30 * t, 100 - 30 * t, 130 - 30 * t, 0.95f * m * a};
    });
    const float e = h * 0.58f; const float sw = tw(s, e);
    g_vl.text(s, true, e, x + (w - std::min(sw, w - 8.f)) / 2.f, y + h * 0.72f, LayerColor{236, 240, 255, a});
}
void face_strip(int ch, float x, float y, float w, float h, bool right_side, float a) {
    int iw = 0, ih = 0; const uint32_t* px = ch > 0 ? dissidia_tagteam_face_px(dissidia_tagteam_char_code(ch), &iw, &ih) : nullptr;
    if (!px) { g_vl.band(x, y, x + w, y + h, [&](float, float) { return LayerColor{30, 34, 70, 0.6f * a}; }); return; }
    const int sh = std::min(ih, 26);
    // the strip's picture sits at its left; show the part that fits the aspect of the box
    const int sw = std::min(iw, std::max(8, int(float(sh) * w / h + 0.5f)));
    (void)right_side;
    g_vl.image(px, iw, ih, 0, 0, sw, sh, x, y, w, h, a);
}
LayerColor side_col(int t, float v) { return t == 0 ? LayerColor{36 + 34 * v, 40 + 34 * v, 118 + 44 * v, 1.f} : LayerColor{104 + 40 * v, 40 + 28 * v, 92 + 30 * v, 1.f}; }
std::string fit_name(int ch) { return dissidia_tagteam_char_name(ch); }

void card_row(const VsCard& c, int t, float x0, float x1, float y, float h, float a) {
    const bool left = t == 0;
    g_vl.band(x0, y, x1, y + h, [&](float x, float yy) {   // the side's band, fading out towards the screen centre
        const float u = left ? (x1 - x) / (x1 - x0) : (x - x0) / (x1 - x0), v = (yy - y) / h;
        LayerColor k = side_col(t, v); k.a = a * (u < 0.8f ? 1.f : std::max(0.f, 1.f - (u - 0.8f) / 0.2f) * 0.9f + 0.1f); return k;
    });
    const float fw = h * 2.5f;   // the face strip at the outer edge (retail: the portrait sits there)
    const float fx = left ? x0 : x1 - fw;
    face_strip(c.ch, fx, y, fw, h, !left, a);
    // role badge on the strip
    const std::string role = c.role == 0 ? "FIGHTER" : "PARTNER";
    const float be = std::min(5.2f, h * 0.2f), bw = tw(role, be) + 5.f;
    g_vl.band(fx, y + h - be - 3.f, fx + bw, y + h, [&](float, float) { return LayerColor{c.role == 0 ? 40.f : 176.f, c.role == 0 ? 60.f : 110.f, c.role == 0 ? 170.f : 30.f, 0.92f * a}; });
    g_vl.text(role, true, be, fx + 2.5f, y + h - 1.8f, LayerColor{255, 255, 255, a});
    // text block next to the strip
    const float tx0 = left ? fx + fw + 6.f : x0 + 8.f, tx1 = left ? x1 - 8.f : fx - 6.f;
    const float ne = std::min(10.5f, h * 0.36f), base1 = y + h * 0.43f;
    const std::string lv = c.level ? "Lv " + std::to_string(c.level) : std::string();
    const std::string nm = fit_name(c.ch);
    const float lvw = lv.empty() ? 0.f : tw(lv, ne * 0.75f) + 6.f;
    if (left) { if (!lv.empty()) label(lv, ne * 0.75f, tx0, base1, LayerColor{225, 230, 255, a}); label(nm, ne, tx0 + lvw, base1, LayerColor{255, 255, 255, a}, tx1 - tx0 - lvw); }
    else { if (!lv.empty()) label(lv, ne * 0.75f, tx1, base1, LayerColor{225, 230, 255, a}, 1e9f, false, true); label(nm, ne, tx1 - lvw, base1, LayerColor{255, 255, 255, a}, tx1 - tx0 - lvw, false, true); }
    const float tag_h = std::min(11.f, h * 0.34f), tag_y = y + h * 0.58f;
    const std::string who = c.who.empty() ? (c.cpu ? "CPU" : "Player") : c.who;
    const float we = tag_h * 0.62f, ww = tw(who, we);
    const float jw = 56.f;
    if (left) { job_tag(c.job, tx0, tag_y, jw, tag_h, true, a); label(who, we, tx0 + jw + 8.f, tag_y + tag_h * 0.78f, c.cpu ? LayerColor{205, 210, 240, a} : LayerColor{255, 226, 120, a}, tx1 - tx0 - jw - 8.f); }
    else { job_tag(c.job, tx1 - jw, tag_y, jw, tag_h, false, a); label(who, we, tx1 - jw - 8.f, tag_y + tag_h * 0.78f, c.cpu ? LayerColor{205, 210, 240, a} : LayerColor{255, 226, 120, a}, tx1 - tx0 - jw - 8.f, false, true); }
    (void)ww;
}
void chips(const std::vector<VsCard>& rs, int t, float x0, float x1, float y, float h, float a) {   // the reserve, in order
    const bool left = t == 0; if (rs.empty()) return;
    const float cap = (x1 - x0 - 34.f) / float(rs.size());
    float x = left ? x0 + 34.f : x1 - 34.f;
    label("RESERVE", 4.6f, left ? x0 : x1, y + h * 0.72f, LayerColor{232, 236, 255, a}, 32.f, false, !left);
    for (size_t i = 0; i < rs.size(); i++) {
        const float cw = std::min(cap - 3.f, 56.f), cx0 = left ? x : x - cw, cx1 = left ? x + cw : x;
        g_vl.band(cx0, y, cx1, y + h, [&](float xx, float yy) { const float e = std::min({xx - cx0, cx1 - xx, yy - y, y + h - yy}); const float v = (yy - y) / h; return e < 0.8f ? LayerColor{150, 156, 214, a} : LayerColor{(left ? 34.f : 80.f) + 22 * v, (left ? 38.f : 36.f) + 18 * v, (left ? 104.f : 84.f) + 26 * v, 0.95f * a}; });
        const std::string s = std::to_string(i + 1) + " " + fit_name(rs[i].ch);
        g_vl.text(s, true, std::min(5.6f, tw(s, 5.6f) > cw - 4.f ? 5.6f * (cw - 4.f) / tw(s, 5.6f) : 5.6f), cx0 + 2.f, y + h * 0.74f, LayerColor{240, 243, 255, a});
        x += left ? cap : -cap;
    }
}
void ring(float cx, float cy, float a, LayerColor c) {   // a gold ring round an orb
    const float s = g_vl.S();
    g_vl.note(7); g_vl.notef(cx); g_vl.notef(cy); g_vl.notef(a); g_vl.notef(c.r); g_vl.notef(c.g); g_vl.notef(c.b); g_vl.notef(c.a); g_vl.notef(s);
    if (!g_vl.want_raster()) return;
    const int x0 = int(g_vl.PX(cx - 12.f)), x1 = int(g_vl.PX(cx + 12.f)), y0 = int(g_vl.PY(cy - 12.f)), y1 = int(g_vl.PY(cy + 12.f));
    const float R = 9.6f * s, T = 1.3f * s;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float dx = x + 0.5f - g_vl.PX(cx), dy = y + 0.5f - g_vl.PY(cy), d = std::sqrt(dx * dx + dy * dy), m = std::clamp(1.f - std::fabs(d - R) / T, 0.f, 1.f);
        if (m > 0.f) g_vl.blend(x, y, c.r, c.g, c.b, c.a * m * a);
    }
}
void orb_rings(const std::vector<VsCard>& party, bool top, float a) {
    const int n = int(party.size()); if (n < 1) return;
    for (const VsCard& c : party) {
        if (c.role > 1) continue;
        const float cx = top ? 189.f + 24.f * float(c.party_idx) : 290.f - 24.f * float(c.party_idx), cy = top ? 32.f : 214.f;
        ring(cx, cy, a, c.role == 0 ? LayerColor{110, 200, 255, 1.f} : LayerColor{255, 214, 90, 1.f});
    }
}
void panel_ground(float a) {   // the framed panel, repainted
    const float x0 = 14.f, x1 = 466.f, y0 = 79.f, y1 = 154.f;
    g_vl.band(x0, y0, x1, y1, [&](float x, float y) {
        const float e = std::min({x - x0, x1 - x, y - y0, y1 - y}), v = (y - y0) / (y1 - y0);
        if (e < 1.f) return LayerColor{150, 146, 214, a};
        if (e < 2.f) return LayerColor{88, 84, 164, a};
        return LayerColor{128 - 8 * v, 126 - 8 * v, 178 - 6 * v, a};
    });
}
void team_screen(const VsLineup& L, float a) {
    panel_ground(a);
    const float xl0 = 20.f, xl1 = 232.f, xr0 = 248.f, xr1 = 460.f;
    for (int t = 0; t < 2; t++) {
        std::vector<VsCard> fld, res;
        for (const VsCard& c : L.team[t]) (c.role <= 1 ? fld : res).push_back(c);
        const float x0 = t == 0 ? xl0 : xr0, x1 = t == 0 ? xl1 : xr1;
        const float ytop = 83.f, ybot = res.empty() ? 150.f : 133.f;
        const int nf = std::max(1, int(fld.size())); const float gap = 2.f, h = std::min(36.f, (ybot - ytop - gap * float(nf - 1)) / float(nf));
        const float y_start = ytop + (ybot - ytop - (h * float(nf) + gap * float(nf - 1))) / 2.f;
        for (int i = 0; i < int(fld.size()); i++) card_row(fld[size_t(i)], t, x0, x1, y_start + (h + gap) * float(i), h, a);
        chips(res, t, x0, x1, 136.f, 14.f, a);
    }
    // VS
    const float e = 17.f, w = tw("VS", e);
    for (const auto& o : {std::pair<float, float>{1.f, 1.f}, {-0.6f, 0.8f}}) g_vl.text("VS", true, e, 240.f - w / 2.f + o.first, 121.f + o.second, LayerColor{26, 24, 84, 0.7f * a});
    g_vl.text("VS", true, e, 240.f - w / 2.f, 121.f, LayerColor{255, 250, 230, a});
    // Assist caption (retail's "Assist" strips are part of the covered panel)
    const std::string as = L.assist ? "Assists: ON (partner call-ins)" : "Assists: OFF";
    label(as, 5.6f, 240.f - tw(as, 5.6f) / 2.f, 176.f, LayerColor{228, 232, 255, 0.9f * a});
    orb_rings(L.party[0], true, a); orb_rings(L.party[1], false, a);
}
void band_cover(bool top, const char* title, float a) {   // retail's two band titles + orb rows are meaningless without parties: repainted
    const float y0 = top ? 15.f : 197.f, y1 = top ? 44.f : 225.f;
    if (top) g_vl.band(0.f, y0, 400.f, y1, [&](float x, float y) { const float u = std::min(x / 330.f, 1.f); const float e = std::min({y - y0, y1 - y}); return LayerColor{26 + 90 * u, 34 + 90 * u, 140 + 56 * u, a * std::clamp(e, 0.f, 1.f) * (x < 330.f ? 1.f : std::max(0.f, 1.f - (x - 330.f) / 70.f))}; });
    else g_vl.band(60.f, y0, 480.f, y1, [&](float x, float y) { const float u = std::clamp((x - 60.f) / 300.f, 0.f, 1.f); const float e = std::min({y - y0, y1 - y}); return LayerColor{210 - 80 * u, 130 - 90 * u, 170 - 80 * u, a * std::clamp(e, 0.f, 1.f) * (x < 100.f ? (x - 60.f) / 40.f : 1.f)}; });
    if (top) label(title, 7.4f, 70.f, (y0 + y1) / 2.f + 3.f, LayerColor{255, 255, 255, a});
    else label(title, 7.4f, 470.f, (y0 + y1) / 2.f + 3.f, LayerColor{255, 255, 255, a}, 1e9f, false, true);
}
void ffa_screen(const VsLineup& L, float a) {
    panel_ground(a);
    const int n = std::max(1, std::min(4, int(L.ffa_list.size())));
    const float gap = 6.f, cw = (440.f - gap * float(n - 1)) / float(n);
    for (int i = 0; i < n; i++) {
        const VsCard& c = L.ffa_list[size_t(i)]; const float x0 = 20.f + (cw + gap) * float(i), x1 = x0 + cw;
        static const LayerColor pc[4] = {{50, 90, 220, 1.f}, {214, 60, 66, 1.f}, {70, 160, 90, 1.f}, {214, 170, 50, 1.f}};
        g_vl.band(x0, 83.f, x1, 150.f, [&](float, float y) { const float v = (y - 83.f) / 67.f; return LayerColor{pc[i].r * (0.45f + 0.2f * (1 - v)), pc[i].g * (0.45f + 0.2f * (1 - v)), pc[i].b * (0.45f + 0.2f * (1 - v)), 0.92f * a}; });
        g_vl.band(x0, 83.f, x0 + 14.f, 97.f, [&](float, float) { return LayerColor{pc[i].r, pc[i].g, pc[i].b, a}; });
        g_vl.text("P" + std::to_string(i + 1), true, 7.f, x0 + 2.f, 94.f, LayerColor{255, 255, 255, a});
        face_strip(c.ch, x0 + 14.f, 83.f, cw - 14.f, 20.f, false, a);
        label(fit_name(c.ch), 9.2f, x0 + 5.f, 118.f, LayerColor{255, 255, 255, a}, cw - 10.f);
        const std::string lv = c.level ? "Lv " + std::to_string(c.level) : std::string();
        if (!lv.empty()) label(lv, 6.6f, x0 + 5.f, 130.f, LayerColor{225, 230, 255, a});
        const std::string who = c.who.empty() ? (c.cpu ? "CPU" : "Player") : c.who;
        label(who, 6.4f, x1 - 5.f, 130.f, c.cpu ? LayerColor{205, 210, 240, a} : LayerColor{255, 226, 120, a}, cw * 0.55f, false, true);
        job_tag(c.job, x0 + 5.f, 135.f, std::min(60.f, cw - 10.f), 11.f, true, a);
    }
    band_cover(true, "Free-for-all", a);
    band_cover(false, "Every fighter for himself", a);
}
}  // namespace

void dissidia_tagteam_vs_draw() {
    static const bool off = std::getenv("PSPRECOMP_TAGTEAM_VSPAGE_OLD") != nullptr; if (off) return;
    VsLineup L; if (!dissidia_tagteam_vs_lineup(&L)) return;
    const double t = now_s(); if (g_last < 0 || t - g_last > 0.4) g_t0 = t; g_last = t;
    const float a = float(std::clamp((t - g_t0 - 0.45) / 0.35, 0.0, 1.0));   // retail's bands slide in first
    if (a <= 0.f) return;
    g_vl.begin(std::max(2, psp_sddialog::output_scale()));
    if (L.ffa && !L.ffa_list.empty()) ffa_screen(L, a); else team_screen(L, a);
    ge_draw_set_game_layer_ttl(200);
    g_vl.publish();
}
