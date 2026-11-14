// Host test for the animated settings screen (SettingsView): renders sample frames and checks the hit geometry.
// Build/run: runtime/tests/run_settings_ui.sh   (DUMP_SETTINGS=/dir writes PPM frames for eyeballing)
#include "../src/hle/psp_savedata_dialog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace psp_sddialog;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static SetRow row(SetKind k, int id, const char* label, const char* value, const char* help) {
    SetRow r; r.kind = k; r.id = id; r.label = label; r.value = value; r.help = help; return r;
}
static SetScene scene() {
    SetScene s;
    const char* names[] = {"Display", "Graphics", "Image", "Effects", "Controls", "Network", "Cloud", "System", "Drivers", "Debug"};
    for (int i = 0; i < 10; ++i) s.cats.push_back({names[i], i});
    s.cat = 2;
    s.crumbs = {"Image"};
    s.rows.push_back(row(SetKind::Header, 1, "Colour", "", ""));
    SetRow b = row(SetKind::Slider, 2, "Brightness", "Default", "Brightness (PSPRECOMP_BRIGHTNESS)\nLifts or lowers the midtones of the whole picture without crushing blacks or clipping whites. 100% is the original look.");
    b.steps = 21; b.cur = 10; b.can_dec = b.can_inc = true; b.badge = "Saved"; b.cost = "High"; s.rows.push_back(b);
    SetRow c = b; c.id = 3; c.label = "Contrast"; c.value = "115%"; c.cur = 13; s.rows.push_back(c);
    SetRow d = b; d.id = 4; d.label = "Saturation"; d.value = "120%"; d.cur = 12; s.rows.push_back(d);
    s.rows.push_back(row(SetKind::Header, 5, "High dynamic range", "", ""));
    SetRow t = row(SetKind::Toggle, 6, "HDR Output", "", "HDR output (PSPRECOMP_HDR)\nSends the picture to an HDR display: highlights (glows, spell flashes, the sky) are expanded past SDR white up to the peak brightness below; the HUD and menus stay at paper white. The game itself is SDR, so this is an inverse tone map, not native HDR. Turn on HDR in your system's display settings first. Saved.");
    t.on = true; t.badge = "Saved"; s.rows.push_back(t);
    SetRow ch = row(SetKind::Choice, 7, "HDR Highlights", "Medium", "HDR highlights\nHow far small bright features rise above paper white."); ch.can_dec = ch.can_inc = true; s.rows.push_back(ch);
    SetRow tt = row(SetKind::Toggle, 8, "HDR Scene", "On (needs HDR)", "HDR scene\nRenders in 16-bit float."); s.rows.push_back(tt);
    s.rows.push_back(row(SetKind::Submenu, 9, "HDR Advanced", "", "Advanced HDR\nPeak brightness, paper white and colour."));
    s.rows.push_back(row(SetKind::Text, 10, "Server", "party.schindlershadow.com:27500", "Party server\nPress to edit."));
    s.rows.push_back(row(SetKind::Action, 11, "Sync Now", "", "Sync now\nUpload changed saves."));
    s.rows.push_back(row(SetKind::Info, 12, "Status", "ok", "Status\nAll good."));
    s.rows.push_back(row(SetKind::Danger, 13, "Quit Game", "", "Quit\nClose the game."));
    s.sel = 6;
    return s;
}
static void dump(const char* dir, const char* name, const Canvas& c) {
    if (!dir) return;
    std::string p = std::string(dir) + "/" + name + ".ppm";
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", c.w, c.h);
    for (uint32_t px : c.px) { unsigned char rgb[3] = {(unsigned char)(px & 255), (unsigned char)((px >> 8) & 255), (unsigned char)((px >> 16) & 255)}; std::fwrite(rgb, 1, 3, f); }
    std::fclose(f);
}

int main() {
    const char* dir = std::getenv("DUMP_SETTINGS");
    const int scale = 2;
    SettingsView v;
    SetScene s = scene();
    double t = 0;
    v.set_open(true);
    Canvas c(scale);
    v.render(c, s, t);
    dump(dir, "open0", c);
    for (int i = 0; i < 8; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "open_mid", c);
    for (int i = 0; i < 100; ++i) { t += 16; v.render(c, s, t); }   // the open animation takes 1.2 s
    dump(dir, "settled", c);
    CHECK(!v.opening_or_closing());
    const SetGeom& g = v.geom();
    CHECK(g.cats.size() == 10);
    CHECK(!g.rows.empty());
    CHECK(g.back.w > 0);
    // every visible row has a rect inside the list; the selected one is visible
    bool sel_seen = false;
    for (auto& r : g.rows) { if (r.idx == s.sel) sel_seen = true; CHECK(r.r.w > 0 && r.r.y + r.r.h > g.list.y); }
    CHECK(sel_seen);
    // a slider row exposes a track and arrows
    for (auto& r : g.rows) if (s.rows[size_t(r.idx)].kind == SetKind::Slider) { CHECK(r.track.w > 10); CHECK(r.dec.w > 0 && r.inc.w > 0); }
    // selecting a row near the bottom scrolls it into view
    s.sel = int(s.rows.size()) - 1;
    for (int i = 0; i < 60; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "scrolled", c);
    sel_seen = false;
    for (auto& r : v.geom().rows) if (r.idx == s.sel) sel_seen = true;
    CHECK(sel_seen);
    CHECK(v.scroll_max() >= 0.f);
    // hover / press / device variants
    s.sel = 2; s.hover_row = 3; s.hover_part = 2; s.device = 1;
    for (int i = 0; i < 30; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "mouse", c);
    s.device = 2;
    for (int i = 0; i < 4; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "touch", c);
    s.device = 0;
    s.cat = 5; v.page_changed(1);
    for (int i = 0; i < 5; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "pagechange", c);
    // keyboard modal
    s.kb.active = true; s.kb.title = "Server"; s.kb.text = "party.schindlershadow.com:27500"; s.kb.row = 1; s.kb.col = 3;
    for (int i = 0; i < 30; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "keyboard", c);
    CHECK(v.geom().keys.size() == 5);
    CHECK(v.geom().keys[0].size() == 10 && v.geom().keys[4].size() == 6);
    CHECK(v.geom().keys[1][3].r.w > 5);
    s.kb.active = false; s.kb.symbols = true;
    for (int i = 0; i < 30; ++i) { t += 16; v.render(c, s, t); }
    // close
    v.set_open(false);
    for (int i = 0; i < 6; ++i) { t += 16; v.render(c, s, t); }
    dump(dir, "closing", c);
    for (int i = 0; i < 60; ++i) { t += 16; v.render(c, s, t); }
    CHECK(v.closed());
    // all frame times (informational)
    {
        v.set_open(true);
        s = scene();
        for (int sc = 1; sc <= 4; ++sc) {
            Canvas c3(sc);
            double a = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 60; ++i) { a += 16; v.render(c3, s, a); }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60.0;
            printf("render scale %d: %.1f ms/frame\n", sc, ms);
        }
    }
    if (g_fail) { printf("test_settings_ui: %d FAILED\n", g_fail); return 1; }
    printf("test_settings_ui: all passed\n");
    return 0;
}
