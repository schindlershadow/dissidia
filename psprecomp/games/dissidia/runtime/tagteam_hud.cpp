// games/dissidia/runtime/tagteam_hud.cpp - see tagteam_hud.h.
//
// The retail HUD (two portraits with bravery / HP at the bottom) belongs to the two original fighters, so the extra fighters
// get compact panels above it: name, HP bar, KO state; the reserve roster of each side is a strip of name chips under the
// top row. The canvas is the 480x272 PSP screen; the engine blends it over the picture each present.
#include "psp_ge_draw.h"
#include "tagteam_hud.h"
#include <algorithm>
#include <cstring>

namespace {

constexpr int W = 480, H = 272;
std::vector<uint32_t> g_canvas(W * H), g_last(W * H);
bool g_shown = false;

constexpr uint32_t rgba(int r, int g, int b, int a) { return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24); }

void blend(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint32_t& d = g_canvas[y * W + x];
    const int sa = c >> 24, da = d >> 24;
    if (sa >= 255 || da == 0) { d = c; return; }
    const int oa = sa + da * (255 - sa) / 255;
    auto mix = [&](int sh) { const int s = (c >> sh) & 255, t = (d >> sh) & 255; return oa ? (s * sa + t * da * (255 - sa) / 255) / oa : 0; };
    d = rgba(mix(0), mix(8), mix(16), oa);
}
void rect(int x, int y, int w, int h, uint32_t c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) blend(x + i, y + j, c); }

// Classic 5x7 font, ASCII 0x20..0x5A, 5 columns per glyph, bit 0 = the top row.
const uint8_t kFont[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

int text_width(const std::string& s, int scale) { return s.empty() ? 0 : static_cast<int>(s.size()) * 6 * scale - scale; }
void text(int x, int y, const std::string& s, uint32_t c, int scale = 1, bool shadow = true) {
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        int cx = x;
        for (char ch : s) {
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
            if (ch < 0x20 || ch > 0x5A) ch = '?';
            const uint8_t* g = kFont[ch - 0x20];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (g[col] & (1 << row)) rect(cx + col * scale + (pass == 0), y + row * scale + (pass == 0), scale, scale, pass == 0 ? rgba(0, 0, 0, 200) : c);
            cx += 6 * scale;
        }
    }
}

constexpr uint32_t kTeam[2] = {rgba(70, 140, 255, 255), rgba(255, 90, 80, 255)};

void panel(const TagHudFighter& f, int x, int y) {
    const int w = 104, h = 24;
    const bool ko = f.hp <= 0;
    rect(x, y, w, h, rgba(8, 10, 24, ko ? 120 : 170));
    rect(f.side == 0 ? x : x + w - 2, y, 2, h, ko ? rgba(110, 110, 110, 255) : kTeam[f.side]);
    const int tx = x + (f.side == 0 ? 6 : 4);
    const std::string nm = f.name.substr(0, 10);
    text(tx, y + 3, nm, ko ? rgba(150, 150, 150, 255) : rgba(255, 255, 255, 255));
    if (f.active) text(tx + text_width(nm, 1) + 4, y + 3, f.side == 0 ? "<" : ">", rgba(255, 230, 90, 255));
    const std::string hp = ko ? "KO" : std::to_string(std::max(f.hp, 0));
    text(x + w - 4 - text_width(hp, 1) - (f.side == 1 ? 2 : 0), y + 3, hp, ko ? rgba(255, 110, 100, 255) : rgba(230, 240, 255, 255));
    const int bx = tx, bw = w - 12, by = y + 14;
    rect(bx, by, bw, 5, rgba(40, 44, 60, 230));
    const int fw = ko ? 0 : std::clamp(bw * f.hp / std::max(f.hp_max, 1), 0, bw);
    const bool low = f.hp * 4 < f.hp_max;
    rect(bx, by, fw, 5, low ? rgba(255, 170, 40, 255) : rgba(90, 220, 120, 255));
    rect(bx, by, fw, 2, low ? rgba(255, 210, 120, 160) : rgba(190, 255, 200, 150));
}

void reserve_strip(const TagHudReserve& r) {
    const int chip_w = 25, chip_h = 13, gap = 2, y = 24;
    const int n = static_cast<int>(r.names.size());
    const std::string label = "RESERVE";
    const int total = text_width(label, 1) + 4 + n * (chip_w + gap);
    int x = r.side == 0 ? 4 : W - 4 - total;
    rect(x - 2, y - 3, total + 4, chip_h + 6, rgba(8, 10, 24, 120));
    text(x, y + 3, label, rgba(190, 200, 230, 255));
    x += text_width(label, 1) + 4;
    for (int i = 0; i < n; i++) {
        rect(x, y, chip_w, chip_h, i == 0 ? rgba(30, 40, 70, 235) : rgba(20, 24, 40, 210));
        rect(x, y, chip_w, 1, i == 0 ? kTeam[r.side] : rgba(90, 96, 120, 255));
        text(x + 2, y + 3, r.names[i].substr(0, 4), i == 0 ? rgba(255, 255, 255, 255) : rgba(170, 176, 200, 255));
        x += chip_w + gap;
    }
}

}  // namespace

void tagteam_hud_draw(const std::vector<TagHudFighter>& fighters, const std::vector<TagHudReserve>& reserves) {
    if (fighters.empty() && reserves.empty()) {
        if (g_shown) { ge_draw_set_game_overlay(nullptr, 0, 0); g_shown = false; }
        return;
    }
    std::fill(g_canvas.begin(), g_canvas.end(), 0u);
    int row[2] = {0, 0};
    for (const TagHudFighter& f : fighters) {
        const int s = f.side != 0;
        const int y = 148 - 28 * row[s]++;
        panel(f, s == 0 ? 4 : W - 4 - 104, y);
    }
    for (const TagHudReserve& r : reserves) if (!r.names.empty()) reserve_strip(r);
    if (g_shown && g_canvas == g_last) return;   // unchanged: nothing to upload
    g_last = g_canvas;
    ge_draw_set_game_overlay(g_canvas.data(), W, H);
    g_shown = true;
}
