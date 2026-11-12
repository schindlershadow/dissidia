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
#include <fstream>
#include <map>
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

enum Row { ROW_DISPLAY, ROW_RES, ROW_SCALE, ROW_FPS, ROW_WIDE, ROW_SOFT, ROW_RENDERER, ROW_AA, ROW_SHARP, ROW_SSAO, ROW_CONTACT, ROW_RT, ROW_RT_REFL, ROW_SSR, ROW_ANISO, ROW_UPSCALE, ROW_VOICE, ROW_TEXPACK, ROW_TOUCH, ROW_TOUCH_EDIT, ROW_UPDATES, ROW_QUIT,
           ROW_NET_MODE, ROW_NET_SERVER, ROW_NET_ROOM, ROW_NET_PEERS, ROW_NET_STATUS,   // Network page
           ROW_CLOUD_PROVIDER, ROW_CLOUD_FOLDER, ROW_CLOUD_URL, ROW_CLOUD_USER, ROW_CLOUD_PASS, ROW_CLOUD_GOOGLE,
           ROW_CLOUD_AUTO, ROW_CLOUD_REPLAYS, ROW_CLOUD_NOW, ROW_CLOUD_STATUS,   // Cloud page
           ROW_PERF, ROW_FX, ROW_SYNC, ROW_RING, ROW_GLOWSOFT, ROW_GLOWPASS,   // Debug page
           ROW_DBG0, ROW_DBG1, ROW_DBG2, ROW_DBG3, ROW_DBG4, ROW_DBG5,   // Debug page rows a game module adds (psp_settings_add_debug_toggle)
           ROW_GPU, ROW_GPU_GET, ROW_GPU_DEL };   // custom GPU drivers (Android, Adreno): System page
std::vector<Row> g_rows;
int g_sel = 0;
constexpr int PAGE_SYSTEM = 0, PAGE_NETWORK = 1, PAGE_CLOUD = 2, PAGE_DEBUG = 3, PAGE_GPU = 4, PAGES = 5;   // page ids
const char* const k_page_names[PAGES] = {"System", "Network", "Cloud", "Debug", "Drivers"};
int g_page = 0;                        // L / R shoulder, Q / E or Tab: next page (an index into page_list())
int g_sel_page[PAGES] = {0, 0, 0, 0, 0};
// The tabs this device shows: Drivers (custom GPU drivers) only where they can be used (an Adreno phone with the Android build).
const std::vector<int>& page_list() {
    static const std::vector<int> v = [] {
        std::vector<int> l{PAGE_SYSTEM};
        if (psp_gpu::supported(nullptr)) l.push_back(PAGE_GPU);
        l.push_back(PAGE_NETWORK); l.push_back(PAGE_CLOUD); l.push_back(PAGE_DEBUG);
        return l;
    }();
    return v;
}
int page_id() { return page_list()[size_t(g_page)]; }
int g_ring_next = -1;                  // vertex upload mode saved for the next launch (-1 = unchanged)
bool g_quit_armed = false;
bool g_dirty = true;
int g_drawn_scale = 0;
int64_t g_last_present = 0;
int g_pending_rs = 0;                  // render scale waiting for the game to park
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

// Text entry for the server / room / peers rows: a keyboard types directly; a controller picks a letter
// (left / right), adds it (Cross) and deletes (Circle), as on a console.
struct Edit {
    bool active = false;
    int field = 0;          // 0 server, 1 room, 2 peers, 3 sync folder, 4 WebDAV address, 5 WebDAV user, 6 WebDAV password
    std::string buf;
    int ci = 0;             // index into the letter list
    int sel = 1;            // selected row of the editor
} g_edit;
const char* const k_edit_chars = "abcdefghijklmnopqrstuvwxyz0123456789.-_:,ABCDEFGHIJKLMNOPQRSTUVWXYZ /@%?=&+#~!$*()[]\\";
constexpr int EDIT_ROWS = 5;   // text, letter, delete, done, cancel
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

std::vector<psp_sddialog::MenuRow> make_edit_rows() {
    std::vector<psp_sddialog::MenuRow> out(EDIT_ROWS);
    std::string shown = g_edit.field == 6 ? std::string(g_edit.buf.size(), '*') + "_" : g_edit.buf + "_";   // a password is never shown
    if (shown.size() > 30) shown = "..." + shown.substr(shown.size() - 27);
    out[0].label = edit_label();
    out[0].value = shown;
    out[0].help = std::string(edit_label()) + "\nType on a keyboard, or pick letters below. Enter or Done saves; Esc or Cancel keeps the old value.";
    const char c = k_edit_chars[g_edit.ci];
    out[1].label = "Letter";
    out[1].value = c == ' ' ? "Space" : std::string(1, c);
    out[1].can_dec = out[1].can_inc = true;
    out[1].help = "Letter\nLeft / right choose a character, Cross adds it.";
    out[2].label = "Delete";
    out[2].help = "Delete\nRemoves the last character. Circle does the same.";
    out[3].label = "Done";
    out[3].help = "Done\nSaves the new value and uses it at once.";
    out[4].label = "Cancel";
    out[4].help = "Cancel\nKeeps the old value.";
    return out;
}

std::vector<psp_sddialog::MenuRow> make_rows() {
    if (g_edit.active) return make_edit_rows();
    std::vector<psp_sddialog::MenuRow> out;
    char buf[96];
    for (Row r : g_rows) {
        psp_sddialog::MenuRow m;
        switch (r) {
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
            m.help = "Internal 3D resolution\nHigher is sharper and costs more GPU time (Steam Deck: 2x or 3x).";
            break;
        }
        case ROW_WIDE: {
            const bool on = g_pending_wide >= 0 ? g_pending_wide != 0 : ge_draw_widescreen();
            m.label = "Widescreen";
            if (!on) m.value = "Off";
            else if (g_pending_wide >= 0) m.value = "On";
            else {
                const int w = ge_draw_widescreen_width();
                std::snprintf(buf, sizeof buf, w > 480 ? "On  (%d px wide)" : "On  (window not wider than 16:9)", w);
                m.value = buf;
            }
            m.can_dec = on; m.can_inc = !on;
            m.help = "Wide screens (PSPRECOMP_WIDESCREEN)\nThe battle scene is drawn wider to fill a window or screen wider than 16:9; the HUD stays centred at its normal size and backgrounds stretch. Saved.";
            break;
        }
        case ROW_FPS: {
            const int i = fps_index();
            m.label = "Frame Rate";
            std::snprintf(buf, sizeof buf, "%d fps", g_fps.rates[i]);
            m.value = buf;
            m.can_dec = i > 0;
            m.can_inc = i + 1 < g_fps.count;
            m.help = "Battle logic rate (multiples of 30)\nBattles run at this many updates a second; the PSP ran 30. Costs CPU and GPU time in proportion. Menus stay at 30.";
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
            m.help = avail ? "Graphics API (PSPRECOMP_RENDERER)\nOpenGL is the default and the most tested. Vulkan renders the same picture through a Vulkan layer (experimental; needs a Vulkan 1.1 GPU). Takes effect the next time the game starts. Saved."
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
        case ROW_QUIT:
            m.label = "Quit Game";
            m.value = g_quit_armed ? "Press again to quit" : "";
            m.help = g_quit_armed ? "Quit\nUnsaved progress is lost. Press again to quit, or move away to cancel."
                                  : "Quit\nClose the game and return to the desktop / Steam.";
            break;
        case ROW_PERF: {
            const bool on = ge_draw_perf_overlay();
            m.label = "Performance Overlay";
            m.value = on ? "On" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Frame counter\nFrames per second and frame times in the top-left corner. Saved.";
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
            const bool on = psp_post_aa() != 0;
            m.label = "Anti-Aliasing";
            m.value = on ? "FXAA" : "Off";
            m.can_dec = on; m.can_inc = !on;
            m.help = "Post-process anti-aliasing (PSPRECOMP_POST_AA)\nSmooths jagged edges on the finished picture (FXAA). It does not touch the game's glow, so it is safe at any render scale. Saved.";
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

// ---- applying --------------------------------------------------------------
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
    if (g_edit.active) {
        if (g_edit.sel == 1) {
            const int n = int(std::strlen(k_edit_chars));
            g_edit.ci = (g_edit.ci + d + n) % n;
            g_dirty = true;
        }
        return;
    }
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
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
            g_sel_page[g_page] = g_sel;
            build_rows();   // the rows differ: server and room, or direct peers
        }
        break;
    case ROW_NET_SERVER: case ROW_NET_ROOM: case ROW_NET_PEERS: case ROW_NET_STATUS: break;
    case ROW_CLOUD_PROVIDER: {
        const int cur = int(g_cloud.provider), nx = std::clamp(cur + d, 0, 3);
        if (nx != cur) {
            g_cloud.provider = psp_cloud_svc::Provider(nx);
            cloud_apply();
            g_sel_page[g_page] = g_sel;
            build_rows();   // the rows differ per provider
        }
        break;
    }
    case ROW_CLOUD_AUTO: if (g_cloud.auto_sync != (d > 0)) { g_cloud.auto_sync = d > 0; cloud_apply(); } break;
    case ROW_CLOUD_REPLAYS: if (g_cloud.replays != (d > 0)) { g_cloud.replays = d > 0; cloud_apply(); } break;
    case ROW_CLOUD_FOLDER: case ROW_CLOUD_URL: case ROW_CLOUD_USER: case ROW_CLOUD_PASS: case ROW_CLOUD_GOOGLE:
    case ROW_CLOUD_NOW: case ROW_CLOUD_STATUS: break;
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
    case ROW_AA:
        if ((d > 0) != (psp_post_aa() != 0)) { psp_post_set_aa(d > 0); save("post_aa", d > 0 ? "1" : "0"); }
        break;
    case ROW_SHARP: {
        const int v = std::min(4, std::max(0, psp_post_sharpen() + d));
        if (v != psp_post_sharpen()) { psp_post_set_sharpen(v); save("sharpen", std::to_string(v)); }
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
        g_sel_page[g_page] = std::max(0, g_sel + (on ? -1 : 1));   // the contact row above disappears / appears
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
    }
    g_dirty = true;
}

void build_rows() {
    if (page_id() == PAGE_GPU) {
        g_rows = {ROW_GPU, ROW_GPU_GET, ROW_GPU_DEL};
    } else if (page_id() == PAGE_NETWORK) {
        net_load();
        g_rows = {ROW_NET_MODE};
        if (g_net_internet) { g_rows.push_back(ROW_NET_SERVER); g_rows.push_back(ROW_NET_ROOM); }
        else g_rows.push_back(ROW_NET_PEERS);
        g_rows.push_back(ROW_NET_STATUS);
    } else if (page_id() == PAGE_CLOUD) {
        cloud_load();
        if (psp_cloud_svc::initialised()) {
            g_rows = {ROW_CLOUD_PROVIDER};
            using P = psp_cloud_svc::Provider;
            if (g_cloud.provider == P::Folder) g_rows.push_back(ROW_CLOUD_FOLDER);
            else if (g_cloud.provider == P::WebDav) { g_rows.push_back(ROW_CLOUD_URL); g_rows.push_back(ROW_CLOUD_USER); g_rows.push_back(ROW_CLOUD_PASS); }
            else if (g_cloud.provider == P::Google) g_rows.push_back(ROW_CLOUD_GOOGLE);
            if (g_cloud.provider != P::Off) {
                g_rows.push_back(ROW_CLOUD_AUTO);
                g_rows.push_back(ROW_CLOUD_REPLAYS);
                g_rows.push_back(ROW_CLOUD_NOW);
            }
            g_rows.push_back(ROW_CLOUD_STATUS);
        } else g_rows = {ROW_CLOUD_STATUS};     // cloud sync is not running in this session (PSPRECOMP_CLOUD=0)
    } else if (page_id() == PAGE_DEBUG) {
        g_rows = {ROW_PERF, ROW_FX, ROW_SYNC, ROW_RING, ROW_GLOWSOFT, ROW_GLOWPASS};
        for (size_t i = 0; i < g_dbg_toggles.size(); i++) g_rows.push_back(Row(ROW_DBG0 + int(i)));
    } else {
        g_rows = {ROW_DISPLAY, ROW_RES, ROW_SCALE};
        if (g_fps.get && g_fps.count > 1) g_rows.push_back(ROW_FPS);
        g_rows.push_back(ROW_WIDE);
        g_rows.push_back(ROW_SOFT);
        if (texpack_row_wanted()) g_rows.push_back(ROW_TEXPACK);
        g_rows.push_back(ROW_AA);
        g_rows.push_back(ROW_SHARP);
        g_rows.push_back(ROW_SSAO);
        if (!psp_rt_active()) g_rows.push_back(ROW_CONTACT);   // ray-traced shadows replace them
        g_rows.push_back(ROW_RT);
        g_rows.push_back(ROW_RT_REFL);
        if (!psp_rt_refl_active()) g_rows.push_back(ROW_SSR);   // ray-traced reflections replace them
        g_rows.push_back(ROW_ANISO);
        g_rows.push_back(ROW_UPSCALE);
        if (renderer_row_wanted()) g_rows.push_back(ROW_RENDERER);
        if (voice_row_wanted()) g_rows.push_back(ROW_VOICE);
#ifdef __ANDROID__   // the on-screen controls exist for Android only
        g_rows.push_back(ROW_TOUCH);
        g_rows.push_back(ROW_TOUCH_EDIT);
#endif
        g_rows.push_back(ROW_UPDATES);
        g_rows.push_back(ROW_QUIT);
    }
    g_sel = std::min(std::max(g_sel_page[g_page], 0), int(g_rows.size()) - 1);
}

// Length of the tab switch animation; PSPRECOMP_TAB_ANIM_MS=<ms> overrides it (0 = no animation).
int64_t tab_anim_ms() {
    static const int64_t v = [] { const char* e = std::getenv("PSPRECOMP_TAB_ANIM_MS"); return e && e[0] ? int64_t(std::atoi(e)) : int64_t(200); }();
    return v;
}
int g_anim_from = -1, g_anim_dir = 0;  // a tab switch in progress: the tab left, and the direction (+1 = R)
int64_t g_anim_t0 = 0;

void switch_page(int dir = 1) {   // dir +1 = the next tab (R), -1 = the previous one (L); wraps around
    g_sel_page[g_page] = g_sel;
    g_anim_from = g_page; g_anim_dir = dir; g_anim_t0 = now_ms();
    const int n = int(page_list().size());
    g_page = (g_page + dir + n) % n;
    build_rows();
    g_quit_armed = false;
    g_dirty = true;
}

void set_open(bool open) {
    if (open == g_open.load()) return;
    if (open && psp_adhoc_in_session()) {   // opening pauses the game, which would stall the other players
        ge_draw_toast("Settings are unavailable during multiplayer", 2500);
        std::fprintf(stderr, "[SETTINGS] menu not opened: in a multiplayer session\n");
        return;
    }
    if (open) {
        build_res_options();
        pick_window_size();
        net_load();
        build_rows();
        g_quit_armed = false;
        g_dirty = true;
        g_dir_held = 0;
        g_open = true;
        std::fprintf(stderr, "[SETTINGS] menu open (game paused)\n");
    } else {
        g_sel_page[g_page] = g_sel;
        if (g_edit.active) edit_end();
        g_open = false;
        g_wait_release = true;
        psp_sddialog::menu_overlay_clear();
        ge_draw_present_overlay_now();
        std::fprintf(stderr, "[SETTINGS] menu closed\n");
    }
}

void move_sel(int d) {
    if (g_edit.active) { g_edit.sel = (g_edit.sel + d + EDIT_ROWS) % EDIT_ROWS; g_dirty = true; return; }
    const int n = int(g_rows.size());
    if (!n) return;
    g_sel = (g_sel + d + n) % n;
    g_quit_armed = false;
    g_dirty = true;
}

void confirm() {
    if (g_edit.active) {
        switch (g_edit.sel) {
        case 1: edit_append(k_edit_chars[g_edit.ci]); break;
        case 2: edit_backspace(); break;
        case 3: edit_commit(); break;
        case 4: edit_end(); break;
        default: break;
        }
        return;
    }
    if (g_sel < 0 || g_sel >= int(g_rows.size())) return;
    switch (g_rows[size_t(g_sel)]) {
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
        else { psp_cloud_svc::signout(); cloud_load(); g_sel_page[g_page] = g_sel; build_rows(); }
        break;
    case ROW_CLOUD_AUTO: change(g_cloud.auto_sync ? -1 : 1); return;
    case ROW_CLOUD_REPLAYS: change(g_cloud.replays ? -1 : 1); return;
    case ROW_CLOUD_NOW:
        if (psp_cloud_svc::configured() && psp_cloud_svc::status().state != psp_cloud_svc::Status::Syncing) psp_cloud_svc::sync_now();
        break;
    case ROW_CLOUD_STATUS: break;
    case ROW_PERF: change(ge_draw_perf_overlay() ? -1 : 1); return;
    case ROW_FX: change(ge_draw_fx_shaders() ? -1 : 1); return;
    case ROW_SYNC: ge_draw_set_sync_mode((ge_draw_sync_mode() + 1) % 5); break;
    case ROW_RING: change((g_ring_next >= 0 ? g_ring_next : ge_draw_ring_mode()) ? -1 : 1); return;
    case ROW_SOFT: change(ge_soft_focus() ? -1 : 1); return;
    case ROW_AA: change(psp_post_aa() ? -1 : 1); return;
    case ROW_SHARP: change(psp_post_sharpen() >= 4 ? -4 : 1); return;
    case ROW_SSAO: change(psp_fx_ssao() >= 3 ? -3 : 1); return;
    case ROW_CONTACT: change(psp_fx_contact() >= 3 ? -3 : 1); return;
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
    }
    g_dirty = true;
}

void nav(uint32_t dir) {
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
    if ((it = g_saved.find("post_aa")) != g_saved.end()) put("PSPRECOMP_POST_AA", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("aniso")) != g_saved.end()) put("PSPRECOMP_ANISO", it->second);
    if ((it = g_saved.find("sharpen")) != g_saved.end()) put("PSPRECOMP_SHARPEN", it->second);
    if ((it = g_saved.find("ssao")) != g_saved.end()) put("PSPRECOMP_SSAO", it->second);
    if ((it = g_saved.find("contact_shadows")) != g_saved.end()) put("PSPRECOMP_CONTACT_SHADOWS", it->second);
    if ((it = g_saved.find("rt_shadow_level")) != g_saved.end()) put("PSPRECOMP_VK_RT", it->second);
    else if ((it = g_saved.find("rt_shadows")) != g_saved.end()) put("PSPRECOMP_VK_RT", it->second == "1" ? "2" : "0");   // before the levels: on = Medium
    if ((it = g_saved.find("rt_refl_level")) != g_saved.end()) put("PSPRECOMP_VK_RT_REFL", it->second);
    else if ((it = g_saved.find("rt_reflections")) != g_saved.end()) put("PSPRECOMP_VK_RT_REFL", it->second == "1" ? "2" : "0");   // before the levels: on = Medium
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
    if ((it = g_saved.find("perf_overlay")) != g_saved.end()) put("PSPRECOMP_PERF_HUD", it->second == "1" ? "1" : "0");
    if ((it = g_saved.find("ge_ring")) != g_saved.end() && (it->second == "map" || it->second == "sub"))
        put("PSPRECOMP_GE_RING", it->second);
    std::fprintf(stderr, "[SETTINGS] %s:%s\n", settings_path().c_str(), applied.c_str());
}

bool psp_settings_menu_open() { return g_open.load(); }

bool psp_settings_handle_event(const SDL_Event& ev) {
    if (!g_enabled) return false;
    const bool open = g_open.load();
    switch (ev.type) {
    case SDL_KEYDOWN: {
        const SDL_Keycode k = ev.key.keysym.sym;
        if (k == SDLK_ESCAPE || k == SDLK_F1) {
            if (open && g_edit.active) { if (!ev.key.repeat) edit_end(); return true; }   // cancel the edit only
            if (!ev.key.repeat) set_open(!open);
            return true;
        }
        if (!open) return false;
        if (g_edit.active) {   // letters arrive as SDL_TEXTINPUT; only navigation keys act here
            switch (k) {
            case SDLK_UP: move_sel(-1); break;
            case SDLK_DOWN: move_sel(1); break;
            case SDLK_LEFT: change(-1); break;
            case SDLK_RIGHT: change(1); break;
            case SDLK_BACKSPACE: edit_backspace(); break;
            case SDLK_RETURN: case SDLK_KP_ENTER:
                if (!ev.key.repeat) { if (g_edit.sel == 1 || g_edit.sel == 2 || g_edit.sel == 4) confirm(); else edit_commit(); }
                break;
            default: break;
            }
            return false;
        }
        switch (k) {
        case SDLK_UP: move_sel(-1); break;
        case SDLK_DOWN: move_sel(1); break;
        case SDLK_LEFT: change(-1); break;
        case SDLK_RIGHT: change(1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_x: case SDLK_SPACE:
            if (!ev.key.repeat) confirm();
            break;
        case SDLK_z: case SDLK_BACKSPACE:
            if (!ev.key.repeat) set_open(false);
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
            set_open(!open);
            return true;
        }
        if (const uint32_t d = dpad_bit(b)) {
            if (down) g_pad_dpad = d; else if (g_pad_dpad == d) g_pad_dpad = 0;
        }
        if (!open || !down) return false;
        const uint8_t ok = g_confirm_cross ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
        const uint8_t back = g_confirm_cross ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
        if (g_edit.active) {   // Circle deletes (or cancels when empty), Start saves
            if (b == ok) confirm();
            else if (b == back) { if (g_edit.buf.empty()) edit_end(); else edit_backspace(); }
            else if (b == SDL_CONTROLLER_BUTTON_START) edit_commit();
            return false;
        }
        if (b == ok) confirm();
        else if (b == back || b == SDL_CONTROLLER_BUTTON_START) set_open(false);
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
    case SDL_WINDOWEVENT:
        if (open) {   // size change (or F11): the overlay scale and the display rows follow
            if (!is_fullscreen()) pick_window_size();
            g_dirty = true;
        }
        return false;
    default:
        return false;
    }
}

void psp_settings_tick() {
    if (!g_open.load()) return;
    held_dir_update();
    // A render scale change waits until the game has parked (no display list in flight).
    if (g_pending_wide >= 0 && g_parked.load() > 0) {
        ge_draw_set_widescreen(g_pending_wide != 0);
        g_pending_wide = -1;
        g_dirty = true;
    }
    if (g_pending_rs && g_parked.load() > 0) {
        if (!ge_draw_set_render_scale(g_pending_rs))
            std::fprintf(stderr, "[SETTINGS] render scale change refused (PSPRECOMP_GE_AB is on)\n");
        g_pending_rs = 0;
        g_dirty = true;
    }
    const int scale = psp_sddialog::output_scale();
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
    if (g_dirty || scale != g_drawn_scale) {
        g_dirty = false;
        g_drawn_scale = scale;
        psp_sddialog::Canvas c(scale);
        std::vector<std::string> page_tabs;
        for (int id : page_list()) page_tabs.push_back(k_page_names[id]);
        psp_sddialog::MenuAnim anim;
        if (g_anim_from >= 0) {
            const float u = tab_anim_ms() > 0 ? float(t - g_anim_t0) / float(tab_anim_ms()) : 1.f;
            if (u >= 1.f) g_anim_from = -1;
            else {
                anim.from_tab = g_anim_from; anim.dir = g_anim_dir;
                anim.p = 0.2f + 0.8f * (1.f - (1.f - u) * (1.f - u) * (1.f - u));   // ease out, starting a little in so no frame shows an empty list
                g_dirty = true;                                       // keep drawing until it has settled
            }
        }
        psp_sddialog::render_menu(c, "Settings", k_page_names[page_id()], make_rows(), g_edit.active ? g_edit.sel : g_sel, g_confirm_cross,
                                  &page_tabs, g_page, g_anim_from >= 0 ? &anim : nullptr);
        psp_sddialog::menu_overlay_publish(c);
        ge_draw_present_overlay_now();
        g_last_present = t;
    } else if (t - g_last_present >= 100) {   // keep the window fresh while the game is paused
        ge_draw_present_overlay_now();
        g_last_present = t;
    }
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
