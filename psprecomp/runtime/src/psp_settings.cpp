// psp_settings.cpp — in-game settings menu. See include/psp_settings.h.
#include "psp_settings.h"
#include "psp_touch.h"
#include "psp_platform.h"
#include "psp_ge_vertex.h"
#include "psp_runtime.h"
#include "psp_event_loop.h"
#include "psp_ge_draw.h"
#include "psp_post.h"
#include "psp_screenfx.h"
#include "psp_rt.h"
#include "psp_ge_texture.h"
#include "hle/psp_savedata_dialog.h"
#include "hle/psp_hle_adhoc.h"
#include "hle/psp_cloud_service.h"
#include "psp_gpu_driver.h"

#include <SDL.h>
#if defined(PSPRECOMP_HAVE_VKGL)
#include "vk/vkgl.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---- saved settings --------------------------------------------------------
std::string settings_path() {
    const char* p = std::getenv("PSPRECOMP_SETTINGS");
    return (p && p[0]) ? p : "settings.ini";
}
std::string mods_dir_path() {
    if (const char* m = std::getenv("PSPRECOMP_MODS")) if (m[0]) return m;
    const std::string sp = settings_path();
    const size_t sl = sp.find_last_of("/\\");
    return (sl == std::string::npos ? std::string(".") : sp.substr(0, sl)) + "/mods";
}
std::map<std::string, std::string> g_saved;   // what settings.ini holds (keys the user changed in the menu)

void read_file() {
    g_saved.clear();
    std::ifstream f(settings_path());
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        g_saved[k] = v;
    }
}
void write_file(const char* key, const std::string& value);
void save(const char* key, const std::string& value) {
    g_saved[key] = value;
    write_file(key, value);
}
void unsave(const char* key) { g_saved.erase(key); }   // dropped at the next save()
void write_file(const char* key, const std::string& value) {
    const std::string path = settings_path();
    std::ofstream f(path, std::ios::trunc);
    if (!f) {   // e.g. a file that `adb push` made read-only for the app (owner shell, mode 644): replace it with one the app owns
        f.clear();
        if (std::remove(path.c_str()) == 0) f.open(path, std::ios::trunc);
    }
    if (!f) { std::fprintf(stderr, "[SETTINGS] cannot write %s\n", path.c_str()); return; }
    f << "# Written by the in-game settings menu. Delete a line (or the file) to go back to the launch options.\n";
    for (const auto& kv : g_saved) f << kv.first << '=' << kv.second << '\n';
    std::fprintf(stderr, "[SETTINGS] saved %s=%s to %s\n", key, value.c_str(), path.c_str());
}

// ---- state -----------------------------------------------------------------
std::atomic<bool> g_open{false};
std::atomic<int> g_parked{0};          // guest threads waiting in psp_settings_guest_wait
bool g_enabled = true;
bool g_wait_release = false;           // after closing: game input stays blocked until all released
bool g_confirm_cross = true;
PspFrameRateHook g_fps{nullptr, 0, nullptr, nullptr};
PspVoiceHook g_voice{nullptr};
std::vector<PspDebugToggle> g_dbg_toggles;   // game-supplied Debug page rows
PspTexPackHook g_texpack{nullptr};

// Graphics API (OpenGL / Vulkan). Chosen at start-up (the window is created for one or the other), so a change applies at the next start.
bool renderer_vulkan_at_start() {
#if defined(PSPRECOMP_HAVE_VKGL)
    return vkgl::active();
#else
    return false;
#endif
}
// Whether Vulkan can be chosen at all; *why says why not (shown in the row's description).
bool renderer_vulkan_available(std::string* why) {
#if defined(PSPRECOMP_HAVE_VKGL)
    return vkgl::available(why);
#else
    if (why) *why = "This build of the game was made without the Vulkan backend (the Vulkan headers or glslang were missing when it was compiled).";
    return false;
#endif
}
bool renderer_vulkan_choice() {
    if (!renderer_vulkan_available(nullptr)) return false;
    auto it = g_saved.find("renderer");
    return it != g_saved.end() ? it->second == "vulkan" : renderer_vulkan_at_start();
}

// Custom GPU drivers (psp_gpu_driver.h): the driver in use, a live list to download more from, and delete. Applied at the next start.
std::string gpu_choice() {
    auto it = g_saved.find("gpu_driver");
    return it != g_saved.end() ? it->second : psp_gpu::selected();
}
int g_gpu_get_idx = 0;                 // the entry of the downloadable list shown by the Download row
std::string g_gpu_installing;          // id being downloaded: selected for the user when it lands
unsigned g_gpu_rev = 0;
bool g_gpu_list_asked = false;
// The choices of the GPU Driver row: the system driver, each installed one, and the saved one when it is no longer installed.
std::vector<psp_gpu::Installed> g_gpu_inst;
std::vector<std::string> gpu_ids(const std::string& cur) {
    g_gpu_inst = psp_gpu::installed();
    std::vector<std::string> ids{""};
    for (auto& in : g_gpu_inst) ids.push_back(in.id);
    if (!cur.empty() && std::find(ids.begin(), ids.end(), cur) == ids.end()) ids.push_back(cur);
    return ids;
}
std::string gpu_trim(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }
void gpu_choose(const std::string& id) {
    save("gpu_driver", id);
    psp_gpu::clear_crash(id);          // picking a driver again retries one that crashed
}
bool renderer_row_wanted() { return true; }   // always listed; greyed out with a reason when Vulkan is unavailable
bool voice_jp_choice();
bool voice_at_start();
bool updates_choice();
bool texpack_choice();
bool texpack_at_start();

enum Row { ROW_DISPLAY, ROW_RES, ROW_SCALE, ROW_FPS, ROW_FPSCOUNT, ROW_WIDE, ROW_SOFT, ROW_RENDERER, ROW_AA, ROW_SHARP, ROW_BRIGHT, ROW_CONTRAST, ROW_SAT, ROW_HDR, ROW_HDR_BOOST, ROW_HDR_GAMUT, ROW_HDR_PEAK, ROW_HDR_PAPER, ROW_HDR_SCENE, ROW_SSAO, ROW_CONTACT, ROW_RT, ROW_RT_SOFT, ROW_RT_REFL, ROW_SSR, ROW_ANISO, ROW_UPSCALE, ROW_VOICE, ROW_TEXPACK, ROW_TOUCH, ROW_TOUCH_EDIT, ROW_UPDATES, ROW_QUIT,
           ROW_NET_MODE, ROW_NET_SERVER, ROW_NET_ROOM, ROW_NET_PEERS, ROW_NET_STATUS,   // Network page
           ROW_CLOUD_PROVIDER, ROW_CLOUD_FOLDER, ROW_CLOUD_URL, ROW_CLOUD_USER, ROW_CLOUD_PASS, ROW_CLOUD_GOOGLE,
           ROW_CLOUD_AUTO, ROW_CLOUD_REPLAYS, ROW_CLOUD_NOW, ROW_CLOUD_STATUS,   // Cloud page
           ROW_PERF, ROW_FX, ROW_SYNC, ROW_RING, ROW_GLOWSOFT, ROW_GLOWPASS,   // Debug page
           ROW_DBG0, ROW_DBG1, ROW_DBG2, ROW_DBG3, ROW_DBG4, ROW_DBG5,   // Debug page rows a game module adds (psp_settings_add_debug_toggle)
           ROW_GPU, ROW_GPU_GET, ROW_GPU_DEL,   // custom GPU drivers (Android, Adreno): Drivers page
           ROW_SUB_HDR, ROW_SUB_RT,             // rows that open a sub-menu
           ROW_SUB_ENV0, ROW_SUB_ENV_END = ROW_SUB_ENV0 + 15,   // Debug page: one row per developer-options category (psp_env_options.inc)
           ROW_ENV0, ROW_ENV_END = ROW_ENV0 + 63,               // a developer-options category page: one row per environment variable
           ROW_MOD0, ROW_MOD_END = ROW_MOD0 + 31, ROW_MODS_NONE,   // Mods page: one toggle per mod folder, or a note when there are none
           ROW_SEC_WINDOW, ROW_SEC_PERF, ROW_SEC_QUALITY, ROW_SEC_AA, ROW_SEC_TEX, ROW_SEC_ENGINE, ROW_SEC_COLOUR, ROW_SEC_HDR, ROW_SEC_LIGHT,
           ROW_SEC_RT, ROW_SEC_TOUCH, ROW_SEC_GAME, ROW_SEC_SESSION, ROW_SEC_REFL, ROW_SEC_SERVER, ROW_SEC_SYNC, ROW_SEC_DRIVER, ROW_SEC_MODS, ROW_SEC_DEV,   // section captions (not selectable)
           ROW_COUNT };
static_assert(ROW_COUNT < psp_sddialog::SET_MAX_IDS, "row ids are animation slots");
bool is_section(Row r) { return r >= ROW_SEC_WINDOW && r < ROW_COUNT; }
std::vector<Row> g_rows;
int g_sel = 0;
// Pages: the categories shown in the sidebar, and the sub-menus a row can open (PAGE_HDR under Image, PAGE_RT under Effects).
enum Page { PAGE_DISPLAY, PAGE_GRAPHICS, PAGE_IMAGE, PAGE_EFFECTS, PAGE_CONTROLS, PAGE_NETWORK, PAGE_CLOUD, PAGE_SYSTEM, PAGE_GPU, PAGE_DEBUG, PAGE_MODS,
            PAGE_HDR, PAGE_RT, PAGE_ENV0, PAGE_ENV_END = PAGE_ENV0 + 10, PAGES };
constexpr int k_env_cats = 11;   // = EC_COUNT (psp_env_options.inc)
const char* const k_page_names[PAGES] = {"Display", "Graphics", "Image", "Effects", "Controls", "Network", "Cloud", "System", "Drivers", "Debug", "Mods",
                                         "HDR", "Ray Tracing",
                                         "Camera", "Input", "Timing", "Graphics", "Effects tuning", "Vulkan", "Textures", "Audio / video", "Game data", "Game logs", "Engine logs"};
int g_page = 0;                        // the category (an index into page_list()): L / R, Q / E or Tab, the sidebar
int g_sub = -1;                        // the open sub-menu page, -1 = none
int g_sel_page[PAGES] = {};
bool g_touch_ui = false;               // a touch screen is present (or the on-screen controls are forced on): the Controls tab is shown
// The categories this device shows: Drivers (custom GPU drivers) only where they can be used (an Adreno phone with the Android build),
// Controls only with a touch screen.
std::vector<int> g_pages;
const std::vector<int>& page_list() {
    if (g_pages.empty()) {
        g_pages = {PAGE_DISPLAY, PAGE_GRAPHICS, PAGE_IMAGE, PAGE_EFFECTS};
        if (g_touch_ui) g_pages.push_back(PAGE_CONTROLS);
        g_pages.push_back(PAGE_NETWORK); g_pages.push_back(PAGE_CLOUD); g_pages.push_back(PAGE_MODS); g_pages.push_back(PAGE_SYSTEM);
        if (psp_gpu::supported(nullptr)) g_pages.push_back(PAGE_GPU);
        g_pages.push_back(PAGE_DEBUG);
    }
    return g_pages;
}
int page_id() { return g_sub >= 0 ? g_sub : page_list()[size_t(g_page)]; }
// Sidebar icon of a page (settings_render.inc draw_icon).
int page_icon(int page) { return page <= PAGE_MODS ? page : (page == PAGE_HDR ? PAGE_IMAGE : page >= PAGE_ENV0 ? PAGE_DEBUG : PAGE_EFFECTS); }
int g_ring_next = -1;                  // vertex upload mode saved for the next launch (-1 = unchanged)
bool g_quit_armed = false;
bool g_dirty = true;
int g_drawn_scale = 0;
int64_t g_last_present = 0;
int g_ui_cap = 3;                      // the highest canvas scale the animated screen is drawn at (lowered when drawing is slow)
double g_ui_ms = 5.0;
int g_ui_interval = 15;                // ms between redraws: a slow CPU gets a lower frame rate before it gets a blurrier picture
int g_ui_calm = 0, g_ui_up = 0;
int g_pending_rs = 0;                  // render scale waiting for the game to park
bool g_pending_scene = false;         // HDR Scene toggled: reformat the scene targets once the game has parked
int g_pending_wide = -1;               // widescreen (0 / 1) waiting for the game to park, -1 = none

// Display: fullscreen or a window. Window sizes: k x 480x272, or the Steam Deck's 1280 x 800 (k = K_DECK; the
// picture is letterboxed as on the Deck), or a size set by hand (k = -1, listed so the row can show it).
struct ResOpt { int k; int w, h; };
constexpr int K_DECK = 100, DECK_W = 1280, DECK_H = 800;
std::vector<ResOpt> g_res;
int g_win_sel = -1;                    // the window size used in Window mode (index into g_res)

// Navigation input (menu only).
uint32_t g_dir_held = 0;               // 1 up, 2 down, 4 left, 8 right (pad d-pad / left stick)
int64_t g_dir_next = 0;
int16_t g_lx = 0, g_ly = 0;
bool g_l3 = false, g_r3 = false;
uint32_t g_pad_dpad = 0;

SDL_Window* win() { return psp_get_sdl_window(); }

void build_res_options() {
    g_res.clear();
    SDL_Window* w = win();
    SDL_Rect r{0, 0, 0, 0};
    const int disp = w ? SDL_GetWindowDisplayIndex(w) : 0;
    if (SDL_GetDisplayUsableBounds(disp < 0 ? 0 : disp, &r) != 0 || r.w <= 0) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(disp < 0 ? 0 : disp, &dm) == 0) { r.w = dm.w; r.h = dm.h; }
    }
    int kmax = r.w > 0 ? std::min(r.w / 480, (r.h - 40) / 272) : 2;
    kmax = std::max(1, std::min(kmax, 8));
    for (int k = 1; k <= kmax; ++k) g_res.push_back({k, 480 * k, 272 * k});
    // The Steam Deck's screen as a window (to test the Deck's layout on a desktop), placed by width.
    if (r.w >= DECK_W && r.h - 40 >= DECK_H) {
        auto at = std::find_if(g_res.begin(), g_res.end(), [](const ResOpt& o) { return o.w > DECK_W; });
        g_res.insert(at, {K_DECK, DECK_W, DECK_H});
    }
}
bool is_fullscreen() {
    SDL_Window* w = win();
    return w && (SDL_GetWindowFlags(w) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}
int res_index(int ww, int wh) {   // the option of that size, added as a hand-set size if there is none
    for (size_t i = 0; i < g_res.size(); ++i)
        if (g_res[i].w == ww && g_res[i].h == wh) return int(i);
    g_res.push_back({-1, ww, wh});
    return int(g_res.size()) - 1;
}
// The window size row's choice: the window itself when windowed; in fullscreen the size last used, else the saved
// one, else the largest whole multiple that fits.
void pick_window_size() {
    SDL_Window* w = win();
    if (w && !is_fullscreen()) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        g_win_sel = res_index(ww, wh);
        return;
    }
    if (g_win_sel >= 0 && g_win_sel < int(g_res.size())) {   // keep the earlier choice (the list may have been rebuilt)
        return;
    }
    int ww = 0, wh = 0;
    auto sz = g_saved.find("window_size");
    auto k = g_saved.find("window_scale");
    if (sz != g_saved.end() && std::sscanf(sz->second.c_str(), "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
        g_win_sel = res_index(ww, wh);
    } else if (k != g_saved.end() && std::atoi(k->second.c_str()) > 0) {
        const int n = std::atoi(k->second.c_str());
        g_win_sel = res_index(480 * n, 272 * n);
    } else {
        g_win_sel = 0;
        for (size_t i = 0; i < g_res.size(); ++i) if (g_res[i].k > 0 && g_res[i].k != K_DECK) g_win_sel = int(i);
    }
}

int fps_index() {
    if (!g_fps.get) return 0;
    const int f = g_fps.get();
    for (int i = 0; i < g_fps.count; ++i) if (g_fps.rates[i] == f) return i;
    return 0;
}
constexpr int k_max_rs = 16;   // 16x = 7680x4352 (8K); the draw layer clamps to the GPU's max texture size

// ---- Network page --------------------------------------------------------------------------------------
constexpr const char* NET_DEFAULT_SERVER = "party.schindlershadow.com:27500";
bool g_net_internet = true;
std::string g_net_server = NET_DEFAULT_SERVER, g_net_room, g_net_peers, g_net_status;
bool g_net_loaded = false;
int64_t g_status_next = 0;

// Text entry for the server / room / peers rows and the cloud fields: the host keyboard types directly; the on-screen keyboard (drawn by
// the settings view) is driven by pad, mouse or touch.
struct Edit {
    bool active = false;
    int field = 0;          // 0 server, 1 room, 2 peers, 3 sync folder, 4 WebDAV address, 5 WebDAV user, 6 WebDAV password
    std::string buf;
    int row = 1, col = 0;   // the focused key of the on-screen keyboard
    bool shift = false, symbols = false;
} g_edit;
const char* edit_label() {
    static const char* const n[] = {"Server", "Room", "Direct Peers", "Sync Folder", "WebDAV Address", "WebDAV User", "WebDAV Password"};
    return n[g_edit.field];
}
size_t edit_max() {
    static const size_t m[] = {63, 31, 127, 255, 255, 63, 63};
    return m[g_edit.field];
}

// ---- Cloud page: the settings live in the cloud service (cloud.ini); this is the page's working copy ----
psp_cloud_svc::Config g_cloud;
unsigned g_cloud_rev = 0;
void cloud_load() { g_cloud = psp_cloud_svc::config(); }
void cloud_apply() { if (psp_cloud_svc::initialised()) psp_cloud_svc::set_config(g_cloud); }
std::string tail_fit(const std::string& s, size_t n) { return s.size() <= n ? s : "..." + s.substr(s.size() - (n - 3)); }

void net_load() {
    if (g_net_loaded) return;
    g_net_loaded = true;
    const char* m = std::getenv("PSPRECOMP_ADHOC_MODE");
    const char* sv = std::getenv("PSPRECOMP_ADHOC_SERVER");
    const char* rm = std::getenv("PSPRECOMP_ADHOC_ROOM");
    const char* pe = std::getenv("PSPRECOMP_ADHOC_PEERS");
    if (m && *m) g_net_internet = !(m[0] == 'l' || m[0] == 'L');
    else if (pe && *pe) g_net_internet = false;
    if (sv && *sv) g_net_server = sv;
    if (rm) g_net_room = rm;
    if (pe) g_net_peers = pe;
}
void net_apply() {   // use and remember the Network page's values
    psp_adhoc_configure(g_net_internet, g_net_server.c_str(), g_net_room.c_str(), g_net_peers.c_str());
    save("net_mode", g_net_internet ? "internet" : "lan");
    save("net_server", g_net_server);
    save("net_room", g_net_room);
    save("net_peers", g_net_peers);
    g_status_next = 0;
}
std::string& edit_target() {
    switch (g_edit.field) {
    case 0: return g_net_server;
    case 1: return g_net_room;
    case 2: return g_net_peers;
    case 3: return g_cloud.folder;
    case 4: return g_cloud.webdav_url;
    case 5: return g_cloud.webdav_user;
    default: return g_cloud.webdav_pass;
    }
}
void edit_begin(int field) {
    g_edit = Edit{};
    g_edit.active = true;
    g_edit.field = field;
    g_edit.buf = edit_target();
    SDL_StartTextInput();
    g_dirty = true;
}
void edit_end() { g_edit.active = false; SDL_StopTextInput(); g_dirty = true; }
void edit_commit() {
    std::string v = g_edit.buf;
    while (!v.empty() && v.back() == ' ') v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    if (g_edit.field == 0 && v.empty()) v = NET_DEFAULT_SERVER;
    edit_target() = v;
    edit_end();
    if (g_edit.field >= 3) cloud_apply();
    else net_apply();
}
void edit_append(char c) {
    if (c < 0x20 || c > 0x7e || g_edit.buf.size() >= edit_max()) return;
    g_edit.buf += c;
    g_dirty = true;
}
void edit_backspace() { if (!g_edit.buf.empty()) { g_edit.buf.pop_back(); g_dirty = true; } }
// The on-screen keyboard's key `row`, `col` under the current layer (shift / symbols).
void edit_clamp_focus() {
    const auto keys = psp_sddialog::settings_keyboard(g_edit.shift, g_edit.symbols);
    g_edit.row = std::clamp(g_edit.row, 0, int(keys.size()) - 1);
    g_edit.col = std::clamp(g_edit.col, 0, int(keys[size_t(g_edit.row)].size()) - 1);
}
void edit_press(const psp_sddialog::SetKey& k) {
    switch (k.kind) {
    case 0: case 3: edit_append(k.ch); break;
    case 1: if (g_edit.symbols) g_edit.symbols = false; else g_edit.shift = !g_edit.shift; break;   // (in the symbol layer this key is "abc")
    case 2: g_edit.symbols = !g_edit.symbols; g_edit.shift = false; break;
    case 4: edit_backspace(); break;
    case 5: edit_commit(); return;
    case 6: edit_end(); return;
    default: break;
    }
    edit_clamp_focus();
    g_dirty = true;
}
void edit_press_focus() {
    const auto keys = psp_sddialog::settings_keyboard(g_edit.shift, g_edit.symbols);
    edit_clamp_focus();
    edit_press(keys[size_t(g_edit.row)][size_t(g_edit.col)]);
}
// Moves the key focus; vertical moves keep the x position (the bottom row's keys are wider than the others').
void edit_move(int dr, int dc, const psp_sddialog::SetGeom* geom) {
    const auto keys = psp_sddialog::settings_keyboard(g_edit.shift, g_edit.symbols);
    edit_clamp_focus();
    if (dc) {
        const int n = int(keys[size_t(g_edit.row)].size());
        g_edit.col = (g_edit.col + dc + n) % n;
    }
    if (dr) {
        const int nr = int(keys.size());
        float cx = -1.f;
        if (geom && int(geom->keys.size()) == nr) cx = geom->keys[size_t(g_edit.row)][size_t(g_edit.col)].r.x + geom->keys[size_t(g_edit.row)][size_t(g_edit.col)].r.w / 2.f;
        g_edit.row = (g_edit.row + dr + nr) % nr;
        const int n = int(keys[size_t(g_edit.row)].size());
        if (cx >= 0.f && geom) {
            float best = 1e9f;
            for (int i = 0; i < n; ++i) {
                const auto& r = geom->keys[size_t(g_edit.row)][size_t(i)].r;
                const float d = std::fabs(r.x + r.w / 2.f - cx);
                if (d < best) { best = d; g_edit.col = i; }
            }
        } else g_edit.col = std::min(g_edit.col, n - 1);
    }
    g_dirty = true;
}

// ---- Mods: one toggle per mod folder -----------------------------------------------------------------------------------
// A mod folder has a mod.json; "enabled": false in it keeps the mod out of the game. Folders are looked for in the mods source
// (PSPRECOMP_MODS_SRC, set by run.sh: mods/ in the repository) and in the game's mods directory (PSPRECOMP_MODS, else <work>/mods).
// The mods are built into the game when it starts, so a change applies at the next start.
struct ModEntry { std::string dir, name, where; bool on = true; int roster_line = -1; };   // roster_line >= 0: a character line of <mods>/roster.txt (dir = that file)
std::vector<ModEntry> g_mods;
bool g_restart_pending = false;        // something was changed that only applies at the next start
std::string read_text_file(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
size_t json_key_value(const std::string& js, const char* key, size_t from = 0) {   // index of the value after "key":, or npos
    const std::string k = std::string("\"") + key + "\"";
    size_t at = js.find(k, from);
    if (at == std::string::npos) return at;
    at = js.find(':', at + k.size());
    if (at == std::string::npos) return at;
    ++at;
    while (at < js.size() && (js[at] == ' ' || js[at] == '\t' || js[at] == '\n' || js[at] == '\r')) ++at;
    return at;
}
bool mod_json_enabled(const std::string& js) { const size_t v = json_key_value(js, "enabled"); return !(v != std::string::npos && js.compare(v, 5, "false") == 0); }
std::string mod_json_display(const std::string& js) {
    size_t v = json_key_value(js, "display");
    if (v == std::string::npos) v = json_key_value(js, "name");
    if (v == std::string::npos || v >= js.size() || js[v] != '"') return {};
    const size_t e = js.find('"', v + 1);
    return e == std::string::npos ? std::string() : js.substr(v + 1, e - v - 1);
}
void mods_scan(const std::string& root, const char* where) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return;
    std::vector<std::string> names;
    for (const auto& de : fs::directory_iterator(root, ec)) {
        const std::string n = de.path().filename().string();
        if (n.empty() || n[0] == '_' || n[0] == '.') continue;
        if (de.is_directory(ec) && fs::exists(de.path() / "mod.json", ec)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        if (g_mods.size() >= size_t(ROW_MOD_END - ROW_MOD0 + 1)) return;
        ModEntry m;
        m.dir = root + "/" + n;
        const std::string js = read_text_file(m.dir + "/mod.json");
        m.on = mod_json_enabled(js);
        const std::string disp = mod_json_display(js);
        m.name = disp.empty() ? n : disp;
        m.where = where;
        g_mods.push_back(std::move(m));
    }
}
// Extra characters installed into the mods folder (e.g. Playable Chaos) are lines of roster.txt; a line starting "#off# " is switched off.
void mods_scan_roster(const std::string& root) {
    std::ifstream f(root + "/roster.txt");
    std::string line;
    for (int n = 0; std::getline(f, line); ++n) {
        if (g_mods.size() >= size_t(ROW_MOD_END - ROW_MOD0 + 1)) return;
        bool on = true;
        std::string spec = line;
        if (spec.compare(0, 6, "#off# ") == 0) { on = false; spec = spec.substr(6); }
        else if (spec.empty() || spec[0] == '#') continue;
        std::istringstream is(spec);
        std::string slot, code, tok, nm;
        if (!(is >> slot >> code) || slot.find_first_not_of("0123456789") != std::string::npos) continue;
        while (is >> tok) if (tok.compare(0, 3, "nm=") == 0) nm = tok.substr(3);
        for (char& ch : nm) if (ch == '_') ch = ' ';
        ModEntry m;
        m.dir = root + "/roster.txt"; m.roster_line = n; m.on = on;
        m.name = nm.empty() ? code : nm;
        m.where = "character";
        g_mods.push_back(std::move(m));
    }
}
void mods_load() {
    g_mods.clear();
    if (const char* src = std::getenv("PSPRECOMP_MODS_SRC")) mods_scan(src, "mods");
    mods_scan(mods_dir_path(), "the game's mods folder");
    mods_scan_roster(mods_dir_path());
}
bool mod_set_enabled(size_t i, bool on) {
    if (i >= g_mods.size()) return false;
    if (g_mods[i].roster_line >= 0) {   // a roster.txt line: comment it out / back in
        std::vector<std::string> lines;
        { std::ifstream f(g_mods[i].dir); std::string l; while (std::getline(f, l)) lines.push_back(l); }
        std::string& l = lines[size_t(g_mods[i].roster_line) < lines.size() ? size_t(g_mods[i].roster_line) : 0];
        if (size_t(g_mods[i].roster_line) >= lines.size()) return false;
        const bool off = l.compare(0, 6, "#off# ") == 0;
        if (on && off) l = l.substr(6);
        else if (!on && !off) l = "#off# " + l;
        std::ofstream o(g_mods[i].dir, std::ios::trunc);
        if (!o) return false;
        for (const std::string& x : lines) o << x << "\n";
        g_mods[i].on = on;
        g_restart_pending = true;
        std::fprintf(stderr, "[SETTINGS] character %s %s (next start)\n", g_mods[i].name.c_str(), on ? "enabled" : "disabled");
        return true;
    }
    const std::string p = g_mods[i].dir + "/mod.json";
    std::string js = read_text_file(p);
    if (js.empty()) return false;
    const size_t v = json_key_value(js, "enabled");
    if (v != std::string::npos && (js.compare(v, 4, "true") == 0 || js.compare(v, 5, "false") == 0)) js.replace(v, js[v] == 't' ? 4 : 5, on ? "true" : "false");
    else { const size_t b = js.find('{'); if (b == std::string::npos) return false; js.insert(b + 1, std::string("\n  \"enabled\": ") + (on ? "true" : "false") + (js.find('"', b) != std::string::npos ? "," : "")); }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << js;
    g_mods[i].on = on;
    g_restart_pending = true;
    std::fprintf(stderr, "[SETTINGS] mod %s %s (next start)\n", g_mods[i].dir.c_str(), on ? "enabled" : "disabled");
    return true;
}
bool is_mod_row(Row r) { return r >= ROW_MOD0 && r <= ROW_MOD_END; }

// ---- Debug > developer options: one row per environment variable (psp_env_options.inc) ----
#include "psp_env_options.inc"
static_assert(EC_COUNT == k_env_cats, "page ids for the developer-options categories");
std::string env_key(const EnvOpt& o) { return std::string("env.") + o.name; }
// The value the variable has / will have: what the menu saved, else what the environment held when the game started.
std::string env_current(const EnvOpt& o) {
    auto it = g_saved.find(env_key(o));
    if (it != g_saved.end()) return it->second;
    const char* e = std::getenv((std::string("PSPRECOMP_") + o.name).c_str());
    return e ? e : "";
}
bool env_is_on(const EnvOpt& o) {
    const std::string v = env_current(o);
    return o.type == 'k' ? v != "0" : (!v.empty() && v != "0");
}
std::vector<std::string> env_presets(const EnvOpt& o) {   // "Default" first (stored as the empty string)
    std::vector<std::string> out{""};
    std::string cur;
    for (const char* c = o.values; ; ++c) {
        if (*c == '|' || !*c) { if (!cur.empty()) out.push_back(cur); cur.clear(); if (!*c) break; }
        else cur += *c;
    }
    return out;
}
std::vector<const EnvOpt*> env_cat_opts(int cat) {
    std::vector<const EnvOpt*> v;
    for (int i = 0; i < k_env_opt_count; ++i) if (k_env_opts[i].cat == cat) v.push_back(&k_env_opts[i]);
    return v;
}
const EnvOpt* env_row_opt(Row r) {   // the variable a row of an open developer-options page stands for
    const int page = page_id();
    if (r < ROW_ENV0 || r > ROW_ENV_END || page < PAGE_ENV0 || page > PAGE_ENV_END) return nullptr;
    const auto v = env_cat_opts(page - PAGE_ENV0);
    const size_t n = size_t(r - ROW_ENV0);
    return n < v.size() ? v[n] : nullptr;
}
void env_set(const EnvOpt& o, const std::string& v) { save(env_key(o).c_str(), v); }
// Left / right (d = +1 / -1) or confirm (wrap = true) on a developer-options row.
void env_change(const EnvOpt& o, int d, bool wrap) {
    if (o.type == 'c') {
        const auto pr = env_presets(o);
        const std::string cur = env_current(o);
        int i = 0;
        for (size_t k = 0; k < pr.size(); ++k) if (pr[k] == cur) i = int(k);
        int n = i + (d > 0 ? 1 : -1);
        if (wrap) n = (n % int(pr.size()) + int(pr.size())) % int(pr.size());
        n = std::clamp(n, 0, int(pr.size()) - 1);
        env_set(o, pr[size_t(n)]);
    } else if (o.type == 'k') env_set(o, d > 0 ? "" : "0");   // on = unset, off = 0
    else env_set(o, d > 0 ? "1" : "");
}

std::vector<psp_sddialog::MenuRow> make_rows() {
    std::vector<psp_sddialog::MenuRow> out;
    char buf[96];
    for (Row r : g_rows) {
        psp_sddialog::MenuRow m;
        if (is_mod_row(r)) {
            const size_t i = size_t(r - ROW_MOD0);
            if (i < g_mods.size()) {
                m.label = g_mods[i].name;
                m.value = g_mods[i].on ? "On" : "Off";
                m.help = g_mods[i].name + (g_mods[i].roster_line >= 0 ? "\nTurn this extra character on or off (its line in roster.txt). Its files stay in the mods folder. Takes effect the next time the game starts."
                                                               : "\nTurn this mod on or off. Folder: " + g_mods[i].dir + ". Takes effect the next time the game starts.");
            }
            out.push_back(std::move(m));
            continue;
        }
        if (const EnvOpt* o = env_row_opt(r)) {
            m.label = o->name;
            if (o->type == 'c') {
                const std::string cur = env_current(*o);
                m.value = cur.empty() ? "Default" : cur;
                const auto pr = env_presets(*o);
                m.can_dec = m.can_inc = pr.size() > 1;
            } else {
                const bool on = env_is_on(*o);
                m.value = on ? "On" : "Off";
                m.can_dec = on; m.can_inc = !on;
            }
            m.help = std::string("PSPRECOMP_") + o->name + "\n" + o->desc + (o->type == 'e' ? " Off by default." : o->type == 'k' ? " On by default." : " Default leaves it to the game.") +
                     " Takes effect the next time the game starts. Saved.";
            out.push_back(std::move(m));
            continue;
        }
        if (r >= ROW_SUB_ENV0 && r <= ROW_SUB_ENV_END) {
            const int cat = int(r - ROW_SUB_ENV0);
            if (cat < EC_COUNT) {
                int on = 0, n = 0;
                for (const EnvOpt* o : env_cat_opts(cat)) { ++n; const std::string cur = env_current(*o); if (o->type == 'k' ? cur == "0" : (!cur.empty() && cur != "0")) ++on; }
                m.label = k_env_cat_names[cat];
                m.value = on ? std::to_string(on) + " changed" : std::to_string(n) + " options";
                m.help = std::string(k_env_cat_names[cat]) + "\n" + k_env_cat_help[cat] + " One row for each environment variable; the choice is saved and applied the next time the game starts. Press Cross to open.";
            }
            out.push_back(std::move(m));
            continue;
        }
        switch (r) {
        case ROW_SEC_DEV: m.label = "Developer options"; break;
        case ROW_SEC_MODS: m.label = "Installed mods"; break;
        case ROW_MODS_NONE:
            m.label = "No mods found";
            m.help = "No mods\nPut a mod folder (it has a mod.json) in the mods folder: " + mods_dir_path() + ". It shows up here, where it can be turned on and off.";
            break;
        case ROW_DISPLAY: {
            const bool fs = is_fullscreen();
            m.label = "Display";
            if (fs) {
                int dw = 0, dh = 0;
                if (win()) SDL_GetWindowSize(win(), &dw, &dh);
                std::snprintf(buf, sizeof buf, "Fullscreen  %d x %d", dw, dh);
                m.value = buf;
            } else m.value = "Window";
            m.can_dec = fs;    // left: Window
            m.can_inc = !fs;   // right: Fullscreen
            m.help = "Fullscreen or window\nFullscreen fills the screen. Saved. (F11 / Alt+Enter switch for this session only.)";
            break;
        }
        case ROW_RES: {
            const int i = g_win_sel < 0 || g_win_sel >= int(g_res.size()) ? 0 : g_win_sel;
            const ResOpt& o = g_res[size_t(i)];
            m.label = "Window Size";
            if (o.k == K_DECK) std::snprintf(buf, sizeof buf, "%d x %d (Deck)", o.w, o.h);
            else std::snprintf(buf, sizeof buf, "%d x %d", o.w, o.h);
            m.value = buf;
            int lo = i - 1, hi = i + 1;
            while (lo >= 0 && g_res[size_t(lo)].k < 0) lo--;
            while (hi < int(g_res.size()) && g_res[size_t(hi)].k < 0) hi++;
            m.can_dec = lo >= 0 || o.k < 0;
            m.can_inc = hi < int(g_res.size());
            m.help = is_fullscreen()
                ? "Window size\nUsed when Display is Window: multiples of the PSP's 480 x 272, or the Deck's screen."
                : "Window size\nMultiples of the PSP's 480 x 272, or the Deck's screen (1280 x 800). Saved.";
            break;
        }
        case ROW_SCALE: {
            const int n = g_pending_rs ? g_pending_rs : ge_draw_render_scale();
            m.label = "Render Scale";
            std::snprintf(buf, sizeof buf, "%dx  %d x %d", n, 480 * n, 272 * n);
            m.value = buf;
            m.can_dec = n > 1;
            m.can_inc = n < k_max_rs;
            char hb[256];
            std::snprintf(hb, sizeof hb, "Internal 3D resolution\nCurrently %d x %d (%dx the PSP's 480 x 272). Higher is sharper and costs more GPU time; 2x is a good fit for most devices and more is rarely needed.", 480 * n, 272 * n, n);
            m.help = hb;
            break;
        }
        case ROW_WIDE: {
            const bool on = g_pending_wide >= 0 ? g_pending_wide != 0 : ge_draw_widescreen();
            m.label = "Ultrawide";
            if (!on) m.value = "Off";
            else if (g_pending_wide >= 0) m.value = "On";
            else {
                const int w = ge_draw_widescreen_width();
                std::snprintf(buf, sizeof buf, w > 480 ? "On  (%d px wide)" : "On  (window not wider than 16:9)", w);
                m.value = buf;
            }
            m.can_dec = on; m.can_inc = !on;
            m.help = "Ultrawide (PSPRECOMP_WIDESCREEN)\nThe battle scene is drawn wider to fill an ultrawide window or screen (wider than 16:9); the HUD stays centred at its normal size and backgrounds stretch. Saved.";
            break;
        }
        case ROW_FPS: {
            const int i = fps_index();
            m.label = "Frame Rate";
            std::snprintf(buf, sizeof buf, "%d fps", g_fps.rates[i]);
            m.value = buf;
            m.can_dec = i > 0;
            m.can_inc = i + 1 < g_fps.count;
            m.help = "Frame rate (multiples of 30)\nBattles run at this many frames a second; the PSP ran 30. Pick a rate your device can hold: if it cannot reach the target, the game will run slower than normal and stutter. Costs CPU and GPU time in proportion. Menus stay at 30. Saved.";
            break;
        }
        case ROW_RENDERER: {
            std::string why;
            const bool avail = renderer_vulkan_available(&why);
            const bool vk = renderer_vulkan_choice();
            m.label = "Graphics API";
            m.value = std::string(vk ? "Vulkan" : "OpenGL") + (vk != renderer_vulkan_at_start() ? "  (next start)" : "");
            m.disabled = !avail;
            m.can_dec = avail && vk;
            m.can_inc = avail && !vk;
            m.help = avail ? "Graphics API (PSPRECOMP_RENDERER)\nVulkan is the default on Android when the GPU supports it (needs Vulkan 1.1); elsewhere OpenGL is the default and the most tested. Vulkan renders the same picture through a Vulkan layer. Takes effect the next time the game starts. Saved."
                           : "Graphics API: Vulkan unavailable\n" + why + " OpenGL is used.";
            break;
        }
        case ROW_VOICE: {
            const bool jp = voice_jp_choice();
            m.label = "Voice Language";
            m.value = std::string(jp ? "Japanese" : "English") + (jp != voice_at_start() ? "  (next start)" : "");
            m.can_dec = jp;
            m.can_inc = !jp;
            m.help = "Character voices and movie audio\nJapanese needs the voice pack made by tools/jp_audio.py from an undub or Japanese disc. Takes effect the next time the game starts.";
            break;
        }
        case ROW_TEXPACK: {
            const bool on = texpack_choice();
            m.label = "HD Textures";
            m.value = std::string(on ? "On" : "Off") + (on != texpack_at_start() ? "  (next start)" : "");
            m.can_dec = on;
            m.can_inc = !on;
            m.help = "Texture pack\nReplaces the game's textures with the installed HD pack (mods/textures). Takes effect the next time the game starts.";
            break;
        }
        case ROW_TOUCH: {
            static const char* const names[3] = {"Auto", "On", "Off"};
            const int t = psp_touch_mode();
            m.label = "Touch Controls";
            m.value = names[t];
            m.can_dec = t > 0;
            m.can_inc = t < 2;
            m.help = "On-screen buttons (touch screens)\nAuto: shown when you touch the screen, hidden when a controller button is pressed. On: always shown. Off: never. Saved.";
            break;
        }
        case ROW_TOUCH_EDIT:
            m.label = "Edit Touch Controls";
            m.help = "Touch layout editor\nMove, resize or hide each on-screen control and add combination buttons (for example L+R, or R+Triangle). Press Cross to open it; the layout is saved.";
            break;
        case ROW_UPDATES: {
            const bool on = updates_choice();
            m.label = "Check for Updates";
            m.value = on ? "On" : "Off";
            m.can_dec = on;
            m.can_inc = !on;
            m.help = "Update check\nWhen on, the launcher (psprecomp_setup --launch) looks for a new release when you start the game and asks before installing it. Never checks while you play. Saved.";
            break;
        }
        case ROW_NET_MODE:
            m.label = "Network";
            m.value = g_net_internet ? "Internet" : "LAN";
            m.can_dec = !g_net_internet;
            m.can_inc = g_net_internet;
            m.help = "Network\nInternet: play with anyone through the party server (the default). LAN: find players on your own network, or at the addresses under Direct Peers. Saved.";
            break;
        case ROW_NET_SERVER:
            m.label = "Server";
            m.value = g_net_server;
            m.help = "Party server\nThe relay server for Internet play: a host name, or host:port. Press Cross to edit. Saved.";
            break;
        case ROW_NET_ROOM:
            m.label = "Room";
            m.value = g_net_room.empty() ? "Public" : g_net_room;
            m.help = "Room\nPlayers in the same room see each other. Leave it empty for the public room, or use a name only your friends know. Press Cross to edit. Saved.";
            break;
        case ROW_NET_PEERS:
            m.label = "Direct Peers";
            m.value = g_net_peers.empty() ? "None" : g_net_peers;
            m.help = "Direct peers\nExtra addresses to contact directly, in addition to the local network: host or host:port, comma separated. Press Cross to edit. Saved.";
            break;
        case ROW_NET_STATUS:
            m.label = "Status";
            m.value = g_net_status.empty() ? "..." : g_net_status;
            m.help = "Network status\nWhether the server answers and how many players are in your room (or nearby, on a LAN). Communications mode starts the connection.";
            break;
        case ROW_CLOUD_PROVIDER:
            m.label = "Cloud Sync";
            m.value = psp_cloud_svc::provider_name(g_cloud.provider);
            m.can_dec = g_cloud.provider != psp_cloud_svc::Provider::Off;
            m.can_inc = g_cloud.provider != psp_cloud_svc::Provider::Google;
            m.help = "Keep saves in step between devices\nLeft / right (or Cross) choose: Off, Folder, WebDAV, Google Drive. Folder: a synced (Syncthing, a Drive / OneDrive folder, a NAS), a WebDAV server (Nextcloud and most NAS boxes) or Google Drive. Saves from every region sync with each other. On the save list, Triangle > Cloud sync shows the status and lets you choose when both sides changed.";
            break;
        case ROW_CLOUD_FOLDER:
            m.label = "Sync Folder";
            m.value = g_cloud.folder.empty() ? "not set" : tail_fit(g_cloud.folder, 30);
            m.help = "Folder\nA folder another program keeps in sync between your devices (or a network share). The saves are stored there as small files. Press Cross to type the path. " + g_cloud.folder;
            break;
        case ROW_CLOUD_URL:
            m.label = "WebDAV Address";
            m.value = g_cloud.webdav_url.empty() ? "not set" : tail_fit(g_cloud.webdav_url, 30);
            m.help = "WebDAV folder address\nThe folder on the server that holds the saves, e.g. https://cloud.example.com/remote.php/dav/files/me/Dissidia (the parent folder must exist). Press Cross to type it. " + g_cloud.webdav_url;
            break;
        case ROW_CLOUD_USER:
            m.label = "WebDAV User";
            m.value = g_cloud.webdav_user.empty() ? "not set" : tail_fit(g_cloud.webdav_user, 30);
            m.help = "WebDAV user name\nPress Cross to type it.";
            break;
        case ROW_CLOUD_PASS:
            m.label = "WebDAV Password";
            m.value = g_cloud.webdav_pass.empty() ? "not set" : "set";
            m.help = "WebDAV password\nFor Nextcloud / ownCloud create an app password in the security settings and use that. It is kept in cloud.ini next to settings.ini; keep that file private. Press Cross to type it.";
            break;
        case ROW_CLOUD_GOOGLE: {
            const psp_cloud_svc::Status st = psp_cloud_svc::status();
            m.label = "Google Drive";
            if (st.state == psp_cloud_svc::Status::SigningIn) {
                m.value = st.user_code.empty() ? "contacting..." : st.user_code;
                m.help = "Signing in\n" + st.text + "\nPress Cross to cancel.";
            } else if (g_cloud.google_refresh.empty()) {
                m.value = "Sign in...";
                m.help = "Sign in to Google Drive\nPress Cross: a code appears. Enter it at google.com/device on your phone or computer. The game only gets access to a folder it creates in your Drive, never to your other files.";
            } else {
                m.value = "Signed in";
                m.help = "Signed in to Google Drive\nPress Cross to sign out on this device. The saves in your Drive stay where they are.";
            }
            break;
        }
        case ROW_CLOUD_AUTO:
            m.label = "Sync Automatically";
            m.value = g_cloud.auto_sync ? "On" : "Off";
            m.can_dec = g_cloud.auto_sync; m.can_inc = !g_cloud.auto_sync;
            m.help = "Automatic sync\nOn: a sync runs at start-up (before the game reads saves) and a few seconds after each save (it uploads this save and downloads newer ones from your other devices). Off: only Sync Now.";
            break;
        case ROW_CLOUD_REPLAYS:
            m.label = "Include Replays";
            m.value = g_cloud.replays ? "On" : "Off";
            m.can_dec = g_cloud.replays; m.can_inc = !g_cloud.replays;
            m.help = "Replays\nReplay saves are large; leave this off unless you want them on every device.";
            break;
        case ROW_CLOUD_NOW: {
            const bool busy = psp_cloud_svc::status().state == psp_cloud_svc::Status::Syncing;
            m.label = "Sync Now";
            m.value = busy ? "working..." : "";
            m.help = "Sync now\nUpload changed saves and download newer ones. The game is paused while this menu is open, so downloading is safe.";
            break;
        }
        case ROW_CLOUD_STATUS: {
            const psp_cloud_svc::Status st = psp_cloud_svc::status();
            m.label = "Status";
            if (!psp_cloud_svc::initialised()) {
                m.value = "disabled";
                m.help = "Cloud sync\nCloud sync is switched off for this run (PSPRECOMP_CLOUD=0).";
                break;
            }
            m.value = st.state == psp_cloud_svc::Status::Syncing ? "working..." : st.state == psp_cloud_svc::Status::Error ? "problem" :
                      st.state == psp_cloud_svc::Status::NeedsChoice ? "needs choice" : st.state == psp_cloud_svc::Status::SigningIn ? "signing in" :
                      st.state == psp_cloud_svc::Status::Off ? "off" : "ok";
            m.help = "Cloud sync status\n" + st.text + (st.conflicts ? "\nChoose which save to keep on the save list: Triangle > Cloud sync." : "");
            break;
        }
        case ROW_GPU: {
            const std::string cur = gpu_choice();
            const std::vector<std::string> ids = gpu_ids(cur);
            std::string nm = "System default";
            int idx = 0;
            for (size_t i = 0; i < ids.size(); i++) if (ids[i] == cur) idx = int(i);
            if (idx > 0) {   // the package id with spaces: the same short name the download list shows
                nm = cur;
                for (auto& c : nm) if (c == '_') c = ' ';
                bool have = false;
                for (auto& in : g_gpu_inst) if (in.id == cur) have = true;
                if (!have) nm += " (missing)";
            }
            m.label = "GPU Driver";
            m.value = gpu_trim(nm, 30) + (cur != psp_gpu::selected() ? "  (next start)" : "");
            m.can_dec = idx > 0;
            m.can_inc = idx + 1 < int(ids.size());
            m.help = "GPU driver (PSPRECOMP_GPU_DRIVER)\nThe Vulkan driver the game uses. System default is the phone's own; the drivers below are newer community builds (Mesa Turnip) that can add features the phone's driver lacks, such as ray tracing on Adreno 740 and newer. Needs Graphics API: Vulkan. Takes effect the next time the game starts; if a driver crashes, the game starts with the system one again. Saved."
                   + (psp_gpu::crashed(cur) ? std::string("\nThis driver crashed the last time it was tried: choose it again to retry.") : std::string());
            break;
        }
        case ROW_GPU_GET: {
            const psp_gpu::Status st = psp_gpu::status();
            const std::vector<psp_gpu::Remote> cat = psp_gpu::catalog();
            m.label = "Download Driver";
            const bool busy = st.state == psp_gpu::Status::Busy;
            if (busy) m.value = "working...";
            else if (!psp_gpu::catalog_loaded()) m.value = st.state == psp_gpu::Status::Error ? "Press to retry" : "Press to load the list";
            else if (cat.empty()) m.value = "none found";
            else {
                const int i = std::min(std::max(g_gpu_get_idx, 0), int(cat.size()) - 1);
                bool have = false;
                for (auto& in : psp_gpu::installed()) if (in.id == cat[size_t(i)].id) have = true;
                std::snprintf(buf, sizeof buf, " %.1f MB", double(cat[size_t(i)].size) / 1e6);
                m.value = gpu_trim(cat[size_t(i)].name, 30) + buf + (have ? "  [installed]" : "");
                m.can_dec = i > 0;
                m.can_inc = i + 1 < int(cat.size());
            }
            m.help = "Download a GPU driver\nLeft / right browse the live list of driver packages; Cross downloads and installs the one shown, and selects it for the next start. Turnip builds for Adreno 6xx / 7xx are the usual choice; try the newest first. Needs an internet connection."
                   + (st.text.empty() ? std::string() : "\n" + st.text);
            break;
        }
        case ROW_GPU_DEL: {
            const std::string cur = gpu_choice();
            m.label = "Delete Driver";
            m.value = cur.empty() ? "" : gpu_trim(cur, 30);
            m.disabled = cur.empty();
            m.help = "Delete the selected driver\nRemoves it from this phone and goes back to the system driver. Press Cross.";
            break;
        }
        case ROW_SUB_HDR: {
            const bool avail = psp_hdr_available(), on = psp_hdr_wanted();
            m.label = "HDR Output";
            m.value = !avail ? "Unavailable" : on ? "On" : "Off";
            m.help = "HDR\nHDR output, highlight strength, wide-gamut colour, peak brightness and paper white, for HDR displays. Press Cross to open.";
            break;
        }
        case ROW_SUB_RT: {
            static const char* const k_rq[4] = {"Off", "Low", "Medium", "High"};
            const bool avail = psp_rt_available();
            m.label = "Ray Tracing";
            m.value = !avail ? "Unavailable" : std::string("Shadows ") + k_rq[psp_rt_quality()];
            m.help = avail ? "Ray tracing\nShadows and reflections traced against the scene's real geometry with the GPU's ray tracing hardware. Press Cross to open."
                           : std::string("Ray tracing: unavailable\n") + psp_rt_unavailable_reason();
            break;
        }
        case ROW_SEC_WINDOW: m.label = "Window"; break;
        case ROW_SEC_PERF: m.label = "Performance"; break;
        case ROW_SEC_QUALITY: m.label = "Image quality"; break;
        case ROW_SEC_AA: m.label = "Edges and sharpness"; break;
        case ROW_SEC_TEX: m.label = "Textures"; break;
        case ROW_SEC_ENGINE: m.label = "Graphics engine"; break;
        case ROW_SEC_COLOUR: m.label = "Colour"; break;
        case ROW_SEC_HDR: m.label = "High dynamic range"; break;
        case ROW_SEC_LIGHT: m.label = "Lighting"; break;
        case ROW_SEC_RT: m.label = "Hardware"; break;
        case ROW_SEC_TOUCH: m.label = "Touch screen"; break;
        case ROW_SEC_GAME: m.label = "Game"; break;
        case ROW_SEC_SESSION: m.label = "Session"; break;
        case ROW_SEC_REFL: m.label = "Reflections"; break;
        case ROW_SEC_SERVER: m.label = "Connection"; break;
        case ROW_SEC_SYNC: m.label = "Sync"; break;
        case ROW_SEC_DRIVER: m.label = "Driver"; break;
        case ROW_COUNT: break;
        case ROW_QUIT:
            m.label = "Quit Game";
            m.value = g_quit_armed ? "Press again to quit" : "";
            m.help = g_quit_armed ? "Quit\nUnsaved progress is lost. Press again to quit, or move away to cancel."
                                  : "Quit\nClose the game and return to the desktop / Steam.";
            break;
        case ROW_FPSCOUNT: {
            const bool on = ge_draw_fps_counter();
            m.label = "FPS Counter";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "FPS counter\nShows the frames per second as a small transparent number in the top-right corner. Saved.";
            break;
        }
        case ROW_PERF: {
            const bool on = ge_draw_perf_overlay();
            m.label = "Performance Overlay";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Performance overlay\nFor debugging, top-left corner: frames per second, average and worst frame time, the battle's average fps and 1% / 0.1% lows (they restart when a battle begins and ignore menus), GPU usage, which parts of the work take the most frame time (draw setup, textures, effects, post-processing, waiting), the costliest GPU passes (Vulkan), render scale and memory use. Saved.";
            break;
        }
        case ROW_FX: {
            const bool avail = ge_draw_fx_available(), on = ge_draw_fx_shaders();
            m.label = "FX Shaders";
            m.value = !avail ? "Not loaded" : (on ? "On" : "Off");
            m.can_dec = avail && on; m.can_inc = avail && !on;
            m.help = avail ? "Effect shader overrides (mods/fx_shaders)\nOff draws effects the original way. This session only."
                           : "Effect shader overrides (mods/fx_shaders)\nNone loaded at start (no manifest, or PSPRECOMP_FX_SHADERS=0).";
            break;
        }
        case ROW_SYNC: {
            static const char* names[5] = {"Off", "Every batch", "Every list", "Every frame", "Flush only"};
            const int s = ge_draw_sync_mode();
            m.label = "GPU Sync";
            m.value = names[s < 0 || s > 4 ? 0 : s];
            m.can_dec = s > 0; m.can_inc = s < 4;
            m.help = "Flicker test (--ge-sync)\nWaits for the GPU at each step: slower, rules out timing. This session only.";
            break;
        }
        case ROW_SOFT: {
            const bool on = ge_soft_focus();
            m.label = "Soft Focus";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Soft focus (PSPRECOMP_SOFT_FOCUS)\nSome stages (Order's Sanctuary) blend a blurred copy of the picture over the scene for a dreamy look. Off gives a sharper picture. Saved.";
            break;
        }
        case ROW_AA: {
            const int v = psp_post_aa();
            m.label = "Anti-Aliasing";
            m.value = v == POST_AA_SMAA ? "SMAA" : v == POST_AA_FXAA ? "FXAA" : "Off";
            m.can_dec = v > 0; m.can_inc = v < POST_AA_SMAA;
            m.help = "Post-process anti-aliasing (PSPRECOMP_POST_AA)\nSmooths jagged edges on the finished picture. FXAA is cheap but softens the whole picture; SMAA finds the edges and shapes, so it keeps textures and the HUD crisp and handles long near-horizontal edges better, for a little more GPU time. Neither touches the game's glow, so both are safe at any render scale. Saved.";
            break;
        }
        case ROW_SHARP: {
            const int v = psp_post_sharpen();
            m.label = "Sharpening";
            m.value = v ? "Level " + std::to_string(v) : "Off";
            m.can_dec = v > 0; m.can_inc = v < 4;
            m.help = "Sharpening (PSPRECOMP_SHARPEN)\nContrast-adaptive sharpening of the finished picture. Levels 1 to 4; use it with FXAA to win back crispness. Saved.";
            break;
        }
        case ROW_BRIGHT: case ROW_CONTRAST: case ROW_SAT: {
            const int v = r == ROW_BRIGHT ? psp_post_brightness() : r == ROW_CONTRAST ? psp_post_contrast() : psp_post_saturation();
            const int lo = r == ROW_SAT ? 0 : 50, hi = r == ROW_SAT ? 200 : 150;
            m.label = r == ROW_BRIGHT ? "Brightness" : r == ROW_CONTRAST ? "Contrast" : "Saturation";
            m.value = v == 100 ? "Default" : std::to_string(v) + "%";
            m.can_dec = v > lo; m.can_inc = v < hi;
            m.help = r == ROW_BRIGHT ? "Brightness (PSPRECOMP_BRIGHTNESS)\nLifts or lowers the midtones of the whole picture without crushing blacks or clipping whites. 100% is the original look. Saved."
                   : r == ROW_CONTRAST ? "Contrast (PSPRECOMP_CONTRAST)\nSpreads the picture's light and dark apart (above 100%) or pulls them together. 100% is the original look. Saved."
                   : "Saturation (PSPRECOMP_SATURATION)\nColour strength. The PSP's screen was washed out next to a modern display: 110 to 125% suits most. 100% is the original look. Saved.";
            break;
        }
        case ROW_HDR: {
            const bool avail = psp_hdr_available(), on = psp_hdr_wanted();
            const int enc = psp_hdr_encoding();
            m.label = "HDR Output";
            m.value = !avail ? "Unavailable" : on ? (enc == 1 ? "On (scRGB)" : enc == 2 ? "On (HDR10)" : "On (waiting)") : "Off";
            m.disabled = !avail;   // greyed out, the reason is in the description
            m.can_dec = avail && on; m.can_inc = avail && !on;
            m.help = avail ? "HDR output (PSPRECOMP_HDR)\nSends the picture to an HDR display: highlights (glows, spell flashes, the sky) are expanded past SDR white up to the peak brightness below; the HUD and menus stay at paper white. The game itself is SDR, so this is an inverse tone map, not native HDR. Turn on HDR in your system's display settings first. Saved."
                           : std::string("HDR output: unavailable\n") + (g_psp_hdr.reason ? g_psp_hdr.reason() : "Needs the Vulkan renderer.");
            break;
        }
        case ROW_HDR_GAMUT: {
            static const char* const k_names[5] = {"Off", "Low", "Medium", "High", "Max"};
            const int v = psp_hdr_gamut();
            const bool live = psp_hdr_available() && psp_hdr_wanted();
            m.label = "HDR Colour";
            m.value = k_names[v];
            m.disabled = !live;
            m.can_dec = live && v > 0; m.can_inc = live && v < 4;
            m.help = !live ? "HDR colour only applies while HDR Output is on (see the HDR Output row for why it may be unavailable)."
                           : "HDR colour (PSPRECOMP_HDR_GAMUT)\nA wide-gamut screen (OLED) stretches ordinary SDR colour to its own primaries, but shows HDR content as plain sRGB, which looks drained next to SDR. This stretches the colours towards Display-P3 by the chosen amount (Max goes past it, for phones whose SDR mode is very vivid). Off is plain sRGB. Saved.";
            break;
        }
        case ROW_HDR_BOOST: {
            static const char* const k_names[4] = {"Off", "Low", "Medium", "High"};
            const int v = psp_hdr_boost();
            const bool live = psp_hdr_available() && psp_hdr_wanted();
            m.label = "HDR Highlights";
            m.value = k_names[v];
            m.disabled = !live;
            m.can_dec = live && v > 0; m.can_inc = live && v < 3;
            m.help = !live ? "HDR highlights only apply while HDR Output is on (see the HDR Output row for why it may be unavailable)."
                           : "HDR highlights (PSPRECOMP_HDR_BOOST)\nHow far small bright features (glows, sparks, spell flashes, specular) rise above paper white, up to the peak brightness. Large bright areas such as menus and white skies are never lifted. Off shows the picture at plain SDR brightness. Saved.";
            break;
        }
        case ROW_HDR_SCENE: {
            const bool live_out = psp_hdr_available() && psp_hdr_wanted() && psp_hdr_encoding() != 0;
            const bool on = psp_hdr_scene_wanted();
            m.label = "HDR Scene";
            m.value = on ? (live_out ? "On" : "On (needs HDR)") : "Off";
            m.disabled = !live_out;   // greyed out, the reason is in the description
            m.can_dec = live_out && on; m.can_inc = live_out && !on;
            m.help = live_out ? "HDR scene (PSPRECOMP_HDR_SCENE)\nRenders the picture in 16-bit float instead of 8-bit, so additive effects (glows, spell flashes, beams) keep their real brightness above white instead of clipping, and HDR output shows them as true highlights. Opaque drawing is unchanged. Costs some memory bandwidth. Experimental. Saved."
                           : "HDR scene: unavailable\nNeeds HDR Output switched on and working (see its row), with the Vulkan renderer.";
            break;
        }
        case ROW_HDR_PEAK: case ROW_HDR_PAPER: {
            const bool pk = r == ROW_HDR_PEAK;
            const int v = pk ? psp_hdr_peak() : psp_hdr_paper();
            m.label = pk ? "HDR Peak Brightness" : "HDR Paper White";
            m.value = std::to_string(v) + " nits";
            const bool live = psp_hdr_available() && psp_hdr_wanted();
            const bool locked = false;
            m.disabled = !live || locked;
            m.can_dec = live && !locked && v > (pk ? 400 : 80); m.can_inc = live && !locked && v < (pk ? 4000 : 1000);
            m.help = !live ? "HDR peak brightness and paper white only apply while HDR Output is on (see the HDR Output row for why it may be unavailable)." : pk ? "HDR peak brightness (PSPRECOMP_HDR_PEAK)\nThe brightest level expanded highlights reach, in nits. Set it to about what your display can show (OLED 600 to 1000, mini-LED 1000 to 1600). Saved."
                        : "HDR paper white (PSPRECOMP_HDR_PAPER)\nHow bright ordinary white (the HUD, menus, a plain SDR picture) is shown, in nits. 203 is the broadcast reference; raise it in a bright room. Saved.";
            break;
        }
        case ROW_SSAO: case ROW_CONTACT: case ROW_SSR: {
            const bool ao = r == ROW_SSAO, cs = r == ROW_CONTACT;
            const int v = ao ? psp_fx_ssao() : cs ? psp_fx_contact() : psp_fx_ssr();
            m.label = ao ? "Ambient Occlusion" : cs ? "Contact Shadows" : "Reflections";
            static const char* const k_q[4] = {"Off", "Low", "Medium", "High"};
            m.value = k_q[v];
            m.can_dec = v > 0; m.can_inc = v < 3;
            m.help = ao ? "Ambient occlusion, SSAO (PSPRECOMP_SSAO)\nDarkens creases and the ground around characters and objects. Low, Medium or High (more samples, more GPU time). Runs on the 3D scene before the glow and the HUD, so the HUD is never darkened. Costs GPU time; not for the Steam Deck at a high render scale. Saved."
                : cs ? "Contact shadows (PSPRECOMP_CONTACT_SHADOWS)\nA short ray-marched shadow cast from fighters and objects onto the floor and each other. Low, Medium or High (more samples, more GPU time). Saved."
                     : "Screen-space reflections (PSPRECOMP_SSR)\nFloors mirror the characters, ruins and sky that are on screen. Low, Medium or High (more samples, more GPU time). Only what is visible in the picture can be reflected. Saved.";
            break;
        }
        case ROW_RT_SOFT: {
            static const char* const k_names[5] = {"Sharp", "Firm", "Normal", "Soft", "Very Soft"};
            const int v = psp_rt_softness();
            m.label = "Shadow Softness";
            m.value = k_names[v];
            const bool usable = psp_rt_available() && psp_rt_quality() >= 2;
            m.disabled = !usable;
            m.can_dec = usable; m.can_inc = usable;   // loops
            m.help = !psp_rt_available() ? std::string("Shadow softness: unavailable\n") + psp_rt_unavailable_reason()
                   : psp_rt_quality() < 2 ? "Shadow softness: needs Ray-Traced Shadows set to Medium or High (Off and Low use at most one hard ray, so there is no soft edge to adjust)."
                   : "Shadow softness (PSPRECOMP_RT_SOFT)\nHow wide the light is for ray-traced shadows. A wider light gives softer edges that sharpen where a shadow meets its caster, like a real shadow; Sharp is almost a point light. Firm is the default. Needs Medium or High ray-traced shadows (Low uses one hard ray). Takes effect at once. Saved.";
            break;
        }
        case ROW_RT: {
            const bool avail = psp_rt_available();
            const int v = avail ? psp_rt_quality() : 0;
            m.label = "Ray-Traced Shadows";
            static const char* const k_rq[4] = {"Off", "Low", "Medium", "High"};
            m.value = k_rq[v];
            m.disabled = !avail;
            m.can_dec = v > 0; m.can_inc = avail && v < 3;
            m.help = avail ? "Ray-traced shadows (PSPRECOMP_VK_RT)\nShadows traced against the scene's real geometry with the GPU's ray tracing hardware, instead of marched through the depth buffer: objects shadow each other and the stage. Low traces one hard ray per pixel; Medium and High trace several rays across the light for soft edges (more GPU time). Replaces Contact Shadows while on (that row is hidden). Takes effect at once. Saved."
                           : std::string("Ray-traced shadows: unavailable\n") + psp_rt_unavailable_reason();
            break;
        }
        case ROW_RT_REFL: {
            const bool avail = psp_rt_available();
            const int v = avail ? psp_rt_refl_quality() : 0;
            m.label = "Ray-Traced Reflections";
            static const char* const k_rrq[4] = {"Off", "Low", "Medium", "High"};
            m.value = k_rrq[v];
            m.disabled = !avail;
            m.can_dec = v > 0; m.can_inc = avail && v < 3;
            m.help = avail ? "Ray-traced reflections (PSPRECOMP_VK_RT_REFL)\nFloors mirror the scene by tracing the reflected view ray against the real geometry with the GPU's ray tracing hardware, so they also show what is off screen or hidden behind a fighter. Where the hit is on screen it uses the real picture, elsewhere the surface's average colour. Low traces solid geometry only; Medium adds orbs, auras and glass; High also blurs the reflection slightly with several rays (most GPU time). Replaces Reflections while on (that row is hidden). Takes effect at once. Saved."
                           : std::string("Ray-traced reflections: unavailable\n") + psp_rt_unavailable_reason();
            break;
        }
        case ROW_ANISO: {
            const int v = ge_texture_aniso();
            m.label = "Anisotropic Filtering";
            m.value = v > 1 ? std::to_string(v) + "x" : "Off";
            m.can_dec = v > 1; m.can_inc = v < 16;
            m.help = "Anisotropic filtering (PSPRECOMP_ANISO)\nKeeps textures sharp at a slant, such as stage floors. Builds mipmaps for the game's textures, so it costs a little load time and video memory. Takes effect at once. Saved.";
            break;
        }
        case ROW_UPSCALE: {
            static const char* const names[3] = {"Bilinear", "Sharp Bilinear", "Bicubic"};
            const int v = psp_post_upscale();
            m.label = "Upscale Filter";
            m.value = names[v];
            m.can_dec = v > 0; m.can_inc = v < 2;
            m.help = "Upscale filter (PSPRECOMP_UPSCALE)\nHow the picture is stretched to the window when its size is not a whole multiple. Sharp Bilinear keeps texels crisp, Bicubic is smoother and sharper than Bilinear. Saved.";
            break;
        }
        case ROW_GLOWSOFT: {
            const bool on = ge_draw_fb_prefilter();
            m.label = "Glow Softening";
            m.value = ge_draw_render_scale() > 1 ? (on ? "On" : "Off") : "On  (render scale 1x: no effect)";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Ghost test (PSPRECOMP_FB_PREFILTER)\nAt render scale above 1, the glow passes sample the picture through its mipmaps. Flip it while looking at a ghost. This session only.";
            break;
        }
        case ROW_GLOWPASS: {
            const bool on = ge_draw_fb_snapshot();
            m.label = "Glow Passes";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Ghost test (PSPRECOMP_GE_FEEDBACK)\nOff skips the passes that read the picture they draw into (the glow). If the ghost goes, it comes from there. This session only.";
            break;
        }
        case ROW_DBG0: case ROW_DBG1: case ROW_DBG2: case ROW_DBG3: case ROW_DBG4: case ROW_DBG5: {
            const size_t i = size_t(r - ROW_DBG0);
            if (i >= g_dbg_toggles.size()) break;
            const bool on = g_dbg_toggles[i].get();
            m.label = g_dbg_toggles[i].label;
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = g_dbg_toggles[i].help;
            break;
        }
        case ROW_RING: {
            const int cur = ge_draw_ring_mode(), nx = g_ring_next >= 0 ? g_ring_next : cur;
            m.label = "Vertex Upload";
            std::snprintf(buf, sizeof buf, "%s%s", nx ? "Map" : "Sub", nx != cur ? "  (next launch)" : "");
            m.value = buf;
            m.can_dec = nx == 1; m.can_inc = nx == 0;
            m.help = "Flicker test (PSPRECOMP_GE_RING)\nHow vertices reach the GPU. Saved; applies when the game next starts.";
            break;
        }
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ---- the rows as the settings view draws them ----------------------------------------------------------------------
// Rows with a number of discrete positions are drawn as sliders (and can be dragged): steps / current position.
bool slider_of(Row r, int* steps, int* cur) {
    auto lin = [&](int lo, int hi, int step, int v) { *steps = (hi - lo) / step + 1; *cur = std::clamp((v - lo) / step, 0, *steps - 1); return true; };
    switch (r) {
    case ROW_BRIGHT: return lin(50, 150, 5, psp_post_brightness());
    case ROW_CONTRAST: return lin(50, 150, 5, psp_post_contrast());
    case ROW_SAT: return lin(0, 200, 5, psp_post_saturation());
    case ROW_HDR_PEAK: return lin(400, 4000, 100, psp_hdr_peak());
    case ROW_HDR_PAPER: return lin(80, 1000, 10, psp_hdr_paper());
    case ROW_ANISO: {
        static const int lv[5] = {1, 2, 4, 8, 16};
        *steps = 5; *cur = 0;
        for (int i = 0; i < 5; ++i) if (lv[i] <= ge_texture_aniso()) *cur = i;
        return true;
    }
    case ROW_SHARP: *steps = 5; *cur = std::clamp(psp_post_sharpen(), 0, 4); return true;
    case ROW_AA: *steps = 3; *cur = std::clamp(psp_post_aa(), 0, 2); return true;
    case ROW_SSAO: *steps = 4; *cur = std::clamp(psp_fx_ssao(), 0, 3); return true;
    case ROW_CONTACT: *steps = 4; *cur = std::clamp(psp_fx_contact(), 0, 3); return true;
    case ROW_SSR: *steps = 4; *cur = std::clamp(psp_fx_ssr(), 0, 3); return true;
    case ROW_RT: *steps = 4; *cur = psp_rt_available() ? std::clamp(psp_rt_quality(), 0, 3) : 0; return true;
    case ROW_RT_REFL: *steps = 4; *cur = psp_rt_available() ? std::clamp(psp_rt_refl_quality(), 0, 3) : 0; return true;
    case ROW_HDR_BOOST: *steps = 4; *cur = std::clamp(psp_hdr_boost(), 0, 3); return true;
    case ROW_HDR_GAMUT: *steps = 5; *cur = std::clamp(psp_hdr_gamut(), 0, 4); return true;
    case ROW_UPSCALE: *steps = 3; *cur = std::clamp(psp_post_upscale(), 0, 2); return true;
    case ROW_FPS: if (g_fps.get && g_fps.count > 1) { *steps = g_fps.count; *cur = fps_index(); return true; } return false;
    case ROW_SYNC: *steps = 5; *cur = std::clamp(ge_draw_sync_mode(), 0, 4); return true;
    case ROW_TOUCH: *steps = 3; *cur = std::clamp(psp_touch_mode(), 0, 2); return true;
    default: return false;
    }
}
psp_sddialog::SetKind row_kind(Row r) {
    using K = psp_sddialog::SetKind;
    if (is_mod_row(r)) return K::Toggle;
    if (r >= ROW_SUB_ENV0 && r <= ROW_SUB_ENV_END) return K::Submenu;
    if (const EnvOpt* o = env_row_opt(r)) return o->type == 'c' ? K::Choice : K::Toggle;
    switch (r) {
    case ROW_FPSCOUNT: case ROW_WIDE: case ROW_SOFT: case ROW_TEXPACK: case ROW_UPDATES: case ROW_HDR: case ROW_HDR_SCENE: case ROW_CLOUD_AUTO:
    case ROW_CLOUD_REPLAYS: case ROW_PERF: case ROW_FX: case ROW_GLOWSOFT: case ROW_GLOWPASS:
    case ROW_DBG0: case ROW_DBG1: case ROW_DBG2: case ROW_DBG3: case ROW_DBG4: case ROW_DBG5:
        return K::Toggle;
    case ROW_TOUCH_EDIT: case ROW_CLOUD_NOW: case ROW_CLOUD_GOOGLE: case ROW_GPU_DEL: return K::Action;
    case ROW_QUIT: return K::Danger;
    case ROW_SUB_HDR: case ROW_SUB_RT: return K::Submenu;
    case ROW_NET_SERVER: case ROW_NET_ROOM: case ROW_NET_PEERS: case ROW_CLOUD_FOLDER: case ROW_CLOUD_URL: case ROW_CLOUD_USER: case ROW_CLOUD_PASS: return K::Text;
    case ROW_NET_STATUS: case ROW_CLOUD_STATUS: case ROW_MODS_NONE: return K::Info;
    default: return is_section(r) ? K::Header : K::Choice;
    }
}
std::string trim_str(std::string v) {
    while (!v.empty() && v.back() == ' ') v.pop_back();
    size_t i = 0;
    while (i < v.size() && v[i] == ' ') ++i;
    return v.substr(i);
}
// Removes `phrase` from `text` (and the space before it); true when it was there.
bool peel(std::string& text, const char* phrase) {
    const size_t at = text.find(phrase);
    if (at == std::string::npos) return false;
    text.erase(at, std::strlen(phrase));
    while (at < text.size() && text[at] == ' ' && (at == 0 || text[at - 1] == ' ' || text[at - 1] == '\n')) text.erase(at, 1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
    return true;
}
// The description as the panel shows it: the environment variable names go from the title, and the "Saved." / "This session only." /
// "Takes effect ..." sentences become a small chip next to it.
void polish_help(std::string& help, std::string& badge) {
    const size_t nl = help.find('\n');
    std::string title = nl == std::string::npos ? std::string() : help.substr(0, nl);
    std::string body = nl == std::string::npos ? help : help.substr(nl + 1);
    const size_t env = title.find(" (PSPRECOMP_");
    if (env != std::string::npos) { const size_t close = title.find(')', env); if (close != std::string::npos) title.erase(env, close - env + 1); }
    if (peel(body, "Takes effect the next time the game starts.") | peel(body, "Takes effect the next time the game starts")) badge = "Next start";
    else if (peel(body, "Applies when the game next starts.")) badge = "Next start";
    peel(body, "Takes effect at once.");
    if (peel(body, "This session only.")) { if (badge.empty()) badge = "This session"; }
    if (peel(body, "Saved.")) { if (badge.empty()) badge = "Saved"; }
    std::string o;   // collapse double spaces left behind
    for (char ch : body) if (!(ch == ' ' && !o.empty() && o.back() == ' ')) o += ch;
    help = nl == std::string::npos ? o : title + "\n" + o;
}
// Estimated cost of a setting at its current value, shown as a badge on the description: "None", "Low", "Medium", "High" or
// "Very high". These are rough, relative figures for an average GPU (not measurements): the point is to tell a cheap row from an
// expensive one. Rows that cost nothing worth mentioning (display, window size, ...) have no badge.
bool cost_of(Row r, std::string& out) {
    static const char* const k_lv[] = {"None", "Low", "Medium", "High", "Very high"};
    auto lv = [&](int i) { out = k_lv[std::clamp(i, 0, 4)]; return true; };
    switch (r) {
    case ROW_SCALE: { const int n = g_pending_rs ? g_pending_rs : ge_draw_render_scale(); return lv(n <= 1 ? 1 : n == 2 ? 2 : n == 3 ? 3 : 4); }
    case ROW_FPS: { const int f = g_fps.get ? g_fps.get() : 30; return lv(f <= 30 ? 1 : f <= 60 ? 2 : f <= 120 ? 3 : 4); }
    case ROW_WIDE: return lv((g_pending_wide >= 0 ? g_pending_wide != 0 : ge_draw_widescreen()) ? 1 : 0);
    case ROW_FPSCOUNT: return lv(ge_draw_fps_counter() ? 1 : 0);
    case ROW_PERF: return lv(ge_draw_perf_overlay() ? 1 : 0);
    case ROW_AA: return lv(psp_post_aa() > 0 ? 1 : 0);
    case ROW_SHARP: return lv(psp_post_sharpen() > 0 ? 1 : 0);
    case ROW_UPSCALE: return lv(psp_post_upscale());
    case ROW_BRIGHT: return lv(psp_post_brightness() != 100 ? 1 : 0);
    case ROW_CONTRAST: return lv(psp_post_contrast() != 100 ? 1 : 0);
    case ROW_SAT: return lv(psp_post_saturation() != 100 ? 1 : 0);
    case ROW_HDR: return lv(psp_hdr_wanted() ? 1 : 0);
    case ROW_HDR_SCENE: return lv(psp_hdr_scene_wanted() ? 2 : 0);
    case ROW_SSAO: return lv(psp_fx_ssao());
    case ROW_CONTACT: return lv(psp_fx_contact());
    case ROW_SSR: return lv(psp_fx_ssr());
    case ROW_RT: return lv(psp_rt_available() && psp_rt_quality() > 0 ? psp_rt_quality() + 1 : 0);
    case ROW_RT_REFL: return lv(psp_rt_available() && psp_rt_refl_quality() > 0 ? psp_rt_refl_quality() + 1 : 0);
    case ROW_RT_SOFT: return lv(psp_rt_available() && psp_rt_quality() >= 2 ? 1 : 0);
    case ROW_ANISO: return lv(ge_texture_aniso() > 1 ? 1 : 0);
    default: return false;
    }
}
std::vector<psp_sddialog::SetRow> make_set_rows() {
    using psp_sddialog::SetKind;
    const std::vector<psp_sddialog::MenuRow> src = make_rows();
    std::vector<psp_sddialog::SetRow> out;
    for (size_t i = 0; i < src.size() && i < g_rows.size(); ++i) {
        const Row r = g_rows[i];
        const psp_sddialog::MenuRow& m = src[i];
        psp_sddialog::SetRow o;
        o.id = int(r);
        o.kind = row_kind(r);
        o.label = m.label;
        o.value = m.value;
        o.help = m.help;
        o.can_dec = m.can_dec; o.can_inc = m.can_inc;
        o.disabled = m.disabled;
        int steps = 0, cur = 0;
        if (o.kind == SetKind::Choice && slider_of(r, &steps, &cur) && steps > 1) { o.kind = SetKind::Slider; o.steps = steps; o.cur = cur; }
        if (o.kind == SetKind::Toggle) {
            const std::string v = m.value;
            auto word = [&](const char* w) { const size_t n = std::strlen(w); return v.compare(0, n, w) == 0 && (v.size() == n || v[n] == ' '); };
            if (word("On")) { o.on = true; o.value = trim_str(v.substr(2)); }
            else if (word("Off")) { o.on = false; o.value = trim_str(v.substr(3)); }
            else { o.on = false; o.value = v; }
        }
        if (r == ROW_SCALE) {   // "2x  960 x 544" is a long value for a row: keep the multiplier (the description has the size)
            const size_t sp = o.value.find(' ');
            if (sp != std::string::npos) o.value = o.value.substr(0, sp);
        }
        polish_help(o.help, o.badge);
        cost_of(r, o.cost);
        if (m.value.find("next start") != std::string::npos || m.value.find("next launch") != std::string::npos) o.badge = "Next start";
        out.push_back(std::move(o));
    }
    return out;
}

// ---- applying --------------------------------------------------------------
// After the user changed the highlighted row: remember that the game has to restart for it to apply.
void note_next_start() {
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    const Row r = g_rows[size_t(g_sel)];
    if (is_mod_row(r)) return;   // mod_set_enabled notes it
    if (row_kind(r) == psp_sddialog::SetKind::Text) return;
    const std::vector<psp_sddialog::SetRow> rows = make_set_rows();
    if (g_sel < int(rows.size()) && rows[size_t(g_sel)].badge == "Next start") g_restart_pending = true;
}
struct NextStartNote { ~NextStartNote() { note_next_start(); } };

void size_window(const ResOpt& o) {
    SDL_Window* w = win();
    if (!w) return;
    SDL_SetWindowSize(w, o.w, o.h);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(w)),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(w)));
}
void apply_display(bool fullscreen) {
    SDL_Window* w = win();
    if (!w || fullscreen == is_fullscreen()) return;
    if (fullscreen) {
        if (SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
            std::fprintf(stderr, "[SETTINGS] fullscreen failed: %s\n", SDL_GetError());
        save("display", "fullscreen");
    } else {
        SDL_SetWindowFullscreen(w, 0);
        if (g_win_sel >= 0 && g_win_sel < int(g_res.size())) size_window(g_res[size_t(g_win_sel)]);
        save("display", "window");
    }
    std::fprintf(stderr, "[SETTINGS] display: %s\n", fullscreen ? "fullscreen" : "window");
}
// Window size: resizes the window now, or (in fullscreen) is remembered for Window mode. Saved either way.
void apply_res(int i) {
    if (i < 0 || i >= int(g_res.size()) || g_res[size_t(i)].k < 0) return;
    g_win_sel = i;
    const ResOpt& o = g_res[size_t(i)];
    if (!is_fullscreen()) size_window(o);
    if (o.k == K_DECK) {
        unsave("window_scale");
        save("window_size", std::to_string(o.w) + "x" + std::to_string(o.h));
    } else {
        unsave("window_size");
        save("window_scale", std::to_string(o.k));
    }
    std::fprintf(stderr, "[SETTINGS] window size %dx%d%s\n", o.w, o.h, is_fullscreen() ? " (for Window mode)" : "");
}
void apply_render_scale(int n) {
    n = std::max(1, std::min(n, k_max_rs));
    g_pending_rs = n;   // applied in tick once the game is parked
    save("render_scale", std::to_string(n));
}
void apply_fps(int i) {
    if (!g_fps.set || i < 0 || i >= g_fps.count) return;
    g_fps.set(g_fps.rates[i]);
    save("fps", std::to_string(g_fps.rates[i]));
}

// Voice language: the saved choice, else the launch environment; changes apply at the next start.
bool voice_jp_choice() {
    auto it = g_saved.find("voice");
    if (it != g_saved.end()) return it->second == "jp";
    const char* e = std::getenv("PSPRECOMP_VOICE");
    return e && (e[0] == 'j' || e[0] == 'J');
}
bool voice_at_start() {   // what this run was started with (the environment is final once the menu exists)
    static const bool v = [] { const char* e = std::getenv("PSPRECOMP_VOICE"); return e && (e[0] == 'j' || e[0] == 'J'); }();
    return v;
}
bool voice_row_wanted() { return g_voice.available && g_voice.available(); }
void apply_voice(bool jp) { save("voice", jp ? "jp" : "en"); }

// HD textures: the saved choice, else the launch environment (PSPRECOMP_TEX_REPLACE=0 turns them off), else on.
bool texpack_choice() {
    auto it = g_saved.find("hd_textures");
    if (it != g_saved.end()) return it->second != "0";
    const char* e = std::getenv("PSPRECOMP_TEX_REPLACE");
    return !(e && e[0] == '0');
}
bool texpack_at_start() {
    static const bool v = [] { const char* e = std::getenv("PSPRECOMP_TEX_REPLACE"); return !(e && e[0] == '0'); }();
    return v;
}
bool texpack_row_wanted() { return g_texpack.available && g_texpack.available(); }
void apply_texpack(bool on) { save("hd_textures", on ? "1" : "0"); }

// Update check: read by the launcher (setup/install.cpp update_check_enabled) from settings.ini; PSPRECOMP_UPDATE_CHECK=0 forces it off.
bool updates_choice() {
    auto it = g_saved.find("check_updates");
    if (it != g_saved.end()) return it->second != "0";
    const char* e = std::getenv("PSPRECOMP_UPDATE_CHECK");
    return !(e && e[0] == '0');
}
void apply_updates(bool on) { save("check_updates", on ? "1" : "0"); }

void build_rows();

void change(int d) {   // left / right on the selected row
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    NextStartNote note_;
    if (is_mod_row(g_rows[size_t(g_sel)])) { const size_t i = size_t(g_rows[size_t(g_sel)] - ROW_MOD0); if (i < g_mods.size() && g_mods[i].on != (d > 0)) mod_set_enabled(i, d > 0); return; }
    if (const EnvOpt* o = env_row_opt(g_rows[size_t(g_sel)])) { env_change(*o, d, false); g_dirty = true; return; }
    switch (g_rows[size_t(g_sel)]) {
    case ROW_DISPLAY:
        apply_display(d > 0);
        break;
    case ROW_RES: {
        int i = g_win_sel < 0 ? 0 : g_win_sel;
        if (g_res[size_t(i)].k < 0) {   // leave a hand-set size: the nearest listed size that way
            const int cw = g_res[size_t(i)].w;
            i = -1;
            for (int j = 0; j < int(g_res.size()); ++j) {
                const ResOpt& o = g_res[size_t(j)];
                if (o.k < 0) continue;
                if (d > 0 && o.w > cw) { i = j; break; }   // first larger
                if (d < 0 && o.w < cw) i = j;              // last smaller
            }
        } else {
            i += d;
            while (i >= 0 && i < int(g_res.size()) && g_res[size_t(i)].k < 0) i += d;
        }
        if (i >= 0 && i < int(g_res.size())) apply_res(i);
        break;
    }
    case ROW_SCALE: {
        const int n = (g_pending_rs ? g_pending_rs : ge_draw_render_scale()) + d;
        if (n >= 1 && n <= k_max_rs) apply_render_scale(n);
        break;
    }
    case ROW_WIDE: {
        const bool on = g_pending_wide >= 0 ? g_pending_wide != 0 : ge_draw_widescreen();
        if ((d > 0) != on) { g_pending_wide = d > 0 ? 1 : 0; save("widescreen", d > 0 ? "1" : "0"); }
        break;
    }
    case ROW_FPS: {
        const int i = fps_index() + d;
        if (i >= 0 && i < g_fps.count) apply_fps(i);
        break;
    }
    case ROW_RENDERER: if (renderer_vulkan_available(nullptr)) save("renderer", d > 0 ? "vulkan" : "gl"); break;   // left: OpenGL, right: Vulkan
    case ROW_GPU: {
        const std::string cur = gpu_choice();
        const std::vector<std::string> ids = gpu_ids(cur);
        int idx = 0;
        for (size_t i = 0; i < ids.size(); i++) if (ids[i] == cur) idx = int(i);
        const int n = idx + d;
        if (n >= 0 && n < int(ids.size())) gpu_choose(ids[size_t(n)]);
        break;
    }
    case ROW_GPU_GET: {
        const int n = int(psp_gpu::catalog().size());
        g_gpu_get_idx = std::min(std::max(g_gpu_get_idx + d, 0), std::max(n - 1, 0));
        break;
    }
    case ROW_GPU_DEL: break;
    case ROW_VOICE: apply_voice(d > 0); break;   // left: English, right: Japanese
    case ROW_TEXPACK: apply_texpack(d > 0); break;
    case ROW_UPDATES: apply_updates(d > 0); break;
    case ROW_TOUCH: {
        const int t = std::clamp(psp_touch_mode() + d, 0, 2);
        psp_touch_set_mode(t);
        save("touch", t == 0 ? "auto" : t == 1 ? "on" : "off");
        break;
    }
    case ROW_QUIT: break;
    case ROW_NET_MODE:
        if (g_net_internet != (d < 0)) {   // left: Internet, right: LAN
            g_net_internet = d < 0;
            net_apply();
            g_sel_page[page_id()] = g_sel;
            build_rows();   // the rows differ: server and room, or direct peers
        }
        break;
    case ROW_NET_SERVER: case ROW_NET_ROOM: case ROW_NET_PEERS: case ROW_NET_STATUS: break;
    case ROW_CLOUD_PROVIDER: {
        const int cur = int(g_cloud.provider), nx = std::clamp(cur + d, 0, 3);
        if (nx != cur) {
            g_cloud.provider = psp_cloud_svc::Provider(nx);
            cloud_apply();
            g_sel_page[page_id()] = g_sel;
            build_rows();   // the rows differ per provider
        }
        break;
    }
    case ROW_CLOUD_AUTO: if (g_cloud.auto_sync != (d > 0)) { g_cloud.auto_sync = d > 0; cloud_apply(); } break;
    case ROW_CLOUD_REPLAYS: if (g_cloud.replays != (d > 0)) { g_cloud.replays = d > 0; cloud_apply(); } break;
    case ROW_CLOUD_FOLDER: case ROW_CLOUD_URL: case ROW_CLOUD_USER: case ROW_CLOUD_PASS: case ROW_CLOUD_GOOGLE:
    case ROW_CLOUD_NOW: case ROW_CLOUD_STATUS: break;
    case ROW_FPSCOUNT:
        if ((d > 0) != ge_draw_fps_counter()) {
            ge_draw_set_fps_counter(d > 0);
            save("fps_counter", d > 0 ? "1" : "0");
        }
        break;
    case ROW_PERF:
        if ((d > 0) != ge_draw_perf_overlay()) {
            ge_draw_set_perf_overlay(d > 0);
            save("perf_overlay", d > 0 ? "1" : "0");
        }
        break;
    case ROW_FX:
        if (ge_draw_fx_available() && (d > 0) != ge_draw_fx_shaders()) ge_draw_set_fx_shaders(d > 0);
        break;
    case ROW_SYNC: {
        const int s = ge_draw_sync_mode() + d;
        if (s >= 0 && s <= 4) ge_draw_set_sync_mode(s);
        break;
    }
    case ROW_SOFT:
        if ((d > 0) != ge_soft_focus()) { ge_set_soft_focus(d > 0); save("soft_focus", d > 0 ? "1" : "0"); }
        break;
    case ROW_AA: {
        const int v = std::min(int(POST_AA_SMAA), std::max(0, psp_post_aa() + d));
        if (v != psp_post_aa()) { psp_post_set_aa(v); save("post_aa", std::to_string(v)); }
        break;
    }
    case ROW_SHARP: {
        const int v = std::min(4, std::max(0, psp_post_sharpen() + d));
        if (v != psp_post_sharpen()) { psp_post_set_sharpen(v); save("sharpen", std::to_string(v)); }
        break;
    }
    case ROW_BRIGHT: case ROW_CONTRAST: case ROW_SAT: {
        const Row r = g_rows[size_t(g_sel)];
        const int lo = r == ROW_SAT ? 0 : 50, hi = r == ROW_SAT ? 200 : 150;
        const int cur = r == ROW_BRIGHT ? psp_post_brightness() : r == ROW_CONTRAST ? psp_post_contrast() : psp_post_saturation();
        const int v = std::min(hi, std::max(lo, cur + d * 5));
        if (v == cur) break;
        if (r == ROW_BRIGHT) { psp_post_set_brightness(v); save("brightness", std::to_string(v)); }
        else if (r == ROW_CONTRAST) { psp_post_set_contrast(v); save("contrast", std::to_string(v)); }
        else { psp_post_set_saturation(v); save("saturation", std::to_string(v)); }
        break;
    }
    case ROW_HDR:
        if (!psp_hdr_available()) break;
        if ((d > 0) != psp_hdr_wanted()) { psp_hdr_set_wanted(d > 0); save("hdr", d > 0 ? "1" : "0"); }
        break;
    case ROW_HDR_SCENE:
        if (!psp_hdr_available() || !psp_hdr_wanted() || psp_hdr_encoding() == 0) break;
        if ((d > 0) != psp_hdr_scene_wanted()) { psp_hdr_scene_set_wanted(d > 0); save("hdr_scene", d > 0 ? "1" : "0"); g_pending_scene = true; }
        break;
    case ROW_HDR_GAMUT: {
        if (!psp_hdr_available() || !psp_hdr_wanted()) break;
        const int v = std::min(4, std::max(0, psp_hdr_gamut() + d));
        if (v != psp_hdr_gamut()) { psp_hdr_set_gamut(v); save("hdr_gamut", std::to_string(v)); }
        break;
    }
    case ROW_HDR_BOOST: {
        if (!psp_hdr_available() || !psp_hdr_wanted()) break;
        const int v = std::min(3, std::max(0, psp_hdr_boost() + d));
        if (v != psp_hdr_boost()) { psp_hdr_set_boost(v); save("hdr_boost", std::to_string(v)); }
        break;
    }
    case ROW_HDR_PEAK: {
        if (!psp_hdr_available() || !psp_hdr_wanted()) break;
        const int v = std::min(4000, std::max(400, psp_hdr_peak() + d * 100));
        if (v != psp_hdr_peak()) { psp_hdr_set_peak(v); save("hdr_peak", std::to_string(v)); }
        break;
    }
    case ROW_HDR_PAPER: {
        if (!psp_hdr_available() || !psp_hdr_wanted()) break;

        const int v = std::min(1000, std::max(80, psp_hdr_paper() + d * 10));
        if (v != psp_hdr_paper()) { psp_hdr_set_paper(v); save("hdr_paper", std::to_string(v)); }
        break;
    }
    case ROW_SSAO: {
        const int v = std::min(3, std::max(0, psp_fx_ssao() + d));
        if (v != psp_fx_ssao()) { psp_fx_set_ssao(v); save("ssao", std::to_string(v)); }
        break;
    }
    case ROW_CONTACT: {
        const int v = std::min(3, std::max(0, psp_fx_contact() + d));
        if (v != psp_fx_contact()) { psp_fx_set_contact(v); save("contact_shadows", std::to_string(v)); }
        break;
    }
    case ROW_RT_SOFT: {
        if (!psp_rt_available() || psp_rt_quality() < 2) break;
        const int v = ((psp_rt_softness() + (d > 0 ? 1 : -1)) % 5 + 5) % 5;   // loops: Very Soft -> Sharp and back
        if (v != psp_rt_softness()) { psp_rt_set_softness(v); save("rt_soft", std::to_string(v)); }
        break;
    }
    case ROW_RT: {
        if (!psp_rt_available()) break;
        const int was = psp_rt_quality(), v = std::min(3, std::max(0, was + d));
        if (v == was) break;
        psp_rt_set_quality(v);
        save("rt_shadow_level", std::to_string(v));
        const bool on = v > 0;
        if ((was > 0) == on) break;   // only the off <-> on edge touches the contact row
        // The ray-traced shadows replace the contact shadows: switch those off (remembering the level) and hide their row.
        if (on) {
            if (psp_fx_contact() > 0) save("contact_before_rt", std::to_string(psp_fx_contact()));
            psp_fx_set_contact(0); save("contact_shadows", "0");
        } else {
            auto it = g_saved.find("contact_before_rt");
            const int back = it != g_saved.end() ? std::atoi(it->second.c_str()) : 0;
            psp_fx_set_contact(back); save("contact_shadows", std::to_string(std::min(3, std::max(0, back))));
        }
        build_rows();
        break;
    }
    case ROW_RT_REFL: {
        if (!psp_rt_available()) break;
        const int v = std::min(3, std::max(0, psp_rt_refl_quality() + d));
        if (v == psp_rt_refl_quality()) break;
        psp_rt_refl_set_quality(v);
        save("rt_refl_level", std::to_string(v));
        build_rows();
        break;
    }
    case ROW_SSR: {
        const int v = std::min(3, std::max(0, psp_fx_ssr() + d));
        if (v != psp_fx_ssr()) { psp_fx_set_ssr(v); save("ssr", std::to_string(v)); }
        break;
    }
    case ROW_ANISO: {
        static const int lv[5] = {1, 2, 4, 8, 16};
        int i = 0;
        while (i < 4 && lv[i] < ge_texture_aniso()) ++i;
        i = std::min(4, std::max(0, i + d));
        if (lv[i] != ge_texture_aniso()) { ge_texture_set_aniso(lv[i]); save("aniso", std::to_string(lv[i])); }
        break;
    }
    case ROW_UPSCALE: {
        static const char* const keys[3] = {"bilinear", "sharp", "bicubic"};
        const int v = std::min(2, std::max(0, psp_post_upscale() + d));
        if (v != psp_post_upscale()) { psp_post_set_upscale(v); save("upscale", keys[v]); }
        break;
    }
    case ROW_GLOWSOFT: ge_draw_set_fb_prefilter(d > 0); break;
    case ROW_GLOWPASS: ge_draw_set_fb_snapshot(d > 0); break;
    case ROW_DBG0: case ROW_DBG1: case ROW_DBG2: case ROW_DBG3: case ROW_DBG4: case ROW_DBG5: {
        const size_t i = size_t(g_rows[size_t(g_sel)] - ROW_DBG0);
        if (i < g_dbg_toggles.size() && g_dbg_toggles[i].get() != (d > 0)) g_dbg_toggles[i].set(d > 0);
        break;
    }
    case ROW_RING: {
        const int nx = d > 0 ? 1 : 0;
        const int cur = g_ring_next >= 0 ? g_ring_next : ge_draw_ring_mode();
        if (nx != cur) {
            g_ring_next = nx;
            save("ge_ring", nx ? "map" : "sub");
        }
        break;
    }
    default: break;
    }
    g_dirty = true;
}

int g_built_page = -1;                 // the page build_rows last built (to keep the cursor on the same row when only the rows change)
void build_rows() {
    const bool had = g_built_page == page_id() && g_sel >= 0 && g_sel < int(g_rows.size());
    const Row keep = had ? g_rows[size_t(g_sel)] : ROW_SEC_WINDOW;
    g_rows.clear();
    auto add = [&](Row r) { g_rows.push_back(r); };
    switch (page_id()) {
    case PAGE_DISPLAY:
        add(ROW_SEC_WINDOW); add(ROW_DISPLAY); if (!is_fullscreen()) add(ROW_RES); add(ROW_WIDE); add(ROW_UPSCALE);
        add(ROW_SEC_PERF);
        if (g_fps.get && g_fps.count > 1) add(ROW_FPS);
        add(ROW_FPSCOUNT);
        break;
    case PAGE_GRAPHICS:
        add(ROW_SEC_QUALITY); add(ROW_SCALE); add(ROW_ANISO); add(ROW_SOFT);
        add(ROW_SEC_AA); add(ROW_AA); add(ROW_SHARP);
        if (texpack_row_wanted()) { add(ROW_SEC_TEX); add(ROW_TEXPACK); }
        if (renderer_row_wanted()) { add(ROW_SEC_ENGINE); add(ROW_RENDERER); }
        break;
    case PAGE_IMAGE:
        add(ROW_SEC_COLOUR); add(ROW_BRIGHT); add(ROW_CONTRAST); add(ROW_SAT);
        add(ROW_SEC_HDR); add(ROW_SUB_HDR);
        break;
    case PAGE_HDR:
        add(ROW_HDR); add(ROW_HDR_SCENE); add(ROW_HDR_BOOST); add(ROW_HDR_GAMUT); add(ROW_HDR_PEAK); add(ROW_HDR_PAPER);
        break;
    case PAGE_EFFECTS:
        add(ROW_SEC_LIGHT); add(ROW_SSAO);
        if (!psp_rt_active()) add(ROW_CONTACT);   // ray-traced shadows replace them
        if (!psp_rt_refl_active()) add(ROW_SSR);  // ray-traced reflections replace them
        add(ROW_SEC_RT); add(ROW_SUB_RT);
        break;
    case PAGE_RT:
        add(ROW_RT); add(ROW_RT_SOFT); add(ROW_RT_REFL);
        break;
    case PAGE_CONTROLS:
        add(ROW_SEC_TOUCH); add(ROW_TOUCH); add(ROW_TOUCH_EDIT);
        break;
    case PAGE_GPU:
        add(ROW_SEC_DRIVER); add(ROW_GPU); add(ROW_GPU_GET); add(ROW_GPU_DEL);
        break;
    case PAGE_NETWORK:
        net_load();
        add(ROW_SEC_SERVER); add(ROW_NET_MODE);
        if (g_net_internet) { add(ROW_NET_SERVER); add(ROW_NET_ROOM); }
        else add(ROW_NET_PEERS);
        add(ROW_NET_STATUS);
        break;
    case PAGE_CLOUD:
        cloud_load();
        if (psp_cloud_svc::initialised()) {
            using P = psp_cloud_svc::Provider;
            add(ROW_SEC_SYNC); add(ROW_CLOUD_PROVIDER);
            if (g_cloud.provider == P::Folder) add(ROW_CLOUD_FOLDER);
            else if (g_cloud.provider == P::WebDav) { add(ROW_CLOUD_URL); add(ROW_CLOUD_USER); add(ROW_CLOUD_PASS); }
            else if (g_cloud.provider == P::Google) add(ROW_CLOUD_GOOGLE);
            if (g_cloud.provider != P::Off) { add(ROW_CLOUD_AUTO); add(ROW_CLOUD_REPLAYS); add(ROW_CLOUD_NOW); }
            add(ROW_CLOUD_STATUS);
        } else add(ROW_CLOUD_STATUS);     // cloud sync is not running in this session (PSPRECOMP_CLOUD=0)
        break;
    case PAGE_MODS:
        mods_load();
        add(ROW_SEC_MODS);
        if (g_mods.empty()) add(ROW_MODS_NONE);
        for (size_t i = 0; i < g_mods.size(); i++) g_rows.push_back(Row(ROW_MOD0 + int(i)));
        break;
    case PAGE_DEBUG:
        add(ROW_PERF); add(ROW_FX); add(ROW_SYNC); add(ROW_RING); add(ROW_GLOWSOFT); add(ROW_GLOWPASS);
        for (size_t i = 0; i < g_dbg_toggles.size(); i++) g_rows.push_back(Row(ROW_DBG0 + int(i)));
        add(ROW_SEC_DEV);
        for (int c = 0; c < EC_COUNT; c++) g_rows.push_back(Row(ROW_SUB_ENV0 + c));
        break;
    case PAGE_ENV0: case PAGE_ENV0 + 1: case PAGE_ENV0 + 2: case PAGE_ENV0 + 3: case PAGE_ENV0 + 4: case PAGE_ENV0 + 5:
    case PAGE_ENV0 + 6: case PAGE_ENV0 + 7: case PAGE_ENV0 + 8: case PAGE_ENV0 + 9: case PAGE_ENV0 + 10: {
        const size_t n = std::min<size_t>(env_cat_opts(page_id() - PAGE_ENV0).size(), size_t(ROW_ENV_END - ROW_ENV0 + 1));
        for (size_t i = 0; i < n; i++) g_rows.push_back(Row(ROW_ENV0 + int(i)));
        break;
    }
    default:   // PAGE_SYSTEM
        add(ROW_SEC_GAME);
        if (voice_row_wanted()) add(ROW_VOICE);
        add(ROW_UPDATES);
        add(ROW_SEC_SESSION); add(ROW_QUIT);
        break;
    }
    g_sel = std::clamp(g_sel_page[page_id()], 0, std::max(0, int(g_rows.size()) - 1));
    if (had) for (size_t i = 0; i < g_rows.size(); ++i) if (g_rows[i] == keep) { g_sel = int(i); break; }   // same row, wherever it moved to
    g_built_page = page_id();
    while (g_sel + 1 < int(g_rows.size()) && is_section(g_rows[size_t(g_sel)])) ++g_sel;   // never rest on a caption
}

// ---- the screen ------------------------------------------------------------------------------------------------
psp_sddialog::SettingsView g_view;
std::vector<psp_sddialog::SetRow> g_set_rows;   // the rows as drawn (rebuilt when something changes, and every 250 ms)
int64_t g_rows_next = 0;
int g_device = 0;                      // 0 pad, 1 keyboard / mouse, 2 touch: what the hints show
int g_hover_row = -1, g_hover_cat = -1, g_hover_part = 0, g_press_row = -1;
bool g_hover_back = false;
bool g_dlg_open = false;               // the "restart now?" question is showing
int g_dlg_sel = 0, g_dlg_hover = -1;
bool g_focus_tabs = true;              // the category list (left) has the focus; Confirm / Right open the category, Back returns here
bool g_closing = false;                // the close animation is running (the game stays paused until it ends)
std::atomic<bool> g_request_open{false};
std::atomic<bool> g_gate_dirty{false};

void set_open(bool open);
bool restart_supported() {
#if defined(_WIN32) || defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}
// Starts the game again the way it was started (run.sh exports the command; without it the same program and arguments).
void restart_now() {
#if !defined(_WIN32) && !defined(__ANDROID__)
    std::fprintf(stderr, "[SETTINGS] restarting the game\n");
    std::fflush(stderr);
    if (const char* cmd = std::getenv("PSPRECOMP_RESTART_CMD")) { if (cmd[0]) execl("/bin/sh", "sh", "-c", cmd, static_cast<char*>(nullptr)); }
    else {
        std::ifstream f("/proc/self/cmdline", std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> args;
        for (size_t a = 0; a < all.size();) { const size_t z = all.find('\0', a); args.push_back(all.substr(a, z == std::string::npos ? std::string::npos : z - a)); if (z == std::string::npos) break; a = z + 1; }
        std::vector<char*> av;
        for (std::string& a : args) av.push_back(a.data());
        av.push_back(nullptr);
        if (!av.empty() && av[0]) execv("/proc/self/exe", av.data());
    }
    std::fprintf(stderr, "[SETTINGS] restart failed\n");
    ge_draw_toast("Could not restart: start the game again by hand", 3500);
#endif
}
std::vector<std::string> dialog_buttons() {
    if (restart_supported()) return {"Restart now", "Later"};
    return {"OK"};
}
void dialog_choose(int i) {
    const std::vector<std::string> b = dialog_buttons();
    g_dlg_open = false;
    g_dlg_hover = -1;
    if (restart_supported() && i == 0) { restart_now(); return; }   // only returns when it failed
    (void)b;
    g_restart_pending = false;   // asked once
    set_open(false);
}
void enter_sub(int page);
void go_back();

void rebuild(int dir) {   // a different page is shown
    if (g_edit.active) edit_end();
    build_rows();
    g_view.page_changed(dir);
    g_quit_armed = false;
    g_hover_row = -1; g_press_row = -1;
    g_dirty = true;
    g_rows_next = 0;
}
void goto_category(int idx) {
    const int n = int(page_list().size());
    idx = std::clamp(idx, 0, n - 1);
    if (idx == g_page && g_sub < 0) return;
    g_sel_page[page_id()] = g_sel;
    const int dir = idx > g_page ? 1 : idx < g_page ? -1 : 0;
    g_page = idx;
    g_sub = -1;
    rebuild(dir);
}
void switch_page(int dir) {   // L / R: the next / previous category; wraps around
    const int n = int(page_list().size());
    goto_category((g_page + dir + n) % n);
}
void enter_sub(int page) {
    g_sel_page[page_id()] = g_sel;
    g_sub = page;
    g_sel_page[page] = 0;
    rebuild(1);
}
void leave_sub() {
    g_sel_page[page_id()] = g_sel;
    g_sub = -1;
    rebuild(-1);
}
void go_back() {   // Back: cancel a text entry, else up one sub-menu level, else close
    if (g_dlg_open) { dialog_choose(int(dialog_buttons().size()) - 1); return; }
    if (g_edit.active) { edit_end(); return; }
    if (g_sub >= 0) { leave_sub(); return; }
    if (!g_focus_tabs) { g_focus_tabs = true; g_dirty = true; return; }   // from a category's rows back to the category list
    set_open(false);
}

void set_open(bool open) {
    if (open) {
        if (g_closing) { g_closing = false; g_view.set_open(true); return; }   // changed our mind during the close animation
        if (open == g_open.load()) return;
        if (psp_adhoc_connected() || psp_adhoc_in_session()) {   // opening pauses the game, which would stall the other players
            ge_draw_toast("Settings are unavailable during multiplayer", 2500);
            std::fprintf(stderr, "[SETTINGS] menu not opened: in a multiplayer session\n");
            return;
        }
        build_res_options();
        pick_window_size();
        net_load();
        g_pages.clear();
        const char* tm = std::getenv("PSPRECOMP_TOUCH");
        g_touch_ui = SDL_GetNumTouchDevices() > 0 || psp_touch_enabled() || (tm && (!std::strcmp(tm, "on") || !std::strcmp(tm, "auto") || !std::strcmp(tm, "1")));
        g_page = std::clamp(g_page, 0, int(page_list().size()) - 1);
        g_sub = -1;
        g_focus_tabs = true;
        g_dlg_open = false;
        g_built_page = -1;
        build_rows();
        g_view = psp_sddialog::SettingsView();
        g_view.set_open(true);
        g_quit_armed = false;
        g_dirty = true;
        g_rows_next = 0;
        g_dir_held = 0;
        g_press_row = g_hover_row = g_hover_cat = -1; g_hover_back = false;
        g_open = true;
        std::fprintf(stderr, "[SETTINGS] menu open (game paused)\n");
    } else {
        if (!g_open.load() || g_closing) return;
        if (g_dlg_open) { dialog_choose(int(dialog_buttons().size()) - 1); return; }   // Back / Esc on the question = Later
        if (g_restart_pending && !g_should_exit.load() && !g_edit.active) {   // changes that need a restart: ask before closing
            g_dlg_open = true; g_dlg_sel = 0; g_dlg_hover = -1;
            return;
        }
        g_sel_page[page_id()] = g_sel;
        if (g_edit.active) edit_end();
        g_closing = true;
        g_view.set_open(false);
        std::fprintf(stderr, "[SETTINGS] menu closing\n");
    }
}
void finish_close() {   // the close animation has ended: give the game back
    g_closing = false;
    g_open = false;
    g_wait_release = true;
    psp_sddialog::menu_overlay_clear();
    ge_draw_present_overlay_now();
    g_gate_dirty.store(true);
    std::fprintf(stderr, "[SETTINGS] menu closed\n");
}

void move_sel(int d) {
    if (g_edit.active) { edit_move(d, 0, &g_view.geom()); return; }
    const int n = int(g_rows.size());
    if (!n) return;
    g_view.follow_selection();
    int s = g_sel;
    for (int i = 0; i < n; ++i) {   // skip captions
        s = (s + d + n) % n;
        if (!is_section(g_rows[size_t(s)])) break;
    }
    if (s != g_sel) { g_sel = s; g_quit_armed = false; g_dirty = true; }
}

void confirm_row() {
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    NextStartNote note_;
    if (is_mod_row(g_rows[size_t(g_sel)])) { const size_t i = size_t(g_rows[size_t(g_sel)] - ROW_MOD0); if (i < g_mods.size()) mod_set_enabled(i, !g_mods[i].on); g_dirty = true; return; }
    if (g_rows[size_t(g_sel)] >= ROW_SUB_ENV0 && g_rows[size_t(g_sel)] <= ROW_SUB_ENV_END) { enter_sub(PAGE_ENV0 + int(g_rows[size_t(g_sel)] - ROW_SUB_ENV0)); return; }
    if (const EnvOpt* o = env_row_opt(g_rows[size_t(g_sel)])) {
        env_change(*o, o->type == 'c' ? 1 : (env_is_on(*o) ? -1 : 1), true);
        g_dirty = true;
        return;
    }
    switch (g_rows[size_t(g_sel)]) {
    case ROW_SUB_HDR: enter_sub(PAGE_HDR); return;
    case ROW_SUB_RT: enter_sub(PAGE_RT); return;
    case ROW_QUIT:
        if (!g_quit_armed) { g_quit_armed = true; g_dirty = true; return; }
        std::fprintf(stderr, "[SETTINGS] quit from the settings menu\n");
        g_should_exit.store(true);
        set_open(false);
        return;
    case ROW_DISPLAY:
        apply_display(!is_fullscreen());
        break;
    case ROW_RES: {   // cycle forward
        const int i = g_win_sel < 0 ? -1 : g_win_sel;
        int j = (i + 1) % int(g_res.size());
        while (g_res[size_t(j)].k < 0) j = (j + 1) % int(g_res.size());
        apply_res(j);
        break;
    }
    case ROW_SCALE: {
        const int n = g_pending_rs ? g_pending_rs : ge_draw_render_scale();
        apply_render_scale(n % k_max_rs + 1);
        break;
    }
    case ROW_WIDE: change((g_pending_wide >= 0 ? g_pending_wide != 0 : ge_draw_widescreen()) ? -1 : 1); return;
    case ROW_FPS:
        apply_fps((fps_index() + 1) % g_fps.count);
        break;
    case ROW_RENDERER: if (renderer_vulkan_available(nullptr)) save("renderer", renderer_vulkan_choice() ? "gl" : "vulkan"); break;
    case ROW_GPU: {   // cycles round: system, each installed driver
        const std::string cur = gpu_choice();
        const std::vector<std::string> ids = gpu_ids(cur);
        size_t idx = 0;
        for (size_t i = 0; i < ids.size(); i++) if (ids[i] == cur) idx = i;
        gpu_choose(ids[(idx + 1) % ids.size()]);
        break;
    }
    case ROW_GPU_GET: {
        if (psp_gpu::status().state == psp_gpu::Status::Busy) break;
        const std::vector<psp_gpu::Remote> cat = psp_gpu::catalog();
        if (!psp_gpu::catalog_loaded()) { psp_gpu::refresh(); break; }
        if (cat.empty()) break;
        const psp_gpu::Remote& r = cat[size_t(std::min(std::max(g_gpu_get_idx, 0), int(cat.size()) - 1))];
        g_gpu_installing = r.id;
        psp_gpu::install(r);
        break;
    }
    case ROW_GPU_DEL: {
        const std::string cur = gpu_choice();
        if (cur.empty()) break;
        psp_gpu::remove(cur);
        gpu_choose("");
        break;
    }
    case ROW_VOICE: apply_voice(!voice_jp_choice()); break;
    case ROW_TEXPACK: apply_texpack(!texpack_choice()); break;
    case ROW_UPDATES: apply_updates(!updates_choice()); break;
    case ROW_TOUCH: change(psp_touch_mode() >= 2 ? -2 : 1); return;
    case ROW_TOUCH_EDIT: psp_touch_edit_begin(); return;
    case ROW_NET_MODE: change(g_net_internet ? 1 : -1); return;
    case ROW_NET_SERVER: edit_begin(0); return;
    case ROW_NET_ROOM: edit_begin(1); return;
    case ROW_NET_PEERS: edit_begin(2); return;
    case ROW_NET_STATUS: break;
    case ROW_CLOUD_PROVIDER: change(g_cloud.provider == psp_cloud_svc::Provider::Google ? -3 : 1); return;   // cycles round
    case ROW_CLOUD_FOLDER: edit_begin(3); return;
    case ROW_CLOUD_URL: edit_begin(4); return;
    case ROW_CLOUD_USER: edit_begin(5); return;
    case ROW_CLOUD_PASS: edit_begin(6); return;
    case ROW_CLOUD_GOOGLE:
        if (psp_cloud_svc::status().state == psp_cloud_svc::Status::SigningIn) psp_cloud_svc::signin_cancel();
        else if (g_cloud.google_refresh.empty()) psp_cloud_svc::signin_start();
        else { psp_cloud_svc::signout(); cloud_load(); g_sel_page[page_id()] = g_sel; build_rows(); }
        break;
    case ROW_CLOUD_AUTO: change(g_cloud.auto_sync ? -1 : 1); return;
    case ROW_CLOUD_REPLAYS: change(g_cloud.replays ? -1 : 1); return;
    case ROW_CLOUD_NOW:
        if (psp_cloud_svc::configured() && psp_cloud_svc::status().state != psp_cloud_svc::Status::Syncing) psp_cloud_svc::sync_now();
        break;
    case ROW_CLOUD_STATUS: break;
    case ROW_FPSCOUNT: change(ge_draw_fps_counter() ? -1 : 1); return;
    case ROW_PERF: change(ge_draw_perf_overlay() ? -1 : 1); return;
    case ROW_FX: change(ge_draw_fx_shaders() ? -1 : 1); return;
    case ROW_SYNC: ge_draw_set_sync_mode((ge_draw_sync_mode() + 1) % 5); break;
    case ROW_RING: change((g_ring_next >= 0 ? g_ring_next : ge_draw_ring_mode()) ? -1 : 1); return;
    case ROW_SOFT: change(ge_soft_focus() ? -1 : 1); return;
    case ROW_AA: change(psp_post_aa() >= POST_AA_SMAA ? -int(POST_AA_SMAA) : 1); return;
    case ROW_SHARP: change(psp_post_sharpen() >= 4 ? -4 : 1); return;
    case ROW_HDR: if (psp_hdr_available()) change(psp_hdr_wanted() ? -1 : 1); return;
    case ROW_HDR_SCENE: if (psp_hdr_available() && psp_hdr_wanted() && psp_hdr_encoding() != 0) change(psp_hdr_scene_wanted() ? -1 : 1); return;
    case ROW_SSAO: change(psp_fx_ssao() >= 3 ? -3 : 1); return;
    case ROW_CONTACT: change(psp_fx_contact() >= 3 ? -3 : 1); return;
    case ROW_RT_SOFT: if (psp_rt_available() && psp_rt_quality() >= 2) change(1); return;
    case ROW_RT: if (psp_rt_available()) change(psp_rt_quality() >= 3 ? -3 : 1); return;
    case ROW_RT_REFL: if (psp_rt_available()) change(psp_rt_refl_quality() >= 3 ? -3 : 1); return;
    case ROW_SSR: change(psp_fx_ssr() >= 3 ? -3 : 1); return;
    case ROW_ANISO: change(ge_texture_aniso() >= 16 ? -4 : 1); return;
    case ROW_UPSCALE: change(psp_post_upscale() >= 2 ? -2 : 1); return;
    case ROW_GLOWSOFT: change(ge_draw_fb_prefilter() ? -1 : 1); return;
    case ROW_GLOWPASS: change(ge_draw_fb_snapshot() ? -1 : 1); return;
    case ROW_DBG0: case ROW_DBG1: case ROW_DBG2: case ROW_DBG3: case ROW_DBG4: case ROW_DBG5: {
        const size_t i = size_t(g_rows[size_t(g_sel)] - ROW_DBG0);
        if (i < g_dbg_toggles.size()) change(g_dbg_toggles[i].get() ? -1 : 1);
        return;
    }
    default: break;
    }
    g_dirty = true;
}


void confirm() {   // Confirm: open the highlighted category, or act on the highlighted row
    if (g_dlg_open) { dialog_choose(g_dlg_sel); return; }
    if (g_focus_tabs && g_sub < 0) { g_focus_tabs = false; g_dirty = true; return; }
    confirm_row();
}

void nav(uint32_t dir) {
    g_device = 0;
    if (g_dlg_open) {
        const int n = int(dialog_buttons().size());
        if (dir & (1 | 4)) g_dlg_sel = std::max(0, g_dlg_sel - 1);
        else if (dir & (2 | 8)) g_dlg_sel = std::min(n - 1, g_dlg_sel + 1);
        return;
    }
    if (g_edit.active) {
        const psp_sddialog::SetGeom* g = &g_view.geom();
        if (dir & 1) edit_move(-1, 0, g);
        else if (dir & 2) edit_move(1, 0, g);
        else if (dir & 4) edit_move(0, -1, g);
        else if (dir & 8) edit_move(0, 1, g);
        return;
    }
    if (g_focus_tabs && g_sub < 0) {   // the category list: up / down choose, right (or Confirm) goes in
        if (dir & 1) switch_page(-1);
        else if (dir & 2) switch_page(1);
        else if (dir & 8) { g_focus_tabs = false; g_dirty = true; }
        return;
    }
    if (dir & 1) move_sel(-1);
    else if (dir & 2) move_sel(1);
    else if (dir & 4) change(-1);
    else if (dir & 8) change(1);
}

uint32_t stick_dir() {
    const int t = 16000;
    if (g_ly < -t) return 1;
    if (g_ly > t) return 2;
    if (g_lx < -t) return 4;
    if (g_lx > t) return 8;
    return 0;
}
void held_dir_update() {   // pad d-pad / stick with repeat (keyboard uses the OS key repeat)
    const uint32_t d = g_pad_dpad ? g_pad_dpad : stick_dir();
    if (d == 0) { g_dir_held = 0; return; }
    const int64_t t = now_ms();
    if (d != g_dir_held) { g_dir_held = d; g_dir_next = t + 380; nav(d); }
    else if (t >= g_dir_next) { g_dir_next = t + 110; nav(d); }
}

uint32_t dpad_bit(uint8_t b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 4;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 8;
    default: return 0;
    }
}

// ---- mouse and touch -----------------------------------------------------------------------------------------------
// Pointer positions arrive as fractions of the window; the overlay occupies the picture's rectangle (ge_draw_overlay_rect), so they are
// mapped to 480x272 canvas units, the space SettingsView's geometry is in.
bool to_canvas(float fx, float fy, float* ux, float* uy) {
    float a, b, c, d;
    ge_draw_overlay_rect(&a, &b, &c, &d);
    if (c <= a || d <= b) return false;
    *ux = (fx - a) / (c - a) * float(psp_sddialog::SCREEN_W);
    *uy = (fy - b) / (d - b) * float(psp_sddialog::SCREEN_H);
    return true;
}
bool mouse_to_canvas(int mx, int my, float* ux, float* uy) {
    int ww = 0, wh = 0;
    if (!win()) return false;
    SDL_GetWindowSize(win(), &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;
    return to_canvas(float(mx) / float(ww), float(my) / float(wh), ux, uy);
}

struct Pointer {
    int mode = 0;               // 0 idle, 1 a row is held, 2 dragging a slider, 3 dragging the list, 4 dragging the scroll bar
    int row = -1;
    float x0 = 0, y0 = 0, scroll0 = 0, last_y = 0, vy = 0;
    int64_t last_t = 0;
    bool moved = false, touch = false;
    SDL_FingerID finger = 0;
} g_ptr;

const psp_sddialog::SetGeom::Row* geom_row(int idx) {
    for (const auto& r : g_view.geom().rows) if (r.idx == idx) return &r;
    return nullptr;
}
void slider_drag_to(int idx, float ux) {   // drag: set the slider's position from x
    const auto* gr = geom_row(idx);
    if (!gr || idx < 0 || idx >= int(g_rows.size()) || idx >= int(g_set_rows.size())) return;
    int steps = 0, cur = 0;
    const Row r = g_rows[size_t(idx)];
    if (!slider_of(r, &steps, &cur) || steps < 2) return;
    const float f = std::clamp((ux - gr->track.x - 3.f) / std::max(1.f, gr->track.w - 6.f), 0.f, 1.f);
    const int target = int(std::lround(f * float(steps - 1)));
    g_sel = idx;
    for (int guard = 0; cur != target && guard < steps + 2; ++guard) {
        const int before = cur;
        change(target > cur ? 1 : -1);
        slider_of(r, &steps, &cur);
        if (cur == before) break;   // the row refused (disabled, at an end)
    }
}
void row_tap(int idx) {   // a click / tap on a row's body
    if (idx < 0 || idx >= int(g_rows.size()) || idx >= int(g_set_rows.size())) return;
    using K = psp_sddialog::SetKind;
    const K k = g_set_rows[size_t(idx)].kind;
    g_sel = idx;
    g_quit_armed = g_quit_armed && g_rows[size_t(idx)] == ROW_QUIT;
    g_focus_tabs = false;
    if (k == K::Header || k == K::Info || k == K::Slider) return;   // sliders are set by dragging or with their arrows
    confirm_row();
}

void pointer_hover(float ux, float uy) {
    const auto& G = g_view.geom();
    if (g_dlg_open) { g_dlg_hover = -1; for (size_t i = 0; i < G.dlg_btns.size(); ++i) if (G.dlg_btns[i].has(ux, uy)) g_dlg_hover = int(i); return; }
    int row = -1, part = 0, cat = -1;
    bool back = G.back.has(ux, uy);
    if (!g_edit.active) {
        for (size_t i = 0; i < G.cats.size(); ++i) if (G.cats[i].has(ux, uy) && G.side.has(ux, uy)) cat = int(i);
        if (G.list.has(ux, uy))
            for (const auto& r : G.rows)
                if (r.r.has(ux, uy) && r.idx < int(g_set_rows.size()) && g_set_rows[size_t(r.idx)].kind != psp_sddialog::SetKind::Header) {
                    row = r.idx;
                    if (r.dec.has(ux, uy) && g_set_rows[size_t(r.idx)].kind != psp_sddialog::SetKind::Toggle) part = 1;
                    if (r.inc.has(ux, uy) && g_set_rows[size_t(r.idx)].kind != psp_sddialog::SetKind::Toggle) part = 2;
                    if (r.track.w > 0 && r.track.has(ux, uy)) part = 3;
                }
    }
    g_hover_row = row; g_hover_part = part; g_hover_cat = cat; g_hover_back = back;
}

void pointer_press(float ux, float uy, bool touch) {
    g_device = touch ? 2 : 1;
    pointer_hover(ux, uy);
    const auto& G = g_view.geom();
    if (g_dlg_open) {
        for (size_t i = 0; i < G.dlg_btns.size(); ++i) if (G.dlg_btns[i].has(ux, uy)) { g_dlg_sel = int(i); g_ptr = Pointer{}; dialog_choose(int(i)); return; }
        return;
    }
    g_ptr = Pointer{};
    g_ptr.touch = touch; g_ptr.x0 = ux; g_ptr.y0 = uy; g_ptr.last_y = uy; g_ptr.last_t = now_ms();
    if (g_edit.active) {
        for (size_t r = 0; r < G.keys.size(); ++r)
            for (size_t k = 0; k < G.keys[r].size(); ++k)
                if (G.keys[r][k].r.has(ux, uy)) { g_edit.row = int(r); g_edit.col = int(k); edit_press_focus(); return; }
        return;
    }
    if (G.back.has(ux, uy)) { go_back(); return; }
    for (size_t i = 0; i < G.cats.size(); ++i)
        if (G.cats[i].has(ux, uy) && G.side.has(ux, uy)) { goto_category(int(i)); g_focus_tabs = false; g_dirty = true; return; }
    if (G.scroll_thumb.w > 0 && G.scroll_thumb.has(ux, uy)) { g_ptr.mode = 4; g_ptr.scroll0 = g_view.scroll_pos(); return; }
    if (G.scroll_track.w > 0 && G.scroll_track.has(ux, uy)) {   // jump to that part of the list, then drag
        const float f = std::clamp((uy - G.scroll_track.y) / std::max(1.f, G.scroll_track.h), 0.f, 1.f);
        g_view.set_scroll_pos(f * g_view.scroll_max());
        g_ptr.mode = 4; g_ptr.scroll0 = g_view.scroll_pos();
        return;
    }
    if (G.list.has(ux, uy)) {
        for (const auto& r : G.rows) {
            if (!r.r.has(ux, uy) || r.idx >= int(g_set_rows.size()) || r.idx >= int(g_rows.size())) continue;
            const auto& row = g_set_rows[size_t(r.idx)];
            if (row.kind == psp_sddialog::SetKind::Header) break;
            g_sel = r.idx;
            g_focus_tabs = false;
            g_quit_armed = g_quit_armed && g_rows[size_t(r.idx)] == ROW_QUIT;
            g_view.follow_selection();
            const bool arrows = row.kind == psp_sddialog::SetKind::Choice || row.kind == psp_sddialog::SetKind::Slider;
            if (arrows && r.dec.w > 0 && r.dec.has(ux, uy)) { change(-1); return; }
            if (arrows && r.inc.w > 0 && r.inc.has(ux, uy)) { change(1); return; }
            if (row.kind == psp_sddialog::SetKind::Slider && r.track.w > 0 && r.track.has(ux, uy)) {
                g_ptr.mode = 2; g_ptr.row = r.idx;
                slider_drag_to(r.idx, ux);
                return;
            }
            g_ptr.mode = 1; g_ptr.row = r.idx; g_press_row = r.idx;
            g_dirty = true;
            return;
        }
        g_ptr.mode = 3; g_ptr.scroll0 = g_view.scroll_pos();   // empty space: drag the list
        g_view.fling(0.f);
    }
}
void pointer_move(float ux, float uy) {
    if (!g_ptr.touch || g_ptr.mode == 0) pointer_hover(ux, uy);
    if (g_ptr.mode == 0) return;
    const int64_t t = now_ms();
    const float dy = uy - g_ptr.y0, dx = ux - g_ptr.x0;
    if (g_ptr.mode == 2) { slider_drag_to(g_ptr.row, ux); return; }
    if (g_ptr.mode == 1) {   // a held row turns into a list drag once the pointer travels
        if (std::fabs(dy) > 4.f || std::fabs(dx) > 8.f) {
            if (std::fabs(dy) >= std::fabs(dx)) { g_ptr.mode = 3; g_ptr.scroll0 = g_view.scroll_pos(); g_press_row = -1; g_ptr.moved = true; }
            else if (!g_ptr.touch) { g_press_row = -1; g_ptr.mode = 0; }
        }
        if (g_ptr.mode == 1) return;
    }
    if (g_ptr.mode == 3) {
        if (std::fabs(dy) > 3.f) g_ptr.moved = true;
        if (g_ptr.moved) {
            g_view.set_scroll_pos(std::clamp(g_ptr.scroll0 - dy, 0.f, g_view.scroll_max()));
            const float dt = float(t - g_ptr.last_t) / 1000.f;
            if (dt > 0.001f) g_ptr.vy = 0.6f * g_ptr.vy + 0.4f * ((g_ptr.last_y - uy) / dt);
        }
        g_ptr.last_y = uy; g_ptr.last_t = t;
    } else if (g_ptr.mode == 4) {
        const auto& G = g_view.geom();
        const float room = std::max(1.f, G.scroll_track.h - 8.f - (G.scroll_thumb.h - 4.f));
        g_view.set_scroll_pos(std::clamp(g_ptr.scroll0 + dy * g_view.scroll_max() / room, 0.f, g_view.scroll_max()));
    }
}
void pointer_release(float ux, float uy) {
    (void)ux; (void)uy;
    if (g_ptr.mode == 1 && !g_ptr.moved) row_tap(g_ptr.row);
    else if (g_ptr.mode == 3 && g_ptr.touch && g_ptr.moved && std::fabs(g_ptr.vy) > 30.f && now_ms() - g_ptr.last_t < 120) g_view.fling(std::clamp(g_ptr.vy, -700.f, 700.f));
    g_ptr = Pointer{};
    g_press_row = -1;
    g_dirty = true;
}

}  // namespace

void psp_settings_set_frame_rate_hook(const PspFrameRateHook& hook) { g_fps = hook; }
void psp_settings_set_voice_hook(const PspVoiceHook& hook) { g_voice = hook; }
void psp_settings_add_debug_toggle(const PspDebugToggle& t) { if (g_dbg_toggles.size() < 6) g_dbg_toggles.push_back(t); }
void psp_settings_set_texpack_hook(const PspTexPackHook& hook) { g_texpack = hook; }

void psp_settings_load_early() {
    const char* e = std::getenv("PSPRECOMP_SETTINGS_MENU");
    g_enabled = !(e && e[0] == '0');
    e = std::getenv("PSPRECOMP_SWAP_AB");
    g_confirm_cross = !(e && e[0] && e[0] != '0');
    read_file();
    if (g_saved.empty()) return;
    std::string applied;
    auto put = [&](const char* var, const std::string& v) {
        psp_plat::setenv_(var, v.c_str());
        applied += std::string(" ") + var + "=" + v;
    };
    for (const auto& kv : g_saved)   // developer options (Debug page): env.<NAME> = value; empty = the default (not set)
        if (kv.first.compare(0, 4, "env.") == 0 && kv.first.size() > 4) {
            if (kv.second.empty()) psp_plat::unsetenv_(("PSPRECOMP_" + kv.first.substr(4)).c_str());
            else put(("PSPRECOMP_" + kv.first.substr(4)).c_str(), kv.second);
        }
    auto it = g_saved.find("display");
    if (it != g_saved.end() && it->second == "fullscreen") put("PSPRECOMP_FULLSCREEN", "1");
    else if (it != g_saved.end() && it->second == "window") {
        put("PSPRECOMP_FULLSCREEN", "0");
        auto k = g_saved.find("window_scale");
        if (k != g_saved.end() && std::atoi(k->second.c_str()) > 0) { put("PSPRECOMP_SCALE", k->second); psp_plat::unsetenv_("PSPRECOMP_WINDOW"); }
    }
    if ((it = g_saved.find("render_scale")) != g_saved.end() && std::atoi(it->second.c_str()) > 0)
        put("PSPRECOMP_RENDER_SCALE", it->second);
    if ((it = g_saved.find("widescreen")) != g_saved.end()) put("PSPRECOMP_WIDESCREEN", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("soft_focus")) != g_saved.end()) put("PSPRECOMP_SOFT_FOCUS", it->second == "0" ? "0" : "1");
    if ((it = g_saved.find("post_aa")) != g_saved.end()) put("PSPRECOMP_POST_AA", it->second == "2" ? "2" : it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("aniso")) != g_saved.end()) put("PSPRECOMP_ANISO", it->second);
    if ((it = g_saved.find("sharpen")) != g_saved.end()) put("PSPRECOMP_SHARPEN", it->second);
    if ((it = g_saved.find("brightness")) != g_saved.end()) put("PSPRECOMP_BRIGHTNESS", it->second);
    if ((it = g_saved.find("contrast")) != g_saved.end()) put("PSPRECOMP_CONTRAST", it->second);
    if ((it = g_saved.find("saturation")) != g_saved.end()) put("PSPRECOMP_SATURATION", it->second);
    if ((it = g_saved.find("hdr")) != g_saved.end()) put("PSPRECOMP_HDR", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("hdr_scene")) != g_saved.end()) put("PSPRECOMP_HDR_SCENE", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("hdr_gamut")) != g_saved.end()) put("PSPRECOMP_HDR_GAMUT", it->second);
    if ((it = g_saved.find("hdr_boost")) != g_saved.end()) put("PSPRECOMP_HDR_BOOST", it->second);
    if ((it = g_saved.find("hdr_peak")) != g_saved.end()) put("PSPRECOMP_HDR_PEAK", it->second);
    if ((it = g_saved.find("hdr_paper")) != g_saved.end()) put("PSPRECOMP_HDR_PAPER", it->second);
    if ((it = g_saved.find("ssao")) != g_saved.end()) put("PSPRECOMP_SSAO", it->second);
    if ((it = g_saved.find("contact_shadows")) != g_saved.end()) put("PSPRECOMP_CONTACT_SHADOWS", it->second);
    // A saved level of 0 is the default: exporting PSPRECOMP_VK_RT=0 / PSPRECOMP_VK_RT_REFL=0 would leave ray query out of the Vulkan
    // device (see vkgl_core.cpp want_ray_query), and the settings rows then say "Turned off for this run" with no way to switch RT back on.
    if ((it = g_saved.find("rt_soft")) != g_saved.end()) psp_rt_set_softness(std::atoi(it->second.c_str()));
    if ((it = g_saved.find("rt_shadow_level")) != g_saved.end()) { if (it->second != "0") put("PSPRECOMP_VK_RT", it->second); }
    else if ((it = g_saved.find("rt_shadows")) != g_saved.end() && it->second == "1") put("PSPRECOMP_VK_RT", "2");   // before the levels: on = Medium
    if ((it = g_saved.find("rt_refl_level")) != g_saved.end()) { if (it->second != "0") put("PSPRECOMP_VK_RT_REFL", it->second); }
    else if ((it = g_saved.find("rt_reflections")) != g_saved.end() && it->second == "1") put("PSPRECOMP_VK_RT_REFL", "2");   // before the levels: on = Medium
    if ((it = g_saved.find("ssr")) != g_saved.end()) put("PSPRECOMP_SSR", it->second);
    if ((it = g_saved.find("upscale")) != g_saved.end() && (it->second == "bilinear" || it->second == "sharp" || it->second == "bicubic"))
        put("PSPRECOMP_UPSCALE", it->second);
    if ((it = g_saved.find("fps")) != g_saved.end() && std::atoi(it->second.c_str()) > 0) put("PSPRECOMP_FPS", it->second);
    if ((it = g_saved.find("voice")) != g_saved.end() && (it->second == "jp" || it->second == "en"))
        put("PSPRECOMP_VOICE", it->second);
    if ((it = g_saved.find("hd_textures")) != g_saved.end() && (it->second == "0" || it->second == "1"))
        put("PSPRECOMP_TEX_REPLACE", it->second);
    if ((it = g_saved.find("renderer")) != g_saved.end() && (it->second == "gl" || it->second == "vulkan")) put("PSPRECOMP_RENDERER", it->second);
    if ((it = g_saved.find("gpu_driver")) != g_saved.end()) {
        if (it->second.empty()) psp_plat::unsetenv_("PSPRECOMP_GPU_DRIVER");
        else put("PSPRECOMP_GPU_DRIVER", it->second);
    }
    if ((it = g_saved.find("touch")) != g_saved.end() && (it->second == "auto" || it->second == "on" || it->second == "off"))
        put("PSPRECOMP_TOUCH", it->second);
    if ((it = g_saved.find("window_size")) != g_saved.end() && g_saved.count("display") && g_saved["display"] == "window"
        && it->second.find('x') != std::string::npos) {
        put("PSPRECOMP_WINDOW", it->second);
        psp_plat::unsetenv_("PSPRECOMP_SCALE");
    }
    if ((it = g_saved.find("net_mode")) != g_saved.end() && (it->second == "internet" || it->second == "lan"))
        put("PSPRECOMP_ADHOC_MODE", it->second);
    if ((it = g_saved.find("net_server")) != g_saved.end() && !it->second.empty()) put("PSPRECOMP_ADHOC_SERVER", it->second);
    if ((it = g_saved.find("net_room")) != g_saved.end()) put("PSPRECOMP_ADHOC_ROOM", it->second);
    if ((it = g_saved.find("net_peers")) != g_saved.end()) put("PSPRECOMP_ADHOC_PEERS", it->second);
    if ((it = g_saved.find("fps_counter")) != g_saved.end()) put("PSPRECOMP_FPS_COUNTER", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("perf_overlay")) != g_saved.end()) put("PSPRECOMP_PERF_HUD", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("ge_ring")) != g_saved.end() && (it->second == "map" || it->second == "sub"))
        put("PSPRECOMP_GE_RING", it->second);
    std::fprintf(stderr, "[SETTINGS] %s:%s\n", settings_path().c_str(), applied.c_str());
}

bool psp_settings_menu_open() { return g_open.load(); }

void psp_settings_request_open() { g_request_open.store(true); }
bool psp_settings_take_gate_dirty() { return g_gate_dirty.exchange(false); }

bool psp_settings_handle_event(const SDL_Event& ev) {
    if (!g_enabled) return false;
    const bool open = g_open.load();
    switch (ev.type) {
    case SDL_KEYDOWN: {
        const SDL_Keycode k = ev.key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_F1) {
            if (ev.key.repeat) return true;
            if (!open) { g_device = 1; set_open(true); return true; }
            if (g_closing) { return true; }
            if (g_edit.active) { edit_end(); return true; }   // cancel the text entry only
            if (k == SDLK_F1) set_open(false); else go_back();
            return true;
        }
        if (!open || g_closing) return false;
        g_device = 1;
        if (g_edit.active) {   // text arrives as SDL_TEXTINPUT; only navigation keys act here
            const psp_sddialog::SetGeom* g = &g_view.geom();
            switch (k) {
            case SDLK_UP: edit_move(-1, 0, g); break;
            case SDLK_DOWN: edit_move(1, 0, g); break;
            case SDLK_LEFT: edit_move(0, -1, g); break;
            case SDLK_RIGHT: edit_move(0, 1, g); break;
            case SDLK_BACKSPACE: edit_backspace(); break;
            case SDLK_RETURN: case SDLK_KP_ENTER: if (!ev.key.repeat) edit_commit(); break;
            default: break;
            }
            return false;
        }
        switch (k) {
        case SDLK_UP: nav(1); g_device = 1; break;
        case SDLK_DOWN: nav(2); g_device = 1; break;
        case SDLK_LEFT: nav(4); g_device = 1; break;
        case SDLK_RIGHT: nav(8); g_device = 1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: case SDLK_SPACE:
            if (!ev.key.repeat) confirm();
            break;
        case SDLK_z: case SDLK_BACKSPACE:
            if (!ev.key.repeat) go_back();
            break;
        case SDLK_q: case SDLK_PAGEUP:
            if (!ev.key.repeat) switch_page(-1);
            break;
        case SDLK_e: case SDLK_TAB: case SDLK_PAGEDOWN:
            if (!ev.key.repeat) switch_page(1);
            break;
        default: break;
        }
        return false;   // the event loop still tracks key state (the game sees none while blocked)
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const bool down = ev.type == SDL_CONTROLLERBUTTONDOWN;
        const uint8_t b = ev.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_LEFTSTICK) g_l3 = down;
        if (b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) g_r3 = down;
        if (down && (b == SDL_CONTROLLER_BUTTON_GUIDE ||
                     ((b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) && g_l3 && g_r3))) {
            if (open && !g_closing) set_open(false); else { g_device = ev.cbutton.which == PSP_TOUCH_PAD_ID ? 2 : 0; set_open(true); }
            return true;
        }
        if (const uint32_t d = dpad_bit(b)) {
            if (down) g_pad_dpad = d; else if (g_pad_dpad == d) g_pad_dpad = 0;
        }
        if (!open || g_closing || !down) return false;
        g_device = ev.cbutton.which == PSP_TOUCH_PAD_ID ? 2 : 0;
        const uint8_t ok = g_confirm_cross ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
        const uint8_t back = g_confirm_cross ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
        if (g_edit.active) {   // the on-screen keyboard: confirm presses a key, back deletes (cancels when empty), Start = done, shoulders = layers
            if (b == ok) edit_press_focus();
            else if (b == back) { if (g_edit.buf.empty()) edit_end(); else edit_backspace(); }
            else if (b == SDL_CONTROLLER_BUTTON_START) edit_commit();
            else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) { g_edit.shift = !g_edit.shift; g_edit.symbols = false; edit_clamp_focus(); }
            else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) { g_edit.symbols = !g_edit.symbols; g_edit.shift = false; edit_clamp_focus(); }
            return false;
        }
        if (b == ok) confirm();
        else if (b == back) go_back();
        else if (b == SDL_CONTROLLER_BUTTON_START) set_open(false);
        else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) switch_page(-1);
        else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) switch_page(1);
        return false;
    }
    case SDL_TEXTINPUT:
        if (open && g_edit.active) {
            for (const char* c = ev.text.text; *c; ++c) edit_append(*c);
            return true;
        }
        return false;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_lx = ev.caxis.value;
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_ly = ev.caxis.value;
        return false;
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL: {
        if (!open || g_closing) return false;
        if (ev.type == SDL_MOUSEWHEEL) {
            if (ev.wheel.which == SDL_TOUCH_MOUSEID || g_edit.active) return false;
            g_device = 1;
            if (g_dlg_open) return false;
            int mx = 0, my = 0; float wx = 0, wy = 0;
            SDL_GetMouseState(&mx, &my);
            if (mouse_to_canvas(mx, my, &wx, &wy) && g_view.geom().side.has(wx, wy)) { g_view.scroll_cats(-float(ev.wheel.preciseY) * 22.f); return false; }   // over the category list
            g_view.fling(0.f);
            g_view.scroll_by(-float(ev.wheel.preciseY) * 22.f);
            return false;
        }
        if (ev.type == SDL_MOUSEMOTION ? ev.motion.which == SDL_TOUCH_MOUSEID : ev.button.which == SDL_TOUCH_MOUSEID) return false;
        float ux = 0, uy = 0;
        const int mx = ev.type == SDL_MOUSEMOTION ? ev.motion.x : ev.button.x, my = ev.type == SDL_MOUSEMOTION ? ev.motion.y : ev.button.y;
        if (!mouse_to_canvas(mx, my, &ux, &uy)) return false;
        if (ev.type == SDL_MOUSEMOTION) {
            if (g_device != 1 && std::abs(ev.motion.xrel) + std::abs(ev.motion.yrel) < 2) return false;   // a settling cursor does not take over from the pad
            g_device = 1;
            pointer_move(ux, uy);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
            if (ev.button.button == SDL_BUTTON_LEFT) pointer_press(ux, uy, false);
            else if (ev.button.button == SDL_BUTTON_RIGHT) { g_device = 1; go_back(); }
        } else if (ev.button.button == SDL_BUTTON_LEFT) pointer_release(ux, uy);
        return false;
    }
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
        if (!open || g_closing || psp_touch_editing()) return false;
        float ux = 0, uy = 0;
        if (!to_canvas(ev.tfinger.x, ev.tfinger.y, &ux, &uy)) return false;
        if (ev.type == SDL_FINGERDOWN) {
            if (g_ptr.mode != 0 && g_ptr.touch) return true;   // one finger drives the menu
            pointer_press(ux, uy, true);
            g_ptr.finger = ev.tfinger.fingerId;
        } else if (g_ptr.touch && g_ptr.finger == ev.tfinger.fingerId) {
            if (ev.type == SDL_FINGERMOTION) pointer_move(ux, uy);
            else pointer_release(ux, uy);
        }
        return true;
    }
    case SDL_WINDOWEVENT:
        if (open) {   // size change (or F11): the display rows follow
            if (!is_fullscreen()) pick_window_size();
            g_dirty = true;
        }
        return false;
    default:
        return false;
    }
}

// First run (no saved or PSPRECOMP_FPS value): the frame rate starts at the display's refresh rate, rounded down to a multiple of
// 30 (a refresh rate that is not a whole multiple of 30 gives the next lower rate: 144 Hz -> 120, 75 Hz -> 60; 59.94 Hz counts
// as 60). Runs once, as soon as the window and the game's frame-rate hook exist.
static void apply_default_frame_rate() {
    static bool done = false;
    if (done || !g_fps.set || g_fps.count < 1) return;
    SDL_Window* w = win();
    if (!w) return;
    done = true;
    if (const char* e = std::getenv("PSPRECOMP_FPS"); e && e[0]) return;   // chosen by the user (saved menu value or environment)
    SDL_DisplayMode dm;
    const int disp = std::max(0, SDL_GetWindowDisplayIndex(w));
    if (SDL_GetCurrentDisplayMode(disp, &dm) != 0 || dm.refresh_rate <= 0) return;
    int want = std::max(30, (dm.refresh_rate + 1) / 30 * 30);   // +1: 59.94 -> 60, 89.9 -> 90
    int best = 0;
    for (int i = 0; i < g_fps.count; ++i) if (g_fps.rates[i] <= want) best = i;
    std::fprintf(stderr, "[SETTINGS] display %d Hz: default frame rate %d fps\n", dm.refresh_rate, g_fps.rates[best]);
    g_fps.set(g_fps.rates[best]);
}

void psp_settings_tick() {
    apply_default_frame_rate();
    if (g_request_open.exchange(false) && !g_open.load()) { set_open(true); g_gate_dirty.store(true); }
    if (!g_open.load()) return;
    if (!g_closing && (psp_adhoc_connected() || psp_adhoc_in_session())) {   // a match started while the menu was open (e.g. from a pad press queued before): give the game back
        std::fprintf(stderr, "[SETTINGS] closing: multiplayer session started\n");
        set_open(false);
    }
    if (!g_closing) held_dir_update();
    // A render scale change waits until the game has parked (no display list in flight).
    if (g_pending_wide >= 0 && g_parked.load() > 0) {
        ge_draw_set_widescreen(g_pending_wide != 0);
        g_pending_wide = -1;
        g_dirty = true;
    }
    // HDR Scene (or HDR output) changed: the scene targets change format once the game has parked.
    if ((g_pending_scene || ge_draw_scene_format_stale()) && g_parked.load() > 0) {
        ge_draw_sync_scene_format();
        g_pending_scene = false;
        g_dirty = true;
    }
    if (g_pending_rs && g_parked.load() > 0) {
        if (!ge_draw_set_render_scale(g_pending_rs))
            std::fprintf(stderr, "[SETTINGS] render scale change refused (PSPRECOMP_GE_AB is on)\n");
        g_pending_rs = 0;
        g_dirty = true;
    }
    const int64_t t = now_ms();
    if (page_id() == PAGE_NETWORK && !g_edit.active && t >= g_status_next) {   // keep the status line live
        g_status_next = t + 500;
        char st[128];
        psp_adhoc_status(st, sizeof st);
        if (g_net_status != st) { g_net_status = st; g_dirty = true; }
    }
    if (page_id() == PAGE_GPU && !g_edit.active) {   // the list / download run on a worker thread
        if (!g_gpu_list_asked) { g_gpu_list_asked = true; psp_gpu::refresh(); }   // load the live list the first time the page is seen
        const psp_gpu::Status st = psp_gpu::status();
        if (st.revision != g_gpu_rev) {
            g_gpu_rev = st.revision;
            if (!g_gpu_installing.empty() && st.state != psp_gpu::Status::Busy) {
                if (st.state == psp_gpu::Status::Done) gpu_choose(g_gpu_installing);   // a driver that was just installed is the one the user wants
                g_gpu_installing.clear();
            }
            g_dirty = true;
        }
    }
    if (page_id() == PAGE_CLOUD && !g_edit.active) {   // the sync runs on its own thread: redraw when its status changes
        const unsigned rev = psp_cloud_svc::status().revision;
        if (rev != g_cloud_rev) {
            g_cloud_rev = rev;
            const psp_cloud_svc::Config now = psp_cloud_svc::config();
            if (now.google_refresh != g_cloud.google_refresh || now.provider != g_cloud.provider) { cloud_load(); build_rows(); }   // a sign-in finished
            g_dirty = true;
        }
    }
    // ~60 frames a second: the screen is animated. The picture is rendered at a lower scale when the CPU cannot keep up.
    if (t - g_last_present < g_ui_interval) return;
    g_last_present = t;
    if ((g_dirty || t >= g_rows_next) && page_id() == PAGE_DISPLAY && g_sub < 0) {   // Window Size only shows while windowed
        const bool has_res = std::find(g_rows.begin(), g_rows.end(), ROW_RES) != g_rows.end();
        if (has_res == is_fullscreen()) build_rows();
    }
    if (g_dirty || t >= g_rows_next) {
        g_set_rows = make_set_rows();
        g_rows_next = t + 250;
        g_dirty = false;
        g_sel = std::clamp(g_sel, 0, std::max(0, int(g_set_rows.size()) - 1));
    }
    psp_sddialog::SetScene sc;
    for (int id : page_list()) sc.cats.push_back({k_page_names[id], page_icon(id)});
    sc.cat = g_page;
    sc.crumbs.push_back(k_page_names[page_list()[size_t(g_page)]]);
    if (g_sub >= 0) sc.crumbs.push_back(k_page_names[g_sub]);
    sc.in_sub = g_sub >= 0;
    sc.focus_tabs = g_focus_tabs && g_sub < 0;
    sc.dlg.active = g_dlg_open;
    if (g_dlg_open) {
        sc.dlg.title = "Restart required";
        sc.dlg.text = restart_supported() ? "Some of the changes only apply the next time the game starts.\nRestart now? Anything not saved in the game is lost."
                                          : "Some of the changes only apply the next time the game starts.\nStart the game again to use them.";
        sc.dlg.buttons = dialog_buttons();
        sc.dlg.sel = g_dlg_sel; sc.dlg.hover = g_dlg_hover;
    }
    sc.rows = g_set_rows;
    sc.sel = g_sel;
    sc.hover_row = g_hover_row; sc.hover_cat = g_hover_cat; sc.hover_part = g_hover_part; sc.hover_back = g_hover_back;
    sc.press_row = g_press_row;
    sc.device = g_device;
    sc.confirm_cross = g_confirm_cross;
    if (g_edit.active) {
        sc.kb.active = true;
        sc.kb.title = edit_label();
        sc.kb.text = g_edit.buf;
        sc.kb.secret = g_edit.field == 6;
        sc.kb.shift = g_edit.shift; sc.kb.symbols = g_edit.symbols;
        sc.kb.row = g_edit.row; sc.kb.col = g_edit.col;
    }
    static std::unique_ptr<psp_sddialog::Canvas> canvas;
    const int scale = std::max(1, std::min(psp_sddialog::output_scale(), g_ui_cap));
    if (!canvas || canvas->scale != scale) canvas = std::make_unique<psp_sddialog::Canvas>(scale);
    const auto r0 = Clock::now();
    g_view.render(*canvas, sc, double(t));
    const auto r1 = Clock::now();
    psp_sddialog::menu_overlay_publish(*canvas, g_view.translucent());
    const double pub_ms = std::chrono::duration<double, std::milli>(Clock::now() - r1).count();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
    g_ui_ms = g_ui_ms * 0.6 + ms * 0.4;
    // Sharpness first: a slow CPU redraws less often (down to ~15 fps) at the full canvas scale; the interval follows the
    // draw time directly. Only a hopeless draw time lowers the scale, and never below 2.
    g_ui_interval = g_ui_ms > 12.0 ? std::min(66, int(g_ui_ms * 1.15) + 1) : 7;   // 7 ms between redraws at best: the picture follows the display
    if (g_ui_ms > 60.0 && g_ui_cap > 2) { --g_ui_cap; g_ui_ms = 30.0; }
    else if (g_ui_ms < 20.0 && g_ui_cap < psp_sddialog::MAX_SCALE && ++g_ui_up > 300) { ++g_ui_cap; g_ui_up = 0; }
    static int s_log_iv = 0, s_log_scale = 0;
    if (std::abs(g_ui_interval - s_log_iv) >= 10 || scale != s_log_scale) {
        s_log_iv = g_ui_interval; s_log_scale = scale;
        std::fprintf(stderr, "[SETTINGS] ui canvas scale %d (output %d, cap %d), redraw every %d ms, draw %.1f ms\n", scale, psp_sddialog::output_scale(), g_ui_cap, g_ui_interval, g_ui_ms);
    }
    const auto r2 = Clock::now();
    ge_draw_present_overlay_now();
    if (std::getenv("PSPRECOMP_SETTINGS_PROFILE")) {
        static double a_r = 0, a_p = 0, a_v = 0; static int n = 0;
        a_r += ms; a_p += pub_ms; a_v += std::chrono::duration<double, std::milli>(Clock::now() - r2).count();
        if (++n == 120) { std::fprintf(stderr, "[SETTINGS] per frame: draw %.1f ms, publish %.1f ms, present %.1f ms (interval %d)\n", a_r / n, a_p / n, a_v / n, g_ui_interval); a_r = a_p = a_v = 0; n = 0; }
    }
    if (g_closing && g_view.closed()) finish_close();
}

bool psp_settings_block_input(uint32_t raw_buttons) {
    if (g_open.load()) return true;
    if (g_wait_release) {
        if (raw_buttons == 0) g_wait_release = false;
        else return true;
    }
    return false;
}

void psp_settings_guest_wait() {
    if (!g_open.load(std::memory_order_relaxed)) return;
    g_parked.fetch_add(1);
    while (g_open.load() && !g_should_exit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    g_parked.fetch_sub(1);
}
