// The animated settings screen (SettingsView, see psp_savedata_dialog.h). Included at the end of psp_savedata_dialog.cpp so it can
// use that file's painter, fonts and theme; nothing here is used by anything else.
//
// Look: the game's menu language. Lavender filigree background, silver chevron bars, a royal-blue selection bar with a gold marker, navy
// header tabs and hint bar, gold corner ornaments, Cinzel headings. When a game theme is installed its background, button glyphs, fonts
// and corner ornaments are used; the rest is drawn procedurally so it scales to any row height.

namespace {

const SetRect R_BACK{392, 8, 78, 20};
const SetRect R_SIDE{10, 43, 108, 201};
const SetRect R_LIST{126, 43, 344, 144};
const SetRect R_DETAIL{126, 192, 344, 52};
// without the category sidebar (SetScene::hide_side) the list and the description panel take its place
const SetRect R_LIST_W{10, 43, 460, 144};
const SetRect R_DETAIL_W{10, 192, 460, 52};
const SetRect R_FOOT{10, 249, 460, 21};
constexpr float ROW_PITCH = 18.5f, HEAD_H = 15.f, LIST_PAD = 3.f;

const Col C_INK = rgb(30, 38, 150), C_DIM = rgb(104, 112, 172), C_NAVY = rgb(26, 28, 88), C_RED = rgb(176, 44, 70);
const Col C_SILVER = rgb(206, 210, 234), C_WHITE = rgb(255, 255, 255);

inline float ease_out(float t) { t = clamp01(t); return 1.f - (1.f - t) * (1.f - t) * (1.f - t); }
inline float ease_io(float t) { t = clamp01(t); return t * t * (3.f - 2.f * t); }
inline float seg(float p, float lo, float hi) { return ease_out((p - lo) / (hi - lo)); }
inline Col with_a(Col c, float a) { c.a *= a; return c; }
inline Col col_mix(Col a, Col b, float t) { return mix(a, b, clamp01(t)); }

// Category icons, drawn on a 12x12 unit cell centred on (cx, cy).
void draw_icon(Painter& P, int icon, float cx, float cy, Col k) {
    const float t = 0.8f;
    switch (icon) {
    case 0:   // display: a screen on a stand
        P.rrect(cx - 5.5f, cy - 4.5f, 11.f, 7.5f, 1.2f, k);
        P.rrect(cx - 4.6f, cy - 3.6f, 9.2f, 5.7f, 0.8f, Col{k.r * 0.2f + 0.7f, k.g * 0.2f + 0.7f, k.b * 0.1f + 0.85f, 0.f});
        P.segment(cx, cy + 3.f, cx, cy + 5.f, t, k);
        P.segment(cx - 3.f, cy + 5.2f, cx + 3.f, cy + 5.2f, t, k);
        break;
    case 1: {   // graphics: a cut crystal
        Poly d; d.add(cx, cy - 6.f); d.add(cx + 5.f, cy - 1.5f); d.add(cx, cy + 6.f); d.add(cx - 5.f, cy - 1.5f);
        P.poly(d, 0.f, [&](float, float) { return k; });
        const Col hi = with_a(C_WHITE, 0.75f);
        P.segment(cx - 5.f, cy - 1.5f, cx + 5.f, cy - 1.5f, 0.45f, hi);
        P.segment(cx - 1.8f, cy - 1.5f, cx, cy - 6.f, 0.45f, hi);
        P.segment(cx + 1.8f, cy - 1.5f, cx, cy - 6.f, 0.45f, hi);
        P.segment(cx - 1.8f, cy - 1.5f, cx, cy + 6.f, 0.45f, hi);
        P.segment(cx + 1.8f, cy - 1.5f, cx, cy + 6.f, 0.45f, hi);
        break;
    }
    case 2:   // image: a sun
        P.ring(cx, cy, 2.6f, 1.5f, k);
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * 0.7853982f;
            P.segment(cx + std::cos(a) * 4.4f, cy + std::sin(a) * 4.4f, cx + std::cos(a) * 6.f, cy + std::sin(a) * 6.f, 0.55f, k);
        }
        break;
    case 3: {   // effects: a four-point sparkle
        Poly s;
        s.add(cx, cy - 6.5f); s.add(cx + 1.5f, cy - 1.5f); s.add(cx + 6.5f, cy); s.add(cx + 1.5f, cy + 1.5f);
        s.add(cx, cy + 6.5f); s.add(cx - 1.5f, cy + 1.5f); s.add(cx - 6.5f, cy); s.add(cx - 1.5f, cy - 1.5f);
        P.poly(s, 0.f, [&](float, float) { return k; });
        break;
    }
    case 4:   // controls: a pad
        P.rrect(cx - 6.f, cy - 3.2f, 12.f, 7.f, 3.2f, k);
        P.segment(cx - 3.6f, cy - 0.2f, cx - 1.2f, cy - 0.2f, 0.5f, C_WHITE);
        P.segment(cx - 2.4f, cy - 1.4f, cx - 2.4f, cy + 1.f, 0.5f, C_WHITE);
        P.ring(cx + 2.4f, cy - 1.f, 0.5f, 0.9f, C_WHITE);
        P.ring(cx + 4.2f, cy + 0.6f, 0.5f, 0.9f, C_WHITE);
        break;
    case 5:   // network: a globe
        P.ring(cx, cy, 5.2f, 1.f, k);
        P.ring(cx, cy, 2.3f, 0.8f, Col{k.r, k.g, k.b, k.a * 0.9f});
        P.segment(cx - 5.2f, cy, cx + 5.2f, cy, 0.4f, k);
        P.segment(cx, cy - 5.2f, cx, cy + 5.2f, 0.4f, k);
        break;
    case 6:   // cloud
        P.rrect(cx - 6.f, cy - 0.5f, 12.f, 5.f, 2.5f, k);
        P.rrect(cx - 3.6f, cy - 4.2f, 6.f, 6.f, 3.f, k);
        P.rrect(cx + 0.4f, cy - 3.f, 5.f, 5.f, 2.5f, k);
        break;
    case 7:   // system: a gear
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * 0.7853982f;
            P.segment(cx + std::cos(a) * 4.4f, cy + std::sin(a) * 4.4f, cx + std::cos(a) * 6.2f, cy + std::sin(a) * 6.2f, 1.1f, k);
        }
        P.ring(cx, cy, 3.6f, 2.1f, k);
        break;
    case 8:   // drivers: a chip
        P.rrect(cx - 3.8f, cy - 3.8f, 7.6f, 7.6f, 1.2f, k);
        for (int i = -1; i <= 1; ++i) {
            P.segment(cx + float(i) * 2.6f, cy - 6.f, cx + float(i) * 2.6f, cy - 3.8f, 0.45f, k);
            P.segment(cx + float(i) * 2.6f, cy + 3.8f, cx + float(i) * 2.6f, cy + 6.f, 0.45f, k);
            P.segment(cx - 6.f, cy + float(i) * 2.6f, cx - 3.8f, cy + float(i) * 2.6f, 0.45f, k);
            P.segment(cx + 3.8f, cy + float(i) * 2.6f, cx + 6.f, cy + float(i) * 2.6f, 0.45f, k);
        }
        break;
    case 10:  // mods: building blocks, one lifted out
        P.rrect(cx - 6.f, cy - 1.f, 5.6f, 5.6f, 0.9f, k);
        P.rrect(cx - 0.2f, cy - 1.f, 5.6f, 5.6f, 0.9f, k);
        P.rrect(cx - 6.f, cy + 4.9f - 0.6f, 5.6f, 2.2f, 0.9f, Col{k.r, k.g, k.b, k.a * 0.55f});
        P.rrect(cx - 3.2f, cy - 6.6f, 5.6f, 5.0f, 0.9f, k);
        break;
    default:  // debug: a bug
        P.rrect(cx - 2.8f, cy - 3.2f, 5.6f, 8.f, 2.8f, k);
        P.rrect(cx - 1.8f, cy - 5.6f, 3.6f, 3.f, 1.6f, k);
        for (int i = -1; i <= 1; ++i) {
            P.segment(cx - 2.8f, cy + float(i) * 2.6f + 0.6f, cx - 5.8f, cy + float(i) * 3.2f + 0.6f, 0.45f, k);
            P.segment(cx + 2.8f, cy + float(i) * 2.6f + 0.6f, cx + 5.8f, cy + float(i) * 3.2f + 0.6f, 0.45f, k);
        }
        break;
    }
}

// A chevron-ended bar (the game's bars have pointed ends): an analytic distance field, much cheaper per pixel than a general polygon.
struct ChevSd {
    float cx, cy, hw, hh, tip, len;
    float operator()(float px, float py) const {
        const float ax = std::fabs(px - cx), ay = std::fabs(py - cy);
        return std::max(ay - hh, ((ax - hw) * hh + ay * tip) / len);
    }
};
inline ChevSd chev_sd(float x, float y, float w, float h, float tip, float s) {
    ChevSd d{(x + w / 2.f) * s, (y + h / 2.f) * s, w / 2.f * s, h / 2.f * s, tip * s, 0.f};
    d.len = std::sqrt(d.hh * d.hh + d.tip * d.tip);
    return d;
}
template <class COL>
void chev(Painter& P, float x, float y, float w, float h, float tip, float grow, COL col) {
    const ChevSd sd = chev_sd(x, y, w, h, tip, P.s);
    const float g = grow * P.s;
    P.shape(x - grow, y - grow, x + w + grow, y + h + grow, [&](float px, float py) { return sd(px, py) - g; }, col);
}

// A small diamond (the game's crystal marker).
void diamond(Painter& P, float cx, float cy, float r, Col k) {
    Poly d; d.add(cx, cy - r); d.add(cx + r * 0.75f, cy); d.add(cx, cy + r); d.add(cx - r * 0.75f, cy);
    P.poly(d, 0.f, [&](float, float) { return k; });
}
// A chevron arrow (">") of two strokes.
void chevron_mark(Painter& P, float cx, float cy, float h, float th, bool right, Col k) {
    const float w = h * 0.55f, d = right ? 1.f : -1.f;
    P.segment(cx - d * w * 0.5f, cy - h, cx + d * w * 0.5f, cy, th, k);
    P.segment(cx + d * w * 0.5f, cy, cx - d * w * 0.5f, cy + h, th, k);
}
// Gold corner ticks around a panel (when the theme has no corner sprites).
void corner_ticks(Painter& P, const SetRect& r, float o, Col k) {
    const float l = 7.f, th = 0.75f;
    const float x0 = r.x - o, y0 = r.y - o, x1 = r.x + r.w + o, y1 = r.y + r.h + o;
    P.segment(x0, y0, x0 + l, y0, th, k); P.segment(x0, y0, x0, y0 + l, th, k);
    P.segment(x1, y0, x1 - l, y0, th, k); P.segment(x1, y0, x1, y0 + l, th, k);
    P.segment(x0, y1, x0 + l, y1, th, k); P.segment(x0, y1, x0, y1 - l, th, k);
    P.segment(x1, y1, x1 - l, y1, th, k); P.segment(x1, y1, x1, y1 - l, th, k);
    diamond(P, x0, y0, 1.7f, k); diamond(P, x1, y0, 1.7f, k); diamond(P, x0, y1, 1.7f, k); diamond(P, x1, y1, 1.7f, k);
}
// A frosted panel: soft shadow, silver rim, translucent white body with a faint vertical gradient.
void glass_panel(Painter& P, const SetRect& r, float radius, float strength = 1.f) {
    P.shadow(r.x, r.y + 1.f, r.w, r.h, radius, 4.f, 0.20f * strength);
    P.rrect(r.x - 0.9f, r.y - 0.9f, r.w + 1.8f, r.h + 1.8f, radius + 0.9f, with_a(rgb(168, 176, 218), 0.95f * strength));
    P.rrect(r.x, r.y, r.w, r.h, radius, Ramp(rgb(255, 255, 255, 0.70f * strength), rgb(236, 238, 252, 0.60f * strength), r.y, r.y + r.h));
    P.hline(r.x + radius, r.x + r.w - radius, r.y + 0.6f, 0.9f, rgb(255, 255, 255, 0.9f * strength));
}
// A navy chevron plate (header tab / hint bar / button).
void navy_plate(Painter& P, float x, float y, float w, float h, float tip, float lift = 0.f, float alpha = 1.f) {
    chev(P, x, y, w, h, tip, 0.9f, [=](float, float) { return rgb(214, 218, 240, alpha); });
    Ramp r(col_mix(rgb(86, 90, 168), C_WHITE, lift), col_mix(rgb(44, 46, 122), C_WHITE, lift * 0.6f), col_mix(rgb(24, 26, 84), C_WHITE, lift * 0.3f), y, y + h);
    chev(P, x, y, w, h, tip, 0.f, [&](float yy, float) { return with_a(r.at(yy), alpha); });
}


// ---- cached panel layers ----------------------------------------------------------------------------------------------
// The frosted panels (shadow, rim, translucent body) are the most expensive shapes on the screen and never change, so each is drawn
// once per canvas scale into a premultiplied RGBA image and composited every frame (at any offset and opacity: the open animation
// slides them in). The alpha is recovered by drawing the panel over black and over white.
const SetRect R_KB{44, 46, 392, 198};
struct PanelImg { int x0 = 0, y0 = 0, w = 0, h = 0; std::vector<uint32_t> px; };
struct PanelCache {
    bool built[MAX_SCALE + 1] = {};
    PanelImg img[MAX_SCALE + 1][6];
};
void build_panel_img(PanelImg& out, const SetRect& r, float radius, float strength, int scale) {
    Canvas cb(scale), cw(scale);
    cb.clear(Canvas::rgba(0, 0, 0));
    cw.clear(Canvas::rgba(255, 255, 255));
    { Painter p(cb); glass_panel(p, r, radius, strength); }
    { Painter p(cw); glass_panel(p, r, radius, strength); }
    const float m = 10.f, s = float(scale);
    const int x0 = std::max(0, int(std::floor((r.x - m) * s))), y0 = std::max(0, int(std::floor((r.y - m) * s)));
    const int x1 = std::min(cb.w, int(std::ceil((r.x + r.w + m) * s))), y1 = std::min(cb.h, int(std::ceil((r.y + r.h + m) * s)));
    out.x0 = x0; out.y0 = y0; out.w = x1 - x0; out.h = y1 - y0;
    out.px.assign(size_t(out.w) * size_t(out.h), 0);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const uint32_t b = cb.px[size_t(y + y0) * size_t(cb.w) + size_t(x + x0)], w = cw.px[size_t(y + y0) * size_t(cw.w) + size_t(x + x0)];
            const int a = std::clamp(255 - (int((w >> 8) & 255) - int((b >> 8) & 255)), 0, 255);
            out.px[size_t(y) * size_t(out.w) + size_t(x)] = (b & 0x00FFFFFFu) | (uint32_t(a) << 24);
        }
}
const PanelImg& panel_img(int kind, int scale) {
    static PanelCache pc;
    scale = std::clamp(scale, 1, MAX_SCALE);
    if (!pc.built[scale]) {   // once per canvas scale (the scale changes only when the output size does, or drawing is slow)
        pc.built[scale] = true;
        build_panel_img(pc.img[scale][0], R_SIDE, 6.f, 0.92f, scale);
        build_panel_img(pc.img[scale][1], R_LIST, 6.f, 1.f, scale);
        build_panel_img(pc.img[scale][2], R_DETAIL, 6.f, 1.f, scale);
        build_panel_img(pc.img[scale][3], R_KB, 7.f, 1.f, scale);
        build_panel_img(pc.img[scale][4], R_LIST_W, 6.f, 1.f, scale);
        build_panel_img(pc.img[scale][5], R_DETAIL_W, 6.f, 1.f, scale);
    }
    return pc.img[scale][kind];
}
// Composites a cached panel moved by (dx, dy) pixels with opacity `ga`.
void blit_panel(Canvas& c, const PanelImg& im, int dx, int dy, float ga) {
    const int g256 = int(clamp01(ga) * 256.f + 0.5f);
    if (g256 <= 0) return;
    for (int y = 0; y < im.h; ++y) {
        const int yy = im.y0 + y + dy;
        if (yy < 0 || yy >= c.h || yy < tl_band_y0 || yy >= tl_band_y1) continue;
        const uint32_t* src = &im.px[size_t(y) * size_t(im.w)];
        for (int x = 0; x < im.w; ++x) {
            const uint32_t sp = src[x];
            unsigned a = sp >> 24;
            if (!a) continue;
            const int xx = im.x0 + x + dx;
            if (xx < 0 || xx >= c.w) continue;
            uint32_t& d = c.px[size_t(yy) * size_t(c.w) + size_t(xx)];
            a = (a * unsigned(g256)) >> 8;
            const unsigned ia = 255u - std::min(255u, a);
            const unsigned r = (((sp & 255u) * unsigned(g256)) >> 8) + (((d & 255u) * ia + 127u) / 255u);
            const unsigned g = ((((sp >> 8) & 255u) * unsigned(g256)) >> 8) + ((((d >> 8) & 255u) * ia + 127u) / 255u);
            const unsigned b = ((((sp >> 16) & 255u) * unsigned(g256)) >> 8) + ((((d >> 16) & 255u) * ia + 127u) / 255u);
            d = 0xFF000000u | (std::min(255u, b) << 16) | (std::min(255u, g) << 8) | std::min(255u, r);
        }
    }
}

}  // namespace

std::vector<std::vector<SetKey>> settings_keyboard(bool shift, bool symbols) {
    static const char* const L0 = "1234567890";
    static const char* const L1[2] = {"qwertyuiop", "QWERTYUIOP"};
    static const char* const L2[2] = {"asdfghjkl-", "ASDFGHJKL_"};
    static const char* const L3[2] = {"zxcvbnm.:/", "ZXCVBNM,;\\"};
    static const char* const S1 = "@#$%&*()[]";
    static const char* const S2 = ":;,.?!=+-_";
    static const char* const S3 = "/\\~^<>|\"'`";
    const char* rows[4] = {L0, symbols ? S1 : L1[shift], symbols ? S2 : L2[shift], symbols ? S3 : L3[shift]};
    std::vector<std::vector<SetKey>> out;
    for (int r = 0; r < 4; ++r) {
        std::vector<SetKey> row;
        for (int i = 0; i < 10; ++i) {
            SetKey k;
            k.ch = rows[r][i];
            k.label = std::string(1, k.ch);
            row.push_back(k);
        }
        out.push_back(std::move(row));
    }
    std::vector<SetKey> bot(6);
    bot[0].label = symbols ? "abc" : (shift ? "SHIFT" : "Shift"); bot[0].kind = 1;
    bot[1].label = symbols ? "ABC" : "#+="; bot[1].kind = 2;
    bot[2].label = "Space"; bot[2].kind = 3; bot[2].ch = ' ';
    bot[3].label = "Delete"; bot[3].kind = 4;
    bot[4].label = "Done"; bot[4].kind = 5;
    bot[5].label = "Cancel"; bot[5].kind = 6;
    out.push_back(std::move(bot));
    return out;
}

void SettingsView::set_open(bool open) { open_ = open; }
void SettingsView::page_changed(int dir) { page_dir_ = dir; page_p_ = 0.f; rows_reset_ = true; }
void SettingsView::bump(int id) { if (id >= 0 && id < SET_MAX_IDS) pulse_[id] = 1.f; }

namespace {
struct SettingsDraw {
    SettingsView& V;
    const SetScene& S;
    Canvas& c;
    Painter P;
    const SdTheme* T = nullptr;
    std::shared_ptr<const SdTheme> keep;
    FT_Face body = nullptr, bodyb = nullptr, cinzel = nullptr;
    float fs = 1.f;
    float e_hdr = 1, e_side = 1, e_list = 1, e_detail = 1, e_foot = 1;
    SetRect side, list, detail, foot, back;
    float list_scroll = 0;
    std::vector<float> ry, rh;
    float content_h = 0;

    SettingsDraw(SettingsView& v, const SetScene& s, Canvas& cv) : V(v), S(s), c(cv), P(cv) {
        const Fonts& F = fonts();
        keep = theme();
        T = keep.get();
        cinzel = F.cinzel ? F.cinzel : F.bodyb;
        body = F.body; bodyb = F.bodyb ? F.bodyb : F.body;
        if (T) {
            ThemeFaces& TF = theme_faces(*T);
            body = TF.reg ? TF.reg : F.body;
            bodyb = TF.bold ? TF.bold : (TF.reg ? TF.reg : F.bodyb);
            fs = T->font_scale;
        }
    }

    // -- layout ------------------------------------------------------------------------------------------------------
    void compute_rows() {
        ry.clear(); rh.clear();
        float y = LIST_PAD;
        for (const SetRow& r : S.rows) {
            const float h = r.kind == SetKind::Header ? HEAD_H : ROW_PITCH;
            ry.push_back(y); rh.push_back(h);
            y += h;
        }
        content_h = y + LIST_PAD;
    }
    static SetRect shifted(const SetRect& r, float dx, float dy) { return {r.x + dx, r.y + dy, r.w, r.h}; }

    // -- state that moves ----------------------------------------------------------------------------------------------
    void step(float dt) {
        SettingsView& v = V;
        const float open_rate = v.open_ ? 1.f / 1.20f : 1.f / 0.70f;
        v.p_open_ = clamp01(v.p_open_ + (v.open_ ? 1.f : -1.f) * dt * open_rate);
        // the iris opens first (p 0 .. 0.4); only then do the panels slide in one after the other (p 0.4 .. 1)
        const float p = clamp01((v.p_open_ - 0.40f) / 0.60f);
        e_hdr = seg(p, 0.00f, 0.55f); e_side = seg(p, 0.10f, 0.65f); e_list = seg(p, 0.20f, 0.80f);
        e_detail = seg(p, 0.30f, 0.90f); e_foot = seg(p, 0.35f, 1.00f);
        v.page_p_ = clamp01(v.page_p_ + dt / 0.45f);
        compute_rows();
        const float list_h = R_LIST.h;   // (same height without the sidebar)
        v.scroll_max_ = std::max(0.f, content_h - list_h);
        const int n = int(S.rows.size());
        const int sel = std::clamp(S.sel, 0, std::max(0, n - 1));
        // scroll target
        if (v.rows_reset_ || !v.glide_init_) {
            v.manual_scroll_ = false;
        }
        if (!v.manual_scroll_ && n > 0) {
            float want_top = ry[size_t(sel)], bot = ry[size_t(sel)] + rh[size_t(sel)];
            if (sel > 0 && S.rows[size_t(sel) - 1].kind == SetKind::Header) want_top = ry[size_t(sel) - 1];
            float t = v.scroll_target_;
            if (want_top - 2.f < t) t = want_top - 2.f;
            if (bot + 2.f > t + list_h) t = bot + 2.f - list_h;
            bool first_sel = true;
            for (int i = 0; i < sel; ++i) if (S.rows[size_t(i)].selectable()) first_sel = false;
            if (first_sel) t = 0.f;
            v.scroll_target_ = t;
        }
        if (std::fabs(v.fling_v_) > 2.f) {
            v.scroll_target_ += v.fling_v_ * dt;
            v.fling_v_ *= std::exp(-dt * 3.2f);
            if (v.scroll_target_ < 0.f || v.scroll_target_ > v.scroll_max_) v.fling_v_ = 0.f;
        } else v.fling_v_ = 0.f;
        v.scroll_target_ = std::clamp(v.scroll_target_, 0.f, v.scroll_max_);
        if (v.rows_reset_ || !v.glide_init_) v.scroll_ = v.scroll_target_;
        else {
            v.scroll_ += (v.scroll_target_ - v.scroll_) * (1.f - std::exp(-dt * 16.f));
            if (std::fabs(v.scroll_target_ - v.scroll_) < 0.05f) v.scroll_ = v.scroll_target_;
        }
        // highlight glide (content space) and category glide
        const float hl = n > 0 ? ry[size_t(sel)] : 0.f;
        if (v.rows_reset_ || !v.glide_init_) v.sel_y_ = hl;
        else v.sel_y_ += (hl - v.sel_y_) * (1.f - std::exp(-dt * 22.f));
        {   // the category list scrolls like the rows when there are more categories than fit
            const float cp = 24.f, vis = R_SIDE.h - 8.f;
            v.cat_scroll_max_ = std::max(0.f, float(S.cats.size()) * cp - vis);
            if (S.cat != v.cat_follow_) { v.cat_follow_ = S.cat; v.cat_manual_ = false; }
            if (!v.cat_manual_) {
                float t = v.cat_scroll_t_;
                const float top = float(S.cat) * cp, bot = top + cp;
                if (top - 2.f < t) t = top - 2.f;
                if (bot + 2.f > t + vis) t = bot + 2.f - vis;
                v.cat_scroll_t_ = t;
            }
            v.cat_scroll_t_ = std::clamp(v.cat_scroll_t_, 0.f, v.cat_scroll_max_);
            if (!v.glide_init_) v.cat_scroll_ = v.cat_scroll_t_;
            else v.cat_scroll_ += (v.cat_scroll_t_ - v.cat_scroll_) * (1.f - std::exp(-dt * 16.f));
        }
        v.translucent_ = clamp01(v.p_open_ / 0.42f) < 0.999f;
        v.dlg_p_ = clamp01(v.dlg_p_ + (S.dlg.active ? 1.f : -1.f) * dt / 0.16f);
        v.tab_focus_ += ((S.focus_tabs ? 1.f : 0.f) - v.tab_focus_) * (1.f - std::exp(-dt * 18.f));
        const float cat_t = float(S.cat);
        if (!v.glide_init_) v.cat_y_ = cat_t;
        else v.cat_y_ += (cat_t - v.cat_y_) * (1.f - std::exp(-dt * 16.f));
        // per-row animation memory
        for (const SetRow& r : S.rows) {
            if (r.id < 0 || r.id >= SET_MAX_IDS || r.kind == SetKind::Header) continue;
            const std::string sig = r.value + (r.on ? "|1|" : "|0|") + std::to_string(r.cur) + "|" + std::to_string(r.steps);
            const float kt = r.on ? 1.f : 0.f;
            const float ft = r.steps > 1 ? float(r.cur) / float(r.steps - 1) : 0.f;
            if (v.rows_reset_ || !v.knob_init_[r.id]) {
                v.knob_[r.id] = kt; v.fill_[r.id] = ft; v.pulse_[r.id] = 0.f; v.knob_init_[r.id] = true;
            } else {
                if (v.last_val_[r.id] != sig) v.pulse_[r.id] = 1.f;
                v.knob_[r.id] += (kt - v.knob_[r.id]) * (1.f - std::exp(-dt * 20.f));
                v.fill_[r.id] += (ft - v.fill_[r.id]) * (1.f - std::exp(-dt * 20.f));
                if (std::fabs(kt - v.knob_[r.id]) < 0.004f) v.knob_[r.id] = kt;
                if (std::fabs(ft - v.fill_[r.id]) < 0.002f) v.fill_[r.id] = ft;
                v.pulse_[r.id] = std::max(0.f, v.pulse_[r.id] - dt * 2.6f);
            }
            v.last_val_[r.id] = sig;
        }
        v.rows_reset_ = false;
        v.glide_init_ = true;
        v.kb_p_ = clamp01(v.kb_p_ + (S.kb.active ? 1.f : -1.f) * dt / 0.18f);
        v.t_ += 0.f;
        list_scroll = v.scroll_;
    }

    // -- background ---------------------------------------------------------------------------------------------------
    void draw_background() {
        if (T && T->background.valid()) {
            // the scaled artwork never changes: scaling it every frame was most of the drawing time (a slow CPU then dropped
            // the picture to a lower, blurry scale)
            static std::vector<uint32_t> cache[MAX_SCALE + 1];
            static const SdSprite* cached_for = nullptr;
            std::vector<uint32_t>& cb = cache[std::clamp(c.scale, 0, int(MAX_SCALE))];
            if (cached_for != &T->background) { for (auto& v : cache) v.clear(); cached_for = &T->background; }
            if (cb.size() != c.px.size()) {
                c.clear(Canvas::rgba(200, 204, 232));
                draw_sprite(c, T->background, 0, 0, float(SCREEN_W), float(SCREEN_H), 1.f, true);
                cb = c.px;
            } else {
                const int y0 = std::max(0, tl_band_y0), y1 = std::min(c.h, tl_band_y1);
                if (y1 > y0) std::memcpy(&c.px[size_t(y0) * size_t(c.w)], &cb[size_t(y0) * size_t(c.w)], size_t(y1 - y0) * size_t(c.w) * 4);
            }
        } else {   // no game theme (a host test): the painted background, built once per scale
            static std::vector<uint32_t> plain[MAX_SCALE + 1];
            std::vector<uint32_t>& pb = plain[std::clamp(c.scale, 0, int(MAX_SCALE))];
            if (pb.size() != c.px.size()) { pb = background(c.scale); c.px = pb; }
            else {
                const int y0 = std::max(0, tl_band_y0), y1 = std::min(c.h, tl_band_y1);
                if (y1 > y0) std::memcpy(&c.px[size_t(y0) * size_t(c.w)], &pb[size_t(y0) * size_t(c.w)], size_t(y1 - y0) * size_t(c.w) * 4);
            }
        }
        const float t = float(V.t_);
        // a slow diagonal sheen crossing the screen (a soft white band, added per pixel)
        {
            const float cyc = 8.f, ph = std::fmod(t, cyc) / cyc;
            if (ph < 0.55f) {
                const float u = ph / 0.55f, x0 = -120.f + u * 720.f, amp = 0.17f * std::sin(u * 3.14159f), hw = 40.f;
                for (int y = 0; y < c.h; ++y) {
                    const float yd = (float(y) + 0.5f) / P.s, cx = x0 - 130.f * yd / float(SCREEN_H);
                    const int xa = std::max(0, int((cx - hw) * P.s)), xb = std::min(c.w, int((cx + hw) * P.s) + 1);
                    for (int x = xa; x < xb; ++x) {
                        const float d = 1.f - std::fabs((float(x) + 0.5f) / P.s - cx) / hw;
                        if (d > 0.f) P.put(x, y, C_WHITE, amp * d * d);
                    }
                }
            }
        }
        // drifting crystal motes
        for (int i = 0; i < 26; ++i) {
            const float h1 = std::fmod(std::sin(float(i) * 12.9898f) * 43758.5453f, 1.f), h2 = std::fmod(std::sin(float(i) * 78.233f) * 12345.678f, 1.f);
            const float a1 = std::fabs(h1), a2 = std::fabs(h2);
            const float speed = 3.f + a2 * 7.f, size = 1.1f + a1 * 2.2f;
            const float x = std::fmod(a1 * 520.f + t * speed * 0.6f + std::sin(t * 0.4f + float(i)) * 6.f, 500.f) - 10.f;
            const float y = std::fmod(a2 * 300.f - t * speed + 600.f, 290.f) - 8.f;
            const float tw = 0.5f + 0.5f * std::sin(t * (1.2f + a1) + float(i) * 1.7f);
            const Col k = (i % 4 == 0) ? rgb(246, 220, 140, 0.55f * tw) : rgb(255, 255, 255, 0.55f * tw);
            diamond(P, x, y, size, k);
        }
    }

    // -- header ----------------------------------------------------------------------------------------------------------
    void draw_header() {
        const float dy = -(1.f - e_hdr) * 22.f;
        P.ga = e_hdr;
        const SetRect bk = shifted(R_BACK, 0, dy);
        back = bk;
        float end = 16.f;
        FT_Face tf = cinzel ? cinzel : bodyb;
        end = themed_title(P, tf, 16.f, 27.f + dy, S.title, rgb(255, 255, 255), rgb(46, 44, 120));
        float x = end + 10.f;
        for (size_t i = 0; i < S.crumbs.size(); ++i) {
            chevron_mark(P, x + 2.f, 22.f + dy, 3.4f, 0.9f, true, rgb(150, 156, 206));
            x += 9.f;
            const bool last = i + 1 == S.crumbs.size();
            x += P.text(bodyb, 10.5f * fs, x, 26.f + dy, S.crumbs[i], last ? C_NAVY : rgb(86, 94, 160), 150.f) + 7.f;
        }
        // silver band under the heading
        for (int row = 0; row < 4; ++row) {
            static const float ay[4] = {0.95f, 0.75f, 0.45f, 0.25f};
            static const uint8_t lum[4] = {250, 214, 168, 128};
            const Col k = rgb(lum[row], lum[row] + (255 - lum[row]) / 4, 255, ay[row]);
            const int xe = SCREEN_W * c.scale;
            const int y0 = int((36.f + dy + float(row)) * float(c.scale)), y1 = int((36.f + dy + float(row) + 1.f) * float(c.scale));
            for (int xx = 0; xx < xe; ++xx) {
                const float fade = 1.f - 0.65f * float(xx) / float(xe);
                for (int yy = y0; yy < y1; ++yy) P.put(xx, yy, k, fade);
            }
        }
        // top right: the project name; inside a sub-menu a Back button
        if (!S.in_sub) {
            FT_Face nf = bodyb;
            const float tw = P.text_width(nf, 9.f * fs, S.brand);
            const float pw = std::max(R_BACK.w, tw + 30.f);
            const SetRect nb = shifted(SetRect{470.f - pw, R_BACK.y, pw, R_BACK.h}, 0, dy);
            back = nb;
            navy_plate(P, nb.x, nb.y, nb.w, nb.h, nb.h * 0.5f, 0.f);
            themed_hint(P, nf, 9.f * fs, nb.x + (nb.w - tw) / 2.f, nb.y + nb.h / 2 + 3.3f, S.brand.c_str(), C_WHITE);
            P.ga = 1.f;
            return;
        }
        const bool hot = S.hover_back;
        navy_plate(P, bk.x, bk.y, bk.w, bk.h, bk.h * 0.5f, hot ? 0.28f : 0.f);
        const char* label = "Back";
        float lx = bk.x + 14.f;
        if (S.device == 0) {   // the pad button that does it
            const SdSprite& g = (S.confirm_cross ? (T ? T->glyph_circle : SdSprite{}) : (T ? T->glyph_cross : SdSprite{}));
            if (false) { (void)g; }
            else { draw_btn_glyph(P, !S.confirm_cross, lx + 5.5f, bk.y + bk.h / 2, C_WHITE); lx += 14.f; }
        } else {   // a cross to tap
            draw_cross(P, lx + 3.f, bk.y + bk.h / 2, 2.4f, 1.2f, C_WHITE);
            lx += 11.f;
        }
        themed_hint(P, bodyb, 9.f * fs, lx, bk.y + bk.h / 2 + 3.3f, label, C_WHITE);
        P.ga = 1.f;
    }

    // -- sidebar ---------------------------------------------------------------------------------------------------------

    // -- sidebar ---------------------------------------------------------------------------------------------------------
    void draw_sidebar() {
        const float dx = -(1.f - e_side) * 44.f;
        P.ga = e_side;
        side = shifted(R_SIDE, dx, 0);
        blit_panel(c, panel_img(0, c.scale), int(std::lround(dx * P.s)), 0, P.ga);
        const int n = int(S.cats.size());
        V.g_.cats.assign(size_t(n), SetRect{});
        if (n == 0) { P.ga = 1.f; return; }
        const float pitch = 24.f;
        const float ix = side.x + 4.f, iw = side.w - 8.f, ih = pitch - 2.5f, y0 = side.y + 4.f - V.cat_scroll_;
        V.g_.side = side;
        P.clip(side.y + 2.f, side.y + side.h - 2.f);
        // the selection bar glides between categories
        const float hy = y0 + V.cat_y_ * pitch;
        {
            const float tf = V.tab_focus_, ga0 = P.ga;
            chev(P, ix, hy, iw, ih, 5.f, 0.9f, [](float, float) { return rgb(214, 218, 240); });
            P.ga = ga0 * (0.5f + 0.5f * tf);   // the category you are inside shows quieter than the one you are choosing
            Ramp rp(rgb(104, 118, 255), rgb(40, 48, 222), rgb(20, 22, 150), hy, hy + ih);
            chev(P, ix, hy, iw, ih, 5.f, 0.f, [&](float yy, float) { return rp.at(yy); });
            P.ga = ga0;
            chev(P, ix + 2.f, hy + 1.f, iw - 4.f, ih * 0.42f, 4.f, 0.f, [](float, float) { return rgb(255, 255, 255, 0.26f); });
            const float pulse = 0.5f + 0.5f * std::sin(float(V.t_) * 3.2f);
            diamond(P, ix + 7.f, hy + ih / 2.f, 2.5f + 0.5f * pulse, rgb(250, 220, 120));
        }
        for (int i = 0; i < n; ++i) {
            const SetRect r{ix, y0 + float(i) * pitch, iw, ih};
            V.g_.cats[size_t(i)] = r;
            const float sf = clamp01(1.f - std::fabs(float(i) - V.cat_y_));
            if (S.hover_cat == i && S.device == 1 && sf < 0.5f) {
                chev(P, r.x, r.y, r.w, r.h, 5.f, 0.f, [](float, float) { return rgb(120, 134, 255, 0.20f); });
            }
            const Col fg = col_mix(C_INK, C_WHITE, sf * (0.7f + 0.3f * V.tab_focus_));
            draw_icon(P, S.cats[size_t(i)].icon, r.x + 17.f, r.y + r.h / 2.f, fg);
            P.text(bodyb, 9.2f * fs, r.x + 28.f, r.y + r.h / 2.f + 3.3f, S.cats[size_t(i)].name, fg, r.w - 32.f);
        }
        P.unclip();
        if (V.cat_scroll_max_ > 0.5f) {   // more categories above / below
            const float bl = 0.5f + 0.5f * std::sin(float(V.t_) * 5.f), cxm = side.x + side.w / 2.f;
            if (V.cat_scroll_ > 1.f) { Poly t; t.add(cxm, side.y + 3.f); t.add(cxm + 4.f, side.y + 8.f); t.add(cxm - 4.f, side.y + 8.f); P.poly(t, 0.f, [&](float, float) { return rgb(226, 160, 40, 0.4f + 0.6f * bl); }); }
            if (V.cat_scroll_ < V.cat_scroll_max_ - 1.f) { Poly t; t.add(cxm, side.y + side.h - 3.f); t.add(cxm + 4.f, side.y + side.h - 8.f); t.add(cxm - 4.f, side.y + side.h - 8.f); P.poly(t, 0.f, [&](float, float) { return rgb(226, 160, 40, 0.4f + 0.6f * bl); }); }
        }
        P.ga = 1.f;
    }

    // -- list ------------------------------------------------------------------------------------------------------------
    // The controls on the right of a row. Fills `gr`'s hit rectangles and returns the x the label must stay left of.
    float draw_controls(const SetRow& r, const SetRect& rr, float sf, Col fg, Col dim, SetGeom::Row& gr, bool hover, int part) {
        const float cy = rr.y + rr.h / 2.f, xr = rr.x + rr.w - 11.f;
        const float vs = 8.8f * fs;
        const float pulse = r.id >= 0 && r.id < SET_MAX_IDS ? V.pulse_[r.id] : 0.f;
        const Col vcol = col_mix(fg, rgb(236, 170, 40), pulse * 0.9f);
        const Col arrow_on = fg, arrow_off = with_a(fg, 0.28f);
        auto arrow = [&](float cx, bool right, bool can, bool hot) {
            if (hot && can) P.rrect(cx - 6.5f, cy - 7.f, 13.f, 14.f, 4.f, with_a(sf > 0.5f ? C_WHITE : rgb(90, 104, 255), 0.22f));
            menu_arrow(P, cx, cy, right, can ? arrow_on : arrow_off);
        };
        switch (r.kind) {
        case SetKind::Toggle: {
            const float pw = 27.f, ph = 12.f, px = xr - pw + 2.f, py = cy - ph / 2.f;
            const float k = r.id >= 0 && r.id < SET_MAX_IDS ? V.knob_[r.id] : (r.on ? 1.f : 0.f);
            if (pulse > 0.f) P.rrect(px, py, pw, ph, ph / 2.f, with_a(rgb(255, 214, 110), 0.55f * pulse), 1.2f + 2.4f * (1.f - pulse));
            const Col off_t = rgb(176, 182, 210), off_b = rgb(140, 146, 184), on_t = rgb(112, 130, 255), on_b = rgb(34, 44, 214);
            P.rrect(px, py, pw, ph, ph / 2.f, with_a(rgb(70, 78, 150), 0.75f), 0.8f);
            P.rrect(px, py, pw, ph, ph / 2.f, Ramp(col_mix(off_t, on_t, k), col_mix(off_b, on_b, k), py, py + ph));
            const float kx = px + 6.f + k * (pw - 12.f);
            P.shadow(kx - 4.6f, cy - 4.2f, 9.2f, 9.2f, 4.6f, 1.6f, 0.35f);
            P.rrect(kx - 4.6f, cy - 4.6f, 9.2f, 9.2f, 4.6f, Ramp(rgb(255, 255, 255), rgb(222, 226, 244), cy - 4.6f, cy + 4.6f));
            gr.inc = gr.dec = SetRect{px, py, pw, ph};
            float left = px - 5.f;
            if (!r.value.empty() && r.value != "On" && r.value != "Off") {
                const float w = P.text(bodyb, vs, px - 6.f, cy + 3.2f, r.value, vcol, 130.f, 1);
                left -= w + 6.f;
            }
            return left;
        }
        case SetKind::Slider: {
            const float tw = 60.f;
            const float inc_x = xr - 3.f, trk_r = inc_x - 8.f, trk_l = trk_r - tw, dec_x = trk_l - 8.f;
            arrow(dec_x, false, r.can_dec, hover && part == 1);
            arrow(inc_x, true, r.can_inc, hover && part == 2);
            const float f = r.id >= 0 && r.id < SET_MAX_IDS ? V.fill_[r.id] : 0.f;
            P.rrect(trk_l, cy - 1.6f, tw, 3.2f, 1.6f, with_a(sf > 0.5f ? rgb(210, 216, 255) : rgb(150, 160, 204), 0.65f));
            if (r.steps > 1 && r.steps <= 9)
                for (int i = 0; i < r.steps; ++i) {
                    const float tx = trk_l + tw * float(i) / float(r.steps - 1);
                    P.segment(tx, cy - 3.f, tx, cy + 3.f, 0.35f, with_a(sf > 0.5f ? C_WHITE : rgb(90, 100, 170), 0.55f));
                }
            const float fx = trk_l + tw * f;
            if (fx > trk_l + 0.5f) P.rrect(trk_l, cy - 1.6f, fx - trk_l, 3.2f, 1.6f, Ramp(rgb(250, 214, 110), rgb(226, 160, 40), cy - 1.6f, cy + 1.6f));
            if (pulse > 0.f) P.rrect(fx - 4.f, cy - 4.f, 8.f, 8.f, 4.f, with_a(rgb(255, 214, 110), 0.5f * pulse), 1.f + 2.f * (1.f - pulse));
            P.rrect(fx - 3.6f, cy - 3.6f, 7.2f, 7.2f, 3.6f, with_a(rgb(70, 78, 150), 0.9f), 0.7f);
            P.rrect(fx - 3.6f, cy - 3.6f, 7.2f, 7.2f, 3.6f, Ramp(rgb(255, 255, 255), rgb(214, 218, 242), cy - 3.6f, cy + 3.6f));
            gr.dec = SetRect{dec_x - 6.5f, rr.y, 13.f, rr.h};
            gr.inc = SetRect{inc_x - 6.5f, rr.y, 13.f, rr.h};
            gr.track = SetRect{trk_l - 3.f, rr.y, tw + 6.f, rr.h};
            float left = dec_x - 8.f;
            if (!r.value.empty()) {
                const float w = P.text(bodyb, vs, left, cy + 3.2f, r.value, vcol, 96.f, 1);
                left -= w + 6.f;
            }
            return left;
        }
        case SetKind::Choice: {
            float x = xr - 3.f;
            arrow(x, true, r.can_inc, hover && part == 2);
            gr.inc = SetRect{x - 6.5f, rr.y, 13.f, rr.h};
            x -= 9.f;
            const float w = P.text(bodyb, vs, x, cy + 3.2f, r.value, vcol, 150.f, 1);
            x -= w + 8.f;
            arrow(x, false, r.can_dec, hover && part == 1);
            gr.dec = SetRect{x - 6.5f, rr.y, 13.f, rr.h};
            return x - 9.f;
        }
        case SetKind::Action: case SetKind::Danger: {
            const Col ring = r.kind == SetKind::Danger ? col_mix(C_RED, rgb(255, 214, 214), sf) : col_mix(rgb(214, 160, 50), rgb(255, 232, 150), sf);
            P.ring(xr - 3.f, cy, 4.6f, 1.1f, ring);
            Poly t; t.add(xr - 1.6f, cy); t.add(xr - 4.4f, cy - 2.2f); t.add(xr - 4.4f, cy + 2.2f);
            P.poly(t, 0.f, [&](float, float) { return ring; });
            float left = xr - 12.f;
            if (!r.value.empty()) {
                const float w = P.text(bodyb, vs, left, cy + 3.2f, r.value, r.kind == SetKind::Danger ? col_mix(C_RED, C_WHITE, sf) : vcol, 150.f, 1);
                left -= w + 6.f;
            }
            return left;
        }
        case SetKind::Progress: {   // a bar (the setup wizard's install progress) with the percentage on its right
            const float pw = 34.f, bw = 200.f, bh = 8.f;
            P.text(bodyb, vs, xr, cy + 3.2f, r.value, vcol, pw, 1);
            const float bar_r = xr - pw - 6.f, bar_l = bar_r - bw, by = cy - bh / 2.f;
            P.rrect(bar_l, by, bw, bh, bh / 2.f, with_a(sf > 0.5f ? rgb(210, 216, 255) : rgb(176, 182, 210), 0.9f));
            P.rrect(bar_l, by, bw, bh, bh / 2.f, with_a(rgb(70, 78, 150), 0.75f), 0.8f);
            const float fw = std::max(bh, bw * clamp01(r.progress));
            if (r.progress > 0.001f) P.rrect(bar_l, by, fw, bh, bh / 2.f, Ramp(rgb(250, 214, 110), rgb(226, 160, 40), by, by + bh));
            return bar_l - 10.f;
        }
        case SetKind::Submenu: {
            chevron_mark(P, xr - 2.f, cy, 4.2f, 1.3f, true, fg);
            chevron_mark(P, xr - 6.f, cy, 4.2f, 1.3f, true, with_a(fg, 0.55f));
            float left = xr - 14.f;
            if (!r.value.empty()) {
                const float w = P.text(bodyb, vs, left, cy + 3.2f, r.value, dim, 130.f, 1);
                left -= w + 6.f;
            }
            return left;
        }
        case SetKind::Text: {
            P.segment(xr - 5.f, cy + 3.6f, xr - 0.6f, cy - 0.9f, 0.95f, fg);
            diamond(P, xr - 5.8f, cy + 4.4f, 1.5f, fg);
            float left = xr - 11.f;
            const float w = P.text(bodyb, vs, left, cy + 3.2f, r.value.empty() ? "-" : r.value, r.value.empty() ? dim : vcol, 150.f, 1);
            return left - w - 6.f;
        }
        default: {   // Info
            float left = xr;
            if (!r.value.empty()) {
                const float w = P.text(bodyb, vs, xr, cy + 3.2f, r.value, col_mix(dim, vcol, pulse), 170.f, 1);
                left -= w + 6.f;
            }
            return left;
        }
        }
    }

    void draw_list() {
        const float dx = (1.f - e_list) * 44.f;
        const float base_ga = e_list;
        P.ga = base_ga;
        list = shifted(S.hide_side ? R_LIST_W : R_LIST, dx, 0);
        blit_panel(c, panel_img(S.hide_side ? 4 : 1, c.scale), int(std::lround(dx * P.s)), 0, P.ga);
        corner_ticks(P, list, 2.4f, with_a(GOLD, 0.9f));
        V.g_.list = list;
        V.g_.rows.clear();
        const int n = int(S.rows.size());
        const float pe = ease_out(V.page_p_), pdx = float(V.page_dir_) * (1.f - pe) * 26.f;
        const float body_ga = base_ga * (0.15f + 0.85f * pe);
        P.ga = body_ga;
        P.clip(list.y + 1.5f, list.y + list.h - 1.5f);
        const float rx = list.x + 5.f + pdx, rw = list.w - 10.f - (V.scroll_max_ > 0.f ? 6.f : 0.f);
        const int sel = std::clamp(S.sel, 0, std::max(0, n - 1));
        // selection bar (glides)
        float hl_c = -1e9f;
        const float lf = 1.f - V.tab_focus_;
        if (n > 0 && S.rows[size_t(sel)].selectable() && lf > 0.02f) {
            const float ga_l = P.ga;
            P.ga = ga_l * lf;
            const float hy = list.y + V.sel_y_ - list_scroll + 0.5f, hh = ROW_PITCH - 1.5f;
            hl_c = hy + hh / 2.f;
            chev(P, rx, hy, rw, hh, 6.f, 0.9f, [](float, float) { return rgb(214, 218, 240); });
            Ramp rp(rgb(104, 118, 255), rgb(40, 48, 222), rgb(20, 22, 150), hy, hy + hh);
            chev(P, rx, hy, rw, hh, 6.f, 0.f, [&](float yy, float) { return rp.at(yy); });
            chev(P, rx + 2.f, hy + 1.f, rw - 4.f, hh * 0.42f, 4.f, 0.f, [](float, float) { return rgb(255, 255, 255, 0.24f); });
            {   // light sweep crossing the bar
                const ChevSd q = chev_sd(rx, hy, rw, hh, 6.f, P.s);
                const float cyc = 2.4f, u = std::fmod(float(V.t_), cyc) / cyc;
                if (u < 0.55f) {
                    const float sx = (rx - 14.f + (u / 0.55f) * (rw + 28.f)) * P.s, ym = (hy + hh / 2.f) * P.s, hw = 7.f * P.s;
                    P.shape(rx, hy, rx + rw, hy + hh,
                            [&](float px, float py) { return std::max(q(px, py), std::fabs(px - sx - (py - ym) * 0.7f) - hw); },
                            [](float, float) { return rgb(255, 255, 255, 0.30f); });
                }
            }
            const float pl = 0.5f + 0.5f * std::sin(float(V.t_) * 3.6f);
            diamond(P, rx + 8.f, hy + hh / 2.f, 2.6f + 0.5f * pl, rgb(250, 220, 120));
            P.ga = ga_l;
            if (lf < 0.5f) hl_c = -1e9f;
        }
        for (int i = 0; i < n; ++i) {
            const SetRow& r = S.rows[size_t(i)];
            const float y = list.y + ry[size_t(i)] - list_scroll;
            if (y + rh[size_t(i)] < list.y || y > list.y + list.h) continue;
            const SetRect rr{rx, y, rw, rh[size_t(i)]};
            const float cy = y + rh[size_t(i)] / 2.f;
            if (r.kind == SetKind::Header) {
                diamond(P, rr.x + 9.f, cy + 1.f, 2.4f, with_a(GOLD, 0.95f));
                const float w = P.text(bodyb, 7.8f * fs, rr.x + 17.f, cy + 3.2f, r.label, C_DIM, rr.w - 40.f);
                const float x0 = rr.x + 17.f + w + 6.f, x1 = rr.x + rr.w - 6.f;
                const int nseg = 14;
                for (int k = 0; k < nseg; ++k) {
                    Col kc = rgb(150, 160, 210, 0.75f * (1.f - float(k) / float(nseg)));
                    P.hline(x0 + (x1 - x0) * float(k) / float(nseg), x0 + (x1 - x0) * float(k + 1) / float(nseg), cy + 1.f, 0.7f, kc);
                }
                continue;
            }
            const float sf = ease_io(clamp01(1.f - std::fabs(cy - hl_c) / (ROW_PITCH * 0.85f)));
            const bool hov = S.hover_row == i && S.device == 1;
            if (hov && sf < 0.6f) {
                chev(P, rr.x, rr.y + 0.5f, rr.w, rr.h - 1.5f, 6.f, 0.f, [](float, float) { return rgb(110, 124, 255, 0.16f); });
            }
            if (S.press_row == i && sf < 0.9f) {
                chev(P, rr.x, rr.y + 0.5f, rr.w, rr.h - 1.5f, 6.f, 0.f, [](float, float) { return rgb(60, 70, 220, 0.22f); });
            }
            if (sf < 0.2f && i + 1 < n && S.rows[size_t(i) + 1].kind != SetKind::Header && i != sel - 1)
                P.hline(rr.x + 10.f, rr.x + rr.w - 10.f, y + rr.h - 0.4f, 0.6f, rgb(190, 196, 228, 0.7f));
            P.ga = body_ga * (r.disabled ? 0.45f : 1.f);
            const Col fg0 = r.kind == SetKind::Danger ? C_RED : C_INK;
            const Col fg = col_mix(fg0, C_WHITE, sf), dim = col_mix(C_DIM, rgb(214, 220, 255), sf);
            SetGeom::Row gr;
            gr.idx = i;
            gr.r = rr;
            const float left = draw_controls(r, rr, sf, fg, dim, gr, hov, S.hover_part);
            const float lw = P.text(bodyb, 9.4f * fs, rr.x + 17.f, cy + 3.5f, r.label, fg, std::max(30.f, left - (rr.x + 17.f)));
            if (!r.badge.empty() && lw < left - (rr.x + 17.f) - 8.f) diamond(P, rr.x + 17.f + lw + 6.f, cy, 1.8f, rgb(236, 170, 40));
            P.ga = body_ga;
            V.g_.rows.push_back(gr);
        }
        // fades where more rows continue
        if (V.scroll_max_ > 0.5f) {
            if (list_scroll > 1.f) P.rrect(list.x + 2.f, list.y + 1.5f, list.w - 4.f, 7.f, 0.f, Ramp(rgb(255, 255, 255, 0.85f), rgb(255, 255, 255, 0.f), list.y, list.y + 8.5f));
            if (list_scroll < V.scroll_max_ - 1.f) P.rrect(list.x + 2.f, list.y + list.h - 8.5f, list.w - 4.f, 7.f, 0.f, Ramp(rgb(255, 255, 255, 0.f), rgb(255, 255, 255, 0.85f), list.y + list.h - 8.5f, list.y + list.h));
        }
        P.unclip();
        P.ga = base_ga;
        if (V.scroll_max_ > 0.5f) {   // scroll bar
            const SetRect tr{list.x + list.w - 8.f, list.y + 6.f, 3.5f, list.h - 12.f};
            const float th = std::max(16.f, tr.h * list.h / content_h);
            const float ty = tr.y + (tr.h - th) * (list_scroll / V.scroll_max_);
            P.rrect(tr.x, tr.y, tr.w, tr.h, 1.75f, rgb(140, 150, 204, 0.40f));
            P.rrect(tr.x - 0.5f, ty - 0.5f, tr.w + 1.f, th + 1.f, 2.25f, rgb(214, 218, 240));
            P.rrect(tr.x, ty, tr.w, th, 1.75f, Ramp(rgb(104, 118, 255), rgb(30, 38, 200), ty, ty + th));
            V.g_.scroll_track = SetRect{tr.x - 5.f, tr.y - 4.f, tr.w + 10.f, tr.h + 8.f};
            V.g_.scroll_thumb = SetRect{tr.x - 5.f, ty - 2.f, tr.w + 10.f, th + 4.f};
        }
        P.ga = 1.f;
    }

    // -- description ----------------------------------------------------------------------------------------------------
    void draw_detail() {
        const float dy = (1.f - e_detail) * 26.f;
        P.ga = e_detail;
        detail = shifted(S.hide_side ? R_DETAIL_W : R_DETAIL, 0, dy);
        V.g_.detail = detail;
        blit_panel(c, panel_img(S.hide_side ? 5 : 2, c.scale), 0, int(std::lround(dy * P.s)), P.ga);
        if (T && T->corner[0].valid()) {
            const SdSprite* q = T->corner;
            const float o = 3.5f, px = detail.x, py = detail.y, pw = detail.w, ph = detail.h;
            draw_sprite(c, q[0], px - o, py - o, float(q[0].w), float(q[0].h), e_detail);
            draw_sprite(c, q[1], px + pw + o - float(q[1].w), py - o, float(q[1].w), float(q[1].h), e_detail);
            draw_sprite(c, q[2], px - o, py + ph + o - float(q[2].h), float(q[2].w), float(q[2].h), e_detail);
            draw_sprite(c, q[3], px + pw + o - float(q[3].w), py + ph + o - float(q[3].h), float(q[3].w), float(q[3].h), e_detail);
        } else corner_ticks(P, detail, 2.4f, with_a(GOLD, 0.9f));
        const int n = int(S.rows.size());
        if (n == 0 || S.sel < 0 || S.sel >= n) { P.ga = 1.f; return; }
        const SetRow& r = S.rows[size_t(S.sel)];
        if (r.help.empty() || S.focus_tabs) { P.ga = 1.f; return; }
        const size_t nl = r.help.find('\n');
        const std::string title = nl == std::string::npos ? r.label : r.help.substr(0, nl);
        const std::string text = nl == std::string::npos ? r.help : r.help.substr(nl + 1);
        const float x0 = detail.x + 14.f, x1 = detail.x + detail.w - 14.f;
        // title: crystal marker, title, chip
        diamond(P, x0 + 2.f, detail.y + 13.4f, 3.6f, rgb(236, 190, 70));
        diamond(P, x0 + 1.4f, detail.y + 12.4f, 1.3f, rgb(255, 250, 220));
        float chip_w = 0.f;
        if (!r.badge.empty()) {
            const float bw = P.text_width(bodyb, 6.8f * fs, r.badge) + 11.f;
            const float bx = x1 - bw, by = detail.y + 6.6f;
            P.rrect(bx - 0.7f, by - 0.7f, bw + 1.4f, 11.4f, 5.7f, rgb(150, 112, 30));
            P.rrect(bx, by, bw, 10.f, 5.f, Ramp(rgb(252, 226, 140), rgb(226, 172, 56), by, by + 10.f));
            P.text(bodyb, 6.8f * fs, bx + bw / 2.f, by + 7.2f, r.badge, rgb(70, 44, 8), 0, 2);
            chip_w = bw + 8.f;
        }
        P.text(bodyb, 10.4f * fs, x0 + 12.f, detail.y + 16.4f, title, C_NAVY, x1 - x0 - 12.f - chip_w);
        {
            const int nseg = 20;
            const float lx0 = x0, lx1 = x1, ly = detail.y + 21.2f;
            for (int k = 0; k < nseg; ++k)
                P.hline(lx0 + (lx1 - lx0) * float(k) / float(nseg), lx0 + (lx1 - lx0) * float(k + 1) / float(nseg), ly, 0.8f,
                        rgb(226, 182, 80, 0.85f * (1.f - float(k) / float(nseg))));
        }
        // body: wrapped; scrolls by itself when it does not fit
        const float by0 = detail.y + 24.5f, by1 = detail.y + detail.h - 3.5f, line_h = 9.5f * fs, size = 8.2f * fs;
        const std::string key = text + "#" + std::to_string(c.scale) + (T ? "t" : "p");
        if (V.help_key_ != key) {
            V.help_key_ = key;
            V.help_t_ = 0.f;
            V.help_lines_ = wrap_text(P, body, size, text, x1 - x0, 60);
        }
        const float total = float(V.help_lines_.size()) * line_h, avail = by1 - by0;
        float off = 0.f;
        const bool overflow = total > avail + 1.f;
        if (overflow) {
            const float maxoff = total - avail, speed = 7.5f, delay = 2.0f, hold = 1.6f;
            const float cyc = delay + maxoff / speed + hold;
            const float t = std::fmod(V.help_t_, cyc);
            off = std::clamp((t - delay) * speed, 0.f, maxoff);
        }
        P.clip(by0 - 1.f, by1 + 0.5f);
        const Col tc = rgb(46, 54, 116);
        for (size_t k = 0; k < V.help_lines_.size(); ++k) {
            const float ly = by0 + 7.f + float(k) * line_h - off;
            if (ly < by0 - 2.f || ly > by1 + 8.f) continue;
            float a = 1.f;
            if (overflow) a = std::min(clamp01((ly - by0 + 1.f) / 6.f), clamp01((by1 + 3.f - ly) / 6.f));
            P.text(body, size, x0, ly, V.help_lines_[k], with_a(tc, 0.92f * a));
        }
        P.unclip();
        if (overflow) {   // a blinking "more" marker
            const float bl = 0.5f + 0.5f * std::sin(float(V.t_) * 5.f);
            Poly tri; tri.add(x1 - 1.f, by1 - 1.f); tri.add(x1 - 7.f, by1 - 1.f); tri.add(x1 - 4.f, by1 + 2.f);
            P.poly(tri, 0.f, [&](float, float) { return rgb(226, 160, 40, 0.4f + 0.6f * bl); });
        }
        P.ga = 1.f;
    }

    // -- hint bar --------------------------------------------------------------------------------------------------------
    struct Hint { int kind; std::string text; };   // kind: icon id
    void keycap(float x, float cy, const std::string& t, float& w) {
        const float tw = P.text_width(bodyb, 6.8f * fs, t);
        w = std::max(11.f, tw + 6.f);
        P.rrect(x, cy - 4.8f, w, 9.6f, 2.4f, C_WHITE);
        P.rrect(x + 0.9f, cy - 3.9f, w - 1.8f, 7.8f, 1.8f, rgb(46, 50, 124));
        P.text(bodyb, 6.8f * fs, x + w / 2.f, cy + 2.4f, t, C_WHITE, 0, 2);
    }
    float glyph_btn(float x, float cy, bool cross) {
        // drawn as vectors: the game's own button sprites are only ~20 px and look soft scaled up
        draw_btn_glyph(P, cross, x + 5.5f, cy, C_WHITE);
        return 11.f;
    }
    void draw_footer() {
        const float dy = (1.f - e_foot) * 26.f;
        P.ga = e_foot;
        foot = shifted(R_FOOT, 0, dy);
        navy_plate(P, foot.x + 2.f, foot.y, foot.w - 4.f, foot.h, foot.h * 0.5f);
        const float cy = foot.y + foot.h / 2.f, ty = cy + 3.2f, ts = 8.4f * fs;
        float x = foot.x + 22.f, rx = foot.x + foot.w - 22.f;
        auto label = [&](const char* t) { const float w = P.text(bodyb, ts, x, ty, t, C_WHITE); x += w + 14.f; };
        auto label_r = [&](const char* t, float gw) {   // right-aligned: text then the glyph to its left
            const float w = P.text_width(bodyb, ts, t);
            P.text(bodyb, ts, rx - w, ty, t, C_WHITE);
            rx -= w + 3.f + gw;
        };
        if (S.kb.active) {
            if (S.device == 0) {
                x += glyph_btn(x, cy, S.confirm_cross) + 3.f; label("Press key");
                x += glyph_btn(x, cy, !S.confirm_cross) + 3.f; label("Delete");
                float w; keycap(x, cy, "Start", w); x += w + 3.f; label("Done");
            } else {
                float w; keycap(x, cy, "Enter", w); x += w + 3.f; label("Done");
                keycap(x, cy, "Esc", w); x += w + 3.f; label("Cancel");
                keycap(x, cy, "Backspace", w); x += w + 3.f; label("Delete");
            }
            P.ga = 1.f;
            return;
        }
        if (S.device == 0) {
            x += glyph_btn(x, cy, S.confirm_cross) + 3.f; label("Select");
            x += glyph_btn(x, cy, !S.confirm_cross) + 3.f; label((S.in_sub || !S.focus_tabs) ? "Back" : "Close");
            float w;
            if (!S.hide_side) { keycap(x, cy, "L", w); x += w + 2.f; keycap(x, cy, "R", w); x += w + 3.f; label("Category"); }
            // right: d-pad glyphs
            const float dk = 0.78f;
            if (false) {
                auto pad = [&](const SdSprite& g, float gx) {
                    draw_sprite(c, g, gx, cy - dk * float(g.h) / 2.f, dk * float(g.w), dk * float(g.h), e_foot);
                    return dk * float(g.w);
                };
                const float w1 = dk * float(T->dpad_left.w + T->dpad_right.w), w2 = dk * float(T->dpad_up.w + T->dpad_down.w);
                label_r("Change", w1);
                float gx = rx; gx += pad(T->dpad_left, gx); pad(T->dpad_right, gx);
                rx -= 14.f;
                label_r("Choose", w2);
                gx = rx; gx += pad(T->dpad_up, gx); pad(T->dpad_down, gx);
            } else {
                label_r("Change", 14.f);
                menu_arrow(P, rx + 3.f, cy, true, C_WHITE); menu_arrow(P, rx + 11.f, cy, false, C_WHITE);
                rx -= 14.f;
                label_r("Choose", 14.f);
                for (int k = 0; k < 2; ++k) {   // up / down triangles
                    Poly tri;
                    const float tx = rx + 3.f + float(k) * 8.f;
                    if (k == 0) { tri.add(tx, cy - 3.6f); tri.add(tx + 3.6f, cy + 2.6f); tri.add(tx - 3.6f, cy + 2.6f); }
                    else { tri.add(tx, cy + 3.6f); tri.add(tx + 3.6f, cy - 2.6f); tri.add(tx - 3.6f, cy - 2.6f); }
                    P.poly(tri, 0.f, [](float, float) { return C_WHITE; });
                }
            }
        } else if (S.device == 1) {
            float w;
            keycap(x, cy, "Enter", w); x += w + 3.f; label("Select");
            keycap(x, cy, "Esc", w); x += w + 3.f; label((S.in_sub || !S.focus_tabs) ? "Back" : "Close");
            if (!S.hide_side) { keycap(x, cy, "Q", w); x += w + 2.f; keycap(x, cy, "E", w); x += w + 3.f; label("Category"); }
            label_r("Scroll", 0.f);
            label_r("Wheel", 0.f);
            // a small mouse glyph: left button highlighted
            P.rrect(rx - 7.f, cy - 5.5f, 7.f, 11.f, 3.4f, C_WHITE);
            P.rrect(rx - 6.2f, cy - 4.7f, 5.4f, 9.4f, 2.7f, rgb(46, 50, 124));
            P.segment(rx - 3.5f, cy - 4.7f, rx - 3.5f, cy - 0.8f, 0.4f, C_WHITE);
        } else {
            label("Tap to choose");
            label("Drag to scroll");
            label("Slide a bar to adjust");
        }
        P.ga = 1.f;
    }

    // -- question dialog (restart now / later) -----------------------------------------------------------------------------
    void draw_dialog() {
        const float p = ease_out(V.dlg_p_);
        V.g_.dlg_btns.clear();
        if (p <= 0.001f) return;
        const SetDialog& D = S.dlg;
        P.ga = 1.f;
        P.rrect(0, 0, float(SCREEN_W), float(SCREEN_H), 0.f, rgb(12, 14, 46, 0.55f * p));
        P.ga = p;
        const float w = 300.f, h = 124.f, x = (float(SCREEN_W) - w) / 2.f, y = (float(SCREEN_H) - h) / 2.f + (1.f - p) * 12.f;
        P.shadow(x, y, w, h, 8.f, 6.f, 0.35f);
        P.rrect(x, y, w, h, 8.f, rgb(236, 238, 252));
        P.rrect(x + 1.f, y + 1.f, w - 2.f, h - 2.f, 7.f, Ramp(rgb(248, 249, 255), rgb(222, 226, 246), y, y + h));
        navy_plate(P, x + 24.f, y - 10.f, w - 48.f, 22.f, 11.f, 0.f);
        P.text(bodyb, 9.6f * fs, x + w / 2.f, y + 5.2f, D.title, C_WHITE, 0, 2);
        float ly = y + 34.f;
        size_t a = 0;
        while (a <= D.text.size()) {
            size_t b = D.text.find('\n', a);
            if (b == std::string::npos) b = D.text.size();
            P.text(body, 8.8f * fs, x + w / 2.f, ly, D.text.substr(a, b - a), C_INK, w - 24.f, 2);
            ly += 13.f;
            a = b + 1;
        }
        const int n = int(D.buttons.size());
        const float bw = 118.f, gap = 14.f, bh = 22.f, by = y + h - bh - 12.f;
        float bx = x + (w - (float(n) * bw + float(n - 1) * gap)) / 2.f;
        for (int i = 0; i < n; ++i) {
            const SetRect r{bx, by, bw, bh};
            V.g_.dlg_btns.push_back(r);
            const bool sel = i == D.sel, hov = i == D.hover;
            if (sel) {
                chev(P, r.x, r.y, r.w, r.h, 6.f, 0.9f, [](float, float) { return rgb(214, 218, 240); });
                Ramp rp(rgb(104, 118, 255), rgb(40, 48, 222), rgb(20, 22, 150), r.y, r.y + r.h);
                chev(P, r.x, r.y, r.w, r.h, 6.f, 0.f, [&](float yy, float) { return rp.at(yy); });
                chev(P, r.x + 2.f, r.y + 1.f, r.w - 4.f, r.h * 0.42f, 4.f, 0.f, [](float, float) { return rgb(255, 255, 255, 0.24f); });
            } else {
                chev(P, r.x, r.y, r.w, r.h, 6.f, 0.9f, [](float, float) { return rgb(190, 196, 228); });
                chev(P, r.x + 0.8f, r.y + 0.8f, r.w - 1.6f, r.h - 1.6f, 5.f, 0.f, [&](float, float) { return hov ? rgb(206, 212, 250) : rgb(232, 235, 250); });
            }
            P.text(bodyb, 9.2f * fs, r.x + r.w / 2.f, r.y + r.h / 2.f + 3.3f, D.buttons[size_t(i)], sel ? C_WHITE : C_INK, r.w - 8.f, 2);
            bx += bw + gap;
        }
        P.ga = 1.f;
    }

    // -- text entry --------------------------------------------------------------------------------------------------------
    void draw_keyboard() {
        const float p = ease_out(V.kb_p_);
        V.g_.keys.clear();
        if (p <= 0.001f) return;
        P.ga = 1.f;
        P.rrect(0, 0, float(SCREEN_W), float(SCREEN_H), 0.f, rgb(12, 14, 46, 0.60f * p));
        P.ga = p;
        const SetRect pn{44.f, 46.f + (1.f - p) * 26.f, 392.f, 198.f};
        blit_panel(c, panel_img(3, c.scale), 0, int(std::lround((1.f - p) * 26.f * P.s)), P.ga);
        corner_ticks(P, pn, 2.6f, with_a(GOLD, 0.95f));
        navy_plate(P, pn.x + 14.f, pn.y - 9.f, 168.f, 19.f, 9.5f);
        P.text(bodyb, 10.f * fs, pn.x + 14.f + 84.f, pn.y + 4.4f, S.kb.title, C_WHITE, 150.f, 2);
        // the text field
        const SetRect fld{pn.x + 14.f, pn.y + 18.f, pn.w - 28.f, 22.f};
        V.g_.kb_field = fld;
        P.rrect(fld.x, fld.y, fld.w, fld.h, 4.f, rgb(150, 160, 210), 0.9f);
        P.rrect(fld.x, fld.y, fld.w, fld.h, 4.f, Ramp(rgb(255, 255, 255), rgb(236, 238, 252), fld.y, fld.y + fld.h));
        std::string shown = S.kb.secret ? std::string(S.kb.text.size(), '*') : S.kb.text;
        // keep the end visible
        float tw = P.text_width(bodyb, 10.f * fs, shown);
        while (tw > fld.w - 22.f && shown.size() > 3) { shown.erase(shown.begin()); tw = P.text_width(bodyb, 10.f * fs, "..." + shown); }
        if (tw > fld.w - 22.f || (S.kb.text.size() > shown.size() && !S.kb.secret)) shown = "..." + shown;
        P.clip(fld.y + 1.f, fld.y + fld.h - 1.f);
        const float tw2 = P.text(bodyb, 10.f * fs, fld.x + 8.f, fld.y + 15.2f, shown, C_NAVY);
        P.unclip();
        if (std::fmod(float(V.t_), 1.0f) < 0.55f) P.segment(fld.x + 9.f + tw2, fld.y + 5.f, fld.x + 9.f + tw2, fld.y + fld.h - 5.f, 0.55f, C_INK);
        // keys
        auto keys = settings_keyboard(S.kb.shift, S.kb.symbols);
        const float gx0 = pn.x + 14.f, gw = pn.w - 28.f, kh = 21.f, gap = 3.5f, gy0 = pn.y + 48.f;
        for (size_t r = 0; r < keys.size(); ++r) {
            const float y = gy0 + float(r) * (kh + gap);
            if (r < 4) {
                const float kw = (gw - 9.f * gap) / 10.f;
                for (size_t k = 0; k < keys[r].size(); ++k) keys[r][k].r = SetRect{gx0 + float(k) * (kw + gap), y, kw, kh};
            } else {
                const float ws[6] = {48.f, 48.f, 0.f, 52.f, 52.f, 52.f};
                float fixed = 0.f;
                for (float w : ws) fixed += w;
                const float space = gw - fixed - 5.f * gap;
                float x = gx0;
                for (size_t k = 0; k < keys[r].size(); ++k) {
                    const float w = k == 2 ? space : ws[k];
                    keys[r][k].r = SetRect{x, y, w, kh};
                    x += w + gap;
                }
            }
        }
        const int fr = std::clamp(S.kb.row, 0, int(keys.size()) - 1);
        const int fc = std::clamp(S.kb.col, 0, int(keys[size_t(fr)].size()) - 1);
        const SetRect fk = keys[size_t(fr)][size_t(fc)].r;
        if (V.kb_p_ < 0.05f || V.kb_fw_ < 1.f) { V.kb_fx_ = fk.x + fk.w / 2.f; V.kb_fy_ = fk.y + fk.h / 2.f; V.kb_fw_ = fk.w; }
        else {
            const float k = 1.f - std::exp(-lastdt * 24.f);
            V.kb_fx_ += (fk.x + fk.w / 2.f - V.kb_fx_) * k;
            V.kb_fy_ += (fk.y + fk.h / 2.f - V.kb_fy_) * k;
            V.kb_fw_ += (fk.w - V.kb_fw_) * k;
        }
        for (size_t r = 0; r < keys.size(); ++r)
            for (size_t k = 0; k < keys[r].size(); ++k) {
                const SetKey& key = keys[r][k];
                const bool on = (key.kind == 1 && (S.kb.shift || S.kb.symbols)) || (key.kind == 2 && S.kb.symbols);
                Col top = rgb(255, 255, 255), bot = rgb(222, 226, 246), edge = rgb(160, 168, 214);
                if (key.kind == 5) { top = rgb(255, 232, 150); bot = rgb(226, 172, 56); edge = rgb(160, 116, 30); }
                else if (key.kind == 6 || key.kind == 4) { top = rgb(236, 238, 250); bot = rgb(200, 206, 236); }
                else if (on) { top = rgb(255, 232, 150); bot = rgb(230, 180, 70); edge = rgb(160, 116, 30); }
                P.rrect(key.r.x, key.r.y, key.r.w, key.r.h, 4.f, edge, 0.7f);
                P.rrect(key.r.x, key.r.y, key.r.w, key.r.h, 4.f, Ramp(top, bot, key.r.y, key.r.y + key.r.h));
                P.text(bodyb, (key.kind == 0 ? 10.f : 8.2f) * fs, key.r.x + key.r.w / 2.f, key.r.y + key.r.h / 2.f + 3.5f, key.label, key.kind == 5 ? rgb(70, 44, 8) : C_INK, key.r.w - 4.f, 2);
            }
        {   // focus: a royal bar that glides from key to key
            const float w = V.kb_fw_, h = kh, x = V.kb_fx_ - w / 2.f, y = V.kb_fy_ - h / 2.f;
            P.rrect(x, y, w, h, 4.f, rgb(214, 218, 240), 1.0f);
            P.rrect(x, y, w, h, 4.f, Ramp(rgb(104, 118, 255), rgb(40, 48, 222), rgb(20, 22, 150), y, y + h));
            P.rrect(x + 1.f, y + 1.f, w - 2.f, h * 0.42f, 3.f, rgb(255, 255, 255, 0.26f));
            const SetKey& key = keys[size_t(fr)][size_t(fc)];
            P.text(bodyb, (key.kind == 0 ? 10.f : 8.2f) * fs, V.kb_fx_, V.kb_fy_ + 3.5f, key.label, C_WHITE, w - 4.f, 2);
        }
        V.g_.keys = keys;
        P.ga = 1.f;
    }

    // -- open / close ---------------------------------------------------------------------------------------------------------
    void draw_iris() {
        const float p = clamp01(V.p_open_ / 0.42f);
        if (p >= 0.999f) return;
        const float e = ease_io(p);
        const float cx = float(SCREEN_W) / 2.f, cy = float(SCREEN_H) / 2.f, R = e * 310.f;
        const float s = float(c.scale);
        const float edge = 14.f;
        const int ya = std::max(0, tl_band_y0), yb = std::min(c.h, tl_band_y1);
        const float R2 = R * R, Re2 = (R + edge) * (R + edge);
        // outside the circle the canvas is transparent: the game's own picture shows there (a soft edge `edge` wide)
        auto shade = [&](uint32_t& px, float veil) { px = (px & 0x00FFFFFFu) | (uint32_t(std::clamp(int((1.f - veil) * 255.f + 0.5f), 0, 255)) << 24); };
        for (int y = ya; y < yb; ++y) {
            const float dy = (float(y) + 0.5f) / s - cy, dy2 = dy * dy * 1.6f;
            uint32_t* row = &c.px[size_t(y) * size_t(c.w)];
            // columns inside the opening are skipped, columns beyond the soft edge get the flat veil: no sqrt for either
            const int xin0 = dy2 >= R2 ? c.w : int(std::ceil((cx - std::sqrt(R2 - dy2)) * s - 0.5f));
            const int xin1 = dy2 >= R2 ? c.w : int(std::floor((cx + std::sqrt(R2 - dy2)) * s - 0.5f));
            const bool flat_all = dy2 >= Re2;
            const int xf0 = flat_all ? c.w : int(std::floor((cx - std::sqrt(Re2 - dy2)) * s - 0.5f));
            const int xf1 = flat_all ? 0 : int(std::ceil((cx + std::sqrt(Re2 - dy2)) * s - 0.5f));
            for (int x = 0; x < c.w; ++x) {
                if (x >= xin0 && x <= xin1) { x = xin1; continue; }
                if (flat_all || x <= xf0 || x >= xf1) { shade(row[x], 1.f); continue; }
                const float dxp = (float(x) + 0.5f) / s - cx;
                const float d = std::sqrt(dxp * dxp + dy2);
                if (d <= R) continue;
                shade(row[x], clamp01((d - R) / edge));
            }
        }
        // a gold ring riding the rim of the opening
        const float ra = std::sin(clamp01(p) * 3.14159f);
        for (int i = 0; i < 3; ++i) P.ring(cx, cy, R / 1.f * (1.f - 0.012f * float(i)), 1.4f - 0.4f * float(i), rgb(250, 220, 130, 0.65f * ra / float(i + 1)));
    }

    float lastdt = 0.016f;
    void draw_all() {
        using Clk = std::chrono::steady_clock;
        static thread_local double acc[8] = {}; static thread_local int cnt = 0;
        auto tick = [&](int i, Clk::time_point& t0) { const auto t1 = Clk::now(); acc[i] += std::chrono::duration<double, std::milli>(t1 - t0).count(); t0 = t1; };
        auto t0 = Clk::now();
        V.g_.back = SetRect{};
        draw_background(); tick(0, t0);
        draw_header(); tick(1, t0);
        V.g_.back = back;
        if (S.hide_side) { V.g_.cats.clear(); V.g_.side = SetRect{}; side = SetRect{}; }
        else draw_sidebar();
        tick(2, t0);
        draw_list(); tick(3, t0);
        draw_detail(); tick(4, t0);
        draw_footer(); tick(5, t0);
        draw_keyboard();
        draw_dialog();
        draw_iris(); tick(6, t0);
        if (std::getenv("PSPRECOMP_SETTINGS_PROFILE") && ++cnt == 120) {
            std::fprintf(stderr, "[SETTINGS] draw ms: bg %.1f header %.1f side %.1f list %.1f detail %.1f footer %.1f kb+iris %.1f\n", acc[0]/cnt, acc[1]/cnt, acc[2]/cnt, acc[3]/cnt, acc[4]/cnt, acc[5]/cnt, acc[6]/cnt);
            cnt = 0; for (double& a : acc) a = 0;
        }
        V.animating_ = true;
    }
};

}  // namespace

void SettingsView::render(Canvas& c, const SetScene& s, double now_ms) {
    const float dt = last_ms_ < 0 ? 0.f : float(std::clamp((now_ms - last_ms_) / 1000.0, 0.0, 0.1));
    last_ms_ = now_ms;
    t_ += dt;
    help_t_ += dt;
    SettingsDraw D(*this, s, c);
    D.lastdt = dt;
    D.step(dt);
    // The canvas is drawn in horizontal bands on several threads (a CPU-drawn 1440 x 816 picture is the whole cost of this
    // screen). Each band runs the complete drawing with every pixel write limited to its rows. The first picture of a
    // size is drawn on one thread: it builds the scaled artwork the others copy.
    static unsigned primed = 0;
    static const int cfg = [] { const char* e = std::getenv("PSPRECOMP_SETTINGS_THREADS"); return e ? std::atoi(e) : 0; }();
    int nt = cfg > 0 ? cfg : int(std::min(8u, std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u)));
    const unsigned bit = 1u << std::clamp(c.scale, 0, 31);
    if (!(primed & bit) || nt <= 1) { primed |= bit; D.draw_all(); return; }
    const int nbands = nt * 3;
    std::atomic<int> next{0};
    auto run = [&](SettingsView& view) {
        for (int b; (b = next.fetch_add(1)) < nbands;) {
            tl_band_y0 = c.h * b / nbands; tl_band_y1 = c.h * (b + 1) / nbands;
            SettingsDraw Db(view, s, c);
            Db.lastdt = dt;
            Db.compute_rows();
            Db.e_hdr = D.e_hdr; Db.e_side = D.e_side; Db.e_list = D.e_list; Db.e_detail = D.e_detail; Db.e_foot = D.e_foot;
            Db.list_scroll = D.list_scroll;
            Db.draw_all();
        }
        tl_band_y0 = 0; tl_band_y1 = 1 << 30;
    };
    std::vector<SettingsView> copies(size_t(nt - 1), *this);   // the other threads draw into throw-away geometry
    std::vector<std::thread> th;
    for (int i = 0; i < nt - 1; ++i) th.emplace_back([&run, &copies, i] { run(copies[size_t(i)]); });
    run(*this);
    for (auto& t : th) t.join();
}
