#include "psp_rt.h"
#include "psp_screenfx.h"
#include "psp_gl_window.h"
#include "psp_event_loop.h"
#include "psp_runtime.h"
#include "psp_render_queue.h"
#include "psp_scheduler.h"
#include "psp_ge_draw.h"
#if defined(PSPRECOMP_HAVE_VKGL)
#include "vk/psp_vk_glsl.h"
#endif
#include "psp_settings.h"
#include "psp_report.h"
#include "psp_tex_pack.h"
#include "hle/psp_savedata_dialog.h"   // on-screen keyboard: host typing
#include "hle/psp_cloud_service.h"      // cloud sync sign-in helper

#include <SDL.h>
#include "psp_gl.h"
#include "psp_touch.h"
#include "psp_platform.h"
void gl_stats_install();
#include <csignal>
#include <cstdio>
#include <atomic>
#include <cstdlib>

// --- KHR_debug driver messages (PSPRECOMP_GL_DEBUG=1) -----------------------
// glad was generated without KHR_debug; fetch the entry points by hand.
typedef void (*GlDebugProcFn)(unsigned source, unsigned type, unsigned id, unsigned severity,
                                       int length, const char* message, const void* user);
typedef void (*PfnDebugMessageCallback)(GlDebugProcFn cb, const void* user);
typedef void (*PfnDebugMessageControl)(unsigned source, unsigned type, unsigned severity,
                                                int count, const unsigned* ids, unsigned char enabled);
static void gl_debug_cb(unsigned source, unsigned type, unsigned id, unsigned severity,
                                 int, const char* message, const void*) {
    static int n = 0;
    if (severity == 0x826B /*GL_DEBUG_SEVERITY_NOTIFICATION*/) return;
    if (++n > 2000) return;   // keep the log bounded
    const char* sev = severity == 0x9146 ? "HIGH" : severity == 0x9147 ? "MED" : severity == 0x9148 ? "LOW" : "?";
    const char* ty = type == 0x824C ? "ERROR" : type == 0x824D ? "DEPRECATED" : type == 0x824E ? "UNDEFINED"
                   : type == 0x824F ? "PORTABILITY" : type == 0x8250 ? "PERFORMANCE" : type == 0x8251 ? "OTHER" : "?";
    std::fprintf(stderr, "[GL_DEBUG] %s/%s src=0x%X id=%u: %s\n", ty, sev, source, id, message);
}
static void gl_debug_install() {
    auto cb = (PfnDebugMessageCallback)SDL_GL_GetProcAddress("glDebugMessageCallback");
    auto ctl = (PfnDebugMessageControl)SDL_GL_GetProcAddress("glDebugMessageControl");
    if (!cb) { std::fprintf(stderr, "[GL_DEBUG] KHR_debug not available\n"); return; }
    glEnable(0x92E0 /*GL_DEBUG_OUTPUT*/);
    glEnable(0x8242 /*GL_DEBUG_OUTPUT_SYNCHRONOUS*/);
    cb(gl_debug_cb, nullptr);
    if (ctl) ctl(0x1100 /*DONT_CARE*/, 0x1100, 0x1100, 0, nullptr, 1);
    std::fprintf(stderr, "[GL_DEBUG] driver debug output on\n");
}
#include <chrono>
#include <vector>
#include <thread>
#include <map>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <unistd.h>

// --- Global state definitions (declared extern in psp_runtime.h) ---
std::atomic<bool> g_should_exit{false};
std::atomic<int>  g_alive_threads{0};
std::thread::id   g_main_thread_id;
std::atomic<uint32_t> g_host_buttons{0};

// ---------------------------------------------------------------------------
// Host input -> PSP controller mapping (generic; no game-specific code)
//
// PSP button bits: SELECT 0x0001, START 0x0008, UP 0x0010, RIGHT 0x0020,
//   DOWN 0x0040, LEFT 0x0080, LTRIGGER 0x0100, RTRIGGER 0x0200,
//   TRIANGLE 0x1000, CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000.
//
// Keyboard:
//   Arrows        -> D-pad AND left analogue stick (full deflection)
//                    (D-pad only if PSPRECOMP_ARROWS_DPAD_ONLY=1)
//   X / Enter     -> CROSS      Z -> CIRCLE     A -> SQUARE   S -> TRIANGLE
//   Q -> LTRIGGER               W -> RTRIGGER
//   Space -> START              Tab -> SELECT
//
// SDL game controller (all connected pads, hot-plug aware):
//   A -> CROSS        B -> CIRCLE      X -> SQUARE      Y -> TRIANGLE
//   LEFTSHOULDER  -> LTRIGGER    TRIGGERLEFT  (>18%) -> LTRIGGER (or PSPRECOMP_PAD_L2)
//   RIGHTSHOULDER -> RTRIGGER    TRIGGERRIGHT (>18%) -> RTRIGGER (or PSPRECOMP_PAD_R2)
//   DPAD up/right/down/left -> UP/RIGHT/DOWN/LEFT
//   START -> START    BACK -> SELECT    GUIDE -> ignored
//   Left stick -> analogue lx/ly, 0..255 (centre 128), ~8% deadzone applied
//                 to the PSP-scale value (SDL y-down == PSP y-down).
//   Right stick -> D-pad bits (>50% deflection, applied in the sceCtrl HLE) by
//                 default; PSPRECOMP_RSTICK_DPAD=0 disables. A game module that
//                 uses the stick itself (Dissidia analogue camera) sets
//                 g_rstick_consumed_until_ms and the D-pad conversion pauses.
//
// Environment:
//   PSPRECOMP_SWAP_AB=1            swap confirm/cancel on the pad:
//                                  A->CIRCLE, B->CROSS (X/Y unchanged)
//   PSPRECOMP_PAD_L2=<buttons>     what the analogue triggers press instead of L / R, as PSP
//   PSPRECOMP_PAD_R2=<buttons>     button names joined by '+' (L R CROSS CIRCLE SQUARE TRIANGLE
//                                  UP DOWN LEFT RIGHT START SELECT; case-insensitive), e.g.
//                                  R+CROSS. All are held together for as long as the trigger is
//                                  past 50%. "none" disables the trigger. The shoulder buttons
//                                  (L1/R1) always stay L / R. An unknown name logs and is ignored.
//   PSPRECOMP_ARROWS_DPAD_ONLY=1   arrows do not drive the analogue stick
//   PSPRECOMP_RSTICK_DPAD=0        right stick does NOT act as D-pad (default on)
//   PSPRECOMP_GAMECONTROLLERDB=f   SDL gamecontrollerdb mapping file to load
//   PSPRECOMP_PAD_LOG=1            log every pad button event (SDL name -> PSP bit); the
//                                  "Controller opened" line always prints GUID + SDL mapping
//
// Sources are tracked separately (keyboard, pad buttons, pad triggers,
// right-stick dpad) and OR-ed into g_host_buttons; analogue goes to
// g_host_analog (packed lx | ly<<8). The debug socket's injected buttons
// are OR-ed in by the sceCtrl HLE.
// ---------------------------------------------------------------------------
std::atomic<uint32_t> g_host_analog{0x8080};
std::atomic<uint32_t> g_host_ranalog{0x8080};
std::atomic<int64_t> g_rstick_consumed_until_ms{0};
// Camera re-center request: R3 (right stick click) on a pad, or the C key. Taken once by the game module that owns
// a free camera (psp_ctrl_take_camera_recenter). L3 together with R3 is the settings menu hotkey, so a press
// while L3 is down, or while the menu is open, is not a request.
static std::atomic<bool> g_recenter_request{false};
bool psp_ctrl_take_camera_recenter() { return g_recenter_request.exchange(false); }

namespace {
constexpr uint32_t P_SELECT = 0x0001, P_START = 0x0008, P_UP = 0x0010,
    P_RIGHT = 0x0020, P_DOWN = 0x0040, P_LEFT = 0x0080, P_L = 0x0100,
    P_R = 0x0200, P_TRI = 0x1000, P_CIRCLE = 0x2000, P_CROSS = 0x4000,
    P_SQUARE = 0x8000;

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

bool g_swap_ab = false, g_arrows_dpad_only = false;

uint32_t g_kb_buttons = 0;      // keyboard keys
uint32_t g_pad_buttons = 0;     // pad digital buttons (all pads)
uint32_t g_pad_triggers = 0;    // analogue triggers as L/R
int16_t  g_rx_raw = 0, g_ry_raw = 0;  // right stick (SDL range)
uint8_t  g_kb_arrows = 0;       // bit0 up, 1 down, 2 left, 3 right
int16_t  g_lx_raw = 0, g_ly_raw = 0;  // left stick (SDL range)
std::map<SDL_JoystickID, SDL_GameController*> g_pads;
std::atomic<uint32_t> g_test_buttons{0}, g_test_analog{0x8080};   // what the game would read, even while the settings menu hides it

uint32_t g_trig_l_mask = P_L, g_trig_r_mask = P_R;  // PSPRECOMP_PAD_L2 / _R2
bool g_lr_triggers = false;   // Controls > L / R Buttons = Triggers: LT/RT press L/R, the shoulders nothing (prompts say LT/RT)
bool g_trig_l_down = false, g_trig_r_down = false;

// "R+CROSS" -> P_R | P_CROSS. Returns fallback if unset/empty or nothing parses.
uint32_t parse_button_set(const char* env, uint32_t fallback) {
    const char* v = std::getenv(env);
    if (!v || !v[0]) return fallback;
    static const struct { const char* name; uint32_t bit; } names[] = {
        {"L", P_L}, {"R", P_R}, {"CROSS", P_CROSS}, {"X", P_CROSS}, {"CIRCLE", P_CIRCLE},
        {"SQUARE", P_SQUARE}, {"TRIANGLE", P_TRI}, {"UP", P_UP}, {"DOWN", P_DOWN},
        {"LEFT", P_LEFT}, {"RIGHT", P_RIGHT}, {"START", P_START}, {"SELECT", P_SELECT},
        {"NONE", 0}};
    uint32_t mask = 0;
    bool any = false;
    const char* p = v;
    while (*p) {
        const char* e = p;
        while (*e && *e != '+' && *e != ',') ++e;
        size_t n = static_cast<size_t>(e - p);
        bool found = false;
        for (const auto& nm : names) {
            if (std::strlen(nm.name) == n && SDL_strncasecmp(nm.name, p, n) == 0) {
                mask |= nm.bit; found = any = true; break;
            }
        }
        if (!found && n)
            std::fprintf(stderr, "[RT] %s: unknown button '%.*s' ignored\n", env, static_cast<int>(n), p);
        p = *e ? e + 1 : e;
    }
    if (!any) return fallback;
    std::fprintf(stderr, "[RT] %s=%s -> psp 0x%04x\n", env, v, mask);
    return mask;
}

void update_triggers() {
    const uint32_t lm = g_lr_triggers ? P_L : g_trig_l_mask, rm = g_lr_triggers ? P_R : g_trig_r_mask;
    g_pad_triggers = (g_trig_l_down ? lm : 0) | (g_trig_r_down ? rm : 0);
}

// ---- Button prompts (psp_input_prompts) ----
std::mutex g_prompt_mu;
PspPrompts g_prompts{PSP_PROMPT_PSP, PSP_POS_SOUTH, PSP_POS_EAST, PSP_POS_WEST, PSP_POS_NORTH, false, false, {}};
int g_prompt_setting = -1;    // -1 Automatic
int g_last_device = 0;        // 0 none (touch or nothing yet), 1 keyboard, 2 gamepad
std::string g_p1_pad;         // Controls > Player 1 Controller: "" any, "keyboard" keyboard only, else the pad's GUID
std::map<SDL_Keycode, uint32_t> g_kb_held;   // the keys held and what each presses (a move key presses two buttons)
bool pad_allowed(SDL_JoystickID which);
SDL_JoystickID g_last_pad = -1;
void prompts_update();
uint32_t key_to_psp_button(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return P_UP;
        case SDLK_RIGHT:  return P_RIGHT;
        case SDLK_DOWN:   return P_DOWN;
        case SDLK_LEFT:   return P_LEFT;
        case SDLK_x:
        case SDLK_RETURN: return P_CROSS;
        case SDLK_z:      return P_CIRCLE;
        case SDLK_a:      return P_SQUARE;
        case SDLK_s:      return P_TRI;
        case SDLK_q:      return P_L;
        case SDLK_w:      return P_R;
        case SDLK_SPACE:  return P_START;
        case SDLK_TAB:    return P_SELECT;
        // One key for a move that takes two buttons at once (both go down in the same pad read, which the game needs:
        // R held first blocks instead). The design is VCKing101's (docs/port/keyboard-controls.md); the keys are ours.
        case SDLK_d:      return P_R | P_CROSS;    // dodge
        case SDLK_f:      return P_R | P_TRI;      // dash
        case SDLK_e:      return P_R | P_SQUARE;   // EX Mode (EX Revenge when hit)
        case SDLK_v:      return P_R | P_CIRCLE;   // summon
        case SDLK_1:      return P_L | P_CIRCLE;   // Brave assist
        case SDLK_2:      return P_L | P_SQUARE;   // HP assist
        default:          return 0;
    }
}

uint32_t pad_button_to_psp(uint8_t b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return g_swap_ab ? P_CIRCLE : P_CROSS;
        case SDL_CONTROLLER_BUTTON_B: return g_swap_ab ? P_CROSS : P_CIRCLE;
        case SDL_CONTROLLER_BUTTON_X: return P_SQUARE;
        case SDL_CONTROLLER_BUTTON_Y: return P_TRI;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return g_lr_triggers ? 0 : P_L;   // L / R Buttons = Triggers: the shoulders are free
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return g_lr_triggers ? 0 : P_R;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return P_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return P_RIGHT;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return P_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return P_LEFT;
        case SDL_CONTROLLER_BUTTON_START: return P_START;
        case SDL_CONTROLLER_BUTTON_BACK:  return P_SELECT;
        default: return 0;  // GUIDE etc. ignored
    }
}

// What the game's button symbols should look like: the setting, or (Automatic) the device player 1 used last, and where on the pad
// the button that sends each PSP face button is. Main thread; the result is read by the renderer (psp_input_prompts).
int pad_prompt_style(SDL_GameController* gc, bool* numbered) {
    *numbered = false;
    if (!gc) return PSP_PROMPT_PSP;
    const char* name = SDL_GameControllerName(gc);
    if (name && std::strstr(name, "Steam Deck")) { *numbered = true; return PSP_PROMPT_XBOX; }   // A B X Y, L1 / R1
    switch (SDL_GameControllerGetType(gc)) {
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: return PSP_PROMPT_PLAYSTATION;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        return PSP_PROMPT_NINTENDO;
    default: return PSP_PROMPT_XBOX;
    }
}
std::string pad_guid(SDL_GameController* gc) {
    char guid[40] = "";
    if (gc) SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(gc)), guid, sizeof guid);
    return guid;
}
bool pad_allowed(SDL_JoystickID which) {
    if (g_p1_pad.empty()) return true;
    if (g_p1_pad == "keyboard") return false;
    auto it = g_pads.find(which);
    return it != g_pads.end() && pad_guid(it->second) == g_p1_pad;
}
void prompts_update() {
    SDL_GameController* gc = nullptr;
    if (g_last_pad >= 0 && g_pads.count(g_last_pad) && pad_allowed(g_last_pad)) gc = g_pads[g_last_pad];
    else for (auto& pr : g_pads) if (pad_allowed(pr.first)) { gc = pr.second; break; }
    PspPrompts p{};
    bool numbered = false;
    const int pad_style = pad_prompt_style(gc, &numbered);
    int style = g_prompt_setting;
    if (style < 0) style = g_last_device == 1 ? PSP_PROMPT_KEYBOARD : gc ? pad_style : PSP_PROMPT_PSP;
    p.style = style;
    p.numbered_shoulders = style == PSP_PROMPT_PLAYSTATION || (style == pad_style && numbered);
    p.lr_on_triggers = g_lr_triggers;
    // SDL names a Nintendo pad's buttons by their labels unless told otherwise: its A (Cross by default) is the right button.
    const bool labels = pad_style == PSP_PROMPT_NINTENDO && gc &&
                        SDL_GetHintBoolean(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, SDL_TRUE);
    auto pos = [&](uint8_t sdl) {
        switch (sdl) {
        case SDL_CONTROLLER_BUTTON_A: return labels ? PSP_POS_EAST : PSP_POS_SOUTH;
        case SDL_CONTROLLER_BUTTON_B: return labels ? PSP_POS_SOUTH : PSP_POS_EAST;
        case SDL_CONTROLLER_BUTTON_X: return labels ? PSP_POS_NORTH : PSP_POS_WEST;
        default: return labels ? PSP_POS_WEST : PSP_POS_NORTH;
        }
    };
    p.cross = pos(g_swap_ab ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A);
    p.circle = pos(g_swap_ab ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B);
    p.square = pos(SDL_CONTROLLER_BUTTON_X);
    p.triangle = pos(SDL_CONTROLLER_BUTTON_Y);
    // keyboard style: the first key bound to each button
    static const uint32_t order[8] = {P_CROSS, P_CIRCLE, P_SQUARE, P_TRI, P_L, P_R, P_START, P_SELECT};
    static const SDL_Keycode keys[] = {SDLK_x, SDLK_z, SDLK_a, SDLK_s, SDLK_q, SDLK_w, SDLK_SPACE, SDLK_TAB, SDLK_RETURN};
    for (int i = 0; i < 8; i++)
        for (SDL_Keycode k : keys)
            if (key_to_psp_button(k) == order[i]) { std::snprintf(p.key_names[i], sizeof p.key_names[i], "%s", SDL_GetKeyName(k)); break; }
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_prompt_mu);
        changed = std::memcmp(&p, &g_prompts, sizeof p) != 0;
        g_prompts = p;
    }
    if (changed) {
        static const char* const names[] = {"PSP", "Xbox", "PlayStation", "Nintendo", "Keyboard"};
        std::fprintf(stderr, "[RT] button prompts: %s%s%s\n", names[p.style], p.numbered_shoulders ? ", L1/R1" : "",
                     p.lr_on_triggers ? ", L and R on the triggers" : "");
        psp_texpack_editor_changed();
    }
}

// SDL axis (-32768..32767) -> PSP 0..255, centre 128, 8% deadzone.
uint8_t axis_to_psp(int16_t v) {
    int p = (static_cast<int>(v) + 32768) >> 8;
    if (p < 0) p = 0;
    if (p > 255) p = 255;
    if (std::abs(p - 128) < 10) p = 128;  // ~8% of 128
    return static_cast<uint8_t>(p);
}

uint8_t axis_to_psp_raw(int16_t v) {
    int p = (static_cast<int>(v) + 32768) >> 8;
    return static_cast<uint8_t>(p < 0 ? 0 : p > 255 ? 255 : p);
}

// Recompute the published host state from all sources.
void publish_input() {
    // Pads work in the background (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS), so with two instances
    // on one machine a pad would drive both. Only the window that has the keyboard focus listens to
    // pads (PSPRECOMP_PAD_BACKGROUND=1 restores listening while unfocused). The pad state keeps
    // being tracked, so a held button is right again when the window regains focus.
    static const bool pad_bg = env_flag("PSPRECOMP_PAD_BACKGROUND");
    const bool pad_ok = pad_bg || SDL_GetKeyboardFocus() != nullptr;
    const uint32_t pad_buttons = pad_ok ? g_pad_buttons : 0;
    const uint32_t pad_triggers = pad_ok ? g_pad_triggers : 0;
    const int16_t lx_raw = pad_ok ? g_lx_raw : 0, ly_raw = pad_ok ? g_ly_raw : 0;
    const int16_t rx_raw = pad_ok ? g_rx_raw : 0, ry_raw = pad_ok ? g_ry_raw : 0;
    // The left stick as the game reads it (keyboard arrows stand in while the pad stick is centred)
    uint8_t lx = axis_to_psp(lx_raw), ly = axis_to_psp(ly_raw);
    if (lx == 128 && ly == 128 && !g_arrows_dpad_only && g_kb_arrows) {
        if (g_kb_arrows & 4) lx = 0;
        if (g_kb_arrows & 8) lx = 255;
        if (g_kb_arrows & 1) ly = 0;
        if (g_kb_arrows & 2) ly = 255;
        if ((g_kb_arrows & 12) == 12) lx = 128;  // opposing keys cancel
        if ((g_kb_arrows & 3) == 3) ly = 128;
    }
    // What the game would read, kept even while the menu hides it (Settings > Controls > Test Controls, psp_input_sample)
    g_test_buttons.store(g_kb_buttons | pad_buttons | pad_triggers, std::memory_order_relaxed);
    g_test_analog.store(lx | (static_cast<uint32_t>(ly) << 8), std::memory_order_relaxed);
    // The settings menu takes all input while open (and until it is released after closing).
    if (psp_settings_block_input(g_kb_buttons | pad_buttons | pad_triggers)) {
        g_host_buttons.store(0, std::memory_order_relaxed);
        g_host_analog.store(0x8080, std::memory_order_relaxed);
        g_host_ranalog.store(0x8080, std::memory_order_relaxed);
        return;
    }
    g_host_buttons.store(g_kb_buttons | pad_buttons | pad_triggers,
                         std::memory_order_relaxed);
    {   // PSPRECOMP_KEY_LOG=1: every change of the buttons the game will read (keyboard move keys: both bits in one change)
        static const bool key_log = env_flag("PSPRECOMP_KEY_LOG");
        static uint32_t last = 0;
        const uint32_t now = g_kb_buttons | pad_buttons | pad_triggers;
        if (key_log && now != last) { std::fprintf(stderr, "[KEYS] buttons 0x%04x\n", now); last = now; }
    }
    g_host_ranalog.store(static_cast<uint32_t>(axis_to_psp_raw(rx_raw)) |
                         (static_cast<uint32_t>(axis_to_psp_raw(ry_raw)) << 8),
                         std::memory_order_relaxed);
    g_host_analog.store(lx | (static_cast<uint32_t>(ly) << 8),
                        std::memory_order_relaxed);
}

}  // namespace

// ---- Test Controls: the raw controller panel ----
// Test rigs only: PSPRECOMP_TEST_VPAD="rx,ry,lt,rt,buttonmask,lx,ly" attaches an SDL virtual game controller holding those raw values
// (axes -32768..32767, triggers 0..32767, buttonmask bit i = SDL_GameControllerButton i), so Test Controls can be checked without a pad.
static SDL_Joystick* g_vpad = nullptr;
static int16_t g_vpad_axes[6];
static int g_vpad_buttons = 0;
// Applied once the runtime has opened the controller, so the changes arrive as events like a real pad's
static void test_vpad_apply() {
    if (!g_vpad) return;
    for (int i = 0; i < 6; ++i) SDL_JoystickSetVirtualAxis(g_vpad, i, g_vpad_axes[i]);
    for (int i = 0; i < 15; ++i) SDL_JoystickSetVirtualButton(g_vpad, i, (g_vpad_buttons >> i) & 1);
    g_vpad = nullptr;
}
void test_vpad_attach() {
    const char* e = std::getenv("PSPRECOMP_TEST_VPAD");
    if (!e || !e[0]) return;
    int v[7] = {0, 0, 0, 0, 0, 0, 0};
    std::sscanf(e, "%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
    const int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    SDL_Joystick* js = idx >= 0 ? SDL_JoystickOpen(idx) : nullptr;
    if (!js) { std::fprintf(stderr, "[RT] test virtual pad failed: %s\n", SDL_GetError()); return; }
    g_vpad = js;
    for (int i = 0; i < 6; ++i) g_vpad_axes[i] = int16_t(std::max(-32768, std::min(32767, i < 2 ? v[5 + i] : i < 4 ? v[i - 2] : v[i - 2] * 2 - 32768)));   // lx ly rx ry; a trigger axis rests at -32768 (SDL maps it to 0..32767)
    g_vpad_buttons = v[4];
    std::fprintf(stderr, "[RT] test virtual pad attached (device %d)\n", idx);
}

namespace {
std::string psp_bits_name(uint32_t m) {
    static const struct { uint32_t bit; const char* n; } nm[] = {{P_L, "L"}, {P_R, "R"}, {P_UP, "Up"}, {P_DOWN, "Down"}, {P_LEFT, "Left"}, {P_RIGHT, "Right"},
        {P_CROSS, "Cross"}, {P_CIRCLE, "Circle"}, {P_SQUARE, "Square"}, {P_TRI, "Triangle"}, {P_START, "Start"}, {P_SELECT, "Select"}};
    std::string r;
    for (auto& e : nm) if (m & e.bit) { if (!r.empty()) r += " + "; r += e.n; }
    return r;
}
// The pad player 1 reads: the one last used if allowed, else the first allowed (as the button prompts pick it)
SDL_GameController* test_p1_pad() {
    if (g_p1_pad == "keyboard") return nullptr;
    if (g_last_pad >= 0 && g_pads.count(g_last_pad) && pad_allowed(g_last_pad)) return g_pads[g_last_pad];
    for (auto& pr : g_pads) if (pad_allowed(pr.first)) return pr.second;
    return nullptr;
}
void test_fill_pad(SDL_GameController* gc, PspInputSample* out) {
    out->pad = true;
    out->rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    out->ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    out->lraw_x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    out->lraw_y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    out->lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    out->rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    out->pad_map.assign(SDL_CONTROLLER_BUTTON_MAX, std::string());
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
        if (SDL_GameControllerGetButton(gc, SDL_GameControllerButton(i))) out->pad_buttons |= 1u << i;
        out->pad_map[size_t(i)] = psp_bits_name(pad_button_to_psp(uint8_t(i)));
    }
    out->pad_map[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "Camera re-center";   // see the button handler: R3 without L3
    out->pad_l2_map = psp_bits_name(g_lr_triggers ? P_L : g_trig_l_mask);
    out->pad_r2_map = psp_bits_name(g_lr_triggers ? P_R : g_trig_r_mask);
    out->pad_rstick_map = "Right analog";
}
}  // namespace

// ---- Test Controls (Settings > Controls): the input a player would give the game, readable while the menu hides it ----
int psp_input_sample_players() { return 2; }
bool psp_input_sample(int player, PspInputSample* out) {
    *out = PspInputSample{};
    out->lx = out->ly = 128;
    if (player <= 0) {   // player 1: keyboard, the pads Player 1 Controller allows, touch; plus the debug socket's injections (what the game reads)
        out->present = true;
        out->name = g_p1_pad.empty() ? "Any controller + keyboard" : g_p1_pad == "keyboard" ? "Keyboard only" : "Selected controller";
        if (g_p1_pad.size() && g_p1_pad != "keyboard")
            for (auto& pr : g_pads) if (pad_guid(pr.second) == g_p1_pad) { const char* n = SDL_GameControllerName(pr.second); out->name = n ? n : "Controller"; }
        uint32_t b = g_test_buttons.load(std::memory_order_relaxed);
        uint32_t a = g_test_analog.load(std::memory_order_relaxed);
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms < g_injected_buttons_deadline_ms.load(std::memory_order_relaxed)) b |= g_injected_buttons.load(std::memory_order_relaxed);
        if (now_ms < g_injected_analog_deadline_ms.load(std::memory_order_relaxed)) a = g_injected_analog.load(std::memory_order_relaxed);
        out->buttons = b;
        out->lx = uint8_t(a & 255); out->ly = uint8_t((a >> 8) & 255);
        if (SDL_GameController* gc = test_p1_pad()) test_fill_pad(gc, out);
        return true;
    }
    // Player 2 (placeholder until split screen): the second connected controller, i.e. the first one Player 1 does not use
    std::vector<SDL_GameController*> others;
    for (auto& pr : g_pads) others.push_back(pr.second);
    if (g_p1_pad.empty()) { if (!others.empty()) others.erase(others.begin()); }   // Any Controller: player 1 has the first
    else if (g_p1_pad != "keyboard") others.erase(std::remove_if(others.begin(), others.end(), [](SDL_GameController* gc) { return pad_guid(gc) == g_p1_pad; }), others.end());
    if (others.empty()) { out->name = "No second controller connected"; return true; }
    SDL_GameController* gc = others.front();
    out->present = true;
    { const char* n = SDL_GameControllerName(gc); out->name = n ? n : "Controller"; }
    uint32_t b = 0;
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
        if (SDL_GameControllerGetButton(gc, SDL_GameControllerButton(i))) b |= pad_button_to_psp(uint8_t(i));
    const uint32_t lm = g_lr_triggers ? P_L : g_trig_l_mask, rm = g_lr_triggers ? P_R : g_trig_r_mask;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= lm;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= rm;
    out->buttons = b;
    out->lx = axis_to_psp(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
    out->ly = axis_to_psp(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    test_fill_pad(gc, out);
    return true;
}

namespace {
void pad_open(int device_index) {
    SDL_GameController* gc = SDL_GameControllerOpen(device_index);
    if (!gc) {
        std::fprintf(stderr, "[RT] Controller open failed (%d): %s\n",
                     device_index, SDL_GetError());
        return;
    }
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    if (g_pads.count(id)) { SDL_GameControllerClose(gc); return; }
    g_pads[id] = gc;
    char guid[40] = "";
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(gc)), guid, sizeof guid);
    char* mapping = SDL_GameControllerMapping(gc);
    std::fprintf(stderr, "[RT] Controller opened: %s (id %d, guid %s)\n[RT]   mapping: %s\n",
                 SDL_GameControllerName(gc), static_cast<int>(id), guid, mapping ? mapping : "(none)");
    if (mapping) SDL_free(mapping);
}

void pad_close(SDL_JoystickID id) {
    auto it = g_pads.find(id);
    if (it == g_pads.end()) return;
    std::fprintf(stderr, "[RT] Controller removed: %s (id %d)\n",
                 SDL_GameControllerName(it->second), static_cast<int>(id));
    SDL_GameControllerClose(it->second);
    g_pads.erase(it);
    if (g_pads.empty()) {  // nothing left to hold buttons down
        g_pad_buttons = g_pad_triggers = 0;
        g_trig_l_down = g_trig_r_down = false;
        g_lx_raw = g_ly_raw = g_rx_raw = g_ry_raw = 0;
    }
}

// The analogue triggers count as pressed from ~18% travel (was 50%): R + X (dodge) pressed together on an Xbox pad reached the game as X first, R a frame or
// two later (the trigger's travel), so X alone started a chase in a chase window (18 Nov, desktop). PSPRECOMP_PAD_TRIGGER_THRESH=<0..32767> overrides.
int16_t trigger_threshold() { static const int t = [] { const char* e = std::getenv("PSPRECOMP_PAD_TRIGGER_THRESH"); return e ? std::atoi(e) : 6000; }(); return static_cast<int16_t>(t); }
void pad_axis(uint8_t axis, int16_t v) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: g_lx_raw = v; break;
        case SDL_CONTROLLER_AXIS_LEFTY: g_ly_raw = v; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            g_trig_l_down = v > trigger_threshold();
            update_triggers();
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            g_trig_r_down = v > trigger_threshold();
            update_triggers();
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX: g_rx_raw = v; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: g_ry_raw = v; break;
        default: break;
    }
}

}  // namespace

static void toggle_fullscreen(SDL_Window* w) {
    if (!w) return;
    bool fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (SDL_SetWindowFullscreen(w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
        std::fprintf(stderr, "[RT] fullscreen toggle failed: %s\n", SDL_GetError());
}

// ---- On-screen keyboard: host text input and the Steam Deck keyboard -------
// While the game's keyboard dialog is open, SDL text input is on (typed text goes to the dialog, see
// handle_sdl_event) and, on a Steam Deck, Steam's own keyboard is shown through its steam:// link (the same
// link SDL and ISteamUtils::ShowFloatingGamepadTextInput use; Mode=0 = single line, so its Enter key sends
// Enter, which finishes the name, and closes it). The game runs in a distrobox container, so the link is
// opened on the host with distrobox-host-exec. PSPRECOMP_STEAM_KEYBOARD=1 forces it on, =0 off; by default
// it is on when Steam sets SteamDeck=1.
static bool steam_keyboard_wanted() {
    if (const char* e = std::getenv("PSPRECOMP_STEAM_KEYBOARD")) return e[0] && e[0] != '0';
    const char* d = std::getenv("SteamDeck");
    return d && d[0] == '1';
}
static void steam_keyboard(bool show) {
    static const bool wanted = steam_keyboard_wanted();
    if (!wanted) return;
    const char* url = show ? "steam://open/keyboard?XPosition=0&YPosition=0&Width=0&Height=0&Mode=0"
                           : "steam://close/keyboard";
    const bool in_box = std::getenv("CONTAINER_ID") || access("/run/.containerenv", F_OK) == 0;
    std::string cmd = in_box ? "distrobox-host-exec steam '" : "steam '";
    cmd += url;
    cmd += "' >/dev/null 2>&1 &";
    const int rc = std::system(cmd.c_str());
    std::fprintf(stderr, "[OSK] Steam keyboard %s (%s, rc %d)\n", show ? "open" : "close", in_box ? "host" : "local", rc);
}
// The clipboard's text into the game's on-screen keyboard (its Paste key, Ctrl+V), as typed keys; line breaks dropped.
static void osk_paste() {
    char* clip = SDL_GetClipboardText();
    if (!clip) return;
    const unsigned char* t = reinterpret_cast<const unsigned char*>(clip);
    for (size_t i = 0; t[i];) {
        uint32_t cp = t[i];
        int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : (cp >> 3) == 30 ? 4 : 1;
        if (n > 1) { cp &= 0xFFu >> (n + 1); for (int k = 1; k < n && t[i + k]; ++k) cp = (cp << 6) | (t[i + k] & 63u); }
        if (cp != '\r' && cp != '\n') psp_sddialog::osk_push_key(cp);
        i += size_t(n);
    }
    SDL_free(clip);
}
static void osk_tick() {
    static bool was = false;
    const bool now = psp_sddialog::osk_active();
    if (now && psp_sddialog::osk_take_paste_request()) osk_paste();
    if (now == was) return;
    was = now;
    if (now) SDL_StartTextInput(); else SDL_StopTextInput();
    steam_keyboard(now);
}

// Handle one SDL event: window close, keyboard and game controller state.
static void handle_sdl_event(const SDL_Event& ev) {
    psp_touch_event(ev);   // on-screen touch controls: fingers become synthetic pad events (and real pad input hides them)
    {   // settings menu first: its hotkey is swallowed; opening / closing re-gates the game's input
        const bool was_open = psp_settings_menu_open();
        const bool eaten = psp_settings_handle_event(ev);
        // A button or key that closes the menu (Back, Start) is not eaten: its press is recorded just below and published
        // there. Publishing here, before that, would see no held button, end the menu's wait-for-release at once and
        // let the very press that closed the menu through to the game.
        const bool closed_by_press = was_open && !psp_settings_menu_open() && !eaten;
        if (was_open != psp_settings_menu_open() && !closed_by_press) publish_input();
        if (eaten) return;
    }
    if (psp_sddialog::osk_active()) {   // the on-screen keyboard is open: the host keyboard types into it
        if (ev.type == SDL_TEXTINPUT) {
            const unsigned char* t = reinterpret_cast<const unsigned char*>(ev.text.text);
            for (size_t i = 0; t[i];) {   // UTF-8 -> code points (Latin text only is kept by the dialog)
                uint32_t cp = t[i];
                int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : (cp >> 3) == 30 ? 4 : 1;
                if (n > 1) { cp &= 0xFFu >> (n + 1); for (int k = 1; k < n && t[i + k]; ++k) cp = (cp << 6) | (t[i + k] & 63u); }
                psp_sddialog::osk_push_key(cp);
                i += size_t(n);
            }
            return;
        }
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_v && (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) { osk_paste(); return; }
        if (ev.type == SDL_KEYDOWN && !(ev.key.keysym.mod & (KMOD_ALT | KMOD_GUI | KMOD_CTRL))) {
            const SDL_Keycode k = ev.key.keysym.sym;
            if (k == SDLK_BACKSPACE || k == SDLK_DELETE) { psp_sddialog::osk_push_key(8); return; }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (!ev.key.repeat) psp_sddialog::osk_push_key(13); return; }
            if (k >= 32 && k < 127) return;   // arrives as SDL_TEXTINPUT; must not also press a PSP button
        }
    }
    switch (ev.type) {
    case SDL_QUIT:
        g_should_exit.store(true);
        return;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (ev.key.repeat) return;  // ignore OS key repeat
        bool down = ev.type == SDL_KEYDOWN;
        SDL_Keycode k = ev.key.keysym.sym;
        if (down && (k == SDLK_F11 ||
                     ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (ev.key.keysym.mod & KMOD_ALT)))) {
            toggle_fullscreen(SDL_GetWindowFromID(ev.key.windowID));
            return;
        }
        {   // stage light editing (psp_screenfx.cpp): F6 drop, F7 remove, F8 edit mode; in edit mode J/L I/K U/O move, , . radius, - = intensity, [ ] select
            int what = -1;
            switch (k) {
                case SDLK_F6: what = PSP_LIGHT_DROP; break;
                case SDLK_F7: what = PSP_LIGHT_REMOVE; break;
                case SDLK_F8: what = PSP_LIGHT_TOGGLE_EDIT; break;
                case SDLK_PAGEUP: what = PSP_LIGHT_DIST_UP; break;
                case SDLK_PAGEDOWN: what = PSP_LIGHT_DIST_DOWN; break;
                case SDLK_LSHIFT: case SDLK_RSHIFT: what = PSP_LIGHT_FAST; break;
                case SDLK_j: what = PSP_LIGHT_LEFT; break;
                case SDLK_l: what = PSP_LIGHT_RIGHT; break;
                case SDLK_i: what = PSP_LIGHT_FWD; break;
                case SDLK_k: what = PSP_LIGHT_BACK; break;
                case SDLK_u: what = PSP_LIGHT_DOWN; break;
                case SDLK_o: what = PSP_LIGHT_UP; break;
                case SDLK_COMMA: what = PSP_LIGHT_RAD_DOWN; break;
                case SDLK_PERIOD: what = PSP_LIGHT_RAD_UP; break;
                case SDLK_MINUS: what = PSP_LIGHT_INT_DOWN; break;
                case SDLK_EQUALS: what = PSP_LIGHT_INT_UP; break;
                case SDLK_p: what = PSP_LIGHT_NEAREST; break;
                case SDLK_t: what = PSP_LIGHT_COPY_COLOR; break;
                case SDLK_g: what = PSP_LIGHT_GLOW; break;
                case SDLK_LEFTBRACKET: what = PSP_LIGHT_PREV; break;
                case SDLK_RIGHTBRACKET: what = PSP_LIGHT_NEXT; break;
                default: break;
            }
            if (what >= 0 && !psp_settings_menu_open() && psp_stage_light_input(what, down)) return;
        }
        if (down && k == SDLK_F9) {
            // RT scene debugging: log the next frame's draws and save a full-resolution screenshot of the same moment (rtdump_N.tga)
            static int seq = 0;
            ++seq;
            char path[64]; std::snprintf(path, sizeof path, "rtdump_%d.tga", seq);
            if (!std::getenv("PSPRECOMP_SHOT_FULL")) psp_plat::setenv_("PSPRECOMP_SHOT_FULL", "1");   // (portable: setenv does not exist on Windows)
            psp_rt_request_dump();
            std::fprintf(stderr, "[RTDUMP] requested #%d (F9), screenshot %s\n", seq, path);
            std::thread([p = std::string(path)] { ge_draw_capture_screenshot(p.c_str(), 5000); }).detach();   // served by this thread's next loop pass
            return;
        }
        uint8_t arrow = k == SDLK_UP ? 1 : k == SDLK_DOWN ? 2 :
                        k == SDLK_LEFT ? 4 : k == SDLK_RIGHT ? 8 : 0;
        if (arrow) {
            if (down) g_kb_arrows |= arrow; else g_kb_arrows &= ~arrow;
        }
        uint32_t bit = key_to_psp_button(k);
        if (bit) {   // what every held key presses (a move key's R must not let go while W still holds R)
            if (down) g_kb_held[k] = bit; else g_kb_held.erase(k);
            g_kb_buttons = 0;
            for (const auto& h : g_kb_held) g_kb_buttons |= h.second;
        }
        if (bit || arrow) publish_input();
        if (down && (bit || arrow) && g_last_device != 1) { g_last_device = 1; prompts_update(); }
        if (down && k == SDLK_c && !psp_settings_menu_open() && !(ev.key.keysym.mod & (KMOD_ALT | KMOD_GUI | KMOD_CTRL)))
            g_recenter_request.store(true);
        return;
    }
    case SDL_CONTROLLERDEVICEADDED:
        pad_open(ev.cdevice.which);  // 'which' is a device index here
        test_vpad_apply();
        prompts_update();
        return;
    case SDL_CONTROLLERDEVICEREMOVED:
        pad_close(ev.cdevice.which);  // instance id here
        if (g_last_pad == ev.cdevice.which) { g_last_pad = -1; if (g_last_device == 2) g_last_device = 0; }
        publish_input();
        prompts_update();
        return;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        if (!pad_allowed(ev.cbutton.which)) return;   // Controls > Player 1 Controller: another pad
        {   // R3 / L3: camera re-center request (see g_recenter_request)
            static bool l3 = false;
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) l3 = ev.type == SDL_CONTROLLERBUTTONDOWN;
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK && ev.type == SDL_CONTROLLERBUTTONDOWN && !l3 &&
                !psp_settings_menu_open() && (env_flag("PSPRECOMP_PAD_BACKGROUND") || SDL_GetKeyboardFocus() != nullptr))
                g_recenter_request.store(true);
        }
        if (ev.type == SDL_CONTROLLERBUTTONDOWN && (g_last_device != 2 || g_last_pad != ev.cbutton.which)) {
            g_last_device = 2; g_last_pad = ev.cbutton.which; prompts_update();
        }
        uint32_t bit = pad_button_to_psp(ev.cbutton.button);
        static const bool pad_log = env_flag("PSPRECOMP_PAD_LOG");
        if (pad_log)
            std::fprintf(stderr, "[PAD] %s %s -> psp 0x%04x\n", SDL_GameControllerGetStringForButton(
                             static_cast<SDL_GameControllerButton>(ev.cbutton.button)),
                         ev.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", bit);
        if (!bit) return;
        if (ev.type == SDL_CONTROLLERBUTTONDOWN) g_pad_buttons |= bit;
        else g_pad_buttons &= ~bit;
        publish_input();
        return;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (!pad_allowed(ev.caxis.which)) return;
        if ((ev.caxis.value > 16000 || ev.caxis.value < -16000) && (g_last_device != 2 || g_last_pad != ev.caxis.which)) {
            g_last_device = 2; g_last_pad = ev.caxis.which; prompts_update();
        }
        pad_axis(ev.caxis.axis, ev.caxis.value);
        publish_input();
        return;
    case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int dw = 0, dh = 0;
            psp_gl_drawable_size(SDL_GetWindowFromID(ev.window.windowID), &dw, &dh);
            std::fprintf(stderr, "[RT] window %dx%d (drawable %dx%d)\n",
                         ev.window.data1, ev.window.data2, dw, dh);
        }
        if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            // KEYUP is not delivered after focus loss; release keyboard
            // state so no key sticks. Pads are muted by publish_input() while unfocused.
            g_kb_buttons = 0;
            g_kb_held.clear();
            g_kb_arrows = 0;
            publish_input();
        }
        if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) publish_input();  // pads listen again
        return;
    default:
        return;
    }
}

// --- Static state (SDL window + GL context, module-private) ---
static SDL_Window*  g_window     = nullptr;
static std::string  g_window_title = "PSPrecomp Runtime";
// A game module names its window (the title shown in the taskbar / window list).
void psp_set_window_title(const char* title) {
    if (!title || !*title) return;
    g_window_title = title;
    if (g_window) SDL_SetWindowTitle(g_window, title);
}
static std::vector<unsigned char> g_icon_px;
static int g_icon_w = 0, g_icon_h = 0;
static void apply_window_icon(SDL_Window* w) {
    if (!w || g_icon_px.empty()) return;
    if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(g_icon_px.data(), g_icon_w, g_icon_h, 32, g_icon_w * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(w, s);   // SDL copies the pixels
        SDL_FreeSurface(s);
    }
}
void psp_set_window_icon(const unsigned char* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    g_icon_px.assign(rgba, rgba + size_t(w) * h * 4);
    g_icon_w = w; g_icon_h = h;
    apply_window_icon(g_window);
}
static SDL_GLContext g_gl_context = nullptr;
static bool vk_renderer_requested() {
#if defined(PSPRECOMP_HAVE_VKGL)
    return vkgl::requested();
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// psp_runtime_init_sdl — create SDL window + GL 3.3 core context + GLAD
// ---------------------------------------------------------------------------
int psp_runtime_init_sdl() {
    // Record main thread id for GL_THREAD_CHECK
    g_main_thread_id = std::this_thread::get_id();

    // 1. Initialize SDL video subsystem
    g_swap_ab          = env_flag("PSPRECOMP_SWAP_AB");
    if (const char* bp = std::getenv("PSPRECOMP_BUTTON_PROMPTS")) {   // auto | psp | xbox | playstation | nintendo | keyboard
        static const char* const keys[6] = {"auto", "psp", "xbox", "playstation", "nintendo", "keyboard"};
        for (int i = 0; i < 6; i++) if (!std::strcmp(bp, keys[i])) g_prompt_setting = i - 1;
    }
    g_lr_triggers = env_flag("PSPRECOMP_LR_TRIGGERS");
    if (const char* pc = std::getenv("PSPRECOMP_P1_CONTROLLER")) g_p1_pad = std::strcmp(pc, "any") == 0 ? "" : pc;
    prompts_update();   // the style to draw from the start (a pad connected at start-up updates it again)
    g_arrows_dpad_only = env_flag("PSPRECOMP_ARROWS_DPAD_ONLY");
    g_trig_l_mask      = parse_button_set("PSPRECOMP_PAD_L2", P_L);
    g_trig_r_mask      = parse_button_set("PSPRECOMP_PAD_R2", P_R);
    // Keep pad input flowing even when the window is unfocused.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Text input is only wanted while the on-screen keyboard is open, and SDL must not summon the
    // Steam Deck keyboard by itself (SDL_StartTextInput does on a Deck): steam_keyboard() below does it.
    SDL_SetHint("SDL_ENABLE_SCREEN_KEYBOARD", "0");
#if defined(__linux__) && !defined(__ANDROID__)
    // SDL2 tries X11 before Wayland, so on a Wayland desktop (GNOME, KDE) the game opened through XWayland, which has no HDR: the surface
    // offered no HDR format and HDR Output read "Unavailable" with HDR on in the system settings. With the Vulkan renderer ask for a
    // native Wayland window first (X11 stays the fallback). Not under gamescope, which shows only X11 windows, and never over a
    // SDL_VIDEODRIVER the user set. Reads PSPRECOMP_RENDERER (the settings file is already in the environment) instead of calling
    // vk_renderer_requested(): its Vulkan probe starts SDL's video layer, which would pick X11 before this hint exists.
    {
        const char* wl = std::getenv("WAYLAND_DISPLAY");
        const char* drv = std::getenv("SDL_VIDEODRIVER");
        const char* rr = std::getenv("PSPRECOMP_RENDERER");
        const bool want_vk = rr && (!std::strcmp(rr, "vulkan") || !std::strcmp(rr, "vk"));
        if (want_vk && wl && wl[0] && !(drv && drv[0]) && !std::getenv("GAMESCOPE_WAYLAND_DISPLAY"))
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
    }
#endif
    // PSPRECOMP_NO_PADS=1: no game controllers at all (test copies on a machine where the user plays: they must never open or read the user's pad).
    static const bool no_pads = [] { const char* e = std::getenv("PSPRECOMP_NO_PADS"); return e && *e && *e != '0'; }();
    if (SDL_Init(no_pads ? SDL_INIT_VIDEO : (SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) != 0) {
        std::fprintf(stderr,
            "[RT] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    test_vpad_attach();

    // 1b. Optional user gamecontrollerdb mapping file
    if (const char* db = std::getenv("PSPRECOMP_GAMECONTROLLERDB")) {
        if (db[0]) {
            int n = SDL_GameControllerAddMappingsFromFile(db);
            if (n < 0)
                std::fprintf(stderr, "[RT] gamecontrollerdb '%s': %s\n",
                             db, SDL_GetError());
            else
                std::fprintf(stderr,
                    "[RT] gamecontrollerdb '%s': %d mappings loaded\n", db, n);
        }
    }

#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");   // the game is landscape: a foldable (or a sensor) must not turn it to portrait, where the picture and the touch layout break
#endif
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   // touch drives the on-screen controls, not a fake mouse
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    // 2. Set GL attributes: OpenGL 3.3 core profile, double-buffered
#ifdef PSP_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);   // Android: OpenGL ES 3.2 (3.0 is the floor the renderer needs)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // PSPRECOMP_GL_DEBUG=1: ask for a debug context so the driver reports
    // errors, undefined behaviour and performance notes (KHR_debug) to the log.
    const bool gl_debug = std::getenv("PSPRECOMP_GL_DEBUG") != nullptr;
    if (gl_debug) SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

    // 3. Create the window. Size: PSPRECOMP_WINDOW=<w>x<h> (explicit), else
    //    PSPRECOMP_SCALE=<n> * 480x272, else the largest integer scale that
    //    fits the desktop's usable area (min 1). Always resizable; drawable
    //    size is queried at present time (high-DPI aware).
    int win_w = 480, win_h = 272;
    {
        int ww = 0, wh = 0;
        const char* wenv = std::getenv("PSPRECOMP_WINDOW");
        const char* senv = std::getenv("PSPRECOMP_SCALE");
        if (wenv && std::sscanf(wenv, "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
            win_w = ww; win_h = wh;
        } else {
            int scale = senv ? std::atoi(senv) : 0;
            if (scale < 1) {
                SDL_Rect r{0, 0, 0, 0};
                if (SDL_GetDisplayUsableBounds(0, &r) != 0 || r.w <= 0) {
                    SDL_DisplayMode dm;
                    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { r.w = dm.w; r.h = dm.h; }
                }
                // Leave room for the title bar / dock / panels vertically.
                scale = (r.w > 0) ? std::min(r.w / 480, (r.h - 40) / 272) : 1;
                if (scale < 1) scale = 1;
            }
            win_w = 480 * scale; win_h = 272 * scale;
        }
    }
    const char* fs_env = std::getenv("PSPRECOMP_FULLSCREEN");
    const bool use_vk = vk_renderer_requested();
    uint32_t wflags = (use_vk ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL) | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                      SDL_WINDOW_ALLOW_HIGHDPI;
    if (fs_env && fs_env[0] && fs_env[0] != '0') wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#ifndef PSP_GLES
    {   // PSPRECOMP_BORDERLESS=1 (settings: Display > Borderless): a window with no frame that fills the display
        const char* bl = std::getenv("PSPRECOMP_BORDERLESS");
        if (bl && bl[0] && bl[0] != '0' && !(wflags & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
            wflags |= SDL_WINDOW_BORDERLESS;
            SDL_Rect r{0, 0, 0, 0};
            if (SDL_GetDisplayBounds(0, &r) == 0 && r.w > 0) { win_w = r.w; win_h = r.h; }
        }
    }
#endif
#ifdef PSP_GLES
    wflags |= SDL_WINDOW_FULLSCREEN;   // Android: one full-screen window; SDL sizes it to the display
    win_w = win_h = 0;
#endif
    g_window = SDL_CreateWindow(
        g_window_title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        win_w, win_h, wflags);
    apply_window_icon(g_window);
    if (!g_window) {
        std::fprintf(stderr,
            "[RT] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 4. Create GL context (must precede gladLoadGL — Pitfall 6). With the Vulkan backend there is no GL context: the
    //    Vulkan layer is brought up after the window exists and installs the GL entry points itself (step 5).
    if (!use_vk) {
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        std::fprintf(stderr,
            "[RT] SDL_GL_CreateContext failed: %s\n",
            SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
    }

    SDL_StopTextInput();   // started while the on-screen keyboard is open (osk_tick)

    // 5. Load GL function pointers via GLAD2 (MUST be after context creation)
    //    GLAD2 requires a loader function; SDL2 provides SDL_GL_GetProcAddress.
#if defined(PSPRECOMP_HAVE_VKGL)
#ifdef PSP_GLES
    void psp_gl_table_use_gles();   // psp_gl_table.cpp
    psp_gl_table_use_gles();   // the GL calls go through a table here (psp_gl_table.h); the Vulkan layer replaces its entries
#endif
    if (use_vk) {
        std::string verr;
        if (!vkgl::init(g_window, psp_vsync_wanted() != 0, &verr)) {
            std::fprintf(stderr, "[RT] Vulkan backend unavailable: %s\n", verr.c_str());
            SDL_DestroyWindow(g_window);
            SDL_Quit();
            return 1;
        }
    } else
#endif
#ifdef PSP_GLES
    if (false) {   // libGLESv3 exports the ES 3.2 entry points directly
#else
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
#endif
        std::fprintf(stderr, "[RT] gladLoadGL failed\n");
        SDL_GL_DeleteContext(g_gl_context);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    gl_stats_install();   // PSPRECOMP_GL_STATS=1 call counters (psp_gl_stats.cpp)

    // Vertical sync is a setting (off by default on desktops): the guest's vblank is simulated (hle display), so
    // without it the swap never waits; with it the swap waits for the display, which on a VRR display follows the game.
    if (!use_vk) psp_vsync_apply_gl();

    // 6. Print GL info for diagnostics
    std::fprintf(stderr, "[RT] GL: %s\n",
        glGetString(GL_VERSION));
    std::fprintf(stderr, "[RT] GL renderer: %s (%s)\n",
        glGetString(GL_RENDERER), glGetString(GL_VENDOR));
    if (!use_vk) psp_report::note_gpu(reinterpret_cast<const char*>(glGetString(GL_VERSION)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    if (gl_debug && !use_vk) gl_debug_install();

    // 7. Clear to black and swap — show initial frame
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    psp_gl_swap(g_window);

#if defined(PSPRECOMP_HAVE_VKGL)
    if (use_vk) {
        // A shader compiled during play (cache miss outside the warm-up) stalls the draw: say so. One toast that every further compile
        // renews (it hides 1.5 s after the last one); ge_draw_toast only stores the text, the render thread draws it. PSPRECOMP_SHADER_TOAST=0 turns it off.
        const char* e = std::getenv("PSPRECOMP_SHADER_TOAST");
        static int toast_ms = 1500;   // PSPRECOMP_SHADER_TOAST=<ms >= 100>: testing, keep the toast up longer
        if (e && std::atoi(e) >= 100) toast_ms = std::atoi(e);
        if (!(e && e[0] == '0'))
            pspvk::set_compile_miss_hook([] {
                static std::atomic<int> n{0};
                if (n.fetch_add(1) == 0) std::fprintf(stderr, "[VKGL] compiling a shader during play (toast shown)\n");
                ge_draw_toast("Compiling shaders...", toast_ms, 10);
            });
    }
    if (use_vk) vkgl::warmup_shaders();   // compile the shader pack's missing shaders behind a progress screen (vk/psp_vk_warmup.cpp)
#endif
    std::fprintf(stderr, "[RT] SDL/GL initialized (480x272)\n");
    return 0;
}

// Google sign-in (cloud sync): when a sign-in code appears, copy it to the clipboard and open the verification page in the
// browser, so the person only has to paste it. SDL's URL opener belongs on this (main) thread, so it is done here, not in the
// sync worker. Once per code.
static void cloud_signin_assist() {
    static std::string opened_code;
    static bool minimized = false;   // we stepped aside for the browser: come back when the sign-in is over
    const psp_cloud_svc::Status st = psp_cloud_svc::status();
    if (st.state != psp_cloud_svc::Status::SigningIn || st.user_code.empty()) {
        if (st.state != psp_cloud_svc::Status::SigningIn) {
            opened_code.clear();
            if (minimized && g_window) {
                minimized = false;
                SDL_RestoreWindow(g_window);
                SDL_RaiseWindow(g_window);
            }
        }
        return;
    }
    if (st.user_code == opened_code) return;
    opened_code = st.user_code;
    if (SDL_SetClipboardText(st.user_code.c_str()) != 0) std::fprintf(stderr, "[CLOUD] cannot copy the code: %s\n", SDL_GetError());
    if (SDL_OpenURL(st.verify_url.c_str()) != 0) std::fprintf(stderr, "[CLOUD] cannot open the browser: %s\n", SDL_GetError());
    else std::fprintf(stderr, "[CLOUD] opened %s (code %s copied)\n", st.verify_url.c_str(), st.user_code.c_str());
#ifndef __ANDROID__
    // A fullscreen game window would hide the browser: minimize it until the sign-in finishes (it is restored above).
    // (Android opens the browser as its own app on top of the game.)
    if (g_window && (SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
        SDL_MinimizeWindow(g_window);
        minimized = true;
    }
#endif
}

// ---------------------------------------------------------------------------
// psp_event_loop — main-thread loop: SDL events + render queue + alive check
// ---------------------------------------------------------------------------
void psp_event_loop(uint8_t* rdram) {
    while (!g_should_exit.load()) {
        // 1. Pump SDL events (window close, keyboard input)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            handle_sdl_event(ev);
        }

        // 2. Drain render queue — execute pending GL work from game threads
        render_queue_process();

        // 2a. Liveness net (issue #59): present the front buffer if lists
        //     were processed but no real page-flip arrived recently. No-op
        //     when a flip just presented (not dirty / within budget).
        ge_draw_present_if_stale(rdram);

        // 2b. Service any pending debug-socket screenshot request here on
        //     the main (GL) thread — the socket thread never touches GL.
        ge_draw_service_screenshot_request();

        // 2c. Settings menu: input repeat, drawing and presenting while the game is paused.
        psp_settings_tick();
        if (psp_settings_take_gate_dirty()) publish_input();   // the menu opened / closed from the tick: gate (or release) the game's input
        osk_tick();
        cloud_signin_assist();

        // 3. Check if any game threads are still alive
        if (g_alive_threads.load() == 0) {
            std::fprintf(stderr,
                "[RT] All game threads finished — lingering 3s to flush render\n");
            auto linger_end = std::chrono::steady_clock::now()
                              + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < linger_end
                   && !g_should_exit.load()) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) {
                    handle_sdl_event(ev);
                }
                render_queue_process();
                ge_draw_present_if_stale(rdram);
                ge_draw_service_screenshot_request();
                SDL_Delay(16);  // ~60fps drain rate
            }
            break;
        }

        // 4. Brief sleep — 1ms balances CPU usage vs responsiveness.
        //    Skip delay when GE lists are pending to drain them faster.
        render_queue_wait_for_work(1);
    }

    // Final drain to unblock any game thread blocked in render_queue_post
    render_queue_process();
}

// ---------------------------------------------------------------------------
// Signal handling — SIGTERM / SIGINT set g_should_exit
// ---------------------------------------------------------------------------
static void signal_handler(int sig) {
    (void)sig;
    g_should_exit.store(true);
}

void psp_install_signal_handlers() {
#ifdef _WIN32
    std::signal(SIGTERM, signal_handler);   // Ctrl+C / console close
    std::signal(SIGINT, signal_handler);
#else
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// psp_runtime_shutdown — clean shutdown: threads, render queue, SDL/GL
// ---------------------------------------------------------------------------
SDL_Window* psp_get_sdl_window() {
    return g_window;
}

void psp_runtime_shutdown() {
    // 1. Ensure exit flag is set (in case called without signal)
    g_should_exit.store(true);

    // 2. Shutdown scheduler — wake all threads, join with timeout
    psp_scheduler_shutdown();

    // 3. Final render queue drain
    render_queue_process();

    // 4. Destroy GL context
#if defined(PSPRECOMP_HAVE_VKGL)
    vkgl::shutdown();
#endif
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = nullptr;
    }

    // 5. Destroy SDL window
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }

    // 5b. Close controllers
    for (auto& kv : g_pads) SDL_GameControllerClose(kv.second);
    g_pads.clear();

    // 6. Quit SDL
    SDL_Quit();

    std::fprintf(stderr, "[RT] Shutdown complete\n");
}

void psp_input_set_prompt_setting(int setting) { g_prompt_setting = setting < -1 || setting > PSP_PROMPT_KEYBOARD ? -1 : setting; prompts_update(); }
int psp_input_prompt_setting() { return g_prompt_setting; }
PspPrompts psp_input_prompts() { std::lock_guard<std::mutex> lk(g_prompt_mu); return g_prompts; }

void psp_input_set_lr_triggers(bool on) { g_lr_triggers = on; update_triggers(); prompts_update(); publish_input(); }
bool psp_input_lr_triggers() { return g_lr_triggers; }
std::vector<std::pair<std::string, std::string>> psp_input_pads() {
    std::vector<std::pair<std::string, std::string>> v;
    for (auto& pr : g_pads) {
        const std::string g = pad_guid(pr.second);
        bool dup = false;
        for (auto& e : v) dup |= e.first == g;
        if (!dup) v.emplace_back(g, SDL_GameControllerName(pr.second) ? SDL_GameControllerName(pr.second) : "Controller");
    }
    return v;
}
void psp_input_set_p1_controller(const std::string& choice) {
    g_p1_pad = choice == "any" ? "" : choice;
    g_pad_buttons = 0; g_pad_triggers = 0; g_trig_l_down = g_trig_r_down = false;   // nothing stays held from a pad that no longer counts
    g_lx_raw = g_ly_raw = g_rx_raw = g_ry_raw = 0;
    publish_input();
    prompts_update();
}
std::string psp_input_p1_controller() { return g_p1_pad; }

