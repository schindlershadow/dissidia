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

void dissidia_tagteam_menu_mode(bool on);   // tagteam.cpp: Battle Mode > Tag Team Battle
bool dissidia_tagteam_menu_mode_on();   // tagteam.cpp: the last Battle Mode / Communications entry chosen was Tag Team Battle
uint32_t dissidia_tagteam_raw_pad();
void dissidia_training_set(bool on); bool dissidia_training_chosen(); void dissidia_training_inject_cross();   // training.cpp
void dissidia_tagteam_modesel_open();   // tagteam_menu.cpp: the mode select screen after choosing Tag Team Battle
bool dissidia_tagteam_modesel_active();
int dissidia_tagteam_modesel_frame(uint8_t* rdram, recomp_context* ctx, bool online);   // 0 open, 1 chosen, 2 cancelled
void dissidia_tagteam_modesel_confirm(bool sp);
void dissidia_tagteam_modesel_draw_frozen(bool online);   // a Cross press for the start menu once the screen is closed
void dissidia_tagteam_lobby_pending(bool on);
uint32_t dissidia_tagteam_modesel_target(bool sp, uint32_t def);   // tagteam_menu.cpp: the list row a Mode Select choice starts (FFA pick: Quick Battle 1-on-1)
bool dissidia_tagteam_inject_old();
bool dissidia_tagteam_ffa_ps_return();
void dissidia_tagteam_main_tick_seen();   // the setup screen opens the lobby (or the Free-for-all players) once

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
void w32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
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
bool confirm_pressed_at(uint8_t* rdram, recomp_context* ctx, uint32_t args);
bool confirm_pressed(uint8_t* rdram, recomp_context* ctx) { return confirm_pressed_at(rdram, ctx, CONFIRM_ARGS); }
bool confirm_pressed_at(uint8_t* rdram, recomp_context* ctx, uint32_t args) {
    const int32_t pad = call(g_pad_obj, rdram, ctx, {0});
    const uint32_t vt = r32(static_cast<uint32_t>(pad) + 52) + 16;
    const int16_t adj = static_cast<int16_t>(r32(vt) & 0xFFFF);
    const uint32_t fn = r32(vt + 4);
    recomp_context save = *ctx;
    ctx->r[4] = pad + adj; ctx->r[5] = static_cast<int32_t>(fn);
    ctx->r[6] = static_cast<int32_t>(r32(args)); ctx->r[7] = static_cast<int32_t>(r32(args + 4));
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
void draw_hand_on(dissidia::MenuLayer& g_layer, float x, float y, float alpha) {
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
void load_ui_sheet();
void draw_hand(float x, float y, float alpha) { draw_hand_on(g_layer, x, y, alpha); }
float g_sel_anim = 0.f;               // 0..1, the selected look fading in
std::chrono::steady_clock::time_point g_last_draw;
std::chrono::steady_clock::time_point g_last_sub_tick;   // when the group sub-list's tick last ran (sub_tick_hook)
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

void rows_fade_tick();   // below
bool tag_row_confirm(uint8_t* rdram, recomp_context* ctx, uint32_t st, bool from_main = false);   // below (Battle Mode > Tag Team Battle): true = the mode select owns this frame, the start menu's update is skipped
extern bool g_ms_go, g_ms_reopen, g_ms_hold, g_ms_flow;   // (the tag menu flow flags, below)
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
    rows_fade_tick();
    dissidia_tagteam_main_tick_seen();
    // 25 Nov (user, RP6: "opening Communications opened the battle menu"): the tag menu flow's flags (g_ms_flow / g_ms_reopen / g_ms_hold / g_ms_go and the Tag Team Battle mode)
    // outlived the flow when it was left some other way than the Mode Select's Circle (after a battle, ...), and the next time a group's list opened the Mode Select came back.
    // The top level of the start menu running means no flow is on. PSPRECOMP_TAGTEAM_FLOWRESET_OFF=1: old.
    if (state && (g_ms_flow || g_ms_reopen || g_ms_hold || g_ms_go) && !dissidia_tagteam_modesel_active() && !std::getenv("PSPRECOMP_TAGTEAM_FLOWRESET_OFF") &&
        std::chrono::steady_clock::now() - g_last_sub_tick > std::chrono::milliseconds(500)) {
        std::fprintf(stderr, "[MENUSET] start menu top level: tag menu flow flags cleared (flow %d reopen %d hold %d go %d)\n", g_ms_flow, g_ms_reopen, g_ms_hold, g_ms_go);
        g_ms_flow = g_ms_reopen = g_ms_hold = g_ms_go = false; dissidia_tagteam_menu_mode(false);
    }
    dissidia_presence_main(state);
    if (state && r32(cur + 4) == 7 && r32(cur + 36) == 7 && !psp_settings_menu_open() && confirm_pressed(rdram, ctx)) {
        call(g_play_se, rdram, ctx, {2});
        std::fprintf(stderr, "[MENUSET] Settings chosen in the start menu\n");
        psp_settings_request_open();
        return;   // this frame's update is skipped: the retail code would read past its 7-entry mode table
    }
    const uint32_t menu = static_cast<uint32_t>(ctx->r[4]);
    if (tag_row_confirm(rdram, ctx, state, true)) return;
    g_orig_main_tick(rdram, ctx);
    if (state && std::getenv("PSPRECOMP_MENU_CURDUMP")) {   // debug: cursor helpers (+4 index, +32 min, +36 max) in the start menu state
        static std::string last; std::string now; char b[64];
        for (uint32_t o = 0; o < 0x4000u; o += 4) { const uint32_t mn = r32(state + o + 32), mx = r32(state + o + 36), ix = r32(state + o + 4);
            if (mn == 0 && mx >= 2 && mx <= 9 && ix <= mx) { std::snprintf(b, sizeof b, " +%X:%u/%u", o, ix, mx); now += b; } }
        if (now != last) { last = now; std::fprintf(stderr, "[CURDUMP] state %08X:%s\n", state, now.c_str()); }
    }
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


// ---- Battle Mode > Tag Team Battle -------------------------------------------------------------------------------------
// The start menu's Battle Mode sub-list (group 1, state + 0xA88) has four rows (scene instances 0x17..0x1A of the start menu scene, origins
// (271, 101 + 16 * row)); its cursor is the cursor helper at state + 0xA58 (+4 index, +36 max). ovl_menu FUN_089E4A60 is the sub-list's
// update: on confirm it starts the mode of row (group, index) from its table at 0x089E7518. A fifth row "Tag Team Battle" is drawn on the
// game layer under Labyrinth; the cursor range is raised to 4, and a confirm on it switches the tag team mode on and lets the game start
// Quick Battle (Party) (index 1); any other row switches it off (a plain group battle is retail).
constexpr uint32_t SUB_TICK = 0x089E4A60u;      // ovl_menu: start menu sub-list update
constexpr uint32_t SUB_CONFIRM_ARGS = 0x089E72A0u;   // the words FUN_089E4A60 passes to the pad test for confirm
FuncPtr g_orig_sub_tick = nullptr;
const char* const kTagLabel = "Tag Team Battle";
const char* const kTagHelp = "Two on two with your party: your team mates join the fight, the fallen are replaced.";
std::chrono::steady_clock::time_point g_sub_last_draw{};
float g_sub_sel = 0.f;
float g_last_row_ox = 0.f, g_last_row_oy = 0.f; int g_last_row_sel = -1; bool g_last_row_two = false;
std::chrono::steady_clock::time_point g_rows_fade_t0{}; bool g_rows_fade = false;   // 26 Nov: after the list's confirm is called for Training, the added rows fade with the list
float g_rows_opacity = 1.f;
void draw_sub_row(float ox, float oy0, int sel, bool two) {   // sel: 0 = Tag Team Battle, 1 = Training, -1 = neither; two: the Training row is there too
    using dissidia::LayerColor;
    g_last_row_ox = ox; g_last_row_oy = oy0; g_last_row_sel = sel; g_last_row_two = two;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_sub_last_draw).count());
    g_sub_last_draw = now;
    static float appear = 0.f;
    appear = dt >= 0.1f ? 0.f : std::min(1.f, appear + dt / 0.2f);
    g_sub_sel = sel >= 0 ? std::min(1.f, g_sub_sel + dt / 0.12f) : 0.f;
    load_ui_sheet();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    g_layer.opacity = g_rows_opacity;
    for (int rr = 0; rr < (two ? 2 : 1); rr++) {
        const float oy = oy0 + 16.f * static_cast<float>(rr); const char* label = rr == 0 ? kTagLabel : "Training"; const char* help = rr == 0 ? kTagHelp : "Practice in a one-on-one battle: no KO, refills, CPU actions, last-hit numbers (pause, Select).";
    const float sa = rr == sel ? g_sub_sel * appear : 0.f, ia = rr == sel ? (1.f - g_sub_sel) * appear : appear;
    if (ia > 0.f) {   // idle: the thin line under the row, like the retail sub-rows
        g_layer.band(ox + 12.f, oy + 10.4f, 470.f, oy + 11.1f, [&](float x, float) {
            const float fade = x < 430.f ? 1.f : std::max(0.f, 1.f - (x - 430.f) / 40.f);
            return LayerColor{119, 114, 149, ia * fade};
        });
    }
    if (sa > 0.f) {   // selected: the band
        g_layer.band(ox - 15.f, oy - 2.f, 480.f, oy + 11.f, [&](float x, float y) {
            const float ramp = std::min(1.f, std::max(0.f, (x - (ox - 15.f)) / 30.f));
            const float t = x < 390.f ? 0.f : std::min(1.f, (x - 390.f) / 80.f);
            float r = 63 + (134 - 63) * t, g = 53 + (136 - 53) * t, b = 174 + (207 - 174) * t;
            if (y < oy - 1.f) { r += 30; g += 33; b += 11; } else if (y > oy + 10.f) { r += 7; g += 8; b -= 9; }
            return LayerColor{r, g, b, sa * ramp};
        });
    }
    const float tx = ox + 26.f, base = oy + 8.6f, em = 7.6f;
    if (ia > 0.f) g_layer.text(label, true, em, tx, base, LayerColor{40, 42, 65, ia});
    if (sa > 0.f) {
        for (const auto& o : {std::pair<float, float>{0.6f, 0.6f}, {0.6f, 0.f}, {0.f, 0.6f}, {-0.4f, 0.f}, {0.f, -0.4f}})
            g_layer.text(label, true, em, tx + o.first, base + o.second, LayerColor{30, 24, 80, 0.75f * sa});
        g_layer.text(label, true, em, tx, base, LayerColor{255, 255, 255, sa});
        draw_hand(ox - 13.f, oy - 2.6f, sa);
        const float hw = g_layer.text(help, true, 7.9f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(help, true, 7.9f, 232.f - hw / 2.f, 239.3f, LayerColor{108, 102, 128, sa});
    }
    }
    g_layer.publish();
}
// The groups that get a Tag Team Battle row: Battle Mode (group 1: 4 rows, the row starts Quick Battle (Party), index 1) and Communications Mode
// (group 2: 5 rows, instances 0x20.., it starts Group Battle, index 1: the multiplayer tag battle, to come).
struct TagGroup { uint32_t group, rows, target, first_inst; };   // first_inst: scene instance of the group's first row (rows 16 apart)
constexpr TagGroup kTagGroups[2] = {{1, 4, 1, 0x17}, {2, 5, 1, 0x20}};
// 25 Nov: Battle Mode > Training (training.cpp): one more row under Tag Team Battle (group 1 only, index rows + 1). Confirming it sets the Training flag and starts
// Quick Battle (1-on-1) (index 0) by the game's own code (a fresh Cross is replayed for the list). Any other confirm in the group clears the flag.
// PSPRECOMP_TRAINING_OFF=1: no row. `ext` = the cursor's highest index of the list with the extra rows.

bool train_row_on() { static const bool on = std::getenv("PSPRECOMP_TRAINING_OFF") == nullptr; return on; }
uint32_t tag_ext(const TagGroup* tg);
bool g_train_go = false; int g_train_go_age = 0;
const TagGroup* tag_group(uint32_t st) { if (!st) return nullptr; const uint32_t g = r32(st + 0xA88); for (const auto& t : kTagGroups) if (t.group == g) return &t; return nullptr; }
uint32_t tag_ext(const TagGroup* tg) { return tg->rows + (tg->group == 1 && train_row_on() ? 1u : 0u); }
// a confirm on the Tag Team Battle row (called from the start menu's top-level and sub-list updates, before the game's code sees it)
bool g_ms_go = false; int g_ms_go_age = 0; int g_ms_group = 0;
// 26 Nov (user: "we should NEVER force a cursor to move" / no injected presses): a Mode Select choice no longer hands the list an injected Cross; the sub-list's own confirm step
// (FUN_089E4A60's Cross branch at 0x089E4BE8) is run directly. PSPRECOMP_TAGTEAM_INJECT_OLD=1: old.
bool g_direct_pending = false;
void rows_fade_tick() {   // the added rows keep their last look and fade out over ~0.33 s with the list (the game draws nothing for them once its confirm ran)
    if (!g_rows_fade) return;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_rows_fade_t0).count() / 0.24f;
    if (t >= 1.f) { g_rows_fade = false; g_rows_opacity = 1.f; return; }
    ge_draw_set_game_layer_ttl(80); g_rows_opacity = 1.f - t; draw_sub_row(g_last_row_ox, g_last_row_oy, g_last_row_sel, g_last_row_two); g_rows_opacity = 1.f;
}
void list_confirm_direct(uint8_t* rdram, recomp_context* ctx, uint32_t menu) {
    // FUN_089E4A60's Cross branch (0x089E4BE8..): cursor memory G + 4 = row, confirm sound, G + 8 = the screen to open (Battle Mode: row + 2, Communications: row + 6), state 4
    const uint32_t st = r32(menu + 320), grp = r32(st + 0xA88u), idx = r32(st + 0xA5Cu), G = 0x09B6D830u;
    w32(G + 4u, idx);
    call(g_play_se, rdram, ctx, {1});
    w32(G + 8u, grp == 2 ? idx + 6u : idx + 2u);
    w32(st, 4u);
    std::fprintf(stderr, "[MENUSET] list confirm called directly (group %u row %u)\n", grp, idx);
}
bool g_ms_reopen = false;
bool g_ms_hold = false;   // 25 Nov (user): after a Mode Select choice the list cursor jumped to Quick Battle (Party) before the next menu opened: the Mode Select stays drawn until then. PSPRECOMP_TAGTEAM_MSHOLD_OFF=1: old
bool g_ms_flow = false;   // 25 Nov: a tag mode was chosen from the Mode Select: when this group's list comes back (Circle out of Party Select / the room screens) the Mode Select opens again. PSPRECOMP_TAGTEAM_MENUBACK_OLD=1: old   // the mode select chose a mode: the confirm is handed to the start menu (injected), the Tag Team Battle mode stays on
bool tag_row_confirm(uint8_t* rdram, recomp_context* ctx, uint32_t st, bool from_main) {
    const TagGroup* tg = tag_group(st); if (!tg) return false;
    const uint32_t cur = st + 0xA58u;
    // 25 Nov (user, RP6: "opening Communications mode opened the battle menu"): the start menu's top level calls this too, and the group sub-list's fields (group, rows, cursor) stay in the
    // state after the list was left: with the cursor last on Tag Team Battle (index == rows) the Cross that opens ANY other top-level row (Communications Mode) looked like a confirm on
    // that row and opened the Mode Select ("Battle Mode > Tag Team Battle"). The top level only counts while a sub-list is ticking (its tick ran within the last 0.15 s).
    // PSPRECOMP_TAGTEAM_FLOWRESET_OFF=1: old.
    if (from_main && !dissidia_tagteam_modesel_active() && !std::getenv("PSPRECOMP_TAGTEAM_FLOWRESET_OFF") &&
        std::chrono::steady_clock::now() - g_last_sub_tick > std::chrono::milliseconds(150)) return false;
    if (dissidia_tagteam_modesel_active()) {   // the mode select has the pad
        const int r = dissidia_tagteam_modesel_frame(rdram, ctx, g_ms_group == 2);
        if (r == 1) { const uint32_t tgt = dissidia_tagteam_modesel_target(tg->group == 1, tg->target); std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &tgt, 4); g_ms_go = true; g_direct_pending = !dissidia_tagteam_inject_old(); g_ms_go_age = 0; g_ms_flow = true; g_ms_hold = !std::getenv("PSPRECOMP_TAGTEAM_MSHOLD_OFF"); dissidia_tagteam_modesel_confirm(tg->group == 1); std::fprintf(stderr, "[MENUSET] mode select: chosen, the game goes on (group %u)\n", tg->group); }
        else if (r == 2) { g_ms_flow = false; dissidia_tagteam_menu_mode(false); std::fprintf(stderr, "[MENUSET] mode select: cancelled\n"); }
        return true;
    }
    if (r32(cur + 36) != tag_ext(tg)) { if (g_ms_go && ++g_ms_go_age > 200) g_ms_go = false; return false; }
    // 25 Nov: the sub-list's test (c1) is true on a Circle press too (SUBLOG: Circle = sub args 1, Cross = main args 1), so Circle on the Tag Team Battle row opened the Mode Select
    // again instead of backing out to the main menu (a loop with the Mode Select's own Circle). PSPRECOMP_TAGTEAM_MENUBACK_OLD=1: old.
    const bool c1 = confirm_pressed_at(rdram, ctx, SUB_CONFIRM_ARGS) && (std::getenv("PSPRECOMP_TAGTEAM_MENUBACK_OLD") || !(dissidia_tagteam_raw_pad() & 0x2000u)), c2 = confirm_pressed(rdram, ctx);
    if (std::getenv("PSPRECOMP_MENU_SUBLOG") && (c1 || c2)) std::fprintf(stderr, "[SUBTICK] confirm seen (sub args %d, main args %d) idx %u\n", c1, c2, r32(cur + 4));
    if (g_ms_go) { if (++g_ms_go_age > 200) g_ms_go = false; if (c1 || c2) { g_ms_go = false; dissidia_tagteam_menu_mode(true); dissidia_tagteam_lobby_pending(true); std::fprintf(stderr, "[MENUSET] Tag Team Battle chosen (group %u)\n", tg->group); } return false; }
    if (!(c1 || c2)) return false;
    const uint32_t idx = r32(cur + 4);
    if (tg->group == 1 && train_row_on()) {
        if (g_train_go) { g_train_go = false; return false; }   // (the replayed Cross: the game starts Quick Battle (1-on-1))
        if (idx == tg->rows + 1) { dissidia_tagteam_menu_mode(false); dissidia_training_set(true); const uint32_t z = 0; std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &z, 4); if (dissidia_tagteam_inject_old()) { g_train_go = true; g_train_go_age = 0; dissidia_training_inject_cross(); } else { list_confirm_direct(rdram, ctx, static_cast<uint32_t>(ctx->r[4])); g_rows_fade = true; g_rows_fade_t0 = std::chrono::steady_clock::now(); rows_fade_tick(); }   /* 26 Nov: the list's own confirm, no replayed Cross */ std::fprintf(stderr, "[MENUSET] Training chosen: Quick Battle (1-on-1)\n"); return true; }
        dissidia_training_set(false);
    }
    dissidia_tagteam_menu_mode(idx == tg->rows);
    if (idx == tg->rows) {
        if (std::getenv("PSPRECOMP_TAGTEAM_NOMODESEL")) { std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &tg->target, 4); std::fprintf(stderr, "[MENUSET] Tag Team Battle chosen (group %u)\n", tg->group); dissidia_tagteam_lobby_pending(true); return false; }
        g_ms_group = static_cast<int>(tg->group); dissidia_tagteam_modesel_open(); std::fprintf(stderr, "[MENUSET] Tag Team Battle: mode select (group %u)\n", tg->group);
        return true;
    }
    return false;
}
void sub_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (overlay_id() != 2) { if (FuncPtr f = dissidia_ovl_battle_lookup(SUB_TICK)) f(rdram, ctx); return; }
    const uint32_t menu = static_cast<uint32_t>(ctx->r[4]), st = r32(menu + 320);
    dissidia_presence_sub(r32(menu + 320));   // discord_presence.cpp: the row the cursor is on
    g_last_sub_tick = std::chrono::steady_clock::now();
    dissidia_tagteam_main_tick_seen();
    const TagGroup* tg = tag_group(st);
    const uint32_t cur = st + 0xA58u;
    if (tg && r32(cur + 32) == 0 && r32(cur + 36) == tg->rows - 1) { const uint32_t e = tag_ext(tg); std::memcpy(rdram + ((cur + 36) & 0x07FFFFFFu), &e, 4); }   // one more row (two with Training)
    if (g_train_go && ++g_train_go_age > 120) g_train_go = false;
    {   // back from a tag battle (or its lobby): the game puts the cursor on the row it started (Group Battle / Quick Battle (Party)); put it on Tag Team Battle
        // (user 22 Nov: "returning to the main menu selects Group Battle instead of Tag Team Battle"). Once per opening of the group's sub-list.
        static uint32_t open_group = 0;
        const uint32_t g = tg ? tg->group : 0u;
        // 24 Nov (user: "going to the second menu still puts the cursor on Quick Battle (Party)"): backing out of Party Select / the lobby comes back to the SAME
        // group, so the group never changed and the row was not put back. A sub-list that was not updated for 0.25 s (another screen ran) is a new opening too.
        // PSPRECOMP_TAGTEAM_ROWBACK_GROUPONLY=1: old.
        static auto last_tick = std::chrono::steady_clock::now();
        const auto now_t = std::chrono::steady_clock::now();
        const bool gap = std::chrono::duration<double>(now_t - last_tick).count() > (std::getenv("PSPRECOMP_TAGTEAM_SLOWMENU") ? 0.25 : 0.12) && !std::getenv("PSPRECOMP_TAGTEAM_ROWBACK_GROUPONLY");
        last_tick = now_t;
        const bool reopen_ms = gap && g_ms_flow && tg && static_cast<int>(tg->group) == g_ms_group && !g_ms_go && !dissidia_tagteam_modesel_active() && !std::getenv("PSPRECOMP_TAGTEAM_MENUBACK_OLD");
        if (reopen_ms) g_ms_reopen = true;
        if (gap) g_ms_hold = false;   // (another screen ran in between: the next menu has been and gone)
        if (g != open_group || gap) {
            open_group = g;
            if (tg && dissidia_tagteam_menu_mode_on() && !dissidia_tagteam_modesel_active() && r32(cur + 36) == tag_ext(tg) && r32(cur + 4) == tg->target && !std::getenv("PSPRECOMP_TAGTEAM_NOROWBACK")) {
                std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &tg->rows, 4);
                std::fprintf(stderr, "[MENUSET] group %u opened with Tag Team Battle on: cursor %u -> %u (the Tag Team Battle row)\n", tg->group, tg->target, tg->rows);
            }
            else if (tg && tg->group == 1 && train_row_on() && dissidia_training_chosen() && r32(cur + 36) == tag_ext(tg) && r32(cur + 4) == 0) {   // back from a Training battle: the cursor on the Training row
                const uint32_t t = tg->rows + 1; std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &t, 4);
            }
        }
    }
    if (g_ms_flow && tg && static_cast<int>(tg->group) == g_ms_group && !g_ms_go && !dissidia_tagteam_modesel_active() && g_ms_reopen) {
        g_ms_reopen = false; dissidia_tagteam_menu_mode(true);
        if (dissidia_tagteam_ffa_ps_return()) { const uint32_t r = tg->rows; std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &r, 4); }   // (the list cursor back on Tag Team Battle under the players screen)
        else { dissidia_tagteam_modesel_open(); std::fprintf(stderr, "[MENUSET] back from the chain: the Mode Select again (group %u)\n", tg->group); }
    }
    if (std::getenv("PSPRECOMP_MENU_SUBLOG") && st) { static std::string last; char b[128]; std::snprintf(b, sizeof b, "group %u idx %u min %u max %u state0 %u", r32(st + 0xA88), r32(cur + 4), r32(cur + 32), r32(cur + 36), r32(st)); if (last != b) { last = b; std::fprintf(stderr, "[SUBTICK] %s\n", b); } }
    if (g_ms_hold && std::getenv("PSPRECOMP_MENU_SUBLOG")) std::fprintf(stderr, "[MSHOLD] sub tick (go %d rows %u)\n", g_ms_go, tg ? r32(cur + 36) : 0u);
    if (g_direct_pending && tg && static_cast<int>(tg->group) == g_ms_group && !dissidia_tagteam_modesel_active()) {   // the Mode Select chose: the list's confirm, called
        g_direct_pending = false; g_ms_go = false;
        list_confirm_direct(rdram, ctx, menu);
        dissidia_tagteam_menu_mode(true); dissidia_tagteam_lobby_pending(true); std::fprintf(stderr, "[MENUSET] Tag Team Battle chosen (group %u)\n", tg->group);
        return;
    }
    if (tag_row_confirm(rdram, ctx, st)) return;
    // Training row (index rows + 1): the game shows its hand and its help text for an index past the table; with the cursor one lower (the Tag Team Battle row, which it draws nothing for)
    // the game stays quiet. So for the game's update the cursor is put on that row and its move is translated back: up = Tag Team Battle, down = the first row, none = still Training.
    const bool sw = tg && tg->group == 1 && train_row_on() && r32(cur + 36) == tag_ext(tg) && r32(cur + 4) == tg->rows + 1;
    if (sw) { const uint32_t v = tg->rows; std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &v, 4); }
    g_orig_sub_tick(rdram, ctx);
    if (sw) { const uint32_t now = r32(cur + 4); const uint32_t v = now == tg->rows ? tg->rows + 1 : now == tg->rows + 1 ? 0u : tg->rows; std::memcpy(rdram + ((cur + 4) & 0x07FFFFFFu), &v, 4); }
    const uint32_t st2 = r32(menu + 320);
    const TagGroup* tg2 = tag_group(st2);
    if (tg2 && r32(st2 + 0xA58 + 36) == tag_ext(tg2)) {   // the extra row, one row under the last (the sub-list's first row is instance 0x17, rows 16 apart)
        const uint32_t inst = r32(st2 + 0x408 + 4 * tg2->first_inst);
        float ox = 271.f, oy = 101.f;
        if (inst >= 0x08800000u && inst < 0x0A000000u) { std::memcpy(&ox, rdram + ((inst + 0x10) & 0x07FFFFFFu), 4); std::memcpy(&oy, rdram + ((inst + 0x14) & 0x07FFFFFFu), 4); }
        oy += 16.f * static_cast<float>(tg2->rows);
        if (g_ms_hold) { dissidia_tagteam_modesel_draw_frozen(g_ms_group == 2); if (std::getenv("PSPRECOMP_MENU_SUBLOG")) std::fprintf(stderr, "[MSHOLD] frozen draw\n"); }
        else if (std::isfinite(ox) && std::isfinite(oy) && ox > 100.f && ox < 400.f && oy > 0.f && oy < 240.f) draw_sub_row(ox, oy, r32(st2 + 0xA58 + 4) == tg2->rows ? 0 : r32(st2 + 0xA58 + 4) == tg2->rows + 1 && tag_ext(tg2) > tg2->rows ? 1 : -1, tag_ext(tg2) > tg2->rows);
    }
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
void dissidia_menu_rows_fade_tick() { rows_fade_tick(); }   // tagteam_menu.cpp's pad filter: every poll (the menus' own updates pause while the list closes)

// The tag team screens (tagteam_menu.cpp) draw the game's pointing hand on their own layer.
void dissidia_menu_draw_hand(dissidia::MenuLayer& L, float x, float y, float a) { load_ui_sheet(); draw_hand_on(L, x, y, a); }

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
