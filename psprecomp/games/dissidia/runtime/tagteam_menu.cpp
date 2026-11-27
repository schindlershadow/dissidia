// games/dissidia/runtime/tagteam_menu.cpp - Communications Mode > Tag Team Battle: the Group Battle Tournament setup screen.
// See docs/tag-team.md "Communications Mode: CPU picks".
//
// The retail screen already lets each player register more than one character (the left column under the player's own: each row opens
// Player Select, the game sends the rows to the other players and puts them in the battle's party records). Every row is the player's own
// character (played by him when its turn comes); in the tag team mode Square on a row from the second on makes it a CPU (the AI fights
// it) and back. This file draws on the game layer: a "CPU" plate on the CPU rows, "Add a character" in the next free row, a Square key
// with "CPU" / "Player" in the button bar while the cursor is on a row that can be switched, and, with three or more players, the
// host's fighters-per-side choice (L on the host's screen cycles it). The plain retail Group Battle (tag team mode off) is untouched.
//
//   ovl_menu FUN_08AA8F9C(screen): the setup screen's per-frame update (runs only while it is up, not under Player Select).
//   screen + 0x7CC4: the cursor's column (0 = the left column), screen + 0x5A4: its row there (0 = the player's own).
#include "psp_runtime.h"
#include "recomp.h"
#include "hle/psp_hle.h"
#include "menu_layer.h"
#include "tagteam_result.h"
void dissidia_tagteam_result_install(FuncPtr orig_request);
#include "psp_ge_draw.h"
#include "../../../runtime/src/hle/psp_savedata_dialog.h"
#include <map>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

extern FuncPtr ovl_menu_lookup(uint32_t vaddr);
extern FuncPtr ovl_battle_lookup(uint32_t vaddr);
bool dissidia_tagteam_group_on();
void dissidia_tagteam_gb_setup_frame(int cursor_row);
uint8_t dissidia_tagteam_local_own_mask();
int dissidia_tagteam_local_regs(uint8_t out[5]);
int dissidia_tagteam_group_size();
bool dissidia_tagteam_is_host();
bool dissidia_tagteam_lobby_editable();   // the host, or single player
bool dissidia_tagteam_lobby_sp();
void dissidia_tagteam_ffa_get(int slot, uint8_t out[4]);   // Free-for-all CPU slots P2..P4 (0..2): {on, main, tag-in, strength}
void dissidia_tagteam_ffa_set(int slot, const uint8_t in[4]);
int dissidia_tagteam_roster(uint8_t out[40]);   // the characters that can be picked
void dissidia_tagteam_ffa_p1_get(uint8_t out[2]);   // P1 (you): {main, tag-in}
void dissidia_tagteam_ffa_p1_set(const uint8_t in[2]);
void dissidia_tagteam_ffa_look_set(int slot, int field, int look);
bool dissidia_tagteam_inject_old();   // below: PSPRECOMP_TAGTEAM_INJECT_OLD / _FFA_PSOLD   // tagteam.cpp: the LOOK picked in Player Select for a Free-for-all fighter (slot 0..3, field 1 main / 2 tag-in)
int dissidia_tagteam_ffa_players();   // P1 + the CPU slots that are on
void dissidia_menu_draw_hand(dissidia::MenuLayer& L, float x, float y, float a);   // menu_settings.cpp: the game's pointing hand
void dissidia_tagteam_sp_write_round(uint32_t screen);   // single player: round 1's stage / rules / judgement from the lobby   // single player: the player is the host, no beacon; Assist stays on Square
uint8_t dissidia_tagteam_mode_byte();
bool dissidia_tagteam_lobby_get(uint8_t out[10]);
void dissidia_tagteam_lobby_set(const uint8_t in[10]);
int dissidia_tagteam_lobby_players(char names[16][32], uint8_t chars[16], int8_t parties[16], bool hosts[16]);
std::string dissidia_tagteam_char_name(int id);
void dissidia_tagteam_result_tick();
void dissidia_tagteam_result_done();
std::string dissidia_tagteam_char_code(int id);
const uint32_t* dissidia_tagteam_face_px(const std::string& code, int* w, int* h);
void dissidia_tagteam_set_mode_byte(uint8_t m);
uint8_t dissidia_tagteam_local_cpu_mask();
void dissidia_tagteam_toggle_cpu_row(int row);
void dissidia_tagteam_get_cpu_cfg(int row, uint8_t out[3]);
void dissidia_tagteam_set_cpu_cfg(int row, const uint8_t in[3]);
int dissidia_tagteam_menu_cpu_row(int party, int row, uint8_t out[3]);
int dissidia_tagteam_local_party();
bool dissidia_tagteam_cpu_party_on();
int dissidia_tagteam_defect_side();
void dissidia_tagteam_add_cpu_request(bool on);
int dissidia_tagteam_bp_party();
bool dissidia_tagteam_bp_can_edit();
int dissidia_tagteam_bp_count();
int dissidia_tagteam_bp_row(int i, uint8_t cfg[3]);
void dissidia_tagteam_bp_add(int ch);
void dissidia_tagteam_bp_remove(int row);
void dissidia_tagteam_bp_set_cfg(int row, const uint8_t in[3]);
void dissidia_tagteam_all_cpu_strength(int v, bool sp);   // tagteam.cpp: the lobby ALL CPUs row
int dissidia_tagteam_bp_pick_list(uint8_t out[64]);
bool dissidia_tagteam_bp_enter_blocked();
int dissidia_tagteam_bp_party_for_select();
void dissidia_tagteam_bp_finalize(uint8_t* rdram, recomp_context* ctx);
bool dissidia_tagteam_add_cpu_pending();
bool dissidia_tagteam_sp_tag_on();
bool dissidia_tagteam_sp_mode();
int dissidia_tagteam_sp_rows(uint32_t screen, uint8_t out[5]);
uint8_t dissidia_tagteam_sp_cpu_mask();
void dissidia_tagteam_sp_toggle(int row);
bool dissidia_tagteam_sp_assist_on();
void dissidia_tagteam_sp_assist_toggle();
void dissidia_tagteam_sp_get_cfg(int row, uint8_t out[3]);
void dissidia_tagteam_sp_set_cfg(int row, const uint8_t in[3]);

namespace {

using dissidia::LayerColor;
constexpr uint32_t OVL_BASE = 0x089DD4C0u;
constexpr uint32_t READY_TEST = 0x08A7046Cu, ENTER_STEP = 0x08AB78E8u;   // ovl_menu: Party Select's 'both parties have a member' test (a0 = the screen; list p at +280 + 12 p, count at +10), Enter Battle's table step
constexpr uint32_t SETUP_TICK = 0x08AA8F9Cu;   // ovl_menu: Tournament setup screen update (a0 = the screen)
// The single player Tournament screen (Quick Battle > Party): its per-frame function and the round dialog's update. screen + 0x7CFC = M: +4 the state, +1444 the round
// the dialog is open on (0 = 1st), +1488 the dialog's cursor row (0 STAGE, 1 STRENGTH, 2 LEVEL, 3 TYPE, 4 RULES, 5 JUDGEMENT).
constexpr uint32_t SP_TICK = 0x08AAB514u, SP_DLG = 0x08AA8C78u, SCR_M = 0x7CFCu;
constexpr uint32_t PLAY_SE = 0x088D4BF4u;      // EBOOT: menu sound effect (1 cursor)
constexpr uint32_t BTN_L = 0x100u, BTN_SQUARE = 0x8000u;
constexpr uint32_t SCR_COLUMN = 0x7CC4u, SCR_ROW = 0x5A4u;
// The left column: five rows of 20 units from y 120 (row 0 = the player's own character), the job name centred at x ~110.
constexpr float kRowY0 = 120.f, kRowPitch = 20.f, kColX0 = 2.f, kColX1 = 164.f;
// The single player Tournament screen (Quick Battle > Party; Battle Mode > Tag Team Battle): its per-frame update (a0 = the screen; tree ttfb found it
// for the round dialog work). screen + 0x7CFC = M (seen = the screen itself): M + 4 the state (2 = the list), M + 1444 the cursor row (0..4 = 1st..5th, 5 = Enter Battle).

uint8_t* g_ram = nullptr;
FuncPtr g_orig_preview = nullptr;
FuncPtr g_orig_ready = nullptr, g_orig_enter = nullptr, g_orig_tick = nullptr, g_play_se = nullptr, g_orig_sp_tick = nullptr, g_orig_sp_dlg = nullptr;
bool g_lob_sp = false;   // the battle lobby (modes 2..4) was opened by the single player screen (else by the Communications setup screen)
bool g_dlg_sp = false;   // the CPU settings dialog edits a single player CPU row (else a Communications row)
dissidia::MenuLayer g_layer("menu", true);
const char* dlg_tag();   // the screen the layer shows (the timing log's label)
uint32_t g_prev_buttons = 0;
// The CPU settings dialog (a CPU row's STRENGTH / LEVEL / TYPE; opened with Select (Triangle is the screen's Customization) on one of the player's own CPU rows). While it is open it takes the pad
// (dissidia_tagteam_menu_pad is in the game's button filter chain), the setup screen underneath sees no buttons.
constexpr uint32_t BTN_UP = 0x10u, BTN_RIGHT = 0x20u, BTN_DOWN = 0x40u, BTN_LEFTK = 0x80u, BTN_R = 0x200u, BTN_OPEN = 0x1u, BTN_CIRCLE = 0x2000u, BTN_CROSS = 0x4000u;
// 26 Nov (menus at 10 fps / lists "too slow"): a direction held in the lobby / players screen / pickers repeats like the retail lists (the first repeat after 400 ms, then
// every 100 ms); it is the same press the player makes, no injected input. Called with the pad state of every poll. PSPRECOMP_TAGTEAM_MENUREPEAT_OFF=1: edges only (old).
uint32_t dir_repeat(uint32_t b, uint32_t pressed, bool active) {
    static const bool off = std::getenv("PSPRECOMP_TAGTEAM_MENUREPEAT_OFF") != nullptr;
    static uint32_t held = 0; static std::chrono::steady_clock::time_point next{};
    const uint32_t dir = b & (BTN_UP | BTN_RIGHT | BTN_DOWN | BTN_LEFTK), d = dir & (~dir + 1u);   // (one direction at a time)
    if (!d || !active || off) { held = 0; return pressed; }
    const auto now = std::chrono::steady_clock::now();
    if (d != held || (pressed & dir)) { held = d; next = now + std::chrono::milliseconds(400); }
    else if (now >= next) { pressed |= d; next = std::max(next + std::chrono::milliseconds(100), now - std::chrono::milliseconds(100)); }
    return pressed;
}
std::atomic<uint32_t> g_raw_pad{0};   // the pad as the filter saw it before the dialog took it
std::atomic<bool> g_dlg_open{false};
std::atomic<int> g_inject_cross{0};
void add_cpu_direct(uint8_t* rdram, recomp_context* ctx, uint32_t screen);
std::atomic<bool> g_add_cpu_direct{false};   // 26 Nov: "Add CPU" (L on the free row): the screen's own Cross step is called (no press) before the next update
const char* g_inj_why = nullptr;   // 26 Nov: why the pad filter is about to add bits to the player's pad (always-on [INJECT] log in dissidia_tagteam_menu_pad)
// 25 Nov (user, RP6: "back does not return to the previous tag menu"): the single player chain is Battle Mode list > Mode Select > Party Select > setup screen + Battle Lobby.
// Circle on the setup screen's list (no dialog) now reopens the lobby (g_lob_flow), Circle in that lobby goes back to Party Select (one injected Circle for retail's screen),
// and menu_settings.cpp reopens the Mode Select when the Battle Mode / Communications list comes back. PSPRECOMP_TAGTEAM_MENUBACK_OLD=1: old.
bool g_lob_flow = false;   // the lobby is a step of the menu chain (opened by the flow or by Circle on the setup screen): Circle goes back a screen, Cross goes on
// (25 Nov, user: no cursor is ever moved for the player: the Down + Up "nudge" that reloaded the setup screen's stage preview after the lobby is gone)
std::atomic<int> g_nudge_unused{0};   // 25 Nov: the setup screen's stage preview only follows the cursor row: after the lobby closed, one Down + Up reloads it (PSPRECOMP_TAGTEAM_NONUDGE=1: old)
std::atomic<int> g_inject_circle{0};   // a Circle for retail's setup screen after the lobby closed (6 .. 1: down while 5 .. 3)
std::atomic<int64_t> g_circle_stamp{0};   // steady-clock ms of the last setup screen tick that took Circle (the pad filter hides Circle from the screen for 100 ms after)
int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// 25 Nov (user: the tag menus are slow): the idle checks were 500 ms (re-arm after backing out) / 450 ms (the Mode Select's frozen layer over the next screen); PSPRECOMP_TAGTEAM_SLOWMENU=1: old values
const int kRearmMs = std::getenv("PSPRECOMP_TAGTEAM_SLOWMENU") ? 500 : 120, kHoldTtlMs = std::getenv("PSPRECOMP_TAGTEAM_SLOWMENU") ? 450 : 150;
bool menuback_old() { static const bool o = std::getenv("PSPRECOMP_TAGTEAM_MENUBACK_OLD") != nullptr; return o; }
std::atomic<int> g_script_t{-1};   // the Free-for-all's scripted pass through retail's Party Select and CPU Select (single player): polls since the confirm, -1 = off
std::atomic<int> g_inject_late{0};   // a confirm for the start menu after the mode select closed: pressed once the dialog hold is over (6 .. 1: Cross is down while 5 .. 3)   // "Add CPU" (L on the free row) presses Cross for the screen: it opens Player Select for the new row
std::atomic<uint32_t> g_pad_polls{0}, g_sq_stamp{0};   // pad polls; the poll at which the setup screen last ran with Square taken (the block ends a few polls after the screen stops ticking)
std::atomic<bool> g_sp_sq_block{false};   // single player setup list: Square is ours (Assist On / Off), the screen does not see it
int g_dlg_row = 0, g_dlg_field = 0;
bool g_dlg_bp = false;   // the CPU settings dialog edits a row of the host's CPU party (docs "Proper PvE")
int g_dlg_mode = 0;      // 0 = the CPU settings dialog, 1 = the CPU party's character picker, 2 = the battle lobby
int g_lob_row = 0;       // the battle lobby's cursor row
uint8_t g_pick_list[64] = {}; int g_pick_n = 0, g_pick_sel = 0;
std::atomic<int> g_enter_block{0};   // 1 = Start is ours (blocked), 2 = Cross too (the cursor is on Enter Battle): the CPU party has no row yet
std::atomic<uint32_t> g_enter_stamp{0};
std::atomic<int> g_col_fix{0};   // the battle lobby replaces the Tournament column: 1 = the cursor is in the party column (Left / Right are ours), 2 = it is on a round row (Cross is ours; Right takes it back to the party column)
uint8_t g_dlg_cfg[3] = {5, 0, 0};
float g_dlg_hold = 0.f;
std::chrono::steady_clock::time_point g_last{};
float g_appear = 0.f;

uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x07FFFFFFu), 4); return v; }
void w32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x07FFFFFFu), &v, 4); }
// ---- The online Group Battle's "Please select a battle format" dialog (27 Nov) ----
// ovl_menu FUN_08A7AE08(list, ctx) is the dialog list's input handler (3 rows: Tournament, Round-Robin (3 Rounds), Round-Robin (5 Rounds)). Its Cross branch posts the
// choice to the screen's listener: FUN_08A6FC24(owner, 0x0A, rec, 6), rec = {format, rounds, enabled, 7, 0, state}, sets the list's state (+184) to 2 and runs
// FUN_08A77614(list, 1) (the close). The hook runs that branch for row 0 at the first call (no pad bit, no cursor move), once per list object.
constexpr uint32_t FMT_DLG = 0x08A7AE08u, FMT_POST = 0x08A6FC24u, FMT_CLOSE = 0x08A77614u, FMT_OWNER_PTR = 0x08A6D7E0u;
FuncPtr g_orig_fmt = nullptr;
uint32_t g_fmt_done_obj = 0;
void fmt_dlg_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(FMT_DLG)) f(rdram, ctx); return; }
    const uint32_t obj = static_cast<uint32_t>(ctx->r[4]), param = static_cast<uint32_t>(ctx->r[5]);
    static const bool log = std::getenv("PSPRECOMP_TAGTEAM_LOG") != nullptr;
    if (!dissidia_tagteam_group_on() || obj == g_fmt_done_obj) {
        if (obj != g_fmt_done_obj) { static uint32_t seen = 0; if (log && seen != obj) { seen = obj; std::fprintf(stderr, "[TTFMT] dialog handler on %08X, tag group off: left to the player\n", obj); } }
        g_orig_fmt(rdram, ctx); return;
    }
    const uint32_t owner = r32(r32(FMT_OWNER_PTR));
    FuncPtr post = ovl_menu_lookup(FMT_POST), close = ovl_menu_lookup(FMT_CLOSE);
    if (!owner || !post || !close) { g_orig_fmt(rdram, ctx); return; }
    g_fmt_done_obj = obj;
    const recomp_context save = *ctx;
    const uint32_t rec = static_cast<uint32_t>(save.r[29]) - 2048u;   // free stack below the handler (the callees' frames are small)
    const int32_t state = static_cast<int32_t>(r32(param + 604u));
    const uint8_t b[6] = {0, 0, static_cast<uint8_t>(state >= 0 ? 1 : 0), 7, 0, static_cast<uint8_t>(state)};   // row 0: Tournament (format 0, no rounds)
    for (int i = 0; i < 6; i++) g_ram[(rec + i) & 0x07FFFFFFu] = b[i];
    ctx->r[4] = static_cast<int32_t>(owner); ctx->r[5] = 0x0A; ctx->r[6] = static_cast<int32_t>(rec); ctx->r[7] = 6; post(rdram, ctx); *ctx = save;
    w32(obj + 184u, 2u);
    ctx->r[4] = static_cast<int32_t>(obj); ctx->r[5] = 1; close(rdram, ctx); *ctx = save;
    std::fprintf(stderr, "[TTFMT] battle format dialog answered with Tournament (list %08X, owner %08X)\n", obj, owner);
}

bool okp(uint32_t a) { return a >= 0x08800000u && a < 0x0A000000u; }

void play_se(uint8_t* rdram, recomp_context* ctx, int id) {
    if (!g_play_se) return;
    recomp_context save = *ctx;
    ctx->r[4] = id;
    g_play_se(rdram, ctx);
    *ctx = save;
}

// 26 Nov (user: no injected presses): back from the single player setup screen (the lobby's Circle) the way the screen's own Circle leaves: FUN_08AAB8B4(screen, 1) = M + 4 = 5
// (its teardown) with no result byte set, so its state 6 reports 2 (back) and the Quick Battle app goes back to Party Select / CPU Select as on a Circle. PSPRECOMP_TAGTEAM_INJECT_OLD=1: old.
uint32_t g_sp_screen_last_fwd();
void sp_back_direct(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t screen = g_sp_screen_last_fwd(); FuncPtr f = ovl_menu_lookup(0x08AAB8B4u);
    if (!(screen >= 0x08800000u && screen < 0x0A000000u) || !f) {   // 26 Nov: no press as a fallback (PSPRECOMP_TAGTEAM_INJECT_OLD=1: the old injected Circle)
        if (dissidia_tagteam_inject_old()) g_inject_circle.store(6, std::memory_order_relaxed);
        else std::fprintf(stderr, "[TTLOBBY] back from the setup screen: screen pointer %08X / teardown function invalid, nothing done (no press injected)\n", screen);
        return;
    }
    const recomp_context save = *ctx; ctx->r[4] = static_cast<int32_t>(screen); ctx->r[5] = 1; f(rdram, ctx); *ctx = save;
    std::fprintf(stderr, "[TTLOBBY] setup screen left the way its Circle does (no press)\n");
}
// The fighters-per-side choices the host cycles through (byte 6 of the tag bytes: bits 0-1 Party A, bits 2-3 Party B).
constexpr int kModeCount = 5;
constexpr uint8_t kModes[kModeCount] = {2 | (2 << 2), 2 | (1 << 2), 1 | (2 << 2), 1 | (1 << 2), 3 | (3 << 2)};   // the last: 3 v 3
std::string mode_label(uint8_t m) {
    auto per = [](int c) { return c == 1 ? 1 : c == 3 ? 3 : 2; };
    const int a = per(m & 3), b = per((m >> 2) & 3);
    return std::to_string(a) + " vs " + std::to_string(b);
}

// A small tag in the style of the screen's plates: a dark blue rounded band with white bold letters.
void tag(float x0, float y0, float x1, float y1, const char* s, float a) {
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, 1.5f}) / 1.5f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{50 + 30 * (1 - t), 46 + 30 * (1 - t), 120 + 40 * (1 - t), a * 0.92f * std::max(0.f, edge)};
    });
    const float em = 6.2f, w = g_layer.text(s, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(s, true, em, (x0 + x1) / 2.f - w / 2.f, y1 - (y1 - y0 - em * 0.72f) / 2.f - 0.4f, LayerColor{255, 255, 255, a});
}

// A button key of the bar, drawn like the screen's own: the face button as a dark disc with a white symbol (as its triangle), a shoulder
// button as a light key with a dark outline and letter (as its R). Returns the key's left edge.
float key(float right, const char* letter, bool square, float a) {
    if (square) {
        const float cx = right - 5.5f, cy = 15.4f, rad = 5.3f;
        g_layer.band(cx - rad - 1.f, cy - rad - 1.f, cx + rad + 1.f, cy + rad + 1.f, [&](float x, float y) {
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            return LayerColor{22, 22, 40, a * std::min(1.f, std::max(0.f, (rad - d) * 2.f))};
        });
        const float h = 2.5f, t = 0.75f;
        const LayerColor w{255, 255, 255, a};
        g_layer.band(cx - h, cy - h, cx + h, cy - h + t, [&](float, float) { return w; });
        g_layer.band(cx - h, cy + h - t, cx + h, cy + h, [&](float, float) { return w; });
        g_layer.band(cx - h, cy - h, cx - h + t, cy + h, [&](float, float) { return w; });
        g_layer.band(cx + h - t, cy - h, cx + h, cy + h, [&](float, float) { return w; });
        return cx - rad;
    }
    const float kx1 = right, kx0 = kx1 - 12.f, y0 = 9.8f, y1 = 21.f;
    g_layer.band(kx0, y0, kx1, y1, [&](float x, float y) {
        const float e = std::min({x - kx0, kx1 - x, y - y0, y1 - y});
        return e < 0.8f ? LayerColor{60, 60, 76, a} : LayerColor{236, 236, 242, a};
    });
    const float lw = g_layer.text(letter, true, 7.4f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(letter, true, 7.4f, (kx0 + kx1) / 2.f - lw / 2.f, 18.6f, LayerColor{40, 40, 52, a});
    return kx0;
}
float bar_text(float right, const std::string& s, float a) {
    const float em = 7.0f, w = g_layer.text(s, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(s, true, em, right - w, 19.2f, LayerColor{40, 42, 65, a});
    return right - w;
}

const char* kFieldName[3] = {"STRENGTH", "LEVEL", "TYPE"};
// The TYPE names of the single player Tournament dialog (read off its screen): entry 1 = Random, entries 2-9 = builder index 0-7 = our TYPE 1-8.
const char* kTypeName[8] = {"Tactician", "Valiant", "Survivor", "Cautious", "Calm", "Extreme", "Conservative", "Vicious"};
std::string type_name(int t) { return t >= 1 && t <= 8 ? kTypeName[t - 1] : "Random"; }
// A CPU row's settings as a short tag ("S5 L-- Random": strength, level (-- = Auto), type).
std::string cfg_text(const uint8_t* c) { return "S" + std::to_string(c[0]) + " L" + (c[1] ? std::to_string(c[1]) : std::string("--")) + " " + type_name(c[2]); }
std::string field_value(int f, const uint8_t* c) {
    if (f == 0) return std::to_string(c[0]);
    if (f == 1) return c[1] ? std::to_string(c[1]) : "Auto";
    return type_name(c[2]);
}
void change_field(int f, int dir, bool big) {
    uint8_t* c = g_dlg_cfg;
    if (f == 0) c[0] = static_cast<uint8_t>(std::clamp(int(c[0]) + dir, 1, 9));
    else if (f == 1) c[1] = static_cast<uint8_t>(std::clamp(int(c[1]) + dir * (big ? 10 : 1), 0, 100));
    else c[2] = static_cast<uint8_t>((int(c[2]) + dir + 9) % 9);
    if (g_dlg_sp) dissidia_tagteam_sp_set_cfg(g_dlg_row, c); else if (g_dlg_bp) dissidia_tagteam_bp_set_cfg(g_dlg_row, c); else dissidia_tagteam_set_cpu_cfg(g_dlg_row, c);
}
// The dialog: a dark blue panel in the style of the screen's plates, three rows "NAME  < value >", the selected row lit.
void dialog(uint8_t* rdram, recomp_context* ctx, uint32_t pressed, float dt, float a) {
    const float x0 = 150.f, x1 = 330.f, y0 = 78.f, y1 = 188.f;
    uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    if (pressed & (BTN_CIRCLE | BTN_CROSS | BTN_OPEN)) { g_dlg_open = false; play_se(rdram, ctx, 1); return; }
    if (pressed & BTN_UP) { g_dlg_field = (g_dlg_field + 2) % 3; play_se(rdram, ctx, 1); }
    if (pressed & BTN_DOWN) { g_dlg_field = (g_dlg_field + 1) % 3; play_se(rdram, ctx, 1); }
    const bool big = (b & (BTN_L | BTN_R)) != 0;
    int dir = 0;
    if (pressed & (BTN_RIGHT | BTN_LEFTK)) { dir = (pressed & BTN_RIGHT) ? 1 : -1; g_dlg_hold = -0.35f; }   // first repeat after 0.35 s
    else if (b & (BTN_RIGHT | BTN_LEFTK)) { g_dlg_hold += dt; if (g_dlg_hold >= 0.07f) { g_dlg_hold = 0.f; dir = (b & BTN_RIGHT) ? 1 : -1; } }
    if (dir) { const uint8_t before[3] = {g_dlg_cfg[0], g_dlg_cfg[1], g_dlg_cfg[2]}; change_field(g_dlg_field, dir, big); if (std::memcmp(before, g_dlg_cfg, 3)) play_se(rdram, ctx, 1); }
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, y - y0, y1 - y, 2.f}) / 2.f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{26 + 20 * (1 - t), 28 + 20 * (1 - t), 78 + 40 * (1 - t), 0.95f * a * std::max(0.f, edge)};
    });
    const char* title = "CPU SETTINGS";
    const float tw = g_layer.text(title, true, 8.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(title, true, 8.2f, (x0 + x1) / 2.f - tw / 2.f, y0 + 13.f, LayerColor{255, 255, 255, a});
    for (int f = 0; f < 3; f++) {
        const float ry = y0 + 24.f + 24.f * float(f);
        if (f == g_dlg_field) g_layer.band(x0 + 8.f, ry, x1 - 8.f, ry + 20.f, [&](float, float y) { return LayerColor{120, 150, 230, 0.55f * a * (1.f - 0.4f * (y - ry) / 20.f)}; });
        g_layer.text(kFieldName[f], true, 7.6f, x0 + 16.f, ry + 14.f, LayerColor{235, 238, 255, a});
        const std::string v = std::string(f == g_dlg_field ? "< " : "  ") + field_value(f, g_dlg_cfg) + (f == g_dlg_field ? " >" : "");
        const float vw = g_layer.text(v, true, 7.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(v, true, 7.6f, x1 - 16.f - vw, ry + 14.f, LayerColor{255, 255, 255, a});
    }
    const char* hint = "Up/Down  Left/Right  (L/R: x10)  Select: close";
    const float hw = g_layer.text(hint, true, 5.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(hint, true, 5.6f, (x0 + x1) / 2.f - hw / 2.f, y1 - 5.f, LayerColor{200, 205, 240, a});
}

// The CPU party's character picker (host, setup screen: L on the order column): a list of the roster, Up / Down (L / R: a page), Cross adds, Circle closes.
void picker(uint8_t* rdram, recomp_context* ctx, uint32_t pressed, float a) {
    if (pressed & BTN_CIRCLE) { g_dlg_open = false; play_se(rdram, ctx, 1); return; }
    if (g_pick_n <= 0) { g_dlg_open = false; return; }
    if (pressed & BTN_CROSS) { dissidia_tagteam_bp_add(g_pick_list[g_pick_sel]); g_dlg_open = false; play_se(rdram, ctx, 1); return; }
    if (pressed & BTN_UP) { g_pick_sel = (g_pick_sel + g_pick_n - 1) % g_pick_n; play_se(rdram, ctx, 1); }
    if (pressed & BTN_DOWN) { g_pick_sel = (g_pick_sel + 1) % g_pick_n; play_se(rdram, ctx, 1); }
    if (pressed & BTN_L) { g_pick_sel = std::max(0, g_pick_sel - 7); play_se(rdram, ctx, 1); }
    if (pressed & BTN_R) { g_pick_sel = std::min(g_pick_n - 1, g_pick_sel + 7); play_se(rdram, ctx, 1); }
    const float x0 = 150.f, x1 = 330.f, y0 = 44.f, y1 = 236.f;
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, y - y0, y1 - y, 2.f}) / 2.f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{26 + 20 * (1 - t), 28 + 20 * (1 - t), 78 + 40 * (1 - t), 0.95f * a * std::max(0.f, edge)};
    });
    const char* title = "ADD CPU OPPONENT";
    const float tw = g_layer.text(title, true, 8.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(title, true, 8.2f, (x0 + x1) / 2.f - tw / 2.f, y0 + 13.f, LayerColor{255, 255, 255, a});
    const int vis = 8; const int top = std::clamp(g_pick_sel - vis / 2, 0, std::max(0, g_pick_n - vis));
    for (int i = 0; i < vis && top + i < g_pick_n; i++) {
        const int k = top + i; const float ry = y0 + 22.f + 20.f * float(i);
        if (k == g_pick_sel) g_layer.band(x0 + 8.f, ry, x1 - 8.f, ry + 18.f, [&](float, float y) { return LayerColor{120, 150, 230, 0.55f * a * (1.f - 0.4f * (y - ry) / 18.f)}; });
        g_layer.text(dissidia_tagteam_char_name(g_pick_list[k]), true, 7.6f, x0 + 16.f, ry + 13.f, LayerColor{235, 238, 255, a});
    }
    const char* hint = "Up/Down  L/R: page  Cross: add  Circle: close";
    const float hw = g_layer.text(hint, true, 5.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(hint, true, 5.6f, (x0 + x1) / 2.f - hw / 2.f, y1 - 5.f, LayerColor{200, 205, 240, a});
}

// ---- The battle lobby (docs/tag-team.md "Battle lobby"): Select on one of the player's own rows (not a CPU row) opens it. The host edits every row; the
// others see the host's settings. Mode first (it sets how many players the battle needs), then the battle's rules.
const char* const kStages[35] = {"Random", "Old Chaos Shrine", "Pandaemonium", "World of Darkness", "Lunar Subterrane", "The Rift", "Kefka's Tower", "Planet's Core",
    "Ultimecia's Castle", "Crystal World", "Dream's End", "Order's Sanctuary", "Edge of Madness", "Empyreal Paradox", "Sky Fortress Bahamut", "Orphan's Cradle",
    "Pandaemonium - Top Floor", "Crystal Tower", "Phantom Train", "M.S. Prima Vista", "Old Chaos Shrine (Omega)", "Pandaemonium (Omega)", "World of Darkness (Omega)",
    "Lunar Subterrane (Omega)", "The Rift (Omega)", "Kefka's Tower (Omega)", "Planet's Core (Omega)", "Ultimecia's Castle (Omega)", "Crystal World (Omega)",
    "Dream's End (Omega)", "Order's Sanctuary (Omega)", "Edge of Madness (Omega)", "Empyreal Paradox (Omega)", "Sky Fortress Bahamut (Omega)", "Orphan's Cradle (Omega)"};   // the round dialog's list, in its order
const char* const kStrName[9] = {"Minimal", "Very Low", "Low (Actions)", "Low (Equipment)", "Average", "High (Equipment)", "High (Actions)", "Very High", "Maximum"};   // retail's CPU STRENGTH labels 1..9 (read off the Tournament round dialog)
const char* const kRules[6] = {"None", "Official", "Official (Skill)", "EX Centric", "Assist Centric", "BRV Centric"};
const char* const kJudge[5] = {"None", "Cosmos Judgment", "Chaos Judgment", "Double Judgment", "Random"};
struct LobbyMode { uint8_t mode, size; const char* label; const char* help; };
const LobbyMode kLobbyModes[] = {
    {0, 1, "1 vs 1 Duel", "One character each; the first KO decides"},
    {1, 2, "Team 2 vs 2", "Two fighters per team"},
    {1, 3, "Team 3 vs 3", "Three fighters per team"},
    {2, 1, "Tag Duel", "One fighter each; your Assist calls in your next member, a KO brings the next in"},
    {3, 4, "Free-for-all", "Every player fights for himself (up to 4)"},
};
constexpr int kLobbyModeCount = sizeof kLobbyModes / sizeof kLobbyModes[0];
int lobby_mode_index(const uint8_t* L) { for (int i = 0; i < kLobbyModeCount; i++) if (kLobbyModes[i].mode == L[0] && (L[0] == 3 || kLobbyModes[i].size == L[1])) return i; return 0; }
int lobby_needed(const uint8_t*) { return 2; }   // players the room needs: party members fill the other places in team and free-for-all battles
int lobby_fighters(const uint8_t* L, int players) { return L[0] == 1 ? 2 * L[1] : L[0] == 3 ? (L[2] ? 4 : std::clamp(players, 2, 4)) : 2; }   // (CPU fill: free-for-all of four)   // fighters on the stage at once
std::string lobby_summary(const uint8_t* L) {
    return std::string(kLobbyModes[lobby_mode_index(L)].label) + "  /  " + kStages[std::min<int>(L[4], 34)] + "  /  " + kRules[std::min<int>(L[5], 5)] + "  /  " + kJudge[std::min<int>(L[6], 4)];
}
constexpr int kLobbyRows = 11;   // MODE, STAGE, RULES, JUDGMENT, EX BURST, ASSIST, CPU FILL, CPU STRENGTH, PLAYERS (single player free-for-all), ALL CPUs
int g_all_str = 5;   // the ALL CPUs row's strength (1..9)
uint32_t g_sp_screen_last = 0;
uint32_t g_sp_screen_last_fwd() { return g_sp_screen_last; }   // the single player setup screen the lobby opened over (sp_draw)
bool g_no_allcpu = std::getenv("PSPRECOMP_TAGTEAM_NOALLCPU") != nullptr;
// The rows the dialog shows (logical row numbers): single player has no ASSIST row (Square on the setup screen toggles it), CPU STRENGTH only with CPU FILL on.
int lobby_visible(const uint8_t* L, int out[kLobbyRows]) {
    int n = 0; const bool sp = dissidia_tagteam_lobby_sp();
    for (int r = 1; r < kLobbyRows; r++) { if (r == 4) continue; /* EX BURST rule: not offered (freeze always; keep fighting is a debug setting, docs Batch sixty-six) */ if (r == 5 && sp) continue; if (r == 7 && !L[2]) continue; if (r == 8 && !(sp && L[0] == 3)) continue; if (r == 9 && (g_no_allcpu || !dissidia_tagteam_lobby_editable())) continue; out[n++] = r; }
    return n;
}
const char* kLobbyRowName[kLobbyRows] = {"MODE", "STAGE", "RULES", "JUDGMENT", "EX BURST", "ASSIST", "CPU FILL", "CPU STRENGTH", "PLAYERS", "ALL CPUs", "CHASE SLOW"};
std::string lobby_value(int r, const uint8_t* L) {
    switch (r) {
    case 0: return kLobbyModes[lobby_mode_index(L)].label;
    case 1: return kStages[std::min<int>(L[4], 34)];
    case 2: return kRules[std::min<int>(L[5], 5)];
    case 3: return kJudge[std::min<int>(L[6], 4)];
    case 4: return L[7] ? "Keep fighting" : "Freeze";
    case 5: return L[8] ? "On" : "Off";
    case 6: return L[2] ? "On" : "Off";
    case 10: return L[9] ? "Chasers only" : "Everyone (retail)";
    case 8: return "P1 + " + std::to_string(dissidia_tagteam_ffa_players() - 1) + " CPU  >";
    case 9: return kStrName[std::clamp(g_all_str, 1, 9) - 1];
    default: return kStrName[std::clamp<int>(L[3], 1, 9) - 1];
    }
}
void lobby_change(int r, int dir, uint8_t* L) {
    switch (r) {
    case 0: { const int k = (lobby_mode_index(L) + dir + kLobbyModeCount) % kLobbyModeCount; L[0] = kLobbyModes[k].mode; L[1] = kLobbyModes[k].size; break; }
    case 1: L[4] = static_cast<uint8_t>((L[4] + dir + 35) % 35); break;
    case 2: L[5] = static_cast<uint8_t>((L[5] + dir + 6) % 6); break;
    case 3: L[6] = static_cast<uint8_t>((L[6] + dir + 5) % 5); break;
    case 4: L[7] ^= 1; break;
    case 5: L[8] ^= 1; break;
    case 6: L[2] ^= 1; break;
    case 10: L[9] ^= 1; break;
    case 8: break;
    case 9: {   // every CPU at once: CPU fill, the CPU partners, the free-for-all players, (single player) the enemy rounds' STRENGTH
        g_all_str = std::clamp(g_all_str + dir, 1, 9); L[3] = static_cast<uint8_t>(g_all_str);
        const bool sp = dissidia_tagteam_lobby_sp(); dissidia_tagteam_all_cpu_strength(g_all_str, sp);
        if (sp && g_sp_screen_last >= 0x08800000u && g_sp_screen_last < 0x0A000000u && !std::getenv("PSPRECOMP_TAGTEAM_ALLCPU_NOENEMY"))
            for (uint32_t i = 0; i < 5; i++) { uint8_t& t = g_ram[(g_sp_screen_last + 0xA5Bu + 0x16DCu * i) & 0x07FFFFFFu]; if (t <= 8) t = static_cast<uint8_t>(g_all_str - 1); }
        break;
    }
    default: L[3] = static_cast<uint8_t>(std::clamp(int(L[3]) + dir, 1, 9)); break;
    }
}
// Single player: the party rows of the Tournament screen (character, CPU flag) for the lobby's right half.
uint8_t g_sp_rows_c[5] = {}; int g_sp_rows_n = 0;   // set by sp_draw every frame
// ---- Full-screen screens in the game's look (22 Nov): opaque pale-lavender background like the Tournament screen, a dark navy title band that fades out to the right (as "Party A"),
// the selected row in the menus' blue band with white text, idle rows with a thin line under them, a navy button bar at the bottom.
constexpr float kScrW = 480.f, kScrH = 272.f;
// 25 Nov (user low priority list: "Battle Lobby: follow the game's design language"): restyled after the retail Options screen (screenshots of it and of the CPU settings
// dialog): pale periwinkle ground with soft swirls, a navy title ribbon with a bevel line and the serif title ("Options") in white with a dark edge, pale bevelled bars with
// pointed ends carrying a blue bullet, the label and the value between two blue triangles, the selected bar royal blue with white text and the pointing hand, a navy help panel
// at the bottom with the button hints (key in cyan). PSPRECOMP_TAGTEAM_LOBBYSTYLE_OLD=1: the flat 22 Nov look.
bool style_old() { static const bool v = std::getenv("PSPRECOMP_TAGTEAM_LOBBYSTYLE_OLD") != nullptr; return v; }
void gs_background() {
    g_layer.band(0.f, 0.f, kScrW, kScrH, [&](float x, float y) { const float t = y / kScrH; return LayerColor{230 - 18 * t + 6 * (x / kScrW), 234 - 16 * t, 252 - 8 * t, 1.f}; });
    if (style_old()) { g_layer.band(0.f, 36.f, kScrW, 37.f, [&](float, float) { return LayerColor{170, 176, 220, 1.f}; }); return; }
    g_layer.band(0.f, 0.f, kScrW, kScrH, [&](float x, float y) {   // a lighter diagonal sweep from the top right, a darker lavender haze at the lower left (the retail ground)
        const float d = (x / kScrW) * 0.8f - (y / kScrH) * 0.6f + 0.35f, k = std::clamp(1.f - std::fabs(d - 0.5f) * 3.2f, 0.f, 1.f);
        return LayerColor{255, 255, 255, 0.34f * k};
    });
    g_layer.glow(400.f, 70.f, 90.f, LayerColor{255, 255, 255, 0.22f});
    g_layer.glow(60.f, 230.f, 110.f, LayerColor{150, 150, 215, 0.20f});
    g_layer.glow(440.f, 200.f, 70.f, LayerColor{255, 255, 255, 0.18f});
}
void gs_title(const std::string& s, const std::string& sub = std::string()) {
    if (style_old()) {
        g_layer.band(0.f, 8.f, kScrW, 30.f, [&](float x, float y) {
            const float t = (y - 8.f) / 22.f, fade = x < 450.f ? 1.f : std::max(0.f, 1.f - (x - 450.f) / 30.f);
            return LayerColor{22 + 28 * (1 - t), 20 + 26 * (1 - t), 104 + 46 * (1 - t), fade};
        });
        g_layer.text(s, true, 10.5f, 16.f, 25.5f, LayerColor{255, 255, 255, 1.f});
        if (!sub.empty()) { const float w = g_layer.text(sub, true, 6.4f, 0, 0, LayerColor{0, 0, 0, 0}, true); g_layer.text(sub, true, 6.4f, kScrW - 24.f - w, 22.f, LayerColor{225, 228, 255, 1.f}); }
        return;
    }
    g_layer.band(0.f, 6.f, kScrW, 31.f, [&](float x, float y) {   // the navy ribbon: lighter at the top, a pale bevel line under it
        const float t = (y - 6.f) / 25.f, fade = x < 440.f ? 1.f : std::max(0.f, 1.f - (x - 440.f) / 40.f);
        if (y > 29.6f) return LayerColor{214, 218, 244, 0.95f * fade};
        return LayerColor{20 + 40 * (1 - t), 22 + 38 * (1 - t), 96 + 56 * (1 - t), fade};
    });
    // the serif title in small capitals with the dark edge the game's headings carry
    const std::string up = s;
    for (const auto& o : {std::pair<float, float>{0.9f, 0.9f}, {0.9f, 0.f}, {0.f, 0.9f}, {-0.6f, 0.f}, {0.f, -0.6f}, {0.9f, -0.6f}})
        g_layer.serif_caps(up, 15.5f, 0.76f, 0.5f, 16.f + o.first, 26.f + o.second, LayerColor{18, 16, 64, 0.8f});
    g_layer.serif_caps(up, 15.5f, 0.76f, 0.5f, 16.f, 26.f, LayerColor{255, 255, 255, 1.f});
    if (!sub.empty()) {   // the Options screen's "START Default" pill: a dark navy capsule with white text
        const float em = 6.2f, w = g_layer.text(sub, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true), x1 = kScrW - 14.f, x0 = x1 - w - 22.f, y0 = 9.f, y1 = 25.f;
        g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
            const float rr = (y1 - y0) / 2.f, cx = std::clamp(x, x0 + rr, x1 - rr), dy = y - (y0 + y1) / 2.f, d = std::sqrt((x - cx) * (x - cx) + dy * dy);
            const float t = (y - y0) / (y1 - y0);
            return LayerColor{14 + 22 * (1 - t), 14 + 22 * (1 - t), 40 + 40 * (1 - t), std::clamp((rr - d) * 2.f, 0.f, 1.f)};
        });
        g_layer.text(sub, true, em, x0 + 11.f, 20.4f, LayerColor{255, 255, 255, 1.f});
    }
}
// The hint line: "Key: action   Key: action"; the key part in the cyan of the Options screen's help panel, the action white. `help` (optional) is the row's help text above it.
void gs_footer(const std::string& s, const std::string& help = std::string()) {
    if (style_old()) {
        g_layer.band(0.f, 248.f, kScrW, kScrH, [&](float, float y) { const float t = (y - 248.f) / 24.f; return LayerColor{30 + 18 * (1 - t), 30 + 18 * (1 - t), 100 + 30 * (1 - t), 1.f}; });
        const float w = g_layer.text(s, true, 6.4f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(s, true, 6.4f, kScrW / 2.f - w / 2.f, 264.f, LayerColor{235, 238, 255, 1.f});
        return;
    }
    const float top = help.empty() ? 244.f : 222.f;
    g_layer.band(0.f, top, kScrW, kScrH, [&](float, float y) {   // the help panel: lavender-navy, lighter at its upper edge
        const float t = (y - top) / (kScrH - top);
        if (y < top + 1.2f) return LayerColor{206, 210, 244, 1.f};
        return LayerColor{66 - 28 * t, 62 - 26 * t, 150 - 40 * t, 1.f};
    });
    if (!help.empty()) g_layer.text(help, true, 6.6f, 22.f, top + 15.f, LayerColor{255, 255, 255, 1.f});
    // split the hints at runs of three spaces
    std::vector<std::pair<std::string, std::string>> parts; size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find("   ", i); if (e == std::string::npos) e = s.size();
        std::string seg = s.substr(i, e - i); const size_t c = seg.find(':');
        if (c == std::string::npos) parts.push_back({seg, ""}); else parts.push_back({seg.substr(0, c + 1), seg.substr(c + 1)});
        i = e; while (i < s.size() && s[i] == ' ') i++;
    }
    const float em = 6.3f, gap = 12.f; float tw = 0.f;
    for (const auto& p : parts) tw += g_layer.text(p.first + p.second, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true) + gap;
    tw -= gap; float x = kScrW / 2.f - tw / 2.f; const float by = kScrH - 8.f;
    for (const auto& p : parts) {
        x += g_layer.text(p.first, true, em, x, by, LayerColor{110, 232, 240, 1.f});
        x += g_layer.text(p.second, true, em, x, by, LayerColor{255, 255, 255, 1.f}) + gap;
    }
}
// a white-ish panel with a silver bevel and a soft shadow (the Party Select's party panel)
void gs_card(float x0, float y0, float x1, float y1) {
    if (style_old()) {
        g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
            const float e = std::min({x - x0, x1 - x, y - y0, y1 - y});
            return e < 1.f ? LayerColor{150, 156, 214, 1.f} : LayerColor{246, 247, 255, 1.f};
        });
        return;
    }
    g_layer.band(x0 + 1.5f, y0 + 2.f, x1 + 2.5f, y1 + 3.f, [&](float, float) { return LayerColor{90, 90, 150, 0.18f}; });
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float e = std::min({x - x0, x1 - x, y - y0, y1 - y}), t = (y - y0) / (y1 - y0);
        if (e < 1.f) return LayerColor{120, 124, 188, 1.f};
        if (e < 2.2f) return LayerColor{255, 255, 255, 1.f};
        return LayerColor{244 - 12 * t, 246 - 12 * t, 255 - 6 * t, 1.f};
    });
}
// a bar with pointed ends (the retail option bars): colour_at(t = 0..1 across, v = 0..1 down)
template <class F> void gs_bar(float x0, float x1, float y, float h, F colour_at) {
    const float pt = std::min(9.f, h * 0.55f);
    g_layer.band(x0, y, x1, y + h, [&](float x, float yy) {
        const float dx = std::min(x - x0, x1 - x), half = h / 2.f * (dx >= pt ? 1.f : 0.45f + 0.55f * dx / pt), dy = std::fabs(yy - (y + h / 2.f));
        const float m = std::clamp((half - dy) * 2.f, 0.f, 1.f);
        LayerColor c = colour_at(std::clamp((x - x0) / (x1 - x0), 0.f, 1.f), (yy - y) / h, std::min(dy / std::max(half, 0.01f), 1.f)); c.a *= m; return c;
    });
}
void gs_tri(float cx, float cy, float r, bool right, LayerColor c) {   // a solid triangle pointing right / left
    g_layer.band(cx - r, cy - r, cx + r, cy + r, [&](float x, float y) {
        const float u = right ? (cx + r - x) / (2 * r) : (x - (cx - r)) / (2 * r), half = r * u, m = std::clamp((half - std::fabs(y - cy)) * 2.f, 0.f, 1.f);
        return LayerColor{c.r, c.g, c.b, c.a * m * std::clamp((1.f - u) * 8.f + 0.2f, 0.f, 1.f) * 1.f};
    });
}
// a menu row: the selected one is the blue band with white text (value with < > when `arrows`), idle ones carry dark text and a thin line under them
void gs_row(float x0, float x1, float y, float h, bool sel, const std::string& name, const std::string& val, bool arrows, float em = 7.6f) {
    if (style_old()) {
        if (sel) g_layer.band(x0, y, x1, y + h, [&](float x, float yy) {
            const float t = std::clamp((x - x0) / (x1 - x0), 0.f, 1.f), v = (yy - y) / h;
            float r = 63 + 71 * t, g = 53 + 83 * t, b = 174 + 33 * t;
            if (v < 0.25f) { r += 24; g += 26; b += 9; } else if (v > 0.8f) { r -= 8; g -= 8; b -= 10; }
            return LayerColor{r, g, b, 1.f};
        });
        else g_layer.band(x0 + 4.f, y + h - 0.8f, x1 - 4.f, y + h, [&](float, float) { return LayerColor{119, 114, 149, 0.8f}; });
        const LayerColor tc = sel ? LayerColor{255, 255, 255, 1.f} : LayerColor{40, 42, 65, 1.f};
        const float by = y + h * 0.5f + em * 0.36f;
        g_layer.text(name, true, em, x0 + 10.f, by, tc);
        if (!val.empty()) {
            const std::string v = (sel && arrows ? "< " : "") + val + (sel && arrows ? " >" : "");
            const float vw = g_layer.text(v, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
            g_layer.text(v, true, em, x1 - 10.f - vw, by, tc);
        }
        return;
    }
    if (sel) gs_bar(x0, x1, y, h, [&](float t, float v, float) {   // royal blue, lighter top edge and right end (as the Options bar)
        float r = 63 + 71 * t * t, g = 53 + 83 * t * t, b = 174 + 33 * t * t;
        if (v < 0.22f) { r += 34; g += 36; b += 14; } else if (v > 0.82f) { r -= 12; g -= 12; b -= 18; }
        return LayerColor{r, g, b, 1.f};
    });
    else gs_bar(x0, x1, y, h, [&](float, float v, float e) {   // pale silver, a darker rim
        if (e > 0.86f) return LayerColor{128, 126, 164, 1.f};
        return LayerColor{248 - 40 * v, 249 - 40 * v, 255 - 26 * v, 1.f};
    });
    const LayerColor tc = sel ? LayerColor{255, 255, 255, 1.f} : LayerColor{40, 42, 65, 1.f};
    const float by = y + h * 0.5f + em * 0.36f, cy = y + h * 0.5f;
    auto put = [&](const std::string& t, float x) {
        if (sel) for (const auto& o : {std::pair<float, float>{0.6f, 0.6f}, {0.6f, 0.f}, {0.f, 0.6f}}) g_layer.text(t, true, em, x + o.first, by + o.second, LayerColor{30, 24, 80, 0.6f});
        g_layer.text(t, true, em, x, by, tc);
    };
    const float bx = x0 + 15.f;
    g_layer.band(bx, cy - 2.4f, bx + 4.8f, cy + 2.4f, [&](float, float) { return sel ? LayerColor{255, 255, 255, 1.f} : LayerColor{96, 98, 190, 1.f}; });
    put(name, bx + 9.f);
    if (!val.empty()) {
        const float cx = x0 + (x1 - x0) * 0.70f, vw = g_layer.text(val, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        put(val, cx - vw / 2.f);
        if (arrows) {
            const LayerColor tcol = sel ? LayerColor{255, 255, 255, 1.f} : LayerColor{70, 74, 200, 1.f};
            gs_tri(cx - vw / 2.f - 9.f, cy, 3.4f, false, tcol);
            gs_tri(cx + vw / 2.f + 9.f, cy, 3.4f, true, tcol);
        }
    }
    if (sel) dissidia_menu_draw_hand(g_layer, x0 - 22.f, cy - 12.f, 1.f);
}

// ---- The mode select (Battle Mode / Communications Mode > Tag Team Battle, 22 Nov): the first screen after choosing the entry; its choice decides the screens that follow
// (team modes: Party Select, the lobby; Free-for-all: Party Select, the Free-for-all player setup, the lobby).
int g_ffa_slot = 0, g_ffa_field = 0;   // the cursor: slot 0..3, field 0 TYPE, 1 MAIN, 2 TAG-IN, 3 STRENGTH
int64_t g_ffa_walk_t0 = 0; bool g_ffa_walk_seen = false;   // 26 Nov: the FFA battle-start walk (list confirm > state 16 -> 28 > setup screen > Enter Battle > app 0x1D .. 0x30), logged as [FFAWALK]
// 26 Nov (user: "I don't want there to be a menu walk"): the single player Free-for-all battle is started DIRECTLY. No setup-screen list tick, dialog, cursor or Cross is waited for or passed:
// at the first app frame in state 29 (the setup screen object exists, its data is the vehicle the app hands to the battle) the lobby's stage / rules / judgement are written into it and the screen's own
// Enter Battle step is called at once; after the battle the same first frame leaves the app for the players screen. [DIRECTSTART] / [DIRECTBACK] log. PSPRECOMP_TAGTEAM_MENUWALK_OLD=1: the old walk (autostart on the list's tick).
bool menuwalk_old() { static const bool o = std::getenv("PSPRECOMP_TAGTEAM_MENUWALK_OLD") != nullptr; return o; }
bool g_nw_start = false, g_nw_armed = false, g_nw_direct = false; int64_t g_nw_t0 = 0;   // g_nw_start: a direct start is waiting for the app's state 29; g_nw_armed: the battle has run (the app left), the next state 29 is the way back
bool ffawalk_old() { static const bool o = std::getenv("PSPRECOMP_TAGTEAM_FFAWALK_OLD") != nullptr; return o; }
void ffawalk_log(const char* step, int st) { std::fprintf(stderr, "[FFAWALK] %s (t=+%.1fs, app state %d)\n", step, g_ffa_walk_t0 ? (steady_ms() - g_ffa_walk_t0) / 1000.0 : 0.0, st); }
bool g_rearm = std::getenv("PSPRECOMP_TAGTEAM_TEST_REARM") != nullptr;   // 25 Nov (user: back out past the lobby, then forward again skipped it): set when the flow was backed out of; once the setup screen has been gone for 0.5 s the lobby (FFA: the players screen) is pending again. PSPRECOMP_TAGTEAM_MENUBACK_OLD=1: old
bool g_ffa_then_lobby = false, g_lobby_pending = false, g_ffa_flow = false, g_ffa_autostart = false;
bool g_ms_pre = false, g_ms_hop = false, g_ms_done = false;   // 25 Nov (user: Free-for-all goes DIRECTLY to the players menu): the players screen and the lobby are hosted by the Mode Select (retail's Party Select / CPU Select are passed by a script afterwards; g_ms_hop: the player asked for the game's selector instead)
std::atomic<int> g_script_lim{1000}; std::atomic<int> g_ps_frames{0};   // frames Party Select's ready test ran since the script started   // the scripted pass stops after the event that starts at or before this poll
// 26 Nov (user: every Free-for-all pick through the game's real Player Select; no pad injections, no Party Select / CPU Select): the Quick Battle app (ovl_menu, app = *0x08AA3F44,
// P = app + 0x154) is one state machine, P + 0x910 = its state (PMF table 0x08AA3F48, run by FUN_08A97DA8), P + 0x914 = the state after a transition state.
//   2 picks the route; 1-on-1: 4 makes Player Select (FUN_08AA0D48(app, 0)), 5 runs it (FUN_08A9E0E8): on YES the pick (FUN_08A81188 = charId) goes to P + 0x804C and the state
//   becomes 6 with 8 (CPU Select) next; Circle: 6 with 0x30 next (0x30 / 0x31 = fade out and leave to the Battle Mode list). Party route: 16 / 17 Party Select, 20 / 21 CPU Select,
//   22 > 28 builds the Tournament setup screen from the party records (P + 0x7C10 Party A, 0x13C bytes; P + 0x7D4C the CPU party; P + 0x82F8 the lineup), 29 runs it.
// The Free-for-all now opens the 1-on-1 route for a pick and, on YES, takes the pick and sends the app to 0x30 (back to the list, where the players screen opens again);
// the battle opens the Party route and jumps from 16 straight to 28 with the party records written (no Party Select / CPU Select object is ever made).
// PSPRECOMP_TAGTEAM_FFA_PSOLD=1: the old CPU Select hop and scripted pass.
enum QbMode { QB_NONE = 0, QB_PICK, QB_RETURN, QB_START, QB_DETOUR };   // QB_DETOUR: Player Select went to another app (Customization, state 50) and comes back to the same Player Select
int g_qb_mode = QB_NONE;
int g_trip_out = 0; bool g_trip_online = false; int64_t g_trip_t0 = 0, g_qb_last_ms = 0, g_main_seen_ms = 0, g_trip_in_t0 = 0;   // the trips' fades (trip_tick)
void trip_draw(float white, bool own_frame); float trip_in_white(); void start_cover_draw(); float post_white();
int64_t g_post_exit_t0 = 0, g_post_in_t0 = 0;
bool g_leave_app = false, g_trip_post = false, g_start_cover = false; int64_t g_start_exit_t0 = 0;   // app levers (qb hooks below) and the start card
bool ps_old() { static const bool o = std::getenv("PSPRECOMP_TAGTEAM_FFA_PSOLD") != nullptr; return o; }
int g_ps_pick = 0, g_ps_look = 0;   // the last Player Select pick (charId, 0 = none / cancelled) and its LOOK (save record +0x774)
int g_hop_slot = -1, g_hop_field = 1;   // the cursor to restore when the players screen comes back from a retail selector
uint8_t g_seen_p1[2] = {0xFF, 0xFF}, g_seen_pick[3] = {0xFF, 0xFF, 0xFF};   // the retail values (party's first two, CPU Select's picks) already taken over (0xFF = none yet)
bool g_ms_first = true, g_ms_ffa = false; int g_ms_sel = 1;
const char* const kModeSelHelp[] = {
    "One character each, no tag partner: the first KO decides. Party Select, then the lobby.",
    "Two fighters a side: CPU rows, CPU fill or the enemy party fill the places. Party Select, then the lobby.",
    "Three fighters a side. Party Select, then the lobby.",
    "One fighter each on the stage; when one falls the next party member tags in, until a team has nobody left.",
    "Everyone for himself, up to 4 players with a character each (and a tag-in). Party Select, then the Free-for-all player setup.",
};
std::atomic<int> g_ms_result{0};
// 26 Nov (user: the Mode Select is laggy): PSPRECOMP_TAGTEAM_MSLOG=1 logs the press -> detect -> drawn latency and the draw cost per frame ([MSLAT])
bool ms_log() { static const bool v = std::getenv("PSPRECOMP_TAGTEAM_MSLOG") != nullptr; return v; }
std::chrono::steady_clock::time_point g_ms_press_t{};
bool ms_lag_old() { static const bool v = std::getenv("PSPRECOMP_TAGTEAM_MSLAG_OLD") != nullptr; return v; }
void modesel_draw(bool group_online); void modesel_prebake(bool group_online);
// Returns 0 while open, 1 = chosen (the lobby holds the mode now), 2 = cancelled.
int modesel_frame(uint8_t* rdram, recomp_context* ctx, bool group_online) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    const uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    uint32_t pressed = b & ~g_prev_buttons; g_prev_buttons = b;
    uint8_t L[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1};
    dissidia_tagteam_lobby_get(L);
    if (g_ms_first) { g_ms_first = false; pressed = 0; g_ms_sel = lobby_mode_index(L); (void)dt; }
    int result = 0;
    static auto last_f = now;
    if (ms_log()) { std::fprintf(stderr, "[MSLAT] frame gap %.1f ms%s\n", std::chrono::duration<double, std::milli>(now - last_f).count(), (pressed & (BTN_UP | BTN_DOWN)) ? " PRESS" : ""); last_f = now; }
    // 26 Nov (user: "just moving up and down takes too long"): a held Up / Down repeats like the retail lists (first after 400 ms, then every 100 ms). PSPRECOMP_TAGTEAM_MSLAG_OLD=1: old (no repeat)
    uint32_t moves = pressed & (BTN_UP | BTN_DOWN);
    if (!ms_lag_old()) {
        static std::chrono::steady_clock::time_point next{}; static uint32_t held = 0;
        const uint32_t dir = b & (BTN_UP | BTN_DOWN) & ~((b & BTN_UP) ? BTN_DOWN : 0u);   // (Up wins when both are down)
        if (!dir) held = 0;
        else if (dir != held || moves) { held = dir; next = now + std::chrono::milliseconds(400); }
        else if (now >= next) { moves |= dir; if (ms_log()) std::fprintf(stderr, "[MSLAT] repeat\n"); next = std::max(next + std::chrono::milliseconds(100), now - std::chrono::milliseconds(100)); }
    }
    if (moves & BTN_UP) { g_ms_sel = (g_ms_sel + kLobbyModeCount - 1) % kLobbyModeCount; play_se(rdram, ctx, 1); }
    else if (moves & BTN_DOWN) { g_ms_sel = (g_ms_sel + 1) % kLobbyModeCount; play_se(rdram, ctx, 1); }
    if (pressed & BTN_CROSS) { g_ms_ffa = kLobbyModes[g_ms_sel].mode == 3; g_seen_p1[0] = g_seen_p1[1] = 0xFF; g_seen_pick[0] = g_seen_pick[1] = g_seen_pick[2] = 0xFF; g_hop_slot = -1; g_ms_hop = false; g_ms_done = false; L[0] = kLobbyModes[g_ms_sel].mode; L[1] = kLobbyModes[g_ms_sel].size; dissidia_tagteam_lobby_set(L); play_se(rdram, ctx, 2); result = 1;
        if (g_ms_ffa && !group_online && !std::getenv("PSPRECOMP_TAGTEAM_FFA_PREOLD")) {   // the Free-for-all: no Party Select first, the players screen (then the lobby) right here
            result = 0; g_ms_pre = true; g_ffa_flow = true; g_ffa_then_lobby = true; g_dlg_mode = 3; g_lob_sp = true; g_dlg_bp = false; g_dlg_sp = false; g_ffa_slot = 1; g_ffa_field = 1; g_lob_row = 0;
            uint8_t p1[2] = {0, 255}; dissidia_tagteam_ffa_p1_get(p1); (void)p1;
            std::fprintf(stderr, "[FFA] Mode Select: Free-for-all, the players screen first\n");
        } }
    else if (pressed & BTN_CIRCLE) { play_se(rdram, ctx, 1); result = 2; }
    const auto d0 = std::chrono::steady_clock::now();
    modesel_draw(group_online);
    if (!result && !moves) modesel_prebake(group_online);
    if (ms_log()) { const auto d1 = std::chrono::steady_clock::now(); std::fprintf(stderr, "[MSLAT] draw %.1f ms%s\n", std::chrono::duration<double, std::milli>(d1 - d0).count(),
        (pressed & (BTN_UP | BTN_DOWN)) ? "" : ""); if (pressed & (BTN_UP | BTN_DOWN)) std::fprintf(stderr, "[MSLAT] press -> published %.1f ms (sel %d)\n", std::chrono::duration<double, std::milli>(d1 - g_ms_press_t).count(), g_ms_sel); }
    return result;
}
void modesel_draw_body(bool group_online);
const char* dlg_tag() { switch (g_dlg_mode) { case 0: return "cpu_dialog"; case 1: return "picker"; case 2: return "lobby"; case 3: return "ffa_players"; case 4: return "ffa_picker"; case 5: return "modesel"; default: return "dialog"; } }
// 26 Nov (user: the Mode Select is laggy; measured on the host 33 ms per frame, ~20 fps): the whole 960x544 layer was rasterised again every frame (gradients, glows, serif title,
// text) although only the cursor row changes. Now each finished image (cursor row x online x scale) is baked once and kept; a frame that shows the same image only extends the layer's
// life (no raster, no upload), a move publishes the cached image of the new row, and the other rows are rendered ahead on idle frames. PSPRECOMP_TAGTEAM_MSLAG_OLD=1: old.
struct MsImg { std::shared_ptr<const std::vector<uint32_t>> img; int w = 0, h = 0; };
std::map<int, MsImg> g_ms_cache; uint64_t g_ms_pub_serial = 0; int g_ms_pub_key = -1;
int ms_key(int sel, bool online, int sc) { return (sc << 8) | (online ? 16 : 0) | sel; }
const MsImg& ms_bake(int sel, bool online, int sc) {
    auto it = g_ms_cache.find(ms_key(sel, online, sc));
    if (it != g_ms_cache.end()) return it->second;
    const int keep = g_ms_sel; g_ms_sel = sel;
    g_layer.begin_raw(sc); modesel_draw_body(online); g_ms_sel = keep;
    MsImg m; m.img = g_layer.bake(); m.w = g_layer.width(); m.h = g_layer.height();
    return g_ms_cache.emplace(ms_key(sel, online, sc), std::move(m)).first->second;
}
void modesel_draw(bool group_online) {
    const int sc = std::max(2, psp_sddialog::output_scale());
    if (ms_lag_old()) { g_layer.begin_raw(sc); modesel_draw_body(group_online); g_layer.publish(); return; }
    const int key = ms_key(g_ms_sel, group_online, sc);
    if (key == g_ms_pub_key && ge_draw_game_layer_serial() == g_ms_pub_serial && ge_draw_game_layer_touch()) return;
    const MsImg& m = ms_bake(g_ms_sel, group_online, sc);
    dissidia::MenuLayer::publish_baked(*m.img, m.w, m.h);
    g_ms_pub_serial = ge_draw_game_layer_serial(); g_ms_pub_key = key;
}
void modesel_prebake(bool group_online) {   // one missing row's image per idle frame
    if (ms_lag_old()) return;
    const int sc = std::max(2, psp_sddialog::output_scale());
    for (int k = 0; k < kLobbyModeCount; k++) if (!g_ms_cache.count(ms_key(k, group_online, sc))) { ms_bake(k, group_online, sc); return; }
}
void modesel_draw_body(bool group_online) {
    gs_background(); gs_title("MODE SELECT", group_online ? "Communications  >  Tag Team Battle" : "Battle Mode  >  Tag Team Battle");
    const float x0 = 70.f, x1 = 410.f, y0 = 56.f, h = 28.f;
    for (int i = 0; i < kLobbyModeCount; i++) gs_row(x0, x1, y0 + h * float(i), h - 2.f, i == g_ms_sel, kLobbyModes[i].label, "", false, 9.f);
    gs_card(40.f, 204.f, 440.f, 240.f);
    static_assert(sizeof kModeSelHelp / sizeof kModeSelHelp[0] == kLobbyModeCount, "one help line per mode");
    const std::string help = kModeSelHelp[std::clamp(g_ms_sel, 0, kLobbyModeCount - 1)];
    // wrap at ~66 characters
    std::string l1 = help, l2; if (help.size() > 66) { size_t c = help.rfind(' ', 66); l1 = help.substr(0, c); l2 = help.substr(c + 1); }
    g_layer.text(l1, true, 6.4f, 50.f, 220.f, LayerColor{40, 42, 80, 1.f});
    if (!l2.empty()) g_layer.text(l2, true, 6.4f, 50.f, 232.f, LayerColor{40, 42, 80, 1.f});
    gs_footer("Up/Down: choose   Cross: confirm   Circle: back");
}

// ---- The Free-for-all player setup (single player): up to four players, each with his own colour. P1 is you (his characters are the first two rows of your party from Party Select),
// P2..P4 are CPU slots with a main character, a tag-in character and a strength. Random (main 0) = a character picked by CPU fill's picker.
const LayerColor kSlotCol[4] = {{40, 90, 220, 1.f}, {210, 50, 60, 1.f}, {40, 160, 80, 1.f}, {220, 150, 20, 1.f}};
const char* const kSlotName[4] = {"P1", "P2", "P3", "P4"};
int g_ffa_after = 0;   // a Free-for-all battle started from the flow: 1 = started, 2 = the setup screen went away (the battle ran), 0 = off
// 25 Nov: Cross on a MAIN (or P1's tag-in) of the players screen opens the game's own selector: the injected Circle(s) take retail's setup screen back to CPU Select (P2..P4) or, with a second one
// 1.8 s later, to Party Select (P1); the picks the player makes there come back as the players screen opens again (ffa_prefill: only what changed in retail is taken over). PSPRECOMP_TAGTEAM_FFA_GRIDMAIN=1: the grid instead.
std::chrono::steady_clock::time_point g_cover_until{};   // the Free-for-all starts without the retail setup screen: it is covered meanwhile
uint8_t g_roster[40] = {}; int g_roster_n = 0, g_pk_sel = 0, g_pk_field = 0, g_pk_slot = 0;
std::string slot_char_text(uint8_t c, bool tag) { return c == 254 ? std::string("Off") : c == 255 ? std::string("None") : c ? dissidia_tagteam_char_name(c) : std::string("Random"); }   // tag-in 255 = None
bool ffa_picker_open() { return g_dlg_mode == 4; }
std::vector<uint8_t> g_pk_opts;   // the list's entries: 0 = Random, 255 = None (tag-in only), then the roster
// 25 Nov (user: "use the game's own character selector, not a list"): the picker is a grid of the characters' face strips (the same art the battle HUD uses,
// "menu/jp/common/chara_face_in_name"), like the game's CPU Select grid: Up / Down / Left / Right move the cursor, Cross picks, Circle goes back to the players
// screen. The special entries (Random, None for a tag-in, Off for P3 / P4) are text tiles in front. PSPRECOMP_TAGTEAM_FFA_LISTPICK=1: the old text list.
bool pk_grid() { static const bool g = std::getenv("PSPRECOMP_TAGTEAM_FFA_LISTPICK") == nullptr; return g; }
constexpr int kPkCols = 5;
void ffa_picker(uint8_t* rdram, recomp_context* ctx, uint32_t pressed) {
    const int n = static_cast<int>(g_pk_opts.size());
    const bool grid = pk_grid();
    if (pressed & BTN_CIRCLE) { g_dlg_mode = 3; play_se(rdram, ctx, 1); return; }
    if (grid) {
        if (pressed & BTN_UP) { g_pk_sel = g_pk_sel >= kPkCols ? g_pk_sel - kPkCols : std::min(n - 1, ((n - 1) / kPkCols) * kPkCols + g_pk_sel % kPkCols); play_se(rdram, ctx, 1); }
        if (pressed & BTN_DOWN) { g_pk_sel = g_pk_sel + kPkCols < n ? g_pk_sel + kPkCols : g_pk_sel % kPkCols; play_se(rdram, ctx, 1); }
        if (pressed & BTN_LEFTK) { g_pk_sel = (g_pk_sel + n - 1) % n; play_se(rdram, ctx, 1); }
        if (pressed & BTN_RIGHT) { g_pk_sel = (g_pk_sel + 1) % n; play_se(rdram, ctx, 1); }
    } else {
        if (pressed & BTN_UP) { g_pk_sel = (g_pk_sel + n - 1) % n; play_se(rdram, ctx, 1); }
        if (pressed & BTN_DOWN) { g_pk_sel = (g_pk_sel + 1) % n; play_se(rdram, ctx, 1); }
        if (pressed & BTN_L) { g_pk_sel = std::max(0, g_pk_sel - 7); play_se(rdram, ctx, 1); }
        if (pressed & BTN_R) { g_pk_sel = std::min(n - 1, g_pk_sel + 7); play_se(rdram, ctx, 1); }
    }
    if (pressed & BTN_CROSS) {
        const uint8_t v = g_pk_opts[g_pk_sel];
        if (g_pk_slot == 0) { uint8_t p[2]; dissidia_tagteam_ffa_p1_get(p); p[g_pk_field - 1] = v; if (g_pk_field == 1 && v && v == p[1]) p[1] = 0; dissidia_tagteam_ffa_p1_set(p); }
        else { uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(g_pk_slot - 1, sl); if (g_pk_field == 1 && v == 254) { sl[0] = 0; sl[1] = 0; } else { sl[g_pk_field] = v; if (g_pk_field == 1) sl[0] = 1; } if (g_pk_field == 1 && v && v == sl[2]) sl[2] = 0; dissidia_tagteam_ffa_set(g_pk_slot - 1, sl); }
        g_dlg_mode = 3; play_se(rdram, ctx, 2); return;
    }
    gs_background(); gs_title(std::string("CHARACTER FOR ") + kSlotName[g_pk_slot], g_pk_field == 1 ? "Main fighter" : "Tag-in fighter");
    if (!grid) {
        const int vis = 8, top = std::clamp(g_pk_sel - vis / 2, 0, std::max(0, n - vis));
        for (int i = 0; i < vis && top + i < n; i++) {
            const int k = top + i;
            gs_row(110.f, 370.f, 48.f + 24.f * float(i), 22.f, k == g_pk_sel, slot_char_text(g_pk_opts[k], g_pk_field == 2), "", false, 8.f);
        }
        gs_footer("Up/Down  L/R: page   Cross: choose   Circle: back");
        return;
    }
    const float tw = 88.f, th = 20.f, gx = 8.f, gy = 2.f, x0 = (kScrW - (kPkCols * tw + (kPkCols - 1) * gx)) / 2.f, y0 = 44.f;
    const int rows = (n + kPkCols - 1) / kPkCols;
    const float ph = rows > 8 ? 17.f : th, pgy = rows > 8 ? 2.f : gy;
    for (int k = 0; k < n; k++) {
        const float x = x0 + float(k % kPkCols) * (tw + gx), y = y0 + float(k / kPkCols) * (ph + pgy);
        const bool sel = k == g_pk_sel; const uint8_t v = g_pk_opts[k];
        g_layer.band(x - (sel ? 2.f : 1.f), y - (sel ? 2.f : 1.f), x + tw + (sel ? 2.f : 1.f), y + ph + (sel ? 2.f : 1.f), [&](float, float) { return sel ? LayerColor{255, 150, 40, 1.f} : LayerColor{150, 156, 214, 1.f}; });
        g_layer.band(x, y, x + tw, y + ph, [&](float, float yy) { const float t = (yy - y) / ph; return sel ? LayerColor{90 + 50 * (1 - t), 96 + 50 * (1 - t), 210.f, 1.f} : LayerColor{30, 32, 60, 1.f}; });
        int iw = 0, ih = 0; const uint32_t* px = (v >= 1 && v <= 33) ? dissidia_tagteam_face_px(dissidia_tagteam_char_code(v), &iw, &ih) : nullptr;
        if (px && iw > 0) g_layer.image(px, iw, ih, 0, 0, iw, std::min(ih, 26), x, y, tw, ph, sel ? 1.f : 0.85f);
        else { const std::string t = slot_char_text(v, g_pk_field == 2); const float w = g_layer.text(t, true, 7.f, 0, 0, LayerColor{0, 0, 0, 0}, true); g_layer.text(t, true, 7.f, x + (tw - w) / 2.f, y + ph / 2.f + 2.6f, LayerColor{235, 238, 255, 1.f}); }
    }
    const std::string cur = slot_char_text(g_pk_opts[g_pk_sel], g_pk_field == 2);
    const float cw = g_layer.text(cur, true, 9.f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(cur, true, 9.f, (kScrW - cw) / 2.f, 240.f, LayerColor{40, 42, 80, 1.f});
    gs_footer("Arrows: choose   Cross: confirm   Circle: back");
}
// which fields of a slot can be selected: bit 1 MAIN, bit 2 TAG-IN, bit 3 STRENGTH (P1: no strength; an Off slot: only MAIN, where "Off" is an entry)
int ffa_fields(int slot) {
    if (slot == 0) return 0x6;
    uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(slot - 1, sl);
    return sl[0] ? 0xE : 0x2;
}
void ffa_screen(uint8_t* rdram, recomp_context* ctx, uint32_t pressed) {
    if (g_dlg_mode == 4) { ffa_picker(rdram, ctx, pressed); return; }
    uint8_t slots[3][4]; for (int i = 0; i < 3; i++) dissidia_tagteam_ffa_get(i, slots[i]);
    auto finish = [&]() { g_dlg_open = false; g_dlg_mode = 0; play_se(rdram, ctx, 1); if (g_ffa_then_lobby) { g_ffa_then_lobby = false; g_dlg_mode = 2; g_dlg_open = true; } };
    if ((pressed & BTN_CIRCLE) && g_ms_pre) { g_ms_pre = false; g_dlg_mode = 5; g_ms_first = true; g_ffa_flow = false; g_ffa_then_lobby = false; play_se(rdram, ctx, 1); return; }   // (hosted by the Mode Select: back to it)
    if ((pressed & BTN_CIRCLE) && g_ffa_flow && !std::getenv("PSPRECOMP_TAGTEAM_FFA_OLDBACK")) {   // user 23 Nov: "Circle on the players screen starts the battle instead of going back":
        // in the Free-for-all flow Circle leaves the flow (no battle starts); the setup screen under it is back (Select: the lobby, Circle: retail's way back)
        g_dlg_open = false; g_dlg_mode = 0; g_ffa_then_lobby = false; g_ffa_flow = false; g_rearm = true; play_se(rdram, ctx, 1);
        std::fprintf(stderr, "[FFA] players screen: Circle, back to the setup screen (no battle)\n");
        return;
    }
    if (pressed & (BTN_CIRCLE | BTN_OPEN)) { finish(); return; }
    auto clamp_field = [&]() { const int m = ffa_fields(g_ffa_slot); if (!((m >> g_ffa_field) & 1)) { for (int f = 1; f <= 3; f++) if ((m >> f) & 1) { g_ffa_field = f; break; } } };
    if (pressed & BTN_UP) { g_ffa_slot = (g_ffa_slot + 4) % 5; clamp_field(); play_se(rdram, ctx, 1); }
    if (pressed & BTN_DOWN) { g_ffa_slot = (g_ffa_slot + 1) % 5; clamp_field(); play_se(rdram, ctx, 1); }
    if (g_ffa_slot < 4 && (pressed & (BTN_LEFTK | BTN_RIGHT))) {
        const int m = ffa_fields(g_ffa_slot), d = (pressed & BTN_RIGHT) ? 1 : -1;
        for (int k = 1, f = g_ffa_field; k <= 3; k++) { f += d; if (f < 1 || f > 3) break; if ((m >> f) & 1) { g_ffa_field = f; play_se(rdram, ctx, 1); break; } }
    }
    clamp_field();
    if ((pressed & BTN_L) && g_ffa_slot < 4 && (g_ffa_field == 1 || g_ffa_field == 2)) {   // L: P3 / P4 on <-> Off (MAIN), a tag-in Random <-> None
        if (g_ffa_field == 1 && g_ffa_slot >= 2) { uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(g_ffa_slot - 1, sl); sl[0] = sl[0] ? 0 : 1; if (!sl[0]) sl[1] = 0; dissidia_tagteam_ffa_set(g_ffa_slot - 1, sl); play_se(rdram, ctx, 1); }
        else if (g_ffa_field == 2) { if (g_ffa_slot == 0) { uint8_t p1[2]; dissidia_tagteam_ffa_p1_get(p1); p1[1] = p1[1] == 255 ? 0 : 255; dissidia_tagteam_ffa_p1_set(p1); } else { uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(g_ffa_slot - 1, sl); sl[2] = sl[2] == 255 ? 0 : 255; dissidia_tagteam_ffa_set(g_ffa_slot - 1, sl); } play_se(rdram, ctx, 1); }
    }
    if ((pressed & BTN_SQUARE) && std::getenv("PSPRECOMP_TAGTEAM_FFA_GRIDPICK") && g_ffa_slot < 4 && g_ffa_field == 1) {   // Square on a MAIN: the quick grid (Random / Off, no trip to the game's selector)
        uint8_t sl[4] = {1, 0, 0, 5}; uint8_t p1[2] = {0, 0}; if (g_ffa_slot == 0) dissidia_tagteam_ffa_p1_get(p1); else dissidia_tagteam_ffa_get(g_ffa_slot - 1, sl);
        uint8_t ros[40]; const int rn = dissidia_tagteam_roster(ros); const uint8_t cur = g_ffa_slot == 0 ? p1[0] : (!sl[0] ? 254 : sl[1]);
        g_pk_opts.clear(); g_pk_opts.push_back(0); if (g_ffa_slot >= 2) g_pk_opts.push_back(254); for (int k = 0; k < rn; k++) g_pk_opts.push_back(ros[k]);
        g_pk_slot = g_ffa_slot; g_pk_field = 1; g_pk_sel = 0; for (size_t k = 0; k < g_pk_opts.size(); k++) if (g_pk_opts[k] == cur) g_pk_sel = static_cast<int>(k);
        g_dlg_mode = 4; play_se(rdram, ctx, 1); return;
    }
    if (pressed & BTN_CROSS) {
        if (g_ffa_slot == 4) { finish(); return; }
        uint8_t sl[4] = {1, 0, 0, 5}; uint8_t p1[2] = {0, 0}; if (g_ffa_slot == 0) dissidia_tagteam_ffa_p1_get(p1); else dissidia_tagteam_ffa_get(g_ffa_slot - 1, sl);
        auto open_list = [&](uint8_t cur) {
            uint8_t ros[40]; const int rn = dissidia_tagteam_roster(ros);
            g_pk_opts.clear(); g_pk_opts.push_back(0);
            if (g_ffa_field == 2) g_pk_opts.push_back(255);          // tag-in: None
            if (g_ffa_field == 1 && g_ffa_slot >= 2) g_pk_opts.push_back(254);   // P3 / P4: Off
            for (int k = 0; k < rn; k++) g_pk_opts.push_back(ros[k]);
            g_pk_slot = g_ffa_slot; g_pk_field = g_ffa_field; g_pk_sel = 0;
            for (size_t k = 0; k < g_pk_opts.size(); k++) if (g_pk_opts[k] == cur) g_pk_sel = static_cast<int>(k);
            g_dlg_mode = 4; play_se(rdram, ctx, 1);
        };
        static const bool grid_main = std::getenv("PSPRECOMP_TAGTEAM_FFA_GRIDMAIN") != nullptr;
        if (g_ffa_flow && !grid_main && g_ffa_field <= 2 && g_ms_pre && !ps_old()) {   // 26 Nov: the game's real Player Select (Quick Battle 1-on-1) for every fighter field
            g_hop_slot = g_ffa_slot; g_hop_field = g_ffa_field;
            g_ms_pre = false; g_ms_hop = true; g_dlg_open = false; g_dlg_mode = 0; g_ffa_then_lobby = false; g_ffa_flow = false; play_se(rdram, ctx, 2);
            g_qb_mode = QB_PICK;
            std::fprintf(stderr, "[FFA] players screen: P%d %s -> the game's Player Select (Quick Battle 1-on-1)\n", g_ffa_slot + 1, g_ffa_field == 1 ? "main" : "tag-in");
            return;
        }
        if (g_ffa_flow && !grid_main && g_ffa_field <= 2) {   // the game's own character grid (CPU Select) for every fighter field; its 1st pick comes back
            g_hop_slot = g_ffa_slot; g_hop_field = g_ffa_field;
            if (g_sp_screen_last >= 0x08800000u && g_sp_screen_last < 0x0A000000u) for (uint32_t k = 0; k < 40u; k++) g_ram[(g_sp_screen_last + 0x8B4u + k) & 0x07FFFFFFu] = 0;   // CPU Select starts empty: its 1st pick is the answer
            if (g_ms_pre) {   // still in the Mode Select: let retail's screens come now (Party Select first; P2..P4: one scripted Cross on it, then CPU Select is the player's)
                g_ms_pre = false; g_ms_hop = true; g_dlg_open = false; g_dlg_mode = 0; g_ffa_then_lobby = false; g_ffa_flow = false; play_se(rdram, ctx, 2);
                std::fprintf(stderr, "[FFA] players screen (Mode Select): P%d %s -> the game's %s\n", g_ffa_slot + 1, g_ffa_field == 1 ? "main" : "tag-in", "character select");
                return;
            }
            g_dlg_open = false; g_dlg_mode = 0; g_ffa_then_lobby = false; g_ffa_flow = false; g_rearm = true; play_se(rdram, ctx, 2);
            if (dissidia_tagteam_inject_old()) g_inject_circle.store(6, std::memory_order_relaxed); else sp_back_direct(rdram, ctx);
            std::fprintf(stderr, "[FFA] players screen: P%d %s -> the game's %s\n", g_ffa_slot + 1, g_ffa_field == 1 ? "main" : "tag-in", "character select");
            return;
        }
        if (g_ffa_slot == 0) open_list(p1[g_ffa_field - 1]);
        else if (g_ffa_field == 3) { sl[3] = static_cast<uint8_t>(sl[3] % 9 + 1); dissidia_tagteam_ffa_set(g_ffa_slot - 1, sl); play_se(rdram, ctx, 1); }
        else open_list(g_ffa_field == 1 && !sl[0] ? 254 : sl[g_ffa_field]);
    }
    gs_background(); gs_title("FREE-FOR-ALL", "Everyone fights for himself");
    const float xa = 24.f, xb = 456.f, cx[4] = {0.f, 100.f, 210.f, 336.f};
    const char* head[4] = {"", "MAIN", "TAG-IN", "STRENGTH"};
    for (int f = 1; f < 4; f++) g_layer.text(head[f], true, 5.6f, cx[f] + 36.f, 46.f, LayerColor{80, 84, 140, 1.f});
    for (int p = 0; p < 4; p++) {
        const float y = 50.f + 42.f * float(p), h = 38.f;
        const bool human = p == 0, on = human || slots[p - 1][0];
        gs_card(xa, y, xb, y + h);
        const LayerColor col = kSlotCol[p];
        g_layer.band(xa, y, xa + 30.f, y + h, [&](float, float) { return LayerColor{on ? col.r : 150.f, on ? col.g : 150.f, on ? col.b : 160.f, 1.f}; });
        g_layer.text(kSlotName[p], true, 10.f, xa + 4.f, y + 23.f, LayerColor{255, 255, 255, 1.f});
        g_layer.text(human ? "You" : on ? "CPU" : "Off", true, 5.4f, xa + 36.f, y + 11.f, LayerColor{110, 114, 160, 1.f});
        std::string vals[4];
        if (human) { uint8_t p1[2]; dissidia_tagteam_ffa_p1_get(p1); vals[1] = slot_char_text(p1[0], false); vals[2] = slot_char_text(p1[1], true); }
        else { const uint8_t* s = slots[p - 1]; vals[1] = s[0] ? slot_char_text(s[1], false) : "Off"; vals[2] = s[0] ? slot_char_text(s[2], true) : "-"; vals[3] = s[0] ? kStrName[std::clamp<int>(s[3], 1, 9) - 1] : "-"; }
        const int m = ffa_fields(p);
        for (int f = 1; f < 4; f++) {
            if (vals[f].empty()) continue;
            const bool sel = p == g_ffa_slot && f == g_ffa_field && ((m >> f) & 1);
            const float fx0 = cx[f] + 36.f - 6.f, fx1 = (f == 3 ? xb : cx[f + 1] + 36.f) - 8.f;
            if (sel) g_layer.band(fx0, y + 14.f, fx1, y + h - 4.f, [&](float, float yy) { return LayerColor{90 + 40 * (yy - y) / h, 96 + 40 * (yy - y) / h, 200.f, 1.f}; });
            g_layer.text(vals[f], true, 7.4f, cx[f] + 36.f, y + 28.f, sel ? LayerColor{255, 255, 255, 1.f} : (on ? LayerColor{40, 42, 65, 1.f} : LayerColor{130, 130, 150, 1.f}));
        }
    }
    gs_row(150.f, 330.f, 220.f, 24.f, g_ffa_slot == 4, "DONE", "", false, 8.6f);
    gs_footer(g_ffa_flow ? "Up/Down: player   Left/Right: field   Cross: pick in the game's character select   L: Off / None   Circle: back" : "Up/Down: player   Left/Right: field   Cross: change   Done: Cross / Circle");
}

void card_head(float cx0, float cx1) {   // the pale pill behind a card's heading (retail's "Party A" plate)
    if (style_old()) return;
    gs_bar(cx0 + 5.f, cx1 - 5.f, 51.f, 15.f, [&](float, float v, float e) { return e > 0.88f ? LayerColor{150, 154, 205, 1.f} : LayerColor{226 - 24 * v, 228 - 24 * v, 250 - 14 * v, 1.f}; });
}

void lobby_dialog(uint8_t* rdram, recomp_context* ctx, uint32_t pressed, float a) {
    (void)a;
    uint8_t L[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1};
    const bool have = dissidia_tagteam_lobby_get(L);
    const bool host = dissidia_tagteam_lobby_editable(), sp = dissidia_tagteam_lobby_sp();
    int vis[kLobbyRows]; int rows = lobby_visible(L, vis);
    if (g_lob_row >= rows) g_lob_row = rows - 1;
    if ((pressed & BTN_CIRCLE) && g_ffa_flow && sp && L[0] == 3 && !std::getenv("PSPRECOMP_TAGTEAM_FFA_OLDBACK")) {   // the Free-for-all flow: Circle goes back to the players screen, only Cross starts
        g_dlg_mode = 3; g_ffa_then_lobby = true; play_se(rdram, ctx, 1);
        std::fprintf(stderr, "[FFA] lobby: Circle, back to the players screen\n");
        return;
    }
    if ((pressed & BTN_CIRCLE) && g_lob_flow && sp && L[0] != 3 && !menuback_old()) {   // a step of the chain: back to Party Select (retail's setup screen gets one Circle once the hold is over)
        std::fprintf(stderr, "[TTLOBBY] lobby: Circle, back to Party Select\n");
        g_dlg_open = false; g_dlg_mode = 0; g_lob_flow = false; g_rearm = true; play_se(rdram, ctx, 1); if (dissidia_tagteam_inject_old()) g_inject_circle.store(6, std::memory_order_relaxed); else sp_back_direct(rdram, ctx);
        return;
    }
    if (pressed & (BTN_CIRCLE | BTN_CROSS | BTN_OPEN)) {
        if ((pressed & BTN_CROSS) && vis[g_lob_row] == 8) { g_dlg_mode = 3; g_ffa_then_lobby = true; g_ffa_slot = 1; g_ffa_field = 1; play_se(rdram, ctx, 1); return; }   // PLAYERS: the Free-for-all setup
        std::fprintf(stderr, "[TTLOBBY] lobby closed by a press %04X (sp %d)\n", pressed, (int)sp);
        g_dlg_open = false; g_dlg_mode = 0; g_lob_flow = false; play_se(rdram, ctx, 1);
        if (g_ffa_flow && sp) { g_ffa_flow = false; g_ffa_autostart = true; g_cover_until = std::chrono::steady_clock::now() + std::chrono::seconds(8); }   // Free-for-all: the battle starts from here
        return;
    }
    if (pressed & BTN_UP) { g_lob_row = (g_lob_row + rows - 1) % rows; play_se(rdram, ctx, 1); }
    if (pressed & BTN_DOWN) { g_lob_row = (g_lob_row + 1) % rows; play_se(rdram, ctx, 1); }
    if (host && (pressed & (BTN_RIGHT | BTN_LEFTK))) { lobby_change(vis[g_lob_row], (pressed & BTN_RIGHT) ? 1 : -1, L); dissidia_tagteam_lobby_set(L); play_se(rdram, ctx, 1); rows = lobby_visible(L, vis); if (g_lob_row >= rows) g_lob_row = rows - 1; }
    gs_background();
    gs_title("BATTLE LOBBY", std::string(kLobbyModes[lobby_mode_index(L)].label));
    const float x0 = style_old() ? 16.f : 38.f, x1 = style_old() ? 262.f : 268.f, y0 = style_old() ? 46.f : 42.f, h = style_old() ? (rows > 8 ? 19.f : 22.f) : (rows > 8 ? 18.f : 20.f);
    for (int i = 0; i < rows; i++) {
        const int r = vis[i];
        gs_row(x0, x1, y0 + h * float(i), h - 1.f, i == g_lob_row, kLobbyRowName[r], lobby_value(r, L), host && r != 8, 7.2f);
    }
    const int cur = vis[g_lob_row];
    const std::string help = [&]() -> std::string {
        static const char* const kRowHelp[] = {"", "The stage of the battle. Random picks one when it starts.", "The rules of the match (None, Official, or a centred rule set).", "The judgment setting of the match (None, Cosmos, Chaos, Double, Random).", "How the EX Burst behaves in this battle.", "How the Assist gauge behaves in this battle.", "On: empty places are filled with CPU fighters.", "Strength of the CPU fill fighters.", "", "", ""};
        std::string h = cur == 0 ? kLobbyModes[lobby_mode_index(L)].help : cur == 8 ? "Cross: choose the players of the free-for-all" : cur == 9 ? "Left/Right: every CPU (fill, partners, enemies, free-for-all) gets this strength" : cur == 10 ? "Chasers only: the slow motion of a Chase affects only the two fighters in it. Everyone: retail, the whole battle slows (the host decides online)" : (cur >= 6 ? "Empty places are filled with CPU fighters of this strength" : "");
        if (h.empty() && cur >= 1 && cur <= 7) h = kRowHelp[cur];
        return h;
    }();
    if (style_old() && !help.empty()) g_layer.text(help, true, 5.6f, x0 + 6.f, y0 + h * float(rows) + 12.f, LayerColor{60, 62, 120, 1.f});
    // the right card: your party (single player) / the room's players
    const float cx0 = 280.f, cx1 = 466.f;
    gs_card(cx0, style_old() ? 46.f : 42.f, cx1, style_old() ? 238.f : 216.f);
    if (sp) {
        const int per = L[0] == 1 ? L[1] : 1;
        card_head(cx0, cx1);
        g_layer.text(L[0] == 3 ? "FREE-FOR-ALL" : "YOUR PARTY", true, 7.4f, cx0 + 10.f, 62.f, LayerColor{40, 50, 140, 1.f});
        g_layer.text(std::string("Fighters on stage  ") + (L[0] == 3 ? "up to 4" : std::to_string(2 * per)), true, 6.2f, cx0 + 10.f, 74.f, LayerColor{40, 120, 60, 1.f});
        if (L[0] == 3) {   // the free-for-all's players
            uint8_t p1[2]; dissidia_tagteam_ffa_p1_get(p1);
            for (int p = 0; p < 4; p++) {
                uint8_t sl[4] = {1, 0, 0, 5}; if (p) dissidia_tagteam_ffa_get(p - 1, sl);
                const float ry = 92.f + 18.f * float(p);
                g_layer.text(kSlotName[p], true, 7.f, cx0 + 10.f, ry, kSlotCol[p]);
                const std::string c = p == 0 ? slot_char_text(p1[0], false) + (p1[1] == 255 ? "" : " + " + slot_char_text(p1[1], true)) : sl[0] ? slot_char_text(sl[1], false) + " (" + kStrName[std::clamp<int>(sl[3], 1, 9) - 1] + ")" : std::string("Off");
                const float cw = g_layer.text(c, true, 5.6f, 0, 0, LayerColor{0, 0, 0, 0}, true);
                g_layer.text(c, true, 5.6f, cx1 - 10.f - std::min(cw, 150.f), ry, LayerColor{40, 42, 65, 1.f});
            }
        } else
        {
        const uint8_t cm = dissidia_tagteam_sp_cpu_mask();
        for (int i = 0; i < g_sp_rows_n && i < 5; i++) {
            const float ry = 92.f + 16.f * float(i);
            g_layer.text(((cm >> i) & 1) ? "CPU" : "You", true, 6.4f, cx0 + 10.f, ry, ((cm >> i) & 1) ? LayerColor{200, 60, 60, 1.f} : LayerColor{40, 90, 220, 1.f});
            const std::string c = dissidia_tagteam_char_name(g_sp_rows_c[i]);
            const float cw = g_layer.text(c, true, 6.4f, 0, 0, LayerColor{0, 0, 0, 0}, true);
            g_layer.text(c, true, 6.4f, cx1 - 10.f - cw, ry, LayerColor{40, 42, 65, 1.f});
        }
        }
        const std::string note = L[0] == 3 ? "You + the CPU players of the player setup" : L[0] == 1 ? "CPU rows (L on the setup screen) are your partners" : "One fighter each; party members tag in on a KO";
        g_layer.text(note, true, 5.2f, cx0 + 10.f, style_old() ? 214.f : 202.f, LayerColor{70, 72, 130, 1.f});
        if (L[2]) g_layer.text("CPU fill on: empty places get a CPU fighter", true, 5.2f, cx0 + 10.f, style_old() ? 226.f : 211.f, LayerColor{170, 100, 20, 1.f});
    } else {
        char names[16][32] = {}; uint8_t chars[16] = {}; int8_t parties[16] = {}; bool hosts[16] = {};
        const int np = dissidia_tagteam_lobby_players(names, chars, parties, hosts);
        const int need = lobby_needed(L);
        card_head(cx0, cx1);
        g_layer.text("PLAYERS  " + std::to_string(np), true, 7.4f, cx0 + 10.f, 62.f, LayerColor{40, 50, 140, 1.f});
        g_layer.text("Fighters on stage  " + std::to_string(lobby_fighters(L, np)), true, 6.2f, cx0 + 10.f, 74.f, np >= need ? LayerColor{40, 120, 60, 1.f} : LayerColor{190, 100, 20, 1.f});
        for (int i = 0; i < np && i < 9; i++) {
            const float ry = 92.f + 14.f * float(i);
            g_layer.text(std::string(names[i]) + (hosts[i] ? " (host)" : ""), true, 6.2f, cx0 + 10.f, ry, kSlotCol[i & 3]);
            const std::string c = chars[i] ? dissidia_tagteam_char_name(chars[i]) : std::string("choosing...");
            const float cw = g_layer.text(c, true, 6.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
            g_layer.text(c, true, 6.2f, cx1 - 10.f - cw, ry, LayerColor{40, 42, 65, 1.f});
        }
        if (np < need && L[0] != 3) g_layer.text("Waiting for " + std::to_string(need - np) + " more player(s)", true, 5.6f, cx0 + 10.f, style_old() ? 226.f : 211.f, LayerColor{190, 100, 20, 1.f});
    }
    gs_footer(g_ffa_flow && sp && L[0] == 3 ? "Up/Down: row   Left/Right: change   Cross: start the battle   Circle: back" : host ? (g_lob_flow && sp && L[0] != 3 && !menuback_old() ? "Up/Down: row   Left/Right: change   Cross: done   Circle: back" : "Up/Down: row   Left/Right: change   Cross / Circle: done") : "Set by the host   Circle: close", style_old() ? std::string() : (help.empty() ? std::string("Left/Right: change the value of the selected row") : help));
    (void)have;
}

// The Tournament column's place on the setup screen: the battle lobby's settings and the players (read-only; Select opens the lobby).
void lobby_panel(const uint8_t* L, float a) {
    const float x0 = 172.f, x1 = 478.f, y0 = 30.f, y1 = 220.f, xm = 330.f;
    // the retail name bars, character banner, "?" box and Party A / B labels: covered in the screen's own pale blue
    g_layer.band(0.f, 26.f, 480.f, 112.f, [&](float, float y) { const float t = (y - 26.f) / 86.f; return LayerColor{226 - 10 * t, 232 - 10 * t, 253 - 5 * t, a}; });
    g_layer.text("YOUR PARTY", true, 6.6f, 12.f, 108.f, LayerColor{40, 46, 100, a});
    g_layer.band(x0, y0, x1, y1, [&](float x, float y) {
        const float edge = std::min({x - x0, x1 - x, y - y0, y1 - y, 2.f}) / 2.f;
        const float t = (y - y0) / (y1 - y0);
        return LayerColor{40 + 16 * (1 - t), 44 + 16 * (1 - t), 96 + 36 * (1 - t), 1.0f * a * std::max(0.f, edge)};
    });
    const char* title = "BATTLE LOBBY";
    const float tw = g_layer.text(title, true, 8.4f, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(title, true, 8.4f, (x0 + x1) / 2.f - tw / 2.f, y0 + 16.f, LayerColor{255, 255, 255, a});
    for (int i = 0, k = 0; i < 6; i++) {
        if (i == 4) continue;   // (EX BURST row: not offered)
        const float ry = y0 + 40.f + 22.f * float(k++);
        g_layer.text(kLobbyRowName[i], true, 6.6f, x0 + 8.f, ry, LayerColor{200, 210, 250, a});
        const std::string v = lobby_value(i, L);
        const float vw = g_layer.text(v, true, 6.8f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(v, true, 6.8f, xm - 6.f - vw, ry, LayerColor{255, 255, 255, a});
    }
    g_layer.band(xm, y0 + 26.f, xm + 0.8f, y1 - 10.f, [&](float, float) { return LayerColor{150, 160, 230, 0.6f * a}; });
    char names[16][32] = {}; uint8_t chars[16] = {}; int8_t parties[16] = {}; bool hosts[16] = {};
    const int np = dissidia_tagteam_lobby_players(names, chars, parties, hosts);
    g_layer.text("PLAYERS  " + std::to_string(np), true, 6.6f, xm + 10.f, y0 + 40.f, LayerColor{170, 240, 170, a});
    for (int i = 0; i < np && i < 6; i++) {
        const float ry = y0 + 62.f + 20.f * float(i);
        g_layer.text(std::string(names[i]) + (hosts[i] ? " *" : ""), true, 6.6f, xm + 8.f, ry, LayerColor{235, 238, 255, a});
        const std::string c = chars[i] ? dissidia_tagteam_char_name(chars[i]) : std::string("...");
        const float cw = g_layer.text(c, true, 6.2f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(c, true, 6.2f, x1 - 8.f - cw, ry, LayerColor{255, 236, 190, a});
    }
}

void draw(uint8_t* rdram, recomp_context* ctx, uint32_t screen) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    g_appear = dt >= 0.1f ? 0.f : std::min(1.f, g_appear + dt / 0.25f);
    const float a = g_appear;
    uint8_t regs[5] = {};
    const int n = dissidia_tagteam_local_regs(regs);
    uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    const uint32_t pressed = dir_repeat(b, b & ~g_prev_buttons, g_dlg_open && g_dlg_mode >= 1 && g_dlg_mode <= 4);
    g_prev_buttons = b;
    // Square on a registered row from the second on: CPU <-> the player's own.
    const bool left = r32(screen + SCR_COLUMN) == 0u;
    const int row = static_cast<int>(r32(screen + SCR_ROW));
    const uint8_t own = dissidia_tagteam_local_own_mask();
    int first = -1; for (int i = 0; i < 5 && first < 0; i++) if ((own >> i) & 1) first = i;
    const bool switchable = left && row >= 0 && row < n && row < 5 && ((own >> row) & 1) && (row != first || dissidia_tagteam_cpu_party_on());   // the player's own rows after his first (the first too: a CPU first row makes him the keeper of a CPU party)
    const bool cpu_row = switchable && ((dissidia_tagteam_local_cpu_mask() >> row) & 1) && row != first;   // (the settings of a keeper's first row are not in the beacon bytes: default strength 5 / Auto / Random)
    if (g_dlg_open && !g_dlg_bp && g_dlg_mode == 0 && !cpu_row) g_dlg_open = false;   // the row went away / is a player again
    const bool free_row = left && n >= 1 && n < 5 && row == n && !g_dlg_open;   // the cursor is on the next free row: L = Add CPU
    if (free_row && (pressed & BTN_L)) { dissidia_tagteam_add_cpu_request(true); if (dissidia_tagteam_inject_old()) { g_inj_why = "Add CPU: Cross for the setup screen (INJECT_OLD)"; g_inject_cross.store(3, std::memory_order_relaxed); play_se(rdram, ctx, 1); } else g_add_cpu_direct.store(true, std::memory_order_relaxed); }   // 26 Nov: the screen's own Cross step is called (add_cpu_direct), no press
    bool opened_now = false;
    // The host's CPU party (docs "Proper PvE"): when every human chose one party the other is a CPU party. Its rows are the order column's (1st..5th): L adds one (picker),
    // Select opens a row's STRENGTH / LEVEL / TYPE, Square takes it out. Everyone sees the rows (plates at the right edge); only the host edits.
    const int bpp = dissidia_tagteam_bp_party();
    const bool bp_host = bpp >= 0 && dissidia_tagteam_is_host();
    const int bpn = bpp >= 0 ? dissidia_tagteam_bp_count() : 0;
    const bool ord = bpp >= 0 && r32(screen + SCR_COLUMN) == 1u && row >= 0 && row < 5;
    if (g_dlg_open && (g_dlg_bp || g_dlg_mode == 1) && !(bp_host && ord)) { g_dlg_open = false; g_dlg_bp = false; g_dlg_mode = 0; }
    if (bp_host && ord && !g_dlg_open) {
        if ((pressed & BTN_L) && bpn < 5) { g_pick_n = dissidia_tagteam_bp_pick_list(g_pick_list); g_pick_sel = 0; g_dlg_mode = 1; g_dlg_bp = false; g_dlg_open = true; opened_now = true; play_se(rdram, ctx, 1); }
        else if ((pressed & BTN_SQUARE) && row < bpn) { dissidia_tagteam_bp_remove(row); play_se(rdram, ctx, 1); }
        else if ((pressed & BTN_OPEN) && row < bpn) { opened_now = true; g_dlg_mode = 0; g_dlg_bp = true; g_dlg_sp = false; g_dlg_row = row; g_dlg_field = 0; g_dlg_hold = 0.f; uint8_t cf[3] = {5, 0, 0}; dissidia_tagteam_bp_row(row, cf); std::memcpy(g_dlg_cfg, cf, 3); g_dlg_open = true; play_se(rdram, ctx, 1); }
    }
    // The battle lobby's mode needs this many players: Enter Battle waits until they are all in the room.
    int lob_short = 0;
    if (bpp < 0) {
        uint8_t L[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1};
        if (dissidia_tagteam_lobby_get(L) && L[0] != 3) {   // free-for-all starts with whoever is in the room
            char nm[16][32] = {}; uint8_t ch[16] = {}; int8_t pa[16] = {}; bool ho[16] = {};
            lob_short = std::max(0, lobby_needed(L) - dissidia_tagteam_lobby_players(nm, ch, pa, ho));
        }
    }
    const bool enter_blocked = (bpp >= 0 && dissidia_tagteam_bp_enter_blocked()) || lob_short > 0;
    {
        uint8_t L[10];
        const bool lob = bpp < 0 && dissidia_tagteam_lobby_get(L) && !std::getenv("PSPRECOMP_TAGTEAM_TOURNAMENT");
        const uint32_t col = r32(screen + SCR_COLUMN);
        g_col_fix.store(!lob ? 0 : col == 0u ? 1 : (col == 1u && row >= 0 && row < 5) ? 2 : 0, std::memory_order_relaxed);
    }
    g_enter_block.store(enter_blocked ? (r32(screen + SCR_COLUMN) == 1u && row == 5 ? 2 : 1) : 0, std::memory_order_relaxed); g_enter_stamp.store(g_pad_polls.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (!g_dlg_open && left && !cpu_row && (pressed & BTN_OPEN)) { opened_now = true; g_lob_sp = false; g_dlg_mode = 2; g_dlg_bp = false; g_dlg_sp = false; g_dlg_open = true; play_se(rdram, ctx, 1); }   // the battle lobby
    if (!g_dlg_open && cpu_row && (pressed & BTN_OPEN)) { opened_now = true; g_dlg_sp = false; g_dlg_bp = false; g_dlg_mode = 0; g_dlg_row = row; g_dlg_field = 0; g_dlg_hold = 0.f; dissidia_tagteam_get_cpu_cfg(row, g_dlg_cfg); g_dlg_open = true; play_se(rdram, ctx, 1); }
    if (!g_dlg_open && switchable && (pressed & BTN_SQUARE)) { dissidia_tagteam_toggle_cpu_row(row); play_se(rdram, ctx, 1); }
    const uint8_t mask = dissidia_tagteam_local_cpu_mask();
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    for (int i = 0; i < n && i < 5; i++) if ((mask >> i) & 1) {   // the CPU rows (the first too, a CPU party's keeper)
        const float y = kRowY0 + kRowPitch * float(i);
        tag(139.f, y + 5.f, 160.f, y + 14.f, "CPU", a);   // after the job name (the left column's hand covers x 45..80 of the row)
    }
    const int dside = dissidia_tagteam_defect_side();   // this player keeps a CPU party: his player rows fight for the other party
    if (dside >= 0) {
        for (int i = 0; i < n && i < 5; i++) if (((own >> i) & 1) && !((mask >> i) & 1)) { const float y = kRowY0 + kRowPitch * float(i); tag(121.f, y + 5.f, 160.f, y + 14.f, dside == 0 ? "-> Party A" : "-> Party B", a); }
        const std::string note = std::string("You keep the CPU party: CPU rows fight for Party ") + (dside == 0 ? "B" : "A") + ", Player rows for Party " + (dside == 0 ? "A" : "B");
        const float em = 5.6f, w = g_layer.text(note, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(note, true, em, 240.f - w / 2.f, 266.f, LayerColor{60, 60, 110, a});
    }
    // CPU rows' settings, read-only: this player's party on the left column (the player's own rows and the party mates' rows), the other party's on the right edge
    const int me_party = dissidia_tagteam_local_party();
    if (me_party == 0 || me_party == 1) for (int col = 0; col < 2; col++) for (int i = 0; i < 5; i++) {
        uint8_t c[3]; bool cpu = false;
        if (col == 0 && i < n && ((mask >> i) & 1)) { dissidia_tagteam_get_cpu_cfg(i, c); cpu = true; }   // (own rows: live, before the beacon round trip)
        else cpu = dissidia_tagteam_menu_cpu_row(col == 0 ? me_party : 1 - me_party, i, c) != 0;
        if (!cpu) continue;
        const float y = kRowY0 + kRowPitch * float(i);
        if (col == 0 && !((i < n) && ((mask >> i) & 1))) tag(139.f, y + 5.f, 160.f, y + 14.f, "CPU", a);   // a party mate's CPU row also gets the plate
        const std::string t = col == 0 ? cfg_text(c) : "CPU " + cfg_text(c);
        const float em = 4.6f, w = g_layer.text(t, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        const float x0 = col == 0 ? kColX0 + 1.f : 478.f - w - 5.f;
        tag(x0, y + 9.f, x0 + w + 5.f, y + 16.f, "", a);
        g_layer.text(t, true, em, x0 + 2.5f, y + 14.4f, LayerColor{255, 255, 255, a});
    }
    if (n >= 1 && n < 5) {   // the next free row: it adds a character (yours; Square makes it a CPU)
        const float y = kRowY0 + kRowPitch * float(n);
        const char* hint = "Add a character   (L: Add CPU)";
        const float w = g_layer.text(hint, true, 7.0f, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(hint, true, 7.0f, (kColX0 + kColX1) / 2.f + 12.f - w / 2.f, y + 12.6f, LayerColor{92, 88, 128, 0.85f * a});
    }
    // The button bar, right to left from the screen's own R hint: Square (on a switchable row), then the host's choice.
    float right = 318.f;
    if (bpp >= 0) {   // the CPU party's hints
        const std::string pname = std::string("Party ") + (bpp == 0 ? "A" : "B");
        if (bp_host && ord && !g_dlg_open) {
            if (row < bpn) { right = bar_text(right, "Select: CPU Settings", a) - 8.f; right = bar_text(right, "Remove", a); right = key(right - 3.f, "", true, a) - 8.f; }
            else if (bpn < 5) { right = bar_text(right, "Add CPU opponent", a); right = key(right - 3.f, "L", false, a) - 8.f; }
        }
        std::string note = pname + " is a CPU party (" + std::to_string(bpn) + (bpn == 1 ? " opponent" : " opponents") + ")";
        if (bp_host) note += bpn < 5 ? ":  move to the 1st-5th rows, L adds an opponent" : "";
        else note += ": the host sets it up";
        if (enter_blocked) note = bp_host ? "Add at least one CPU opponent (cursor on a 1st-5th row, L) before Enter Battle" : "Waiting for the host to add the CPU opponents";
        const float em = 5.6f, w = g_layer.text(note, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
        g_layer.text(note, true, em, 240.f - w / 2.f, 266.f, enter_blocked ? LayerColor{150, 40, 40, a} : LayerColor{60, 60, 110, a});
    }
    if (cpu_row && !g_dlg_open) right = bar_text(right, "Select: CPU Settings", a) - 8.f;
    if (switchable) {
        right = bar_text(right, (mask >> row) & 1 ? "Player" : "CPU", a);
        right = key(right - 3.f, "", true, a) - 8.f;
    }
    // The battle lobby (Select on one of the player's own rows): its hint in the button bar, the host's settings on one line at the bottom.
    {
        uint8_t L[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1};
        const bool have = dissidia_tagteam_lobby_get(L);
        if (left && !cpu_row && !g_dlg_open) right = bar_text(right, "Select: Battle Lobby", a) - 8.f;
        if (have && bpp < 0 && !std::getenv("PSPRECOMP_TAGTEAM_TOURNAMENT")) lobby_panel(L, a);
        if (have && bpp < 0 && !g_dlg_open && lob_short > 0) {
            const std::string s = lob_short > 0 ? "Waiting for " + std::to_string(lob_short) + " more player(s) for " + kLobbyModes[lobby_mode_index(L)].label + "  (Select: Battle Lobby to change the mode)" : lobby_summary(L);
            const float em = 5.6f, w = g_layer.text(s, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
            g_layer.text(s, true, em, 240.f - w / 2.f, 266.f, lob_short > 0 ? LayerColor{150, 40, 40, a} : LayerColor{60, 60, 110, a});
        }
    }
    { static bool po = false; static int pm = -1; const bool o2 = g_dlg_open.load(); if (o2 != po || (o2 && g_dlg_mode != pm)) { po = o2; pm = g_dlg_mode;
        std::fprintf(stderr, "[TTLOBBY] setup screen dialog %s (mode %d, column %u row %d, pressed %04X)\n", o2 ? "open" : "closed", g_dlg_mode, r32(screen + SCR_COLUMN), row, pressed); } }
    if (g_dlg_open) { if (g_dlg_mode == 1) picker(rdram, ctx, opened_now ? 0u : pressed, a); else if (g_dlg_mode == 2) lobby_dialog(rdram, ctx, opened_now ? 0u : pressed, a); else dialog(rdram, ctx, opened_now ? 0u : pressed, dt, a); }
    g_layer.tag(g_dlg_open ? dlg_tag() : "partysel"); g_layer.publish();
}


// Party Select's test FUN_08A7046C(screen): 1 when every member chose a party and both party lists (screen + 280 + 12 p, count at +10) are not empty. In a group whose humans all chose
// one party the other is the host's CPU party: it may be empty (the test is run again with a stand-in entry in that list, then the list is put back).
void ready_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(READY_TEST)) f(rdram, ctx); return; }
    if (g_script_t.load(std::memory_order_relaxed) == 0) g_ps_frames.fetch_add(1, std::memory_order_relaxed);
    const uint32_t base = static_cast<uint32_t>(ctx->r[4]);
    g_orig_ready(rdram, ctx);
    if (ctx->r[2] != 0 || !dissidia_tagteam_group_on()) return;
    uint16_t c0, c1; std::memcpy(&c0, g_ram + ((base + 290u) & 0x07FFFFFFu), 2); std::memcpy(&c1, g_ram + ((base + 302u) & 0x07FFFFFFu), 2);
    if ((c0 == 0) == (c1 == 0) || std::max<int>(c0, c1) < 2) return;   // the humans (two or more) all chose one party: the other is the CPU party
    const int cp = c0 == 0 ? 0 : 1;
    const uint32_t list = base + 280u + 12u * static_cast<uint32_t>(cp);
    uint16_t cnt, id0; std::memcpy(&cnt, g_ram + ((list + 10u) & 0x07FFFFFFu), 2); std::memcpy(&id0, g_ram + (list & 0x07FFFFFFu), 2);
    const uint16_t one = 1, stand = 0x7FFE;
    std::memcpy(g_ram + ((list + 10u) & 0x07FFFFFFu), &one, 2); std::memcpy(g_ram + (list & 0x07FFFFFFu), &stand, 2);
    ctx->r[4] = base; g_orig_ready(rdram, ctx);
    std::memcpy(g_ram + ((list + 10u) & 0x07FFFFFFu), &cnt, 2); std::memcpy(g_ram + (list & 0x07FFFFFFu), &id0, 2);
    static bool logged = false; if (ctx->r[2] != 0 && !logged) { logged = true; std::fprintf(stderr, "[TTBP] Party Select: the CPU party (Party %c) may be empty, the match can start\n", cp ? 'B' : 'A'); }
}
void enter_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(ENTER_STEP)) f(rdram, ctx); return; }
    if (dissidia_tagteam_group_on()) dissidia_tagteam_bp_finalize(rdram, ctx);
    g_orig_enter(rdram, ctx);
}

void setup_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SETUP_TICK)) f(rdram, ctx); return; }
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    // 26 Nov (user: no injected presses): the lobby panel covers the Tournament column; when the cursor lands on one of its round rows (Up from Enter Battle) it is
    // put on the same row of the party column before the screen's update (the old way pressed Right for the screen). PSPRECOMP_TAGTEAM_INJECT_OLD=1: old.
    if (!dissidia_tagteam_inject_old() && dissidia_tagteam_group_on() && !std::getenv("PSPRECOMP_TAGTEAM_TOURNAMENT") && r32(screen + SCR_COLUMN) == 1u && r32(screen + SCR_ROW) < 5u) {
        uint8_t L[10]; if (dissidia_tagteam_bp_party() < 0 && dissidia_tagteam_lobby_get(L)) { w32(screen + SCR_COLUMN, 0u); std::fprintf(stderr, "[TTLOBBY] cursor on a covered round row: party column, same row (no press)\n"); }
    }
    if (g_add_cpu_direct.exchange(false, std::memory_order_relaxed)) add_cpu_direct(rdram, ctx, screen);
    g_orig_tick(rdram, ctx);
    if (std::getenv("PSPRECOMP_TTCPU_CURDUMP")) {   // debug: small words of the screen object that changed this frame
        static std::vector<uint32_t> prev; static uint32_t prev_scr = 0;
        std::vector<uint32_t> now(0x8000 / 4); std::memcpy(now.data(), rdram + (screen & 0x07FFFFFFu), 0x8000);
        if (prev_scr == screen && prev.size() == now.size()) {
            std::string out; char b[48];
            for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i] && now[i] < 16 && prev[i] < 16) { std::snprintf(b, sizeof b, " +%zX:%u>%u", i * 4, prev[i], now[i]); out += b; }
            if (!out.empty()) std::fprintf(stderr, "[TTCPU] screen %08X:%s\n", screen, out.c_str());
        }
        prev = now; prev_scr = screen;
    }
    if (!dissidia_tagteam_group_on()) return;
    dissidia_tagteam_gb_setup_frame(r32(screen + SCR_COLUMN) == 0u ? static_cast<int>(r32(screen + SCR_ROW)) : -1);
    { static uint32_t pc = ~0u, pr = ~0u, ps = ~0u; const uint32_t col = r32(screen + SCR_COLUMN), rw = r32(screen + SCR_ROW), M = r32(screen + SCR_M), st = (M >= 0x08800000u && M < 0x0A000000u) ? r32(M + 4u) : 0xFFFFFFFFu;
      if (col != pc || rw != pr || st != ps) { pc = col; pr = rw; ps = st; std::fprintf(stderr, "[TTGB] setup screen %08X: column %u row %u state %u\n", screen, col, rw, st); } }
    if (dissidia_tagteam_add_cpu_pending()) {   // "Add CPU": Player Select was opened for the new row; if it closes without a row (Circle) nothing stays pending
        static bool seen = false; static int calm = 0;
        const uint32_t M = r32(screen + SCR_M), st = (M >= 0x08800000u && M < 0x0A000000u) ? r32(M + 4u) : 7u;
        if (st != 7u) { seen = true; calm = 0; } else if (seen && ++calm > 20) { dissidia_tagteam_add_cpu_request(false); seen = false; calm = 0; std::fprintf(stderr, "[TTCPU] Add CPU: Player Select was closed without a character\n"); }
    }
    draw(rdram, ctx, screen);
}


// ---- single player tag mode: the round dialog of matchups 2.. ----
// The tag match is ONE continuous battle: only the first matchup's STAGE / RULES / JUDGEMENT apply. In tag mode the round dialog of the 2nd round on keeps its STRENGTH / LEVEL /
// TYPE rows (they apply per enemy member) and the others are covered with the panel's own colour; the pad filter keeps the cursor off them (Up on STRENGTH and Down on TYPE are
// swallowed, a dialog that opens on STAGE gets one Down). (Writing the cursor row itself desyncs the dialog's highlight bar.)
bool g_sp_dlg_ran = false; int g_sp_hide_round = -1; std::atomic<int> g_sp_row{-1};   // g_sp_row: the dialog cursor row while the rows are hidden, else -1 (read by the pad filter)
int g_sp_row_set_round = -1;   // the round whose dialog hides STAGE (set after its first update; the next update starts it on STRENGTH)
bool sp_hidden_row(int r) { return r == 0 || r == 4 || r == 5; }
void sp_dlg_hook(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    // 26 Nov (user: no cursor is ever moved by a pressed button): the round dialog opens on STAGE, a row the tag mode hides; it used to get one injected Down. Now its
    // cursor helper (M + 0x5CC, index + 4) starts on STRENGTH and the dialog's own row-changed step FUN_08AAD670(screen) runs before its first update (no Down,
    // nothing moves on screen). PSPRECOMP_TAGTEAM_INJECT_OLD=1: the injected Down.
    if (!dissidia_tagteam_inject_old() && dissidia_tagteam_sp_tag_on() && g_sp_row_set_round >= 0) {
        const uint32_t M0 = r32(screen + SCR_M);
        if (okp(M0) && r32(M0 + 1488u) == 0u && static_cast<int>(r32(M0 + 1444u)) == g_sp_row_set_round) {
            w32(M0 + 1488u, 1u);
            if (FuncPtr f = ovl_menu_lookup(0x08AAD670u)) { const recomp_context save = *ctx; ctx->r[4] = static_cast<int32_t>(screen); f(rdram, ctx); *ctx = save; }
            std::fprintf(stderr, "[TTSP] round dialog %d: opens on STRENGTH (hidden STAGE row skipped, no press)\n", g_sp_row_set_round + 1);
        }
    }
    g_orig_sp_dlg(rdram, ctx);
    if (!dissidia_tagteam_sp_tag_on()) return;
    const uint32_t M = r32(screen + SCR_M); if (M < 0x08800000u || M >= 0x0A000000u) return;
    const uint32_t round = r32(M + 1444u); const int row = static_cast<int>(r32(M + 1488u));
    // 23 Nov (user, low priority list: "remove the options the Battle Lobby now covers (stage, rules, judgment)"): round 1 too, unless the lobby is off
    // (PSPRECOMP_TAGTEAM_SPLOBBY_OFF) or PSPRECOMP_TAGTEAM_R1ROWS=1 (old: round 1 kept them).
    static const bool r1rows = std::getenv("PSPRECOMP_TAGTEAM_R1ROWS") || std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF");
    g_sp_dlg_ran = true; g_sp_hide_round = round >= (r1rows ? 1u : 0u) && round < 5u ? static_cast<int>(round) : -1;
    g_sp_row.store(g_sp_hide_round >= 0 ? row : -1, std::memory_order_relaxed);
    g_sp_row_set_round = g_sp_hide_round;
}
// ---- Single player: CPU rows on the Tournament screen (docs/tag-team.md "Single player: CPU rows") ----
// Square on one of the player's matchup rows from the 2nd on makes his member of that row a CPU partner and back; Select on a CPU row opens the CPU
// settings dialog (the same one as Communications). Drawn on the game layer: the "CPU" plate after the member's job name, the bar's hints.
bool g_sp_layer_up = false;
void sp_dump(uint32_t screen) {   // debug (PSPRECOMP_TTSP_CURDUMP=1): small words of the screen object and of M that changed this frame
    static std::vector<uint32_t> prev, prevm; static uint32_t prev_scr = 0;
    const uint32_t M = r32(screen + SCR_M);
    std::vector<uint32_t> now(0x8000 / 4), nowm(0x800 / 4, 0); std::memcpy(now.data(), g_ram + (screen & 0x07FFFFFFu), 0x8000);
    if (M >= 0x08800000u && M < 0x0A000000u) std::memcpy(nowm.data(), g_ram + (M & 0x07FFFFFFu), 0x800);
    if (prev_scr == screen && prev.size() == now.size()) {
        std::string out; char b[48];
        for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i] && now[i] < 16 && prev[i] < 16) { std::snprintf(b, sizeof b, " +%zX:%u>%u", i * 4, prev[i], now[i]); out += b; }
        for (size_t i = 0; i < nowm.size(); i++) if (nowm[i] != prevm[i] && nowm[i] < 16 && prevm[i] < 16) { std::snprintf(b, sizeof b, " M+%zu:%u>%u", i * 4, prevm[i], nowm[i]); out += b; }
        if (std::getenv("PSPRECOMP_TTSP_CURDUMP")[0] == '2') { int cnt = 0; std::string all; for (size_t i = 0; i < now.size(); i++) if (now[i] != prev[i]) { cnt++; std::snprintf(b, sizeof b, " +%zX:%X>%X", i * 4, prev[i], now[i]); all += b; } if (cnt && cnt <= 10) std::fprintf(stderr, "[TTSP] ALL%s\n", all.c_str()); }
        if (!out.empty()) std::fprintf(stderr, "[TTSP] screen %08X M %08X:%s\n", screen, M, out.c_str());
    }
    prev = now; prevm = nowm; prev_scr = screen;
}
void sp_scrdump(uint32_t screen) {   // debug (PSPRECOMP_TTSP_SCRDUMP=<file>): the setup screen object (0x8000 bytes) once, 120 ticks in
    static int tick = 0; if (++tick != 120) return;
    if (FILE* f = std::fopen(std::getenv("PSPRECOMP_TTSP_SCRDUMP"), "wb")) { std::fwrite(g_ram + (screen & 0x07FFFFFFu), 1, 0x8000, f); std::fclose(f); }
}
void sp_partyscan(uint32_t screen) {   // debug (PSPRECOMP_TTSP_PARTYSCAN="c0,c1,c4"): where are the player's party members (first, second, fifth) while the screen runs
    static bool done = false; if (done) return; done = true;
    int c0 = 0, c1 = 0, c4 = 0; if (std::sscanf(std::getenv("PSPRECOMP_TTSP_PARTYSCAN"), "%d,%d,%d", &c0, &c1, &c4) != 3) return;
    for (uint32_t a = 0x08800000u; a < 0x09FF0000u; a++) {
        if (g_ram[a & 0x07FFFFFFu] != c0) continue;
        for (uint32_t st : {1u, 2u, 4u, 8u, 12u, 16u, 20u, 24u, 32u, 40u, 48u, 64u, 0x78u, 0x80u, 0xC6u, 0x100u, 0x778u})
            if (g_ram[(a + st) & 0x07FFFFFFu] == c1 && g_ram[(a + 4u * st) & 0x07FFFFFFu] == c4) std::fprintf(stderr, "[TTSP] party at %08X stride %u (screen %08X, %+d): %u %u %u %u %u\n", a, st, screen, static_cast<int>(a - screen),
                g_ram[a & 0x07FFFFFFu], g_ram[(a + st) & 0x07FFFFFFu], g_ram[(a + 2u * st) & 0x07FFFFFFu], g_ram[(a + 3u * st) & 0x07FFFFFFu], g_ram[(a + 4u * st) & 0x07FFFFFFu]);
    }
}

// ---- Single player Tournament screen labels (19 Nov, tree ttlock): the format bar reads "Assists: ON / OFF" and each matchup row shows who plays that member instead of the stage ----
// Both are drawn over the game's own text with the bar's colours measured on screenshots (the bar interior is flat vertically, a plain horizontal gradient).
// PSPRECOMP_TAGTEAM_SPLABELS_OFF=1: the game's "Tournament" / "Random" stay.
constexpr uint32_t PLAYER_NAME_ADDR = 0x08BCEB40u;   // the save's player name, UTF-16 (same address as discord_presence.cpp)
std::string sp_player_name() {
    std::string out;
    for (int i = 0; i < 16; i++) {
        uint16_t c; std::memcpy(&c, g_ram + ((PLAYER_NAME_ADDR + 2u * i) & 0x07FFFFFFu), 2);
        if (c == 0) break;
        if (c < 0x20 || c == 0xFFFF) return "Player";
        out += c < 0x80 ? char(c) : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "Player" : out;
}
LayerColor lerp_col(float x, float x0, float x1, const float c0[3], const float c1[3], float a = 1.f) {
    const float t = std::clamp((x - x0) / (x1 - x0), 0.f, 1.f);
    return LayerColor{c0[0] + (c1[0] - c0[0]) * t, c0[1] + (c1[1] - c0[1]) * t, c0[2] + (c1[2] - c0[2]) * t, a};
}
void sp_label(float cx, float baseline, const std::string& txt, float em, LayerColor col, float maxw) {
    std::string t = txt;
    while (t.size() > 1 && g_layer.text(t.c_str(), true, em, 0, 0, LayerColor{0, 0, 0, 0}, true) > maxw) t.pop_back();
    const float w = g_layer.text(t.c_str(), true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
    g_layer.text(t.c_str(), true, em, cx - w / 2.f, baseline, col);
}
void sp_format_bar(bool assists) {   // "Tournament" / "Round-Robin" -> "Assists: ON / OFF"
    static const float a0[3] = {114, 123, 161}, a1[3] = {124, 134, 167};
    g_layer.band(198.f, 103.f, 284.f, 115.f, [&](float x, float) { return lerp_col(x, 200.f, 300.f, a0, a1); });
    sp_label(240.f, 112.f, assists ? "Assists: DEFAULT" : "Assists: PARTNER", 8.f, LayerColor{255, 255, 255, 1.f}, 80.f);
}
// The enemy side's CPU strength of round i: the round dialog keeps its STRENGTH in the screen object, one block of 0x16DC bytes per round (found with PSPRECOMP_TTSP_CURDUMP=2 as the
// word at +0x2134 / +0x3810 whose top byte was 4 > 3 > 2 / 4 > 5 while the dialog's STRENGTH of round 2 / 3 moved; tier = screen value - 1).
int sp_enemy_strength(uint32_t screen, int i) {
    const int tier = g_ram[(screen + 0xA5Bu + 0x16DCu * uint32_t(i)) & 0x07FFFFFFu];
    return tier >= 0 && tier <= 8 ? tier + 1 : 0;
}
void sp_row_label(int i, bool hi, const std::string& ours, const std::string& foe) {   // the stage text of matchup row i: our side on the left, the enemy's on the right
    static const float n0[3] = {194, 200, 216}, n1[3] = {160, 166, 179}, h0[3] = {64, 49, 211}, h1[3] = {37, 19, 158};
    const float y0 = 123.f + 20.f * float(i), y1 = y0 + 11.f;
    g_layer.band(246.f, y0, 342.f, y1, [&](float x, float) { return hi ? lerp_col(x, 276.f, 380.f, h0, h1) : lerp_col(x, 276.f, 380.f, n0, n1); });
    const LayerColor col = hi ? LayerColor{255, 255, 255, 1.f} : LayerColor{58, 60, 76, 1.f};
    sp_label(293.f, y0 + 9.f, ours, 7.5f, col, 62.f);
    if (!foe.empty()) {
        g_layer.band(336.f, y0 + 2.f, 337.f, y1 - 1.f, [&](float, float) { return LayerColor{col.r, col.g, col.b, 0.5f}; });
        sp_label(363.f, y0 + 9.f, foe, 7.5f, col, 46.f);
    }
}
void sp_hide_bands() {   // the round dialog (every matchup, the lobby sets them): STAGE / RULES / JUDGEMENT covered
    for (const auto& r : {std::pair<float, float>{103.f, 125.f}, std::pair<float, float>{183.f, 229.f}})
        g_layer.band(66.f, r.first, 414.f, r.second, [&](float, float) { return LayerColor{208, 210, 219, 1.f}; });
}
// 25 Nov: the Free-for-all walks the game's own Party Select and CPU Select (no scripted pad presses). Arriving at the setup screen, the players screen is filled from them:
// P1 = the party's first two members, P2 / P3 / P4 = CPU Select's 1st / 2nd / 3rd pick (the screen object keeps the picks at +0x8B4, 8 bytes apart, charId or 0 = empty;
// found by diffing two screen dumps with different picks); P2 stays on (random when empty), an empty P3 / P4 is Off. Strength / tag-in settings are kept.
void ffa_prefill(uint32_t screen, const uint8_t* rows, int n, int only) {
    if (only >= 0) {   // the player opened the character grid (CPU Select) for one field: its first pick is the answer (the picks were cleared before)
        int pick = 0; for (int i = 0; i < 5 && !pick; i++) { const int c = g_ram[(screen + 0x8B4u + 8u * uint32_t(i)) & 0x07FFFFFFu]; if (c >= 1 && c <= 33) pick = c; }
        std::fprintf(stderr, "[FFA] character select for P%d %s: pick %d\n", only + 1, g_hop_field == 1 ? "main" : "tag-in", pick);
        if (pick) {
            if (only == 0) { uint8_t p1[2] = {0, 255}; dissidia_tagteam_ffa_p1_get(p1); const int f = g_hop_field - 1; p1[f] = static_cast<uint8_t>(pick); if (f == 0 && p1[1] == pick) p1[1] = 255; if (f == 1 && p1[0] == pick) p1[0] = 0; dissidia_tagteam_ffa_p1_set(p1); }
            else { uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(only - 1, sl); if (g_hop_field == 1) { sl[0] = 1; sl[1] = static_cast<uint8_t>(pick); if (sl[2] == pick) sl[2] = 0; } else sl[2] = static_cast<uint8_t>(pick); dissidia_tagteam_ffa_set(only - 1, sl); }
        }
        for (uint32_t k = 0; k < 40u; k++) g_ram[(screen + 0x8B4u + k) & 0x07FFFFFFu] = 0;
        return;
    }
    // legacy (the setup screen came back some other way): only what retail's screens changed is taken over
    uint8_t p1[2] = {0, 255}; dissidia_tagteam_ffa_p1_get(p1);
    if (n >= 1 && rows[0]) {
        const uint8_t r0 = rows[0], r1 = (n >= 2 && rows[1] && rows[1] != rows[0]) ? rows[1] : 255;
        if (r0 != g_seen_p1[0] || r1 != g_seen_p1[1]) { p1[0] = r0; p1[1] = r1; dissidia_tagteam_ffa_p1_set(p1); }
        g_seen_p1[0] = r0; g_seen_p1[1] = r1;
    }
    for (int i = 0; i < 3; i++) {
        const int c = g_ram[(screen + 0x8B4u + 8u * uint32_t(i)) & 0x07FFFFFFu];
        const uint8_t pick = (c >= 1 && c <= 33) ? static_cast<uint8_t>(c) : 0;
        if (!pick || pick == g_seen_pick[i]) { g_seen_pick[i] = pick; continue; }
        g_seen_pick[i] = pick;
        uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(i, sl);
        sl[0] = 1; sl[1] = pick; if (sl[2] == pick) sl[2] = 0;
        dissidia_tagteam_ffa_set(i, sl);
    }
}
// 26 Nov: Enter Battle without a press: what the list does on Cross with the cursor on Enter Battle (ovl_menu 0x08AABCD8): the row's select animation (FUN_088D9F5C(M + 0x6C, 5, 1)),
// sound 0x11, M + 0x624 = 1 (the result the screen reports: 1 = Enter Battle) and M + 4 = 5 (its own leave: 5 tears down, 6 reports the result) = FUN_08AAB8B4(screen, 0).
void sp_enter_battle(uint8_t* rdram, recomp_context* ctx, uint32_t screen) {
    const uint32_t M = r32(screen + SCR_M); if (!okp(M)) return;
    const recomp_context save = *ctx;
    if (FuncPtr f = psp_dispatch_probe_lookup(0x088D9F5Cu); f && okp(r32(M + 0x6Cu))) { ctx->r[4] = static_cast<int32_t>(r32(M + 0x6Cu)); ctx->r[5] = 5; ctx->r[6] = 1; f(rdram, ctx); *ctx = save; }
    play_se(rdram, ctx, 0x11);
    if (FuncPtr f = ovl_menu_lookup(0x08AAB8B4u)) { ctx->r[4] = static_cast<int32_t>(screen); ctx->r[5] = 0; f(rdram, ctx); *ctx = save; }
    else { g_ram[(M + 0x624u) & 0x07FFFFFFu] = 1; w32(M + 4u, 5u); }
    std::fprintf(stderr, "[FFA] setup screen: Enter Battle (the list's own step, no press)\n");
}
// 26 Nov (user: no injected presses): "Add CPU" (L on the free row) = what the Communications setup screen's list does on Cross with the cursor on a row other than Enter Battle
// (ovl_menu 0x08AAF010: with screen flags (screen + 0x7CC0) bit 1 and no pending word (+0x7CC4): FUN_08AAA608(screen, +0x7CC8) must accept, then flags |= 8, M + 4 = 5, the cursor object's +8 / +12 = +0x7CC4 / +0x7CC8 and FUN_08AAB0B8(screen), sound 1 (refused: sound 4); the plain path: sound 1, FUN_08AA9634(screen) (opens the row's selector), M + 4 = 3 and, with flags bit 0,
// the row's animation steps FUN_08AB58E0(obj, 9, row), (obj, 6, 3), (obj, 7, 0) and FUN_08AB5790(obj, 2) on obj = *(screen + 0x7CBC). PSPRECOMP_TAGTEAM_INJECT_OLD=1: the old injected Cross.
void add_cpu_direct(uint8_t* rdram, recomp_context* ctx, uint32_t screen) {
    const uint32_t M = r32(screen + SCR_M); FuncPtr f9634 = ovl_menu_lookup(0x08AA9634u), fA608 = ovl_menu_lookup(0x08AAA608u), fB0B8 = ovl_menu_lookup(0x08AAB0B8u);
    const uint32_t st = okp(M) ? r32(M + 4u) : 0xFFFFFFFFu;
    if (!okp(screen) || !okp(M) || (st != 2u && st != 7u)) { std::fprintf(stderr, "[TTCPU] Add CPU: setup screen not on its list (state %u), nothing done (no press injected)\n", st); dissidia_tagteam_add_cpu_request(false); return; }
    const uint32_t flags = r32(screen + 31936u);
    const recomp_context save = *ctx;
    if ((flags & 2u) && r32(screen + 31940u) == 0u) {   // the Communications setup screen: the row's own selector (state 7 = its list)
        const uint32_t obj = r32(screen + 31908u);
        if (!fA608 || !fB0B8 || !okp(obj)) { std::fprintf(stderr, "[TTCPU] Add CPU: the screen's selector step is not reachable, nothing done (no press injected)\n"); dissidia_tagteam_add_cpu_request(false); return; }
        ctx->r[4] = static_cast<int32_t>(screen); ctx->r[5] = static_cast<int32_t>(r32(screen + 31944u)); fA608(rdram, ctx); const bool ok = ctx->r[2] != 0; *ctx = save;
        if (!ok) { play_se(rdram, ctx, 4); std::fprintf(stderr, "[TTCPU] Add CPU: the screen refuses the row (its own buzz), nothing opened\n"); dissidia_tagteam_add_cpu_request(false); return; }
        w32(screen + 31936u, flags | 8u); w32(M + 4u, 5u); w32(obj + 8u, r32(screen + 31940u)); w32(obj + 12u, r32(screen + 31944u));
        ctx->r[4] = static_cast<int32_t>(screen); fB0B8(rdram, ctx); *ctx = save;
        play_se(rdram, ctx, 1);
        std::fprintf(stderr, "[TTCPU] Add CPU: the Communications setup screen's own Cross step called on row %u, flags %08X (no press)\n", r32(screen + SCR_ROW), flags);
        return;
    }
    if (!f9634) { dissidia_tagteam_add_cpu_request(false); return; }
    play_se(rdram, ctx, 1);
    ctx->r[4] = static_cast<int32_t>(screen); f9634(rdram, ctx); *ctx = save;
    w32(M + 4u, 3u);
    if (flags & 1u) {
        const uint32_t obj = r32(screen + 31948u), row = r32(screen + SCR_ROW);
        if (okp(obj)) {
            if (FuncPtr f = ovl_menu_lookup(0x08AB58E0u)) { const uint32_t a[3][2] = {{9, row}, {6, 3}, {7, 0}}; for (auto& e : a) { ctx->r[4] = static_cast<int32_t>(obj); ctx->r[5] = e[0]; ctx->r[6] = e[1]; f(rdram, ctx); *ctx = save; } }
            if (FuncPtr f = ovl_menu_lookup(0x08AB5790u)) { ctx->r[4] = static_cast<int32_t>(obj); ctx->r[5] = 2; f(rdram, ctx); *ctx = save; }
        }
    }
    std::fprintf(stderr, "[TTCPU] Add CPU: the setup screen's own Cross step called on row %u, flags %08X (no press)\n", r32(screen + SCR_ROW), flags);
}
void sp_draw(uint8_t* rdram, recomp_context* ctx, uint32_t screen, bool hide) {
    dissidia_tagteam_result_done();
    g_sp_screen_last = screen;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;
    g_appear = dt >= 0.1f ? 0.f : std::min(1.f, g_appear + dt / 0.25f);
    const float a = g_appear;
    uint8_t rows[5] = {};
    const int n = dissidia_tagteam_sp_rows(screen, rows);
    const uint32_t b = g_raw_pad.load(std::memory_order_relaxed);
    const uint32_t pressed = dir_repeat(b, b & ~g_prev_buttons, g_dlg_open && g_dlg_mode >= 1 && g_dlg_mode <= 4);
    g_prev_buttons = b;
    const uint32_t M = r32(screen + SCR_M);
    const bool mok = M >= 0x08800000u && M < 0x0A000000u;
    const uint32_t state = mok ? r32(M + 4u) : 0xFFFFFFFFu;
    static const uint32_t kListState = std::getenv("PSPRECOMP_TTSP_LISTSTATE") ? static_cast<uint32_t>(std::atoi(std::getenv("PSPRECOMP_TTSP_LISTSTATE"))) : 2u;   // M + 4: 2 = the matchup list, 3 / 4 = the round dialog opening / open (seen with PSPRECOMP_TTSP_CURDUMP)
    const bool list = mok && state == kListState;   // the matchup list has the cursor (not the round dialog, Player Select, ...)
    const int row = mok ? static_cast<int>(r32(M + 1444u)) : -1;
    const bool switchable = list && row >= 1 && row < n && row < 5;
    { static int pn = -1, prow = -9; static uint32_t pst = 0; if (n != pn || row != prow || state != pst) { pn = n; prow = row; pst = state;
        std::fprintf(stderr, "[TTSP] setup screen: %d rows (%d %d %d %d %d), state %u, cursor row %d\n", n, rows[0], rows[1], rows[2], rows[3], rows[4], state, row); } }
    if (std::getenv("PSPRECOMP_TTSP_GNODES")) {   // debug: the group struct's type-5 commands (CPU side) while the screen runs
        static std::string prev; const uint32_t base = r32(0x089D8508u); std::string cur; char bb[64];
        if (base >= 0x08800000u && base < 0x0A000000u) for (int i = 0; i < 10; i++) for (int j = 0; j < 23; j++) {
            const uint32_t c = base + 104u + 744u * uint32_t(i) + 0x20u * uint32_t(j); uint16_t t; std::memcpy(&t, g_ram + (c & 0x07FFFFFFu), 2);
            if (t == 5 || t == 0) { std::snprintf(bb, sizeof bb, " n%d.%d t%u:", i, j, t); cur += bb; for (int k = 0; k < 8; k++) { std::snprintf(bb, sizeof bb, "%d,", int(int8_t(g_ram[(c + 0x10u + k) & 0x07FFFFFFu]))); cur += bb; } }
        }
        if (cur != prev) { prev = cur; std::fprintf(stderr, "[TTGN] state %u cursor %d base %08X:%s\n", state, row, base, cur.c_str()); }
    }
    const uint8_t mask0 = dissidia_tagteam_sp_cpu_mask();
    const bool cpu_row = switchable && ((mask0 >> row) & 1);
    std::memcpy(g_sp_rows_c, rows, 5); g_sp_rows_n = n;
    if (g_dlg_open && g_dlg_mode == 0 && (!cpu_row || row != g_dlg_row)) g_dlg_open = false;   // (the CPU settings dialog follows its row; the lobby stays)
    if (g_dlg_open && g_dlg_mode >= 2 && g_dlg_mode <= 4 && !list) g_dlg_open = false;
    bool opened_now = false;
    const bool lobby_ok = list && row >= 0 && row < 5 && !cpu_row && !std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF");   // Select on the player's own row: the battle lobby
    if (!g_dlg_open && lobby_ok && (pressed & BTN_OPEN)) { opened_now = true; g_lob_flow = false; g_lob_sp = true; g_dlg_mode = 2; g_dlg_bp = false; g_dlg_sp = false; g_dlg_open = true; play_se(rdram, ctx, 1); }
    // Free-for-all, after the battle (23 Nov, PR #129 TODO "after the battle the player lands in the Group Battle menu, which makes no sense"): "Return to
    // Battle Setup" brought back retail's Group Battle setup screen, which the flow had only passed through. Now the Free-for-all players screen opens over
    // it again (fighters, then the lobby, then the next battle); Circle there leaves the flow as before. The setup screen's tick does not run during the
    // battle, so a gap of more than 2 s after the start marks the way back. PSPRECOMP_TAGTEAM_FFA_NOREOPEN=1: old.
    {   static auto last = std::chrono::steady_clock::now(); const auto now = std::chrono::steady_clock::now();
        if (g_ffa_after == 1 && now - last > std::chrono::seconds(2) && !g_nw_direct) g_ffa_after = 2;   // (new: the app's state 29 hook, qb_frame_hook)
        last = now;
        if (g_ffa_after == 2 && list && !g_dlg_open && !g_ffa_autostart && !ps_old()) {   // 26 Nov: leave the app (as on Circle) for the players screen over the list
            sp_back_direct(rdram, ctx);   // (the screen's own teardown: state 0x1E of the app waits for it before 0x30)
            g_ffa_after = 0; g_leave_app = true; g_qb_mode = QB_RETURN; g_post_exit_t0 = 0; g_post_in_t0 = 0; g_ps_pick = 0; g_hop_slot = -1; g_hop_field = 1; g_ms_ffa = true; g_trip_post = true;
            std::fprintf(stderr, "[FFA] back from the battle: the app leaves to the list, the players screen opens there\n");
        }
        if (g_ffa_after == 2 && list && !g_dlg_open && !g_ffa_autostart) {
            g_ffa_after = 0; opened_now = true; g_lob_sp = true; g_dlg_bp = false; g_dlg_sp = false; g_dlg_open = true; g_lob_row = 0;
            g_ffa_flow = true; g_dlg_mode = 3; g_ffa_then_lobby = true; g_ffa_slot = 1; g_ffa_field = 1;
            std::fprintf(stderr, "[FFA] back from the battle: the players screen again (not the Group Battle setup)\n");
        }
    }
    {   static auto last_sp = std::chrono::steady_clock::now() - (std::getenv("PSPRECOMP_TAGTEAM_TEST_REARM") ? std::chrono::seconds(10) : std::chrono::seconds(0)); const auto nw = std::chrono::steady_clock::now();
        if (g_rearm && (g_ffa_autostart || g_qb_mode == QB_START || g_start_cover) && !ffawalk_old()) { g_rearm = false; ffawalk_log("stale re-arm dropped (a start is under way)", -1); }
        if (g_rearm && nw - last_sp > std::chrono::milliseconds(kRearmMs) && !menuback_old()) { g_rearm = false; g_lobby_pending = true; std::fprintf(stderr, "[TTLOBBY] the setup screen is back after backing out: the lobby again\n"); }
        last_sp = nw; }
    if (g_lobby_pending && (g_ffa_autostart || g_qb_mode == QB_START) && !ffawalk_old()) { g_lobby_pending = false; ffawalk_log("stale lobby request dropped (a start is under way)", -1); }
    if (!g_dlg_open && g_lobby_pending && list && !std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF")) {   // the flow: Tag Team Battle > mode select > Party Select > (the Free-for-all player setup >) the lobby > this screen
        g_lobby_pending = false; opened_now = true; g_lob_sp = true; g_dlg_bp = false; g_dlg_sp = false; g_dlg_open = true; g_lob_row = 0;
        uint8_t L0[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1}; dissidia_tagteam_lobby_get(L0);
        if (L0[0] == 3) {
            g_ffa_flow = true; g_dlg_mode = 3; g_ffa_then_lobby = true; g_ffa_slot = 1; g_ffa_field = 1;
            const int only = g_hop_slot; g_ms_hop = false;
            if (g_hop_slot >= 0) { g_ffa_slot = g_hop_slot; g_ffa_field = g_hop_field; g_hop_slot = -1; }
            if (!std::getenv("PSPRECOMP_TAGTEAM_FFA_NOPREFILL")) ffa_prefill(screen, rows, n, only);
        } else { g_dlg_mode = 2; g_lob_flow = true; }
    }
    {   // Circle on the setup screen's list (no dialog): back to the Battle Lobby, the step before (the Free-for-all keeps retail's Circle)
        uint8_t Lc[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1}; dissidia_tagteam_lobby_get(Lc);
        const bool ours = list && Lc[0] != 3 && !g_ffa_flow && !std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF") && !menuback_old();
        if (ours) g_circle_stamp.store(steady_ms(), std::memory_order_relaxed);   // (the pad filter hides Circle from the list while it is ticking)
        if (ours && !g_dlg_open && (pressed & BTN_CIRCLE)) { opened_now = true; g_lob_sp = true; g_lob_flow = true; g_dlg_mode = 2; g_dlg_bp = false; g_dlg_sp = false; g_dlg_open = true; g_lob_row = 0; play_se(rdram, ctx, 1); std::fprintf(stderr, "[TTLOBBY] setup screen: Circle, back to the Battle Lobby\n"); }
    }
    if (!g_dlg_open && cpu_row && (pressed & BTN_OPEN)) { opened_now = true; g_dlg_mode = 0; g_dlg_bp = false; g_dlg_sp = true; g_dlg_row = row; g_dlg_field = 0; g_dlg_hold = 0.f; dissidia_tagteam_sp_get_cfg(row, g_dlg_cfg); g_dlg_open = true; play_se(rdram, ctx, 1); }
    if (!g_dlg_open && switchable && (pressed & BTN_L)) { dissidia_tagteam_sp_toggle(row); play_se(rdram, ctx, 1); }   // L (Square: Assist On / Off, below)
    const bool sq_ours = list && !std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE_OFF") && !(std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE") && std::getenv("PSPRECOMP_TAGTEAM_ASSISTTOGGLE")[0] == '0');
    g_sp_sq_block.store(sq_ours, std::memory_order_relaxed); g_sq_stamp.store(g_pad_polls.load(std::memory_order_relaxed), std::memory_order_relaxed);   // the pad filter hides Square from the screen (no "Swap format" = no Round-Robin) while the list has the cursor
    if (!g_dlg_open && sq_ours && (pressed & BTN_SQUARE)) { dissidia_tagteam_sp_assist_toggle(); play_se(rdram, ctx, 1); }
    const uint8_t mask = dissidia_tagteam_sp_cpu_mask();
    { uint8_t L[10]; dissidia_tagteam_lobby_get(L); if (mok && list && !std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF")) dissidia_tagteam_sp_write_round(screen); }   // (the saved lobby is read once, the mode's switches follow it)
    if (list) g_script_t.store(-1, std::memory_order_relaxed);   // the scripted Party Select pass is over
    if (g_ffa_autostart && list && !ffawalk_old()) {   // 26 Nov: the walk's watchdog: whatever dialog is up, the start goes on
        if (!g_ffa_walk_seen) { g_ffa_walk_seen = true; ffawalk_log("setup screen is up", row); }
        if (g_dlg_open) { g_dlg_open = false; g_dlg_mode = 0; g_ffa_then_lobby = false; ffawalk_log("a dialog was open over the start: closed", row); }
    }
    if (g_ffa_autostart && list && !g_dlg_open) {   // Enter Battle (the cursor starts on it)
        g_ffa_autostart = false; if (dissidia_tagteam_inject_old()) g_inject_late.store(9, std::memory_order_relaxed); else sp_enter_battle(rdram, ctx, screen); if (!std::getenv("PSPRECOMP_TAGTEAM_FFA_NOREOPEN")) g_ffa_after = 1; std::fprintf(stderr, "[FFA] lobby done: Enter Battle (cursor row %d)\n", row); ffawalk_log("Enter Battle called", row);
    }
    {   // 26 Nov (user: the stage preview must follow a lobby stage change; the Down + Up nudge is gone): when the lobby closes over the list, the screen's preview step
        // FUN_08AA9350 (the one the list runs on a cursor move) is called once. PSPRECOMP_TAGTEAM_PREVIEW_OLD=1: not called.
        static bool lob_was = false; const bool lob_now = g_dlg_open && g_dlg_mode == 2;
        if (lob_was && !lob_now && list && !std::getenv("PSPRECOMP_TAGTEAM_PREVIEW_OLD")) if (FuncPtr f = psp_dispatch_probe_lookup(0x08AA9350u)) {
            const recomp_context save = *ctx; ctx->r[4] = static_cast<int32_t>(screen); f(rdram, ctx); *ctx = save; std::fprintf(stderr, "[TTLOBBY] lobby closed: the stage preview step called\n"); }
        lob_was = lob_now;
    }
    if (g_start_cover && !ps_old()) { start_cover_draw(); g_sp_layer_up = true; return; }
    if (g_trip_post && !ps_old()) { trip_draw(post_white(), true); g_sp_layer_up = true; return; }   // after a battle: the setup screen is only passed   // the Free-for-all start card is the only layer while the setup screen passes
    const bool cover = std::chrono::steady_clock::now() < g_cover_until && ps_old();   // (26 Nov: the start card is trip_tick's)
    const bool labels = mok && (list || g_dlg_open) && !std::getenv("PSPRECOMP_TAGTEAM_SPLABELS_OFF");
    if (!mask && !switchable && !sq_ours && !g_sp_layer_up && !hide && !labels && !cover) return;   // nothing to draw (the layer is left to the screen's other users)
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    if (hide) sp_hide_bands();
    if (labels) {
        sp_format_bar(dissidia_tagteam_sp_assist_on());
        const std::string me = sp_player_name();
        for (int i = 0; i < n && i < 5; i++) {
            std::string t = me;
            if ((mask >> i) & 1) { uint8_t cf[3]; dissidia_tagteam_sp_get_cfg(i, cf); t = "CPU STR " + std::to_string(cf[0]); }
            const int es = sp_enemy_strength(screen, i);
            sp_row_label(i, row == i, t, es ? "CPU STR " + std::to_string(es) : std::string());
        }
    }
    if (mok && (list || g_dlg_open)) {
        for (int i = 1; i < n && i < 5; i++) if ((mask >> i) & 1) {
            const float y = kRowY0 + kRowPitch * float(i);
            tag(141.f, y + 5.f, 162.f, y + 14.f, "CPU", a);   // after the member's job name, before the matchup's "2nd"
        }
        float right = 334.f;   // the bar at the top, left of the screen's own hints ("Swap format" from x ~341, "R Job Settings"; user screenshot 18 Nov: at 370 ours covered "Swap format")
        if (sq_ours && !g_dlg_open) { right = bar_text(right, dissidia_tagteam_sp_assist_on() ? "Assist: Default" : "Assist: Partner", a); key(right - 3.f, "", true, a); right -= 20.f; }   // Square (not drawn by the game: its "Swap format" is blocked); host-unverified placement
        if (cpu_row && !g_dlg_open) right = bar_text(right, "Select: CPU Settings", a) - 8.f;
        if (lobby_ok && !g_dlg_open) right = bar_text(right, "Select: Lobby", a) - 8.f;
        if (switchable && !g_dlg_open) { right = bar_text(right, (mask >> row) & 1 ? "Player" : "CPU", a); key(right - 3.f, "L", false, a); }
        if (list && !g_dlg_open && !std::getenv("PSPRECOMP_TAGTEAM_SPLOBBY_OFF")) {   // the lobby's choice on one line at the bottom
            uint8_t L[10] = {1, 2, 0, 5, 0, 0, 0, 0, 0, 1};
            if (dissidia_tagteam_lobby_get(L)) {
                const std::string t = lobby_summary(L) + (L[2] ? "  /  CPU fill " + std::to_string(L[3]) : std::string());
                const float em = 5.6f, w = g_layer.text(t, true, em, 0, 0, LayerColor{0, 0, 0, 0}, true);
                g_layer.text(t, true, em, 240.f - w / 2.f, 266.f, LayerColor{60, 60, 110, a});
            }
        }
        if (g_dlg_open) { if (g_dlg_mode == 2) lobby_dialog(rdram, ctx, opened_now ? 0u : pressed, a); else if (g_dlg_mode == 3 || g_dlg_mode == 4) ffa_screen(rdram, ctx, opened_now ? 0u : pressed); else dialog(rdram, ctx, opened_now ? 0u : pressed, dt, a); }
    }
    if (cover && !g_dlg_open) { gs_background(); const std::string t = "Starting the battle..."; const float w = g_layer.text(t, true, 10.f, 0, 0, LayerColor{0, 0, 0, 0}, true); g_layer.text(t, true, 10.f, kScrW / 2.f - w / 2.f, 140.f, LayerColor{40, 42, 100, 1.f}); }
    g_layer.tag(g_dlg_open ? dlg_tag() : "sp_setup"); g_layer.publish(); g_sp_layer_up = mask || switchable || sq_ours || g_dlg_open || hide || labels || cover;
}
void sp_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SP_TICK)) f(rdram, ctx); return; }
    const uint32_t screen = static_cast<uint32_t>(ctx->r[4]);
    g_sp_dlg_ran = false; g_sp_hide_round = -1;
    g_orig_sp_tick(rdram, ctx);
    if (!g_sp_dlg_ran) g_sp_row.store(-1, std::memory_order_relaxed);
    const bool hide = g_sp_dlg_ran && g_sp_hide_round >= 0;
    if (std::getenv("PSPRECOMP_TTSP_CURDUMP")) sp_dump(screen);
    if (std::getenv("PSPRECOMP_TTSP_PARTYSCAN")) sp_partyscan(screen);
    if (std::getenv("PSPRECOMP_TTSP_SCRDUMP") && g_ram[(screen + 0x788u) & 0x07FFFFFFu]) sp_scrdump(screen);
    if (std::getenv("PSPRECOMP_TAGTEAM_NOSPCPU") || !dissidia_tagteam_sp_mode()) {
        if (g_dlg_open && (g_dlg_sp || (g_lob_sp && g_dlg_mode >= 2 && g_dlg_mode <= 4))) g_dlg_open = false;   // (not the Communications lobby: PR #129, it closed at once)
        if (hide || g_sp_layer_up) { g_layer.begin(std::max(2, psp_sddialog::output_scale())); if (hide) sp_hide_bands(); g_layer.tag("sp_hide"); g_layer.publish(); g_sp_layer_up = hide; }
        return;
    }
    sp_draw(rdram, ctx, screen, hide);
}

// ---- 26 Nov: the Quick Battle app (see QbMode above) ----
constexpr uint32_t QB_APP_FRAME = 0x08A97DA8u, QB_SAVE_PTR = 0x089D712Cu, QB_SAVE_PARTY_A = 0x3E75Cu;   // the save's Party A: flags, name, five members (charId, word) = the 0x48 bytes at Party record + 0xF4
FuncPtr g_orig_qb_frame = nullptr, g_orig_sel_tick = nullptr;
// FUN_08AA1C1C(app): the network poll the states 5 / 29 call first; 1 = leave (their own fade out and state 0x30, to the list). Offline it returns -1: forced to 1 once to leave.
constexpr uint32_t QB_NET_POLL = 0x08AA1C1Cu;
FuncPtr g_orig_net_poll = nullptr;
void net_poll_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(QB_NET_POLL)) f(rdram, ctx); return; }
    g_orig_net_poll(rdram, ctx);
    if (g_leave_app) { g_leave_app = false; ctx->r[2] = 1; std::fprintf(stderr, "[FFA] the Quick Battle app leaves to the list (its own way out)\n"); }
}
constexpr uint32_t QB_SEL_TICK = 0x08A9A460u;   // FUN_08A9A460(app): ticks the select object (P + 0x92C), returns its result (2 = YES, 4 = Circle, 0 = running)
int g_sel_force = 0;
void sel_tick_hook(uint8_t* rdram, recomp_context* ctx) {
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(QB_SEL_TICK)) f(rdram, ctx); return; }
    if (g_sel_force) { ctx->r[2] = g_sel_force; g_sel_force = 0; return; }
    g_orig_sel_tick(rdram, ctx);
}
// The party records Party Select / CPU Select leave in P (seen after a retail pass: Party A = the save's Party A, the CPU party = one pick), written for the jump from state 16 to 28.
// Bytes not tied to a pick are retail's (a dump after Party A + Lightning, OK).
const uint8_t kPartyHead[0x4C] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x00,0x84,0x7F,0x79,0x00,0x00,0x00,0x00,0x7E,0x78,0x76,0x7D,0x7D,0x78,0x73,0xAE,
    0xFC,0xD8,0xF4,0xF1,0xEE,0xEB,0xE9,0xE6,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
void ffa_fill_party(uint32_t P, int cpu_char) {
    uint8_t* A = g_ram + ((P + 0x7C10u) & 0x07FFFFFFu);
    std::memset(A, 0, 0x13C); std::memcpy(A, kPartyHead, sizeof kPartyHead); std::memset(A + 0x4C, 0xFF, 20);
    const uint32_t save = r32(QB_SAVE_PTR);
    uint8_t blk[0x48] = {};
    if (okp(save)) std::memcpy(blk, g_ram + ((save + QB_SAVE_PARTY_A) & 0x07FFFFFFu), sizeof blk);
    int n = 0; for (int i = 0; i < 5; i++) { const uint32_t c = blk[0x20 + 8 * i]; if (c >= 1 && c <= 40) n++; }
    if (n == 0) {   // no saved Party A: the Free-for-all's own P1 (the battle uses its own team anyway)
        uint8_t p1[2] = {0, 255}; dissidia_tagteam_ffa_p1_get(p1); const uint8_t c = p1[0] ? p1[0] : 31;
        std::memset(blk, 0, sizeof blk); blk[0] = 1; blk[2] = 1; blk[0x20] = c; blk[0x24] = 1; n = 1;
    }
    std::memcpy(A + 0xF4, blk, sizeof blk);
    uint8_t* C = g_ram + ((P + 0x7D4Cu) & 0x07FFFFFFu);   // the CPU party: one member
    std::memset(C, 0, 0x13C); C[0] = 1; std::memset(C + 8, 0xFF, 4); C[0xC] = 5; C[0xE6] = 1; C[0x104] = static_cast<uint8_t>(cpu_char);
    for (int k = 0; k < 4; k++) std::memset(C + 0x110 + 8 * k, 0xFF, 4);
    uint8_t* L = g_ram + ((P + 0x82F8u) & 0x07FFFFFFu);   // the lineup: the party's five, then the CPU's
    std::memset(L, 0, 0x50);
    for (int i = 0; i < 5; i++) std::memcpy(L + 8 * i, blk + 0x20 + 8 * i, 8);
    L[0x28] = static_cast<uint8_t>(cpu_char); L[0x2C] = 1; std::memset(L + 0x34, 0xFF, 4); L[0x44] = 1; std::memset(L + 0x4C, 0xFF, 4);
    std::fprintf(stderr, "[FFA] party records written for the setup screen (Party A: %d members, CPU %d)\n", n, cpu_char);
}
void qb_frame_hook(uint8_t* rdram, recomp_context* ctx) {
    dissidia_tagteam_result_frame(rdram, ctx);   // (the result screen's sound cues)
    if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(QB_APP_FRAME)) f(rdram, ctx); return; }
    const uint32_t app = static_cast<uint32_t>(ctx->r[4]), P = okp(app) ? r32(app + 0x154u) : 0u;
    const int before = okp(P) ? static_cast<int>(r32(P + 0x910u)) : -1;
    g_qb_last_ms = steady_ms();
    if (g_qb_mode == QB_START && before == 16) {   // the Free-for-all battle: no Party Select / CPU Select, straight to the setup screen
        uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(0, sl);
        ffa_fill_party(P, sl[1] ? sl[1] : 31);
        w32(P + 0x910u, 28u); g_qb_mode = QB_NONE; g_start_cover = true; g_start_exit_t0 = 0;
        std::fprintf(stderr, "[FFA] Quick Battle (Party): state 16 -> 28 (setup screen)\n"); ffawalk_log("state 16 -> 28", before);
    }
    if (g_qb_mode == QB_PICK && before == 16) { g_qb_mode = QB_NONE; std::fprintf(stderr, "[FFA] Player Select trip: the app took the Party route (state 16), dropped\n"); }
    if (g_qb_mode == QB_DETOUR && before == 5) { g_qb_mode = QB_PICK; std::fprintf(stderr, "[FFA] Player Select: back from the detour, picking again\n"); }
    if (okp(P) && !menuwalk_old()) {   // the direct start / way back (see menuwalk_old)
        const int st29 = static_cast<int>(r32(P + 0x910u));
        const uint32_t screen = r32(P + 0x8044u), M = okp(screen) ? r32(screen + SCR_M) : 0u;
        if (okp(screen) && st29 == 29) g_sp_screen_last = screen;
        const bool ready = st29 == 29 && okp(M) && r32(M + 4u) == 2u;   // the screen is built and its list is up (the app ticked it in its own frames; nothing of it is shown)
        if (g_nw_start && ready) {
            g_nw_start = false; g_ffa_autostart = false; g_rearm = false; g_lobby_pending = false; g_dlg_open = false; g_dlg_mode = 0;
            dissidia_tagteam_sp_write_round(screen);   // stage / rules / judgement / CPU strength of the lobby into the screen's round blocks
            sp_enter_battle(rdram, ctx, screen);
            if (!std::getenv("PSPRECOMP_TAGTEAM_FFA_NOREOPEN")) g_ffa_after = 1;
            g_nw_t0 = steady_ms(); g_nw_armed = false; g_nw_direct = (g_ffa_after == 1);
            std::fprintf(stderr, "[DIRECTSTART] Free-for-all: battle start request handed to the app (state 29, screen %08X not shown / not pressed): lobby stage / rules / judgement into the round blocks, Enter Battle; fighters from the players screen, CPU strength %d\n", screen, sp_enemy_strength(screen, 0));
            ffawalk_log("DIRECTSTART: Enter Battle called", 29);
        } else if (g_nw_direct && g_ffa_after == 1 && !g_nw_armed && st29 >= 0x30) g_nw_armed = true;   // the app left for the battle
        else if (g_nw_direct && g_ffa_after == 1 && g_nw_armed && ready && !g_nw_start && !ps_old()) {   // the battle has run: the app is back in state 29
            g_ffa_after = 0; g_nw_armed = false; g_nw_direct = false;
            sp_back_direct(rdram, ctx);   // (the screen's own teardown: the app waits for it before 0x30)
            g_leave_app = true; g_qb_mode = QB_RETURN; g_post_exit_t0 = 0; g_post_in_t0 = 0; g_ps_pick = 0; g_hop_slot = -1; g_hop_field = 1; g_ms_ffa = true; g_trip_post = true;
            std::fprintf(stderr, "[DIRECTBACK] Free-for-all: back from the battle, the app leaves for the players screen without a retail screen shown\n");
        }
    }
    g_orig_qb_frame(rdram, ctx);
    if (g_ffa_walk_t0 && okp(P)) {   // [FFAWALK]: every app state change of the start, and a note every 5 s while it sits
        static int last_st = -1; static int64_t last_note = 0; const int st = static_cast<int>(r32(P + 0x910u)), sub = static_cast<int>(r32(P + 0x91Cu)); const int64_t nw = steady_ms();
        if (st != last_st) { last_st = st; last_note = nw; ffawalk_log(("app state change, sub " + std::to_string(sub)).c_str(), st); }
        else if (nw - last_note > 5000) { last_note = nw; ffawalk_log(("STALL: app state unchanged, sub " + std::to_string(sub) + (g_ffa_autostart ? ", Enter Battle still pending" : "")).c_str(), st); }
        if (st >= 0x30) g_ffa_walk_t0 = 0;
    }
    if (g_start_cover && !g_start_exit_t0 && okp(P) && r32(P + 0x910u) >= 0x30u) g_start_exit_t0 = steady_ms();   // the battle start's fade out
    if (g_trip_post && !g_post_exit_t0 && okp(P) && r32(P + 0x910u) >= 0x30u) g_post_exit_t0 = steady_ms();   // after a battle: the app's way out to the list
    if (g_qb_mode == QB_PICK && before == 5 && okp(P) && r32(P + 0x910u) == 6u) {
        const uint32_t next = r32(P + 0x914u);
        if (next == 8u || next == 0xCu) {   // YES on "Play as this character?": the pick, then back to the list instead of CPU Select
            g_ps_pick = static_cast<int>(r32(P + 0x804Cu));
            const uint32_t save = r32(QB_SAVE_PTR); g_ps_look = 0;
            for (uint32_t i = 0; i < 31 && okp(save); i++) if (g_ram[(0x089CBC98u + i) & 0x07FFFFFFu] == g_ps_pick) { g_ps_look = g_ram[(save + 0xF58u + 0x778u * i + 0x774u) & 0x07FFFFFFu]; break; }
            // leave the way Circle does (its fade out, then 0x30): the state goes back to 5 and the next select tick reports 4 (= Circle) to the state's own code
            w32(P + 0x910u, 5u); g_sel_force = 4; g_qb_mode = QB_RETURN;
            std::fprintf(stderr, "[FFA] Player Select: YES on char %d (look %d): the app leaves to the list as on Circle (not CPU Select, state %u)\n", g_ps_pick, g_ps_look, next);
        } else if (next != 0x30u && next != 0x31u && !std::getenv("PSPRECOMP_TAGTEAM_FFA_CUSTOLD")) {
            // 26 Nov (user, Deck: Customization from the Free-for-all's Player Select = white screen): state 50 is the Customization app; retail brings the app back to this same
            // Player Select (state 5) when it closes. It was taken for "left without a pick": the white cover of the trip back stayed up over Customization. Now nothing of ours draws
            // and the pick goes on afterwards. PSPRECOMP_TAGTEAM_FFA_CUSTOLD=1: old.
            g_qb_mode = QB_DETOUR; std::fprintf(stderr, "[FFA] Player Select: detour to state %u (Customization): it comes back to the Player Select, the pick goes on\n", next);
        } else { g_ps_pick = 0; g_qb_mode = QB_RETURN; std::fprintf(stderr, "[FFA] Player Select: left without a pick (next state %u)\n", next); }
    }
}

// The Free-for-all trips to the Quick Battle app and back, drawn so that no other screen shows: retail fades the list to white (~10 frames), stays white while the app
// loads, then fades the next screen in; the players screen (or the "Starting the battle" card) follows the same fades on the game layer.
constexpr float kTripFadeMs = 330.f;
void trip_draw(float white, bool own_frame) {
    if (own_frame) { ge_draw_set_game_layer_ttl(90); g_layer.begin(std::max(2, psp_sddialog::output_scale())); }
    else g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    if (g_trip_out == 2) modesel_draw_body(g_trip_online);
    else if (g_qb_mode == QB_START || g_ms_done || g_start_cover) { gs_background(); gs_title("FREE-FOR-ALL", "Everyone fights for himself"); const char* t = "Starting the battle..."; const float w = g_layer.text(t, true, 9.f, 0, 0, LayerColor{0, 0, 0, 0}, true); g_layer.text(t, true, 9.f, (kScrW - w) / 2.f, 140.f, LayerColor{40, 42, 80, 1.f}); }
    else { const int sl = g_ffa_slot, fi = g_ffa_field, dm = g_dlg_mode; const bool fl = g_ffa_flow; g_ffa_flow = true; g_dlg_mode = 3; ffa_screen(nullptr, nullptr, 0u); g_ffa_flow = fl; g_ffa_slot = sl; g_ffa_field = fi; g_dlg_mode = dm; }
    if (white > 0.f) g_layer.band(0.f, 0.f, kScrW, 272.f, [&](float, float) { return LayerColor{255, 255, 255, std::min(1.f, white)}; });
    g_layer.tag("trip"); if (own_frame) g_layer.publish();
}

float post_white() {   // after a battle: fade in from retail's white as the setup screen comes, fade out with the app's way out
    const int64_t now = steady_ms(); if (!g_post_in_t0) g_post_in_t0 = now;
    const float in = std::max(0.f, 1.f - static_cast<float>(now - g_post_in_t0) / kTripFadeMs);
    return std::max(in, g_post_exit_t0 ? std::min(1.f, static_cast<float>(now - g_post_exit_t0) / kTripFadeMs) : 0.f);
}
void start_cover_draw() {   // the battle start: the card from the lobby's Cross until the app leaves for the battle, then its fade to white with retail's
    if (!g_start_exit_t0) { trip_draw(0.f, true); return; }
    const float t = static_cast<float>(steady_ms() - g_start_exit_t0) / kTripFadeMs;
    if (t < 1.3f) trip_draw(t, true); else g_start_cover = false;
}
float trip_in_white() {   // the fade in after the trip back (1 = white .. 0)
    if (!g_trip_in_t0) return 0.f;
    const float t = static_cast<float>(steady_ms() - g_trip_in_t0) / kTripFadeMs;
    if (t >= 1.f) { g_trip_in_t0 = 0; return 0.f; }
    return 1.f - t;
}
void trip_tick() {   // every pad poll
    if (ps_old()) return;
    const int64_t now = steady_ms();
    if (g_start_cover) start_cover_draw();
    if (g_trip_post && g_qb_mode == QB_RETURN && now - g_qb_last_ms <= 100) trip_draw(post_white(), true);   // after a battle: the players screen over the setup screen, fading to white with the app's way out
    if (g_trip_out) { const float t = static_cast<float>(now - g_trip_t0) / kTripFadeMs; if (t < 1.3f && now - g_qb_last_ms > 200) trip_draw(t, true); else g_trip_out = 0; }
    if (g_qb_mode == QB_RETURN && now - g_qb_last_ms > 100) {   // the app is gone: white until the start menu runs again, then the players screen fades in
        if (g_main_seen_ms > g_qb_last_ms + 50) { if (!g_trip_in_t0) g_trip_in_t0 = now; trip_draw(trip_in_white(), true); }
        else trip_draw(1.f, true);
    }
}

}  // namespace

bool dissidia_tagteam_post_flow() { return g_trip_post && g_qb_mode == QB_RETURN; }
void dissidia_tagteam_main_tick_seen() { g_main_seen_ms = steady_ms(); }   // menu_settings.cpp: the start menu's top level or sub-list ran

void dissidia_menu_rows_fade_tick();
static uint32_t menu_pad_inner(uint32_t raw) {
    trip_tick(); dissidia_menu_rows_fade_tick();
    if (ms_log()) { static uint32_t prev = 0; const uint32_t e = raw & ~prev; prev = raw; if (e & (BTN_UP | BTN_DOWN)) g_ms_press_t = std::chrono::steady_clock::now(); }
    g_raw_pad.store(raw, std::memory_order_relaxed);
    const int sr = g_sp_row.load(std::memory_order_relaxed);
    if (sr >= 0) {   // single player tag mode, round dialog of the 2nd round on: the cursor stays on STRENGTH / LEVEL / TYPE
        static bool inj = false;
        if (sr == 0 && dissidia_tagteam_inject_old()) { g_inj_why = "round dialog opened on STAGE: Down (INJECT_OLD)"; inj = !inj; raw = inj ? (raw | BTN_DOWN) : (raw & ~BTN_DOWN); }   // opened on STAGE: one Down (old; now sp_dlg_hook starts it on STRENGTH)
        else if (sr == 0) raw &= ~BTN_UP;
        else if (sr == 1) raw &= ~BTN_UP;
        else if (sr == 3) raw &= ~BTN_DOWN;
    }
    if (int ij = g_inject_cross.load(std::memory_order_relaxed); ij > 0) { g_inj_why = "Add CPU: Cross for the setup screen (INJECT_OLD)"; g_inject_cross.store(ij - 1, std::memory_order_relaxed); raw = (ij == 2) ? (raw | BTN_CROSS) : (raw & ~BTN_CROSS); }   // Add CPU: one Cross press (edge) for the screen
    if (steady_ms() - g_circle_stamp.load(std::memory_order_relaxed) < 100) raw &= ~BTN_CIRCLE;   // the setup screen's list: Circle is ours (back to the lobby)
    if (g_sp_sq_block.load(std::memory_order_relaxed) && g_pad_polls.fetch_add(1, std::memory_order_relaxed) - g_sq_stamp.load(std::memory_order_relaxed) < 40u) raw &= ~BTN_SQUARE;
    else g_pad_polls.fetch_add(1, std::memory_order_relaxed);
    if (const int eb = g_enter_block.load(std::memory_order_relaxed); eb && g_pad_polls.load(std::memory_order_relaxed) - g_enter_stamp.load(std::memory_order_relaxed) < 40u) { raw &= ~0x8u; if (eb == 2) raw &= ~BTN_CROSS; }   // the CPU party has no opponent yet: Start (and Cross on Enter Battle) do nothing
    if (int st = g_script_t.load(std::memory_order_relaxed); st >= 0) {   // the scripted pass (Party A, the CPU's 1st slot, a character, down to OK, OK)
        // 25 Nov (user: slow menus): it started at poll 220 (7 s at 30 fps; the game's Party Select is up after ~0.5 s) and spaced its steps 4 s apart (retail's transitions take 0.45 s): now 40 polls, and the later steps follow
        // at kScriptScale of the old spacing (1 = old; PSPRECOMP_TAGTEAM_SCRIPT_SCALE). The first event alone is a hop to CPU Select (g_script_lim < 1000).
        static const float sc = std::getenv("PSPRECOMP_TAGTEAM_SCRIPT_SCALE") ? static_cast<float>(std::atof(std::getenv("PSPRECOMP_TAGTEAM_SCRIPT_SCALE"))) : 0.3f;
        static const bool old_t = std::getenv("PSPRECOMP_TAGTEAM_SLOWMENU") != nullptr;
        const int lim = g_script_lim.load(std::memory_order_relaxed);
        {
            const int base0 = old_t ? 220 : (std::getenv("PSPRECOMP_TAGTEAM_SCRIPT_WAIT") ? std::atoi(std::getenv("PSPRECOMP_TAGTEAM_SCRIPT_WAIT")) : 40), sp = old_t ? 1 : 0;
            struct Ev { int t; uint32_t b; };
            const Ev ev[] = {{0, BTN_CROSS}, {int(120 * (old_t ? 1.f : sc)), BTN_CROSS}, {int(240 * (old_t ? 1.f : sc)), BTN_CROSS}, {int(360 * (old_t ? 1.f : sc)), BTN_DOWN}, {int(380 * (old_t ? 1.f : sc)), BTN_DOWN}, {int(400 * (old_t ? 1.f : sc)), BTN_DOWN}, {int(420 * (old_t ? 1.f : sc)), BTN_DOWN}, {int(440 * (old_t ? 1.f : sc)), BTN_DOWN}, {int(500 * (old_t ? 1.f : sc)), BTN_CROSS}};
            (void)sp;
            static int zero = -1; if (st == 0 || zero < 0) { /* st counts polls since Party Select was up */ }
            uint32_t m = 0; for (size_t i = 0; i < sizeof ev / sizeof ev[0]; i++) { if (lim < 1000 && i > 0) break; const int t0 = ev[i].t + base0; if (st >= t0 && st < t0 + 4) m |= ev[i].b; }
            const int endt = (lim < 1000 ? 4 + 20 : ev[8].t + 40) + base0;
            if (!old_t && !std::getenv("PSPRECOMP_TAGTEAM_FFA_NOCOVER")) {   // 25 Nov (user: Party Select must never show in the Free-for-all): the screens the script passes are covered by the players screen's background
                ge_draw_set_game_layer_ttl(160); g_layer.begin(std::max(2, psp_sddialog::output_scale()));
                gs_background(); gs_title("FREE-FOR-ALL", "Everyone fights for himself");
                const char* t = lim < 1000 ? "Opening the character select..." : "Starting the battle...";
                const float w = g_layer.text(t, true, 9.f, 0, 0, LayerColor{0, 0, 0, 0}, true); g_layer.text(t, true, 9.f, (kScrW - w) / 2.f, 140.f, LayerColor{40, 42, 80, 1.f});
                g_layer.tag("ffa_cover"); g_layer.publish();
            }
            g_script_t.store(st > endt ? -1 : st + 1, std::memory_order_relaxed);
            { static bool logged = false; if (st == 0 && !logged) { logged = true; std::fprintf(stderr, "[INJECT] scripted Party Select / CPU Select pass active (it overrides Cross / Down / Up / Circle of the player's pad; FFA_PSOLD / KEEPPARTY / PREOLD only)\n"); } else if (st > 0) logged = false; }
            if (m) g_inj_why = "scripted Party Select / CPU Select pass (FFA_PSOLD / KEEPPARTY / PREOLD)";
            raw = (raw & ~(BTN_CROSS | BTN_DOWN | BTN_UP | BTN_CIRCLE)) | m;
        }
    }
    if (const int cf = g_col_fix.load(std::memory_order_relaxed); cf && (cf == 1 || dissidia_tagteam_inject_old()) && !std::getenv("PSPRECOMP_TAGTEAM_COLFIX_OFF") && g_pad_polls.load(std::memory_order_relaxed) - g_enter_stamp.load(std::memory_order_relaxed) < 40u) {
        if (cf == 1) raw &= ~(BTN_RIGHT | BTN_LEFTK);
        else { g_inj_why = "Communications setup: Right from a round row (INJECT_OLD)"; static bool inj = false; inj = !inj; raw &= ~(BTN_CROSS | BTN_LEFTK); raw = inj ? (raw | BTN_RIGHT) : (raw & ~BTN_RIGHT); }   // a round row: back to the party column
    }
    dissidia_tagteam_result_tick();   // (a decided Free-for-all: the result screen's rows)
    raw = dissidia_tagteam_result_pad(raw);   // the Free-for-all result screen (own, tagteam_result.cpp)
    if (dissidia_tagteam_result_active()) return raw;
    static std::atomic<int> hold{0};   // a button that closed a dialog stays down for a few polls: the screen under it must not see it
    if (g_dlg_open.load(std::memory_order_relaxed)) { hold.store(12, std::memory_order_relaxed); return 0u; }
    if (const int h = hold.load(std::memory_order_relaxed); h > 0) { hold.store(h - 1, std::memory_order_relaxed); return 0u; }
    if (int ic = g_inject_circle.load(std::memory_order_relaxed); ic > 0) { g_inj_why = "Circle for retail's setup screen (INJECT_OLD)"; g_inject_circle.store(ic - 1, std::memory_order_relaxed); raw = (ic >= 3 && ic <= 5) ? (raw | BTN_CIRCLE) : (raw & ~BTN_CIRCLE); }
    if (int il = g_inject_late.load(std::memory_order_relaxed); il > 0) { g_inj_why = "list confirm Cross after the Mode Select (INJECT_OLD)"; g_inject_late.store(il - 1, std::memory_order_relaxed); raw = (il >= 3 && il <= 5) ? (raw | BTN_CROSS) : (raw & ~BTN_CROSS); }
    return raw;
}

// 26 Nov (user: no button is ever pressed on the player's behalf in the menus): the always-on proof. Any bit this filter adds to the player's pad is logged with its reason
// (nothing is logged by the default paths; every old injection sits behind PSPRECOMP_TAGTEAM_INJECT_OLD=1 and says so here).
void dissidia_tagteam_inject_log(const char* who, uint32_t before, uint32_t after, const char* why) {
    static uint32_t lasts[2] = {0, 0}; uint32_t& last = lasts[who[0] == 't' && who[1] == 'a' ? 0 : 1];
    const uint32_t added = after & ~before;
    if (added != last) { if (added) std::fprintf(stderr, "[INJECT] %s adds pad bits %04X to the player's pad (%04X -> %04X): %s\n", who, added, before, after, why ? why : "unknown"); last = added; }
}
void dissidia_tagteam_inject_why(const char* why) { g_inj_why = why; }
uint32_t dissidia_tagteam_menu_pad(uint32_t raw) {
    g_inj_why = nullptr;
    const uint32_t out = menu_pad_inner(raw);
    dissidia_tagteam_inject_log("tag menu pad filter", raw, out, g_inj_why);
    return out;
}
// The mode select (menu_settings.cpp calls these from the start menu's updates): the screen after choosing Tag Team Battle.
uint32_t dissidia_tagteam_raw_pad() { return g_raw_pad.load(std::memory_order_relaxed); }   // the pad before the dialogs took it
void dissidia_tagteam_modesel_open() { g_ms_first = true; g_dlg_mode = 5; g_dlg_open = true; }
bool dissidia_tagteam_modesel_active() { return g_dlg_open.load() && (g_dlg_mode == 5 || g_ms_pre); }
int dissidia_tagteam_modesel_frame(uint8_t* rdram, recomp_context* ctx, bool online) {
    if (g_ms_pre) {   // the Free-for-all's players screen / lobby, hosted here
        const uint32_t b = g_raw_pad.load(std::memory_order_relaxed), pressed = dir_repeat(b, b & ~g_prev_buttons, g_dlg_open && g_dlg_mode >= 2 && g_dlg_mode <= 4); g_prev_buttons = b;
        g_layer.begin(std::max(2, psp_sddialog::output_scale()));
        const bool was_lobby = g_dlg_mode == 2;
        if (g_dlg_mode == 2) lobby_dialog(rdram, ctx, pressed, 1.f); else ffa_screen(rdram, ctx, pressed);
        if (!ps_old() && (!g_ms_pre || !g_dlg_open)) { if (was_lobby && !g_dlg_open) g_start_cover = true; trip_draw(0.f, false); }   // the choice was made this frame: the screen stays (the trip's fade to white follows)
        else { const float w = trip_in_white(); if (w > 0.f) g_layer.band(0.f, 0.f, kScrW, 272.f, [&](float, float) { return LayerColor{255, 255, 255, w}; }); }
        g_layer.tag(dlg_tag()); g_layer.publish();
        if (g_ms_pre && !g_dlg_open) { g_ms_pre = false; g_ms_done = true; g_dlg_mode = 0; std::fprintf(stderr, "[FFA] Mode Select: players and lobby done, the game's screens are passed by the script\n"); return 1; }
        if (!g_ms_pre && g_ms_hop && !g_dlg_open) return 1;
        return 0;
    }
    const int r = modesel_frame(rdram, ctx, online);
    if (g_ms_pre) return 0;
    if (r) { g_dlg_open = false; g_dlg_mode = 0; }
    return r;
}
// A list screen in the menu style (the Battle Lobby's look) for other modes: Training's options panel (training.cpp). rows = label / value; publishes the game layer.
void dissidia_menu_style_list(const std::string& title, const std::string& sub, const std::vector<std::pair<std::string, std::string>>& rows, int sel, const std::string& help, const std::string& footer) {
    g_layer.begin(std::max(2, psp_sddialog::output_scale()));
    gs_background(); gs_title(title, sub);
    const float x0 = 38.f, x1 = 442.f, y0 = 40.f, h = 16.f;
    for (size_t i = 0; i < rows.size(); i++) gs_row(x0, x1, y0 + h * float(i), h - 1.f, static_cast<int>(i) == sel, rows[i].first, rows[i].second, !rows[i].second.empty(), 6.6f);
    gs_footer(footer, help);
    g_layer.tag("train_options"); g_layer.publish();
}
void dissidia_menu_style_clear() { g_layer.begin(std::max(2, psp_sddialog::output_scale())); g_layer.tag("style_clear"); g_layer.publish(); }
void dissidia_menu_play_se(uint8_t* rdram, recomp_context* ctx, int id) { play_se(rdram, ctx, id); }
void dissidia_tagteam_modesel_draw_frozen(bool online) {
    if (g_trip_out && !dissidia_tagteam_inject_old()) return;   // (trip_tick draws it)
    ge_draw_set_game_layer_ttl(kHoldTtlMs);
    if (g_ms_ffa && !ps_old() && (g_qb_mode == QB_PICK || g_qb_mode == QB_START || g_qb_mode == QB_DETOUR)) return;   // the Free-for-all trips draw their own fade (trip_tick)
    modesel_draw(online);
}   // after a choice: the Mode Select stays on screen, cursor on the chosen row, until the next menu opens (the layer expires 150 ms after the last draw)
// The list row the confirm starts (menu_settings.cpp writes it to the sub-list cursor): a Free-for-all pick opens Quick Battle (1-on-1) (row 0) for its Player Select.
uint32_t dissidia_tagteam_modesel_target(bool sp, uint32_t def) { return (sp && g_ms_ffa && !ps_old() && g_qb_mode == QB_PICK) ? 0u : def; }
bool dissidia_tagteam_inject_old() { static const bool o = std::getenv("PSPRECOMP_TAGTEAM_INJECT_OLD") != nullptr || std::getenv("PSPRECOMP_TAGTEAM_FFA_PSOLD") != nullptr; return o; }   // 26 Nov: the list confirm is called, not pressed
void dissidia_tagteam_modesel_confirm(bool sp) {
    if (dissidia_tagteam_inject_old()) g_inject_late.store(9, std::memory_order_relaxed);
    else { g_trip_out = 2; g_trip_online = !sp; g_trip_t0 = steady_ms(); }   // 26 Nov: the Mode Select fades to white with the list (the list is never seen behind it)
    if (sp && g_ms_ffa && !ps_old() && !std::getenv("PSPRECOMP_TAGTEAM_FFA_KEEPPARTY") && !std::getenv("PSPRECOMP_TAGTEAM_FFA_PREOLD")) {
        if (g_ms_hop) g_qb_mode = QB_PICK; else { g_qb_mode = QB_START; g_ffa_autostart = menuwalk_old(); g_nw_start = !menuwalk_old(); g_nw_armed = false; if (!ffawalk_old()) { g_rearm = false; g_lobby_pending = false; g_dlg_open = false; g_dlg_mode = 0; g_ffa_walk_t0 = steady_ms(); g_ffa_walk_seen = false; ffawalk_log("start", 0); } }
        if (g_qb_mode == QB_PICK) { g_trip_out = 1; g_trip_t0 = steady_ms(); } else { g_trip_out = 0; g_start_cover = true; g_start_exit_t0 = 0; }
        g_trip_in_t0 = 0;
        std::fprintf(stderr, "[FFA] list confirm: %s\n", g_qb_mode == QB_PICK ? "Quick Battle (1-on-1) for the Player Select pick" : "Quick Battle (Party) straight to the setup screen (no Party Select / CPU Select)");
        return;
    }
    if (sp && g_ms_ffa && !std::getenv("PSPRECOMP_TAGTEAM_FFA_KEEPPARTY") && !std::getenv("PSPRECOMP_TAGTEAM_FFA_PREOLD")) {
        if (g_ms_hop) { g_lobby_pending = true; g_script_lim.store(220, std::memory_order_relaxed); }   // the player's own trip to the game's selector (P2..P4: Party Select is passed for him)
        else g_script_lim.store(1000, std::memory_order_relaxed);
        g_ps_frames.store(0, std::memory_order_relaxed);
        if (g_script_lim.load() >= 0) g_script_t.store(0, std::memory_order_relaxed);
    }
}   // the start menu sees a Cross press after the hold
// menu_settings.cpp, when the Battle Mode list comes back after the Quick Battle app: a Free-for-all pick trip ends on the players screen (hosted over the list again), with the pick
// in its field. Returns true when it took over (the Mode Select is not reopened).
bool dissidia_tagteam_ffa_ps_return() {
    if (g_qb_mode != QB_RETURN && g_qb_mode != QB_DETOUR) return false;   // (a detour that ended on the list: no pick)
    if (g_qb_mode == QB_DETOUR) g_ps_pick = 0;
    g_qb_mode = QB_NONE;
    if (g_ps_pick >= 1 && g_ps_pick <= 33 && g_hop_slot >= 0) {
        const int pick = g_ps_pick;
        if (g_hop_slot == 0) { uint8_t p1[2] = {0, 255}; dissidia_tagteam_ffa_p1_get(p1); const int f = g_hop_field - 1; p1[f] = static_cast<uint8_t>(pick); if (f == 0 && p1[1] == pick) p1[1] = 255; if (f == 1 && p1[0] == pick) p1[0] = 0; dissidia_tagteam_ffa_p1_set(p1); }
        else { uint8_t sl[4] = {1, 0, 0, 5}; dissidia_tagteam_ffa_get(g_hop_slot - 1, sl); if (g_hop_field == 1) { sl[0] = 1; sl[1] = static_cast<uint8_t>(pick); if (sl[2] == pick) sl[2] = 0; } else sl[2] = static_cast<uint8_t>(pick); dissidia_tagteam_ffa_set(g_hop_slot - 1, sl); }
        dissidia_tagteam_ffa_look_set(g_hop_slot, g_hop_field, g_ps_look);
    }
    std::fprintf(stderr, "[FFA] back from Player Select: P%d %s = %d (look %d): the players screen again\n", g_hop_slot + 1, g_hop_field == 1 ? "main" : "tag-in", g_ps_pick, g_ps_look);
    g_ms_pre = true; g_ms_hop = false; g_ms_ffa = true; g_ffa_flow = true; g_ffa_then_lobby = true; g_dlg_mode = 3; g_dlg_open = true; g_lob_sp = true; g_dlg_bp = false; g_dlg_sp = false; g_lob_row = 0;
    g_ffa_slot = g_hop_slot >= 0 ? g_hop_slot : 1; g_ffa_field = g_hop_field; g_hop_slot = -1; g_ps_pick = 0; g_prev_buttons = g_raw_pad.load(std::memory_order_relaxed);
    if (!g_trip_in_t0) g_trip_in_t0 = steady_ms();
    g_trip_post = false;
    return true;
}
void dissidia_tagteam_lobby_pending(bool on) {
    if (on && !ps_old() && (g_qb_mode == QB_PICK || g_qb_mode == QB_START || g_qb_mode == QB_DETOUR || g_start_cover)) { g_lobby_pending = false; g_ms_done = false; return; }   // (26 Nov: the FFA trips go on by themselves)
    if (on && g_ms_done) { g_ms_done = false; return; } g_lobby_pending = on;
}   // (a Free-for-all that was set up in the Mode Select goes straight on)

void dissidia_tagteam_menu_install(uint8_t* rdram) {
    g_ram = rdram;
    if (std::getenv("PSPRECOMP_MENU_NOTAGTEAM")) return;
    g_orig_tick = ovl_menu_lookup(SETUP_TICK);
    g_play_se = psp_dispatch_probe_lookup(PLAY_SE);
    {   // every sound request of the game passes here: the SELOG debug log, and the retail result scene's own requests are replaced while our result screen is up (tagteam_result.cpp)
        static FuncPtr o_se = g_play_se, o_req = psp_dispatch_probe_lookup(0x088A24D4u);
        static const bool lg = std::getenv("PSPRECOMP_TAGTEAM_SELOG") != nullptr;
        if (o_req) {
            dissidia_tagteam_result_install(o_req);
            psp_dispatch_register(0x088A24D4u, [](uint8_t* rdram, recomp_context* ctx) {
                const uint32_t ra = static_cast<uint32_t>(ctx->r[31]);
                if (lg) std::fprintf(stderr, "[SELOG] t=%lld request a0=%u a1=%u a2=%u (ra %08X)%s\n", static_cast<long long>(steady_ms()), static_cast<uint32_t>(ctx->r[4]), static_cast<uint32_t>(ctx->r[5]), static_cast<uint32_t>(ctx->r[6]), ra, dissidia_tagteam_result_mute_scene(ra) ? " muted" : "");
                if (dissidia_tagteam_result_mute_scene(ra)) { ctx->r[2] = 0; return; }
                o_req(rdram, ctx);
            });
        }
        (void)o_se;
    }
    if (!g_orig_tick) { std::fprintf(stderr, "[TTCPU] setup screen hook not installed\n"); return; }
    psp_dispatch_register(SETUP_TICK, setup_tick_hook);
    // 27 Nov (user: the retail "Please select a battle format" dialog of the online Group Battle should not show in the tag modes): the host's dialog (Tournament /
    // Round-Robin 3 / 5 Rounds) is answered by its own confirm at its first frame, with Tournament (the only format the tag tally supports). PSPRECOMP_TAGTEAM_FMTDLG_OLD=1: the dialog stays.
    if (!std::getenv("PSPRECOMP_TAGTEAM_FMTDLG_OLD") && (g_orig_fmt = ovl_menu_lookup(FMT_DLG))) psp_dispatch_register(FMT_DLG, fmt_dlg_hook);
    // Proper PvE (docs/tag-team.md): Party Select accepts an empty CPU party; Enter Battle's table step builds the CPU party's blocks on its owner's machine
    g_orig_ready = ovl_menu_lookup(READY_TEST); g_orig_enter = ovl_menu_lookup(ENTER_STEP);
    if (g_orig_ready) psp_dispatch_register(READY_TEST, ready_hook);
    if (g_orig_enter) psp_dispatch_register(ENTER_STEP, enter_hook);
    // single player Tournament screen: the round dialog of matchups 2.. (tag mode) and the CPU rows (docs/tag-team.md "Single player: CPU rows"), one tick hook
    if (!std::getenv("PSPRECOMP_TAGTEAM_FFA_PSOLD") && (g_orig_qb_frame = ovl_menu_lookup(QB_APP_FRAME))) { psp_dispatch_register(QB_APP_FRAME, qb_frame_hook); if ((g_orig_sel_tick = ovl_menu_lookup(QB_SEL_TICK))) psp_dispatch_register(QB_SEL_TICK, sel_tick_hook);
        if ((g_orig_net_poll = ovl_menu_lookup(QB_NET_POLL))) psp_dispatch_register(QB_NET_POLL, net_poll_hook);
}
    g_orig_sp_tick = ovl_menu_lookup(SP_TICK); g_orig_sp_dlg = ovl_menu_lookup(SP_DLG);
    if (g_orig_sp_tick) psp_dispatch_register(SP_TICK, sp_tick_hook);
    else std::fprintf(stderr, "[TTSP] Tournament screen hook not installed\n");
    // 25 Nov (user, RP6: "hovering Enter Battle shows no stage preview"): FUN_08AA9350 picks the preview's texture: the stage of the cursor row's round block for rows 0-4, the "?" picture
    // (index + 35) for the Enter Battle row (row 5). In tag mode the row is shown as the 5th round row for that call (every block holds the lobby's stage), then put back.
    // PSPRECOMP_TAGTEAM_ENTERPREVIEW_OFF=1: old.
    if (!std::getenv("PSPRECOMP_TAGTEAM_ENTERPREVIEW_OFF") && !std::getenv("PSPRECOMP_TAGTEAM_NOSPCPU") && ovl_menu_lookup(0x08AA9350u)) {
        g_orig_preview = ovl_menu_lookup(0x08AA9350u);
        psp_dispatch_register(0x08AA9350u, [](uint8_t* rdram, recomp_context* ctx) {
            if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(0x08AA9350u)) f(rdram, ctx); return; }
            const uint32_t M = r32(static_cast<uint32_t>(ctx->r[4]) + 31996u);
            const bool fake = dissidia_tagteam_sp_mode() && M >= 0x08800000u && M < 0x0A000000u && r32(M + 1444u) == 5u;
            if (fake) w32(M + 1444u, 4u);
            g_orig_preview(rdram, ctx);
            if (fake) w32(M + 1444u, 5u);
        });
    }
    if (g_orig_sp_dlg && !std::getenv("PSPRECOMP_TAGTEAM_SPDLG_OFF"))
        psp_dispatch_register(SP_DLG, [](uint8_t* rdram, recomp_context* ctx) { if (r32(OVL_BASE) != 2) { if (FuncPtr f = ovl_battle_lookup(SP_DLG)) f(rdram, ctx); return; } sp_dlg_hook(rdram, ctx); });
}
