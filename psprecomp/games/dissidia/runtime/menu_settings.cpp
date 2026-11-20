// games/dissidia/runtime/menu_settings.cpp — a "Settings" entry in the game's own menus (battle pause menu, start menu, title
// screen) that opens the runtime's settings screen (psp_settings_request_open). PSPRECOMP_MENU_SETTINGS=0 turns them off.
// The pause menu's row is made by the game's own list widget; the start menu and title rows are drawn on the game layer
// (menu_layer.h) in the measured retail style, with the game's own layout moved to make room.
//
// Battle pause menu
//   ovl_battle FUN_08B1EBE8 builds the pause list for the battle's mode with the generic EBOOT list widget:
//   FUN_088D72E8(list, label id, help id, 0, 0, 0) adds an item (the widget holds up to 7, count at list+92+480), then
//   FUN_088D743C opens it. The wrapper adds one more item just before the open. When the list opens, every label and the
//   help line are made with FUN_0887AD78(bank, 0, id, ...) -> FUN_0887D868(text system, ..., UTF-16 string, ...); a
//   marker id makes the accessor hook create the text from a host string instead (the same call with our buffer).
//   The selection core (list + 36) registers a confirm in its update FUN_088D80AC; FUN_088D75F0(list) is the label id of
//   the selected row. A confirm of our row is taken back there and the settings screen opens over the paused game.
#include "psp_runtime.h"
#include "recomp.h"
#include "psp_settings.h"
#include "hle/psp_hle.h"
#include "hle/psp_hle_adhoc.h"
#include "gim_decode.h"
#include "menu_layer.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include "package_mods.h"
#include "psp_tex_pack.h"
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

std::string dissidia_package_dir();   // hooks_io.cpp
std::string dissidia_mods_dir();
extern FuncPtr dissidia_ovl_battle_lookup(uint32_t vaddr);
extern FuncPtr dissidia_ovl_menu_lookup(uint32_t vaddr);
void dissidia_presence_title();            // discord_presence.cpp: where the player is
void dissidia_presence_main(uint32_t state);
void dissidia_presence_sub(uint32_t state);

namespace {

constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t PAUSE_BUILD = 0x08B1EBE8u;   // ovl_battle: pause list for the current mode
constexpr uint32_t LIST_ADD = 0x088D72E8u;      // EBOOT list widget: add item (list, label, help, 0, 0, 0)
constexpr uint32_t LIST_OPEN = 0x088D743Cu;     // EBOOT list widget: open (list, ?, start label, 0, flag)
constexpr uint32_t CORE_UPDATE = 0x088D80ACu;   // EBOOT list widget: selection core update (pad input, confirm / cancel)
constexpr uint32_t LIST_VALUE = 0x088D75F0u;    // EBOOT list widget: label id of the selected row (-1 none)
constexpr uint32_t LIST_COUNT = 0x088D768Cu;    // EBOOT list widget: item count
constexpr uint32_t TEXT_GET = 0x0887AD78u;      // EBOOT text bank accessor: (bank, kind, id, a3, t0, t1) -> text object
constexpr uint32_t TEXT_MAKE = 0x0887D868u;     // EBOOT: text object from a UTF-16 string
constexpr uint32_t TEXT_SYS = 0x089D6998u;      // EBOOT: pointer to the text system (first argument of TEXT_MAKE)
constexpr int kMaxItems = 7;

constexpr int32_t ID_LABEL = 0x7E5E;            // marker ids: no retail bank is that large
constexpr int32_t ID_HELP = 0x7E5F;
constexpr uint32_t STR_LABEL = 0x08701D00u;     // guest UTF-16 buffers (scratch area, see roster_max.h)
constexpr uint32_t STR_HELP = 0x08701D40u;

const char* const kLabel = "Settings";
const char* const kHelp = "Display, graphics, controls and other settings of this port.";

uint8_t* g_ram = nullptr;
FuncPtr g_orig_build = nullptr, g_orig_open = nullptr, g_list_add = nullptr, g_orig_core_update = nullptr, g_list_value = nullptr,
        g_list_count = nullptr, g_orig_text = nullptr, g_text_make = nullptr;
bool g_in_pause_build = false;
uint32_t g_pause_list = 0;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
uint32_t overlay_id() { return r32(OVL_BASE); }

void put_utf16(uint32_t at, const char* s, size_t cap_units) {
    uint8_t* p = g_ram + (at & 0x07FFFFFFu);
    size_t i = 0;
    for (; s[i] && i + 1 < cap_units; i++) { p[2 * i] = static_cast<uint8_t>(s[i]); p[2 * i + 1] = 0; }
    p[2 * i] = p[2 * i + 1] = 0;
}

// Calls a guest function with the given arguments, leaving the caller's registers as they were (only v0 is returned).
int32_t call(FuncPtr f, uint8_t* rdram, recomp_context* ctx, std::initializer_list<int32_t> args) {
    recomp_context save = *ctx;
    int reg = 4;
    for (int32_t a : args) ctx->r[reg++] = a;
    f(rdram, ctx);
    const int32_t v0 = ctx->r[2];
    *ctx = save;
    return v0;
}

void pause_build_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 1) { if (FuncPtr f = dissidia_ovl_menu_lookup(PAUSE_BUILD)) f(rdram, ctx); return; }
    g_in_pause_build = true;
    g_orig_build(rdram, ctx);
    g_in_pause_build = false;
}

void list_open_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_in_pause_build) {
        g_in_pause_build = false;
        const int32_t list = ctx->r[4];
        const int32_t n = call(g_list_count, rdram, ctx, {list});
        // Not in a wireless match (the settings screen gives the game back at once there) and only while the widget has room.
        if (n > 0 && n < kMaxItems && !psp_adhoc_connected() && !psp_adhoc_in_session()) {
            if (call(g_list_add, rdram, ctx, {list, ID_LABEL, ID_HELP, 0, 0, 0}) == 0) g_pause_list = static_cast<uint32_t>(list);
        }
    }
    g_orig_open(rdram, ctx);
}

// The selection core's per-frame update (list + 36; +12 state, 1 = taking input; +16 event, 1 = confirmed; +20 cursor) is
// where a confirm is registered (and the game's confirm sound played). Right after it, before the list widget reacts
// (its update starts the closing animation on a confirm), a confirm of our row is taken back and the settings screen opens.
void core_update_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t core = static_cast<uint32_t>(ctx->r[4]);
    g_orig_core_update(rdram, ctx);
    if (g_pause_list == 0 || core != g_pause_list + 36u || r32(core + 16) != 1) return;
    if (call(g_list_value, rdram, ctx, {static_cast<int32_t>(g_pause_list)}) != ID_LABEL) return;
    const uint32_t active = 1, none = 0;
    std::memcpy(rdram + (core & 0x07FFFFFFu) + 12, &active, 4);
    std::memcpy(rdram + (core & 0x07FFFFFFu) + 16, &none, 4);
    if (!psp_settings_menu_open()) {
        std::fprintf(stderr, "[MENUSET] Settings chosen in the pause menu\n");
        psp_settings_request_open();
    }
}

void text_get_hook(uint8_t* rdram, recomp_context* ctx) {
    const int32_t id = ctx->r[6];
    if ((id == ID_LABEL || id == ID_HELP) && g_text_make) {
        const uint32_t buf = id == ID_LABEL ? STR_LABEL : STR_HELP;
        put_utf16(buf, id == ID_LABEL ? kLabel : kHelp, id == ID_LABEL ? 32 : 96);
        // as FUN_0887AD78 does after decoding the record: FUN_0887D868(text system, kind, string, 0, 0, a3, t0, 0)
        const int32_t kind = ctx->r[5], a3 = ctx->r[7], t0 = ctx->r[8];
        ctx->r[4] = static_cast<int32_t>(r32(TEXT_SYS)); ctx->r[5] = kind; ctx->r[6] = static_cast<int32_t>(buf);
        ctx->r[7] = 0; ctx->r[8] = 0; ctx->r[9] = a3; ctx->r[10] = t0; ctx->r[11] = 0;
        g_text_make(rdram, ctx);
        return;
    }
    g_orig_text(rdram, ctx);
}


// ---- Start menu (main menu) ------------------------------------------------------------------------------------------
// The start menu (menu overlay) lists 7 modes; their positions are nodes 0..6 of entry 0 of
// menu/menu_pk_upper_start_menu_seq_0.bin (int16 x, y at entry offset 1630 + 104 * row, and again 40 bytes later: the node's second keyframe). The rows are pulled 21 px apart
// instead of 24 (x follows the original curve) so an eighth row fits above the help bar. The cursor is the generic cursor
// helper FUN_088D688C on state + 0xA28 (+4 index, +32 min, +36 max), state = [menu + 320]; its max is raised from 6 to 7.
// Index 7 is "Settings": the game draws no row there (it has 7 row objects), the runtime layer draws it, and a confirm on it
// (ovl_menu FUN_089E45E8 would index its 7-entry mode table) opens the settings screen instead.
constexpr uint32_t MAIN_TICK = 0x089E45E8u;     // ovl_menu: top-level start menu update (input, confirm)
constexpr uint32_t PAD_OBJ = 0x088729A0u;       // EBOOT: pad object (a0 = 0)
constexpr uint32_t CONFIRM_ARGS = 0x089E7298u;  // ovl_menu: the two words FUN_089E45E8 passes to the pad's "pressed" test for confirm
constexpr uint32_t PLAY_SE = 0x088D4BF4u;       // EBOOT: menu sound effect (1 cursor, 2 confirm)
constexpr const char* kStartSeq = "menu/menu_pk_upper_start_menu_seq_0.bin";
constexpr int kRowNodeOff = 1630, kRowNodeStride = 104, kKeyStride = 40;   // each node has two keyframes with the position
constexpr int16_t kRowX[8] = {204, 207, 211, 217, 226, 237, 252, 271};
constexpr int16_t kRowY0 = 60, kRowPitch = 21;

FuncPtr g_orig_main_tick = nullptr, g_pad_obj = nullptr, g_play_se = nullptr;
uint32_t g_main_state = 0;       // start menu state block while the top level is on screen (0 otherwise)
int g_main_seen_frames = 0;

bool edit_start_seq(std::vector<uint8_t>& pak) {
    const uint8_t* p = nullptr; size_t len = 0;
    if (!dissidia::menu_pak_entry(pak.data(), pak.size(), 0, &p, &len) || len < size_t(kRowNodeOff + 7 * kRowNodeStride + 4)) return false;
    uint8_t* e = pak.data() + (p - pak.data());
    static const int16_t retail[7][2] = {{204, 60}, {207, 84}, {213, 108}, {220, 132}, {232, 156}, {248, 180}, {268, 204}};
    for (int r = 0; r < 7; r++)   // only a retail layout is touched
        for (int k : {0, kKeyStride}) {
            int16_t xy[2]; std::memcpy(xy, e + kRowNodeOff + kRowNodeStride * r + k, 4);
            if (xy[0] != retail[r][0] || xy[1] != retail[r][1]) return false;
        }
    for (int r = 0; r < 7; r++)
        for (int k : {0, kKeyStride}) {
            const int16_t xy[2] = {kRowX[r], static_cast<int16_t>(kRowY0 + kRowPitch * r)};
            std::memcpy(e + kRowNodeOff + kRowNodeStride * r + k, xy, 4);
        }
    return true;
}

// The pad's "pressed" test exactly as FUN_089E45E8 calls it for confirm (it follows the Cross / Circle swap).
bool confirm_pressed(uint8_t* rdram, recomp_context* ctx) {
    const int32_t pad = call(g_pad_obj, rdram, ctx, {0});
    const uint32_t vt = r32(static_cast<uint32_t>(pad) + 52) + 16;
    const int16_t adj = static_cast<int16_t>(r32(vt) & 0xFFFF);
    const uint32_t fn = r32(vt + 4);
    recomp_context save = *ctx;
    ctx->r[4] = pad + adj; ctx->r[5] = static_cast<int32_t>(fn);
    ctx->r[6] = static_cast<int32_t>(r32(CONFIRM_ARGS)); ctx->r[7] = static_cast<int32_t>(r32(CONFIRM_ARGS + 4));
    RECOMP_LOOKUP(fn)(rdram, ctx);
    const bool on = ctx->r[2] != 0;
    *ctx = save;
    return on;
}


// ---- The Settings row of the start menu, drawn on the game layer ----------------------------------------------------
// Measured from the retail rows (the Options row, layout origin = its node): text in the game's bold face at 8.6 units, left at
// origin + 28, baseline + 9.2, colour (40, 42, 65); idle: a 0.75-unit line (119, 114, 149) at + 11.3 from + 10.5 to + 206 (fading
// out over the last 40) with a soft white dot at its start; selected: a royal-blue band (63, 53, 174) from - 2 to + 12 that fades in over the
// first 30 units and lightens towards (134, 136, 207) at the right edge, white text with a dark edge, the pointing hand
// (common UI sheet, native size) at - 10, and the row's help text in the help bar (bold 7.9, centred at 232, baseline 239.3).
dissidia::MenuLayer g_layer;
dissidia::GimImage g_ui_sheet;        // menu/en/menu_lpk_label_gim_0.bin entry 0 (128x128): the pointing hand at (2, 50, 38, 24)
int g_ui_sheet_state = 0;
// The same sheet's texture-pack id (psp_texpack_make_id of its texels; tools/texpack_extract.py prints it for the GIM). With an
// HD pack (our id-named files or a PPSSPP-style pack the player installed) the game draws its hand from the replacement, so the
// rows added here take the image the game got (psp_texpack_watch) and fall back to the original sheet.
constexpr uint64_t kUiSheetTexId = 0xC6F05E9985EEBE31ull;
std::vector<uint8_t> g_ui_hd;
int g_ui_hd_w = 0, g_ui_hd_h = 0;
bool g_ui_hd_ok = false;

// The pointing hand at native size (36 x 24 design units; wider would take in the next sprite of an HD sheet, the game draws it 1:1), from the HD image when the game has one.
void draw_hand(float x, float y, float alpha) {
    if (!g_ui_hd_ok && psp_texpack_watched(kUiSheetTexId, g_ui_hd, g_ui_hd_w, g_ui_hd_h) && g_ui_hd_w >= 128 && g_ui_hd_h >= 128) {
        g_ui_hd_ok = true;
        std::fprintf(stderr, "[MENUSET] pointing hand from the HD texture (%dx%d)\n", g_ui_hd_w, g_ui_hd_h);
    }
    if (g_ui_hd_ok) {
        const float fx = g_ui_hd_w / 128.f, fy = g_ui_hd_h / 128.f;
        g_layer.image(reinterpret_cast<const uint32_t*>(g_ui_hd.data()), g_ui_hd_w, g_ui_hd_h, int(2 * fx), int(50 * fy), int(36 * fx), int(24 * fy),
                      x, y, 36.f, 24.f, alpha);
    } else if (g_ui_sheet_state == 1) {
        g_layer.image(g_ui_sheet.rgba.data(), g_ui_sheet.w, g_ui_sheet.h, 2, 50, 36, 24, x, y, 36.f, 24.f, alpha);
    }
}
float g_sel_anim = 0.f;               // 0..1, the selected look fading in
std::chrono::steady_clock::time_point g_last_draw;
const char* const kMainHelp = "Display, graphics, controls and other settings of this port.";

void load_ui_sheet() {
    if (g_ui_sheet_state != 0) return;
    g_ui_sheet_state = -1;
    std::vector<uint8_t> bytes;
    const std::string dir = dissidia_package_dir();
    if (dir.empty()) { g_ui_sheet_state = 0; return; }   // try again once the game has opened PACKAGE.BIN
    const uint8_t* p = nullptr; size_t len = 0;
    if (dissidia::read_asset(dir, dissidia_mods_dir(), "menu/en/menu_lpk_label_gim_0.bin", &bytes) &&
        dissidia::menu_pak_entry(bytes.data(), bytes.size(), 0, &p, &len) && len && dissidia::gim_decode(p, len, 0, &g_ui_sheet))
        g_ui_sheet_state = 1;
}

void draw_settings_row(float ox, float oy, bool selected, float alpha) {
    using dissidia::LayerColor;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last_draw).count());   // 0.1 = first frame after a gap
    g_last_draw = now;
    static float appear = 0.f;   // fades in when the top level comes back (after a gap the row was not drawn)
    appear = dt >= 0.1f ? 0.f : std::min(1.f, appear + dt / 0.2f);
    alpha *= appear;
    g_sel_anim = selected ? std::min(1.f, g_sel_anim + dt / 0.12f) : 0.f;
    load_ui_sheet();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    const float sa = g_sel_anim * alpha, ia = (1.f - g_sel_anim) * alpha;
    if (ia > 0.f) {   // idle: the line with its dot
        g_layer.band(ox + 10.5f, oy + 10.9f, ox + 206.f, oy + 11.65f, [&](float x, float) {
            const float fade = x < ox + 166.f ? 1.f : std::max(0.f, 1.f - (x - (ox + 166.f)) / 40.f);
            return LayerColor{119, 114, 149, ia * fade};
        });
        g_layer.glow(ox + 10.5f, oy + 11.3f, 2.6f, LayerColor{255, 255, 255, 0.95f * ia});
    }
    if (sa > 0.f) {   // selected: the band
        g_layer.band(ox - 15.f, oy - 2.f, 480.f, oy + 12.f, [&](float x, float y) {
            const float ramp = std::min(1.f, std::max(0.f, (x - (ox - 15.f)) / 30.f));
            const float t = x < 390.f ? 0.f : std::min(1.f, (x - 390.f) / 90.f);
            float r = 63 + (134 - 63) * t, g = 53 + (136 - 53) * t, b = 174 + (207 - 174) * t;
            if (y < oy - 1.f) { r += 30; g += 33; b += 11; }          // lighter top edge
            else if (y > oy + 11.f) { r += 7; g += 8; b -= 9; }       // darker bottom edge
            return LayerColor{r, g, b, sa * ramp};
        });
    }
    const float tx = ox + 28.f, base = oy + 9.2f, em = 8.6f;
    if (ia > 0.f) g_layer.text(kLabel, true, em, tx, base, LayerColor{40, 42, 65, ia});
    if (sa > 0.f) {
        for (const auto& o : {std::pair<float, float>{0.6f, 0.6f}, {0.6f, 0.f}, {0.f, 0.6f}, {-0.4f, 0.f}, {0.f, -0.4f}})
            g_layer.text(kLabel, true, em, tx + o.first, base + o.second, LayerColor{30, 24, 80, 0.75f * sa});
        g_layer.text(kLabel, true, em, tx, base, LayerColor{255, 255, 255, sa});
        draw_hand(ox - 10.f, oy - 2.3f, sa);
        const float hw = g_layer.text(kMainHelp, true, 7.9f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(kMainHelp, true, 7.9f, 232.f - hw / 2.f, 239.3f, LayerColor{108, 102, 128, sa});
    }
    g_layer.publish();
}

void main_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 2) { if (FuncPtr f = dissidia_ovl_battle_lookup(MAIN_TICK)) f(rdram, ctx); return; }
    const uint32_t state = r32(static_cast<uint32_t>(ctx->r[4]) + 320);
    const uint32_t cur = state + 0xA28u;
    if (state && r32(cur + 32) == 0 && r32(cur + 36) == 6) {   // the top level's range: one more row
        const uint32_t seven = 7;
        std::memcpy(rdram + ((cur + 36) & 0x07FFFFFFu), &seven, 4);
    }
    g_main_state = state;
    g_main_seen_frames = 0;
    dissidia_presence_main(state);
    if (state && r32(cur + 4) == 7 && r32(cur + 36) == 7 && !psp_settings_menu_open() && confirm_pressed(rdram, ctx)) {
        call(g_play_se, rdram, ctx, {2});
        std::fprintf(stderr, "[MENUSET] Settings chosen in the start menu\n");
        psp_settings_request_open();
        return;   // this frame's update is skipped: the retail code would read past its 7-entry mode table
    }
    const uint32_t menu = static_cast<uint32_t>(ctx->r[4]);
    g_orig_main_tick(rdram, ctx);
    // The eighth row follows the Options row (instance 0xE of the start menu scene, handle state + 0x408 + 4 * 0xE; float origin
    // at +0x10 / +0x14), one row further down the curve.
    const uint32_t st = r32(menu + 320);
    if (st && r32(st + 0xA28 + 36) == 7) {
        const uint32_t inst = r32(st + 0x408 + 4 * 0xE);
        float ox = kRowX[6], oy = kRowY0 + kRowPitch * 6;
        if (inst >= 0x08800000u && inst < 0x0A000000u) { std::memcpy(&ox, rdram + ((inst + 0x10) & 0x07FFFFFFu), 4); std::memcpy(&oy, rdram + ((inst + 0x14) & 0x07FFFFFFu), 4); }
        if (std::isfinite(ox) && std::isfinite(oy) && ox > 100.f && ox < 400.f && oy > 0.f && oy < 260.f)
            draw_settings_row(ox + float(kRowX[7] - kRowX[6]), oy + float(kRowPitch), r32(st + 0xA28 + 4) == 7, 1.f);
    }
}


// The start menu's sub-lists (group at state + 0xA88: 1 Battle Mode, 2 Communications Mode; cursor helper at state + 0xA58, +4 index)
// are updated by ovl_menu FUN_089E4A60; the row the cursor is on is reported for Discord (discord_presence.cpp).
constexpr uint32_t SUB_TICK = 0x089E4A60u;
FuncPtr g_orig_sub_tick = nullptr;
void sub_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 2) { if (FuncPtr f = dissidia_ovl_battle_lookup(SUB_TICK)) f(rdram, ctx); return; }
    const uint32_t menu = static_cast<uint32_t>(ctx->r[4]);
    dissidia_presence_sub(r32(menu + 320));
    g_orig_sub_tick(rdram, ctx);
}


// ---- Title screen ----------------------------------------------------------------------------------------------------
// NEW GAME / LOAD GAME / EXTRAS are artwork rows (ovl_menu FUN_089DF14C, state T = [title + 476]: cursor T + 76, row count
// T + 0x3FAA8, row instances T + 56 + 4 * row, the selection bar and hand = instance T + 52). Their layout is entry 0 of
// menu/en/menu_lpk_title_seq_0.bin: row nodes (455, 224), (439, 240), (423, 256) (with their keyframes and the intro
// keyframes one row lower) and the bar's per-row nodes (240, 220 / 237 / 254). Everything moves up 16 so a fourth row fits
// where EXTRAS was; the fourth row's label is drawn on the game layer in matching lettering (colour taken from the title
// sheet the game uses), its underline copied from another row of the game's picture.
// The game keeps three rows. "Settings selected" is a state of this module with the game's cursor on EXTRAS and the game's bar
// and hand (instance T + 52, origin = the selected row's node) moved onto the fourth row, so they are the game's own, in any
// texture pack: Down on EXTRAS or Up on NEW GAME enters it (the frame's update is
// skipped so the game does not wrap), Up
// leaves it by putting the cursor on NEW GAME and letting the game's own Up move to EXTRAS (its sound and bar animation),
// Down by letting the game's Down wrap from EXTRAS to NEW GAME, and Confirm / START open the settings screen.
constexpr uint32_t TITLE_TICK = 0x089DF14Cu;    // ovl_menu: title menu update
constexpr uint32_t PAD_OBJ2 = 0x08872088u;      // EBOOT: second pad object (the title's START test)
constexpr uint32_t TITLE_MASKS = 0x089E07B8u;   // ovl_menu: START, Confirm, Up, Down masks (two words each)
constexpr const char* kTitleSeq = "menu/en/menu_lpk_title_seq_0.bin";
FuncPtr g_orig_title_tick = nullptr, g_pad_obj2 = nullptr;
bool g_title_settings = false;
uint32_t g_title_bar = 0;   // the bar instance hidden while Settings is selected

std::chrono::steady_clock::time_point g_title_last;

bool edit_title_seq(std::vector<uint8_t>& pak) {
    const uint8_t* p = nullptr; size_t len = 0;
    if (!dissidia::menu_pak_entry(pak.data(), pak.size(), 0, &p, &len)) return false;
    uint8_t* e = pak.data() + (p - pak.data());
    struct Node { int off; int16_t x, y; };
    static const Node nodes[] = {
        {4490, 455, 224}, {4550, 455, 224}, {4590, 455, 224}, {5498, 455, 224}, {5538, 455, 224}, {4654, 455, 240},
        {4694, 439, 240}, {4754, 439, 240}, {4794, 439, 240}, {5602, 439, 240}, {5642, 439, 240}, {4858, 439, 256},
        {4898, 423, 256}, {5706, 423, 256}, {5746, 423, 256},
        {16942, 240, 220}, {16982, 240, 220}, {17270, 240, 220}, {17310, 240, 220},
        {17046, 240, 237}, {17086, 240, 237}, {17374, 240, 237}, {17414, 240, 237},
        {17150, 240, 254}, {17190, 240, 254}, {17478, 240, 254}, {17518, 240, 254}};
    for (const Node& n : nodes) {   // a retail layout only
        if (size_t(n.off) + 4 > len) return false;
        int16_t xy[2]; std::memcpy(xy, e + n.off, 4);
        if (xy[0] != n.x || xy[1] != n.y) return false;
    }
    for (const Node& n : nodes) { const int16_t y = int16_t(n.y - 16); std::memcpy(e + n.off + 2, &y, 2); }
    return true;
}

// The pad tests FUN_089DF14C makes: `which` 0 START, 1 Confirm (pressed), 2 Up, 3 Down (with key repeat).
bool title_pad(uint8_t* rdram, recomp_context* ctx, int which) {
    const int32_t pad = call(which == 0 ? g_pad_obj2 : g_pad_obj, rdram, ctx, {0});
    const uint32_t vt = r32(static_cast<uint32_t>(pad) + 52) + (which >= 2 ? 32 : 16);
    const int16_t adj = static_cast<int16_t>(r32(vt) & 0xFFFF);
    const uint32_t fn = r32(vt + 4), m = TITLE_MASKS + 8u * uint32_t(which);
    recomp_context save = *ctx;
    ctx->r[4] = pad + adj; ctx->r[5] = static_cast<int32_t>(fn);
    ctx->r[6] = static_cast<int32_t>(r32(m)); ctx->r[7] = static_cast<int32_t>(r32(m + 4));
    RECOMP_LOOKUP(fn)(rdram, ctx);
    const bool on = ctx->r[2] != 0;
    *ctx = save;
    return on;
}

// The title sheet (menu/en/menu_lpk_title_gim_0.bin entry 0, 512x512) as the game draws it: its labels give the lettering colour
// of the added row (navy on retail and on our upscale, whatever a PPSSPP-style pack drew, e.g. white on a black title).
constexpr uint64_t kTitleSheetTexId = 0x585CA08F8A5250D3ull;
float g_title_ink[3] = {66, 66, 118};
float g_title_scale = 1.f, g_title_dy = 0.f;   // lettering size and baseline shift of the sheet in use (1, 0 on retail)
bool g_title_ink_done = false;

void title_ink_update() {
    if (g_title_ink_done) return;
    std::vector<uint8_t> px; int w = 0, h = 0;
    if (!psp_texpack_watched(kTitleSheetTexId, px, w, h) || w < 512 || h < 512) return;   // retail colours until an HD image shows up
    g_title_ink_done = true;
    const float f = w / 512.f;
    std::vector<std::array<float, 4>> ink;   // r, g, b, luminance of the opaque label pixels (NEW GAME / LOAD GAME / EXTRAS)
    for (int y = int(443 * f); y < int(495 * f); y++)
        for (int x = int(439 * f); x < int(505 * f); x++) {
            const uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            if (p[3] > 240) ink.push_back({float(p[0]), float(p[1]), float(p[2]), 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]});
        }
    if (ink.size() < 50) return;
    {   // the labels' size: total width of the three (retail 42 + 58 + 63 sheet units) and the bottom of NEW GAME (retail row 473)
        float wsum = 0, bottom = -1;
        static const int cells[3][2] = {{440, 458}, {458, 478}, {478, 500}};
        for (int c = 0; c < 3; c++) {
            int x0 = 1 << 30, x1 = -1, y1 = -1;
            for (int y = int(cells[c][0] * f); y < int(cells[c][1] * f); y++)
                for (int x = int(435 * f); x < int(512 * f) && x < w; x++)
                    if (px[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] > 128) { x0 = std::min(x0, x); x1 = std::max(x1, x); y1 = std::max(y1, y); }
            if (x1 >= x0) wsum += (x1 - x0 + 1) / f;
            if (c == 1 && y1 >= 0) bottom = (y1 + 1) / f;
        }
        if (wsum > 40.f) g_title_scale = std::min(1.5f, std::max(0.4f, wsum / 163.f));
        if (bottom > 0.f) g_title_dy = bottom - 474.f;
    }
    std::sort(ink.begin(), ink.end(), [](const auto& a, const auto& b) { return a[3] < b[3]; });
    float mean = 0; for (const auto& k : ink) mean += k[3]; mean /= float(ink.size());
    const auto& pick = ink[size_t(float(ink.size() - 1) * (mean < 128.f ? 0.15f : 0.85f))];   // the body of dark or of light lettering
    for (int i = 0; i < 3; i++) g_title_ink[i] = pick[i];
    std::fprintf(stderr, "[MENUSET] title lettering from the HD sheet: colour %.0f %.0f %.0f, size x%.2f, baseline %+.1f\n", g_title_ink[0], g_title_ink[1], g_title_ink[2], g_title_scale, g_title_dy);
}

// The fourth row: its label, and the underline copied from a row of the game's own picture (src node: a retail row that is
// not selected), so the line looks like the others whatever texture pack draws them. The bar and hand are the game's own
// (title_tick_hook moves them here).
void draw_title_row(float nx, float ny, float srcx, float srcy) {
    using dissidia::LayerColor;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_title_last).count());
    g_title_last = now;
    static float appear = 0.f;
    appear = dt >= 0.1f ? 0.f : std::min(1.f, appear + dt / 0.25f);
    title_ink_update();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    if (appear >= 1.f) g_layer.copy(srcx - 57.f, srcy + 5.5f, 479.f, srcy + 8.f, nx - 57.f, ny + 5.5f, 479.f, ny + 8.f);
    else g_layer.band(nx - 58.f, ny + 6.f, 480.f, ny + 7.f, [&](float, float) { return LayerColor{148, 145, 183, appear}; });
    g_layer.serif_caps(kLabel, 11.5f * g_title_scale, 0.8f, 0.45f * g_title_scale, nx - 44.f, ny + 3.f + g_title_dy, LayerColor{g_title_ink[0], g_title_ink[1], g_title_ink[2], appear});
    g_layer.publish();
}

void title_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 2) { if (FuncPtr f = dissidia_ovl_battle_lookup(TITLE_TICK)) f(rdram, ctx); return; }
    dissidia_presence_title();
    const uint32_t title = static_cast<uint32_t>(ctx->r[4]);
    const uint32_t st = r32(title + 476);
    const bool menu_up = st && r32(st + 12) == 0 && r32(st + 0x3FAA8) == 3 && r32(st + 64) != 0;
    auto leave = [&]() { g_title_settings = false; };
    if (!menu_up) { leave(); g_orig_title_tick(rdram, ctx); return; }
    const uint32_t cur = st + 76;
    if (g_title_settings && r32(cur) != 2) leave();
    if (!g_title_settings) {
        const bool down_from_last = r32(cur) == 2 && title_pad(rdram, ctx, 3);
        const bool up_from_first = !down_from_last && r32(cur) == 0 && title_pad(rdram, ctx, 2);
        if (!psp_settings_menu_open() && (down_from_last || up_from_first)) {   // Down on EXTRAS / Up on NEW GAME
            if (up_from_first) { const uint32_t two = 2; std::memcpy(rdram + (cur & 0x07FFFFFFu), &two, 4); }
            g_title_settings = true;
            call(g_play_se, rdram, ctx, {0});
            g_title_bar = r32(st + 52);
        } else {
            g_orig_title_tick(rdram, ctx);
        }
    } else if (psp_settings_menu_open()) {
        g_orig_title_tick(rdram, ctx);
    } else if (title_pad(rdram, ctx, 1) || title_pad(rdram, ctx, 0)) {          // Confirm / START: the settings screen
        call(g_play_se, rdram, ctx, {2});
        std::fprintf(stderr, "[MENUSET] Settings chosen on the title screen\n");
        psp_settings_request_open();
    } else if (title_pad(rdram, ctx, 2)) {                                        // Up: the game moves from NEW GAME to EXTRAS
        leave();
        const uint32_t zero = 0;
        std::memcpy(rdram + (cur & 0x07FFFFFFu), &zero, 4);
        g_orig_title_tick(rdram, ctx);
    } else if (title_pad(rdram, ctx, 3)) {                                        // Down: the game wraps from EXTRAS to NEW GAME
        leave();
        g_orig_title_tick(rdram, ctx);
    } else {
        g_orig_title_tick(rdram, ctx);
    }
    // The fourth row follows the EXTRAS row (instance T + 64, float origin at +0x10 / +0x14), one step further. While it is
    // selected the game's bar (instance T + 52, origin = the selected row's node) is moved onto it.
    const uint32_t row1 = r32(st + 60), row2 = r32(st + 64);
    auto origin = [&](uint32_t inst, float dx, float dy, float* x, float* y) {
        *x = dx; *y = dy;
        if (inst < 0x08800000u || inst >= 0x0A000000u) return false;
        std::memcpy(x, rdram + ((inst + 0x10) & 0x07FFFFFFu), 4); std::memcpy(y, rdram + ((inst + 0x14) & 0x07FFFFFFu), 4);
        return std::isfinite(*x) && std::isfinite(*y) && *x > 200.f && *x < 480.f && *y > 100.f && *y < 260.f;
    };
    float x2, y2, x1, y1;
    if (!origin(row2, 423.f, 240.f, &x2, &y2)) return;
    const float nx = x2 - 16.f, ny = y2 + 16.f;
    if (g_title_settings && g_title_bar >= 0x08800000u && g_title_bar < 0x0A000000u) {
        std::memcpy(rdram + ((g_title_bar + 0x10) & 0x07FFFFFFu), &nx, 4);
        std::memcpy(rdram + ((g_title_bar + 0x14) & 0x07FFFFFFu), &ny, 4);
    }
    // underline source: EXTRAS, or LOAD GAME while EXTRAS carries the bar
    const bool extras_lit = !g_title_settings && r32(cur) == 2;
    if (extras_lit && origin(row1, 439.f, 224.f, &x1, &y1)) draw_title_row(nx, ny, x1, y1);
    else draw_title_row(nx, ny, x2, y2);
}

}  // namespace

void dissidia_add_asset_patch(const std::string& asset, std::function<bool(std::vector<uint8_t>&)> edit);   // hooks_io.cpp

void dissidia_menu_settings_install(uint8_t* rdram) {
    g_ram = rdram;
    psp_texpack_watch(kUiSheetTexId);
    psp_texpack_watch(kTitleSheetTexId);
    if (const char* e = std::getenv("PSPRECOMP_MENU_SETTINGS")) if (e[0] == '0') return;
    g_orig_build = dissidia_ovl_battle_lookup(PAUSE_BUILD);
    g_orig_open = psp_dispatch_probe_lookup(LIST_OPEN);
    g_list_add = psp_dispatch_probe_lookup(LIST_ADD);
    g_orig_core_update = psp_dispatch_probe_lookup(CORE_UPDATE);
    g_list_value = psp_dispatch_probe_lookup(LIST_VALUE);
    g_list_count = psp_dispatch_probe_lookup(LIST_COUNT);
    g_orig_text = psp_dispatch_probe_lookup(TEXT_GET);
    g_text_make = psp_dispatch_probe_lookup(TEXT_MAKE);
    if (!g_orig_build || !g_orig_open || !g_list_add || !g_orig_core_update || !g_list_value || !g_list_count || !g_orig_text || !g_text_make) {
        std::fprintf(stderr, "[MENUSET] pause menu hooks not installed (a function is missing)\n");
        return;
    }
    psp_dispatch_register(TEXT_GET, text_get_hook);
    psp_dispatch_register(PAUSE_BUILD, pause_build_hook);
    psp_dispatch_register(LIST_OPEN, list_open_hook);

    g_orig_main_tick = dissidia_ovl_menu_lookup(MAIN_TICK);
    g_pad_obj = psp_dispatch_probe_lookup(PAD_OBJ);
    g_play_se = psp_dispatch_probe_lookup(PLAY_SE);
    g_orig_title_tick = dissidia_ovl_menu_lookup(TITLE_TICK);
    g_pad_obj2 = psp_dispatch_probe_lookup(PAD_OBJ2);
    g_pad_obj = psp_dispatch_probe_lookup(PAD_OBJ);
    g_play_se = psp_dispatch_probe_lookup(PLAY_SE);
    if (g_orig_title_tick && g_pad_obj && g_pad_obj2 && g_play_se) {
        dissidia_add_asset_patch(kTitleSeq, edit_title_seq);
        psp_dispatch_register(TITLE_TICK, title_tick_hook);
    } else {
        std::fprintf(stderr, "[MENUSET] title hooks not installed (a function is missing)\n");
    }
    if (g_orig_main_tick && g_pad_obj && g_play_se) {
        dissidia_add_asset_patch(kStartSeq, edit_start_seq);
        psp_dispatch_register(MAIN_TICK, main_tick_hook);
        g_orig_sub_tick = dissidia_ovl_menu_lookup(SUB_TICK);
        if (g_orig_sub_tick) psp_dispatch_register(SUB_TICK, sub_tick_hook);
    } else {
        std::fprintf(stderr, "[MENUSET] start menu hooks not installed (a function is missing)\n");
    }
    psp_dispatch_register(CORE_UPDATE, core_update_hook);
}
