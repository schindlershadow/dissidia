// On-screen touch controls and their layout editor, see include/psp_touch.h.
#include "psp_touch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "psp_gl.h"

namespace {

// ===================================================================================================================
// Controls
// ===================================================================================================================
enum Type { T_STICK, T_DPAD, T_FACE, T_RECT, T_MENU, T_COMBO };
using Btn = SDL_GameControllerButton;

struct Ctl {
    std::string id;
    Type type = T_FACE;
    float x = 0.5f, y = 0.5f;     // centre as a fraction of the window
    float s = 1.0f;               // size multiplier
    float a = 1.0f;               // opacity multiplier
    bool on = true;               // false: hidden and ignores touches
    std::vector<Btn> btns;        // what a press does (one button, or a combination)
    bool latch = false;           // a tap toggles the press (stays held) instead of pressing while touched
    bool latched = false;
    int face = -1;                // 0 triangle 1 circle 2 cross 3 square (symbol drawn on a T_FACE control)
    char glyph = 0;               // 'L' 'R' 'S'tart 'E' select: label of a T_RECT control
    bool custom() const { return type == T_COMBO; }
    bool pressed = false;         // drawing state
};

std::vector<Ctl> g_ctl;
bool g_loaded = false;
bool g_float = false;             // floating stick: touching an empty part of the screen starts a stick right there (saved in the layout)

// short names of the buttons a combination can hold (picker order)
struct BtnName { Btn b; const char* name; };
const BtnName k_names[12] = {
    {SDL_CONTROLLER_BUTTON_DPAD_UP, "UP"}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "DN"}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "LF"}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "RT"},
    {SDL_CONTROLLER_BUTTON_Y, "TRI"}, {SDL_CONTROLLER_BUTTON_B, "CIR"}, {SDL_CONTROLLER_BUTTON_A, "CRS"}, {SDL_CONTROLLER_BUTTON_X, "SQR"},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L"}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R"}, {SDL_CONTROLLER_BUTTON_START, "STA"}, {SDL_CONTROLLER_BUTTON_BACK, "SEL"}};
const char* btn_name(Btn b) {
    for (auto& n : k_names) if (n.b == b) return n.name;
    return "?";
}
std::string combo_label(const Ctl& c) {
    std::string t;
    for (size_t i = 0; i < c.btns.size(); i++) { if (i) t += "+"; t += btn_name(c.btns[i]); }
    return t;
}

Ctl make(const char* id, Type t, float x, float y, std::vector<Btn> b = {}, int face = -1, char glyph = 0) {
    Ctl c;
    c.id = id; c.type = t; c.x = x; c.y = y; c.btns = std::move(b); c.face = face; c.glyph = glyph;
    return c;
}
void default_controls(std::vector<Ctl>& v, float w, float h) {
    v.clear();
    const float u = h, m = 0.05f * u;
    const float sx = m + 0.20f * u, sy = h - m - 0.20f * u;           // stick
    const float dx = sx, dy = sy - 0.17f * u - 0.04f * u - 0.16f * u;   // d-pad above it
    const float fx = w - m - 0.26f * u, fy = h - m - 0.27f * u, d = 0.115f * u;
    v.push_back(make("stick", T_STICK, sx / w, sy / h));
    v.push_back(make("dpad", T_DPAD, dx / w, dy / h));
    v.push_back(make("tri", T_FACE, fx / w, (fy - d) / h, {SDL_CONTROLLER_BUTTON_Y}, 0));
    v.push_back(make("cir", T_FACE, (fx + d) / w, fy / h, {SDL_CONTROLLER_BUTTON_B}, 1));
    v.push_back(make("cro", T_FACE, fx / w, (fy + d) / h, {SDL_CONTROLLER_BUTTON_A}, 2));
    v.push_back(make("sqr", T_FACE, (fx - d) / w, fy / h, {SDL_CONTROLLER_BUTTON_X}, 3));
    v.push_back(make("l", T_RECT, (m + 0.13f * u) / w, (m + 0.055f * u) / h, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, -1, 'L'));
    v.push_back(make("r", T_RECT, (w - m - 0.13f * u) / w, (m + 0.055f * u) / h, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, -1, 'R'));
    v.push_back(make("select", T_RECT, (w * 0.5f - 0.16f * u) / w, (h - m - 0.0375f * u) / h, {SDL_CONTROLLER_BUTTON_BACK}, -1, 'E'));
    v.push_back(make("start", T_RECT, (w * 0.5f + 0.16f * u) / w, (h - m - 0.0375f * u) / h, {SDL_CONTROLLER_BUTTON_START}, -1, 'S'));
    v.push_back(make("menu", T_MENU, 0.5f, (m + 0.04f * u) / h, {SDL_CONTROLLER_BUTTON_GUIDE}));
    for (auto& c : v) if (c.id == "start" || c.id == "select") c.s = 1.0f;
}

// ---- geometry (pixels) -----------------------------------------------------------------------------------------------
int g_win_w = 0, g_win_h = 0;
struct Circle { float x, y, r; };
struct Rect { float x, y, w, h; };

Circle circle_of(const Ctl& c) {
    const float h = (float)g_win_h;
    float r = 0.075f * h;
    if (c.type == T_STICK) r = 0.17f * h;
    else if (c.type == T_MENU) r = 0.04f * h;
    return {c.x * g_win_w, c.y * g_win_h, r * c.s};
}
Rect rect_of(const Ctl& c) {
    const float h = (float)g_win_h;
    float w = 0.26f * h, hh = 0.11f * h;
    if (c.type == T_DPAD) { w = hh = 0.32f * h; }
    else if (c.glyph == 'S' || c.glyph == 'E') { w = 0.22f * h; hh = 0.075f * h; }
    w *= c.s; hh *= c.s;
    return {c.x * g_win_w - w / 2, c.y * g_win_h - hh / 2, w, hh};
}
bool is_round(const Ctl& c) { return c.type == T_STICK || c.type == T_FACE || c.type == T_MENU || c.type == T_COMBO; }
bool in_circle(const Circle& c, float x, float y, float grow = 1.25f) {
    const float dx = x - c.x, dy = y - c.y;
    return dx * dx + dy * dy <= c.r * grow * c.r * grow;
}
bool in_rect(const Rect& r, float x, float y, float grow = 0.0f) {
    return x >= r.x - grow && x <= r.x + r.w + grow && y >= r.y - grow && y <= r.y + r.h + grow;
}
bool hit(const Ctl& c, float x, float y) {
    if (is_round(c)) return in_circle(circle_of(c), x, y, c.type == T_STICK ? 1.5f : c.type == T_MENU ? 1.4f : 1.25f);
    return in_rect(rect_of(c), x, y, 0.02f * g_win_h);
}

// ===================================================================================================================
// Persistence: <work folder>/touch_layout.txt
//   ctl <id> x=.. y=.. s=.. a=.. on=0|1                              a built-in control
//   combo <id> x=.. y=.. s=.. a=.. on=0|1 latch=0|1 btns=L,R,TRI     a combination button
// ===================================================================================================================
const char* k_layout_file = "touch_layout.txt";

Btn btn_from_name(const std::string& n, bool* ok) {
    for (auto& e : k_names) if (n == e.name) { *ok = true; return e.b; }
    *ok = false;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

void save_layout() {
    FILE* f = std::fopen(k_layout_file, "w");
    if (!f) { std::fprintf(stderr, "[TOUCH] cannot write %s\n", k_layout_file); return; }
    std::fprintf(f, "# Touch control layout (edited in Settings > System > Edit Touch Controls). Positions are fractions of the window.\n");
    std::fprintf(f, "option stick=%s\n", g_float ? "floating" : "fixed");
    for (auto& c : g_ctl) {
        if (c.custom()) {
            std::string b;
            for (size_t i = 0; i < c.btns.size(); i++) { if (i) b += ","; b += btn_name(c.btns[i]); }
            std::fprintf(f, "combo %s x=%.4f y=%.4f s=%.3f a=%.3f on=%d latch=%d btns=%s\n", c.id.c_str(), c.x, c.y, c.s, c.a, c.on, c.latch, b.c_str());
        } else {
            std::fprintf(f, "ctl %s x=%.4f y=%.4f s=%.3f a=%.3f on=%d\n", c.id.c_str(), c.x, c.y, c.s, c.a, c.on);
        }
    }
    std::fclose(f);
}

void load_layout() {
    if (g_loaded || g_win_w <= 0) return;
    g_loaded = true;
    default_controls(g_ctl, (float)g_win_w, (float)g_win_h);
    FILE* f = std::fopen(k_layout_file, "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        std::vector<std::string> tok;
        size_t p = 0;
        while (p < l.size()) {
            size_t e = l.find(' ', p);
            if (e == std::string::npos) e = l.size();
            if (e > p) tok.push_back(l.substr(p, e - p));
            p = e + 1;
        }
        if (tok.size() == 2 && tok[0] == "option") { if (tok[1] == "stick=floating") g_float = true; else if (tok[1] == "stick=fixed") g_float = false; continue; }
        if (tok.size() < 2 || (tok[0] != "ctl" && tok[0] != "combo")) continue;
        Ctl* c = nullptr;
        for (auto& k : g_ctl) if (k.id == tok[1] && !k.custom() == (tok[0] == "ctl")) { c = &k; break; }
        Ctl fresh;
        if (!c) {
            if (tok[0] == "ctl") continue;   // an unknown built-in id (older/newer version): ignore
            fresh.type = T_COMBO; fresh.id = tok[1];
            c = &fresh;
        }
        for (size_t i = 2; i < tok.size(); i++) {
            const size_t eq = tok[i].find('=');
            if (eq == std::string::npos) continue;
            const std::string k = tok[i].substr(0, eq), v = tok[i].substr(eq + 1);
            if (k == "x") c->x = std::clamp((float)std::atof(v.c_str()), 0.0f, 1.0f);
            else if (k == "y") c->y = std::clamp((float)std::atof(v.c_str()), 0.0f, 1.0f);
            else if (k == "s") c->s = std::clamp((float)std::atof(v.c_str()), 0.4f, 3.0f);
            else if (k == "a") c->a = std::clamp((float)std::atof(v.c_str()), 0.2f, 2.5f);
            else if (k == "on") c->on = v != "0";
            else if (k == "latch") c->latch = v == "1";
            else if (k == "btns") {
                c->btns.clear();
                size_t q = 0;
                while (q <= v.size()) {
                    size_t e = v.find(',', q);
                    if (e == std::string::npos) e = v.size();
                    bool ok;
                    Btn b = btn_from_name(v.substr(q, e - q), &ok);
                    if (ok && std::find(c->btns.begin(), c->btns.end(), b) == c->btns.end()) c->btns.push_back(b);
                    q = e + 1;
                }
            }
        }
        if (c == &fresh && !fresh.btns.empty()) g_ctl.push_back(fresh);
    }
    std::fclose(f);
}

// ===================================================================================================================
// State and events
// ===================================================================================================================
struct Finger { float x, y, ox, oy; bool stick; int dpad = -1; };   // dpad: the d-pad control this finger went down on (it stays captured)   // ox,oy: where the finger went down (the centre of a floating stick)
std::map<SDL_FingerID, Finger> g_fingers;
bool g_btn_state[SDL_CONTROLLER_BUTTON_MAX] = {};
int16_t g_ax = 0, g_ay = 0;
float g_knob_x = 0, g_knob_y = 0;
bool g_dpad_dir[4] = {};          // up right down left, for drawing
bool g_visible = false;
int g_mode = -1;                  // 0 auto, 1 on, 2 off
bool g_edit = false;
bool g_fl_active = false;         // drawing: a floating stick is being used
float g_fl_cx = 0, g_fl_cy = 0;

int resolve_mode() {
    if (g_mode >= 0) return g_mode;
    const char* e = std::getenv("PSPRECOMP_TOUCH");
#ifdef __ANDROID__
    g_mode = 0;
#else
    g_mode = 2;
#endif
    if (e && e[0]) {
        if (!std::strcmp(e, "on") || !std::strcmp(e, "1")) g_mode = 1;
        else if (!std::strcmp(e, "off") || !std::strcmp(e, "0")) g_mode = 2;
        else if (!std::strcmp(e, "auto")) g_mode = 0;
    }
    return g_mode;
}
bool shown() {
    const int m = resolve_mode();
    return g_edit || m == 1 || (m == 0 && g_visible);
}

void push_button(Btn b, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    e.cbutton.which = PSP_TOUCH_PAD_ID;
    e.cbutton.button = (Uint8)b;
    e.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&e);
}
void push_axis(SDL_GameControllerAxis a, int16_t v) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_CONTROLLERAXISMOTION;
    e.caxis.which = PSP_TOUCH_PAD_ID;
    e.caxis.axis = (Uint8)a;
    e.caxis.value = v;
    SDL_PushEvent(&e);
}

// Recompute every control from the fingers and the latches; push the differences as controller events.
void recompute() {
    int want[SDL_CONTROLLER_BUTTON_MAX] = {};
    float sx = 0, sy = 0;
    g_fl_active = false;
    g_dpad_dir[0] = g_dpad_dir[1] = g_dpad_dir[2] = g_dpad_dir[3] = false;
    for (auto& c : g_ctl) c.pressed = false;
    if (!g_edit) {
        for (auto& kv : g_fingers) {
            const Finger& f = kv.second;
            if (f.stick) {
                for (auto& c : g_ctl) {
                    if (c.type != T_STICK || (!c.on && !g_float)) continue;
                    Circle sc = circle_of(c);
                    if (g_float) { sc.x = f.ox; sc.y = f.oy; g_fl_active = true; g_fl_cx = f.ox; g_fl_cy = f.oy; }
                    float dx = (f.x - sc.x) / sc.r, dy = (f.y - sc.y) / sc.r;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 1.0f) { dx /= len; dy /= len; }
                    sx = dx; sy = dy;
                    c.pressed = true;
                }
                continue;
            }
            for (size_t ci = 0; ci < g_ctl.size(); ci++) {
                Ctl& c = g_ctl[ci];
                if (!c.on || c.type == T_STICK || c.latch) continue;
                if (c.type == T_DPAD) {
                    const Rect r = rect_of(c);
                    const bool captured = f.dpad == (int)ci;   // a finger that went down on the d-pad keeps steering it, even outside the box
                    if (!captured && !in_rect(r, f.x, f.y, 0.03f * g_win_h)) continue;
                    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, dx = f.x - cx, dy = f.y - cy;
                    if (dx * dx + dy * dy <= (0.05f * g_win_h * c.s) * (0.05f * g_win_h * c.s)) { c.pressed = true; continue; }   // dead centre
                    const float ang = std::atan2(dy, dx);
                    // cardinal zones are 60 degrees wide, diagonals 30 (a thumb slightly off the axis must not add a second direction)
                    const float t = 0.5f, co = std::cos(ang), si = std::sin(ang);
                    if (si < -t) g_dpad_dir[0] = true;
                    if (co > t) g_dpad_dir[1] = true;
                    if (si > t) g_dpad_dir[2] = true;
                    if (co < -t) g_dpad_dir[3] = true;
                    c.pressed = true;
                    continue;
                }
                if (!hit(c, f.x, f.y)) continue;
                c.pressed = true;
                for (Btn b : c.btns) want[b]++;
            }
        }
        for (auto& c : g_ctl) {
            if (c.on && c.latch && c.latched) { c.pressed = true; for (Btn b : c.btns) want[b]++; }
        }
        if (g_dpad_dir[0]) want[SDL_CONTROLLER_BUTTON_DPAD_UP]++;
        if (g_dpad_dir[1]) want[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]++;
        if (g_dpad_dir[2]) want[SDL_CONTROLLER_BUTTON_DPAD_DOWN]++;
        if (g_dpad_dir[3]) want[SDL_CONTROLLER_BUTTON_DPAD_LEFT]++;
    }
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        const bool now = want[b] > 0;
        if (now != g_btn_state[b]) { g_btn_state[b] = now; push_button((Btn)b, now); }
    }
    g_knob_x = sx; g_knob_y = sy;
    const float mag = std::sqrt(sx * sx + sy * sy);
    int16_t ax = 0, ay = 0;
    if (mag > 0.12f) {   // radial dead zone, rescaled so the full range is still reachable
        const float k = (mag - 0.12f) / (1.0f - 0.12f) / mag;
        ax = (int16_t)std::lround(std::clamp(sx * k, -1.0f, 1.0f) * 32767.0f);
        ay = (int16_t)std::lround(std::clamp(sy * k, -1.0f, 1.0f) * 32767.0f);
    }
    if (ax != g_ax) { g_ax = ax; push_axis(SDL_CONTROLLER_AXIS_LEFTX, ax); }
    if (ay != g_ay) { g_ay = ay; push_axis(SDL_CONTROLLER_AXIS_LEFTY, ay); }
}

void release_all() {
    g_fingers.clear();
    for (auto& c : g_ctl) c.latched = false;
    recompute();
}

// ===================================================================================================================
// Layout editor (touch driven, drawn in the overlay)
// ===================================================================================================================
enum Act { A_NONE, A_DONE, A_RESET, A_ADD, A_STICKMODE, A_SIZE_DN, A_SIZE_UP, A_OPAC_DN, A_OPAC_UP, A_TOGGLE_ON, A_LATCH, A_BUTTONS, A_DELETE,
           A_PICK_OK, A_PICK_CANCEL, A_PICK_BASE = 100 };
struct UiBtn { Rect r; std::string text; Act act; bool lit; int icon = -1; };   // icon: index into k_names (the picker draws the button's shape)
std::vector<UiBtn> g_ui;
int g_sel = -1;                    // selected control (index into g_ctl)
bool g_pick = false;               // the button picker is open
bool g_pick_new = false;           // ... for a new combination
std::vector<Btn> g_pick_set;
struct Drag { int ctl; float dx, dy; };
std::map<SDL_FingerID, Drag> g_drags;
std::map<SDL_FingerID, bool> g_ui_fingers;   // fingers that went down on a UI button (they do not drag)

std::string new_combo_id() {
    for (int i = 1; i < 1000; i++) {
        char id[16];
        std::snprintf(id, sizeof id, "c%d", i);
        bool used = false;
        for (auto& c : g_ctl) if (c.id == id) used = true;
        if (!used) return id;
    }
    return "c0";
}

void build_ui() {
    g_ui.clear();
    const float h = (float)g_win_h, w = (float)g_win_w;
    const float bh = 0.085f * h, bw = 0.2f * h, gap = 0.015f * h;
    auto row = [&](float y, std::vector<std::pair<std::string, Act>> items, const std::vector<bool>& lit = {}) {
        const float total = items.size() * bw + (items.size() - 1) * gap;
        float x = w / 2 - total / 2;
        for (size_t i = 0; i < items.size(); i++) {
            g_ui.push_back({{x, y, bw, bh}, items[i].first, items[i].second, i < lit.size() && lit[i]});
            x += bw + gap;
        }
    };
    if (g_pick) {
        const float cw = 0.17f * h, ch = 0.1f * h, cg = 0.012f * h;
        const float x0 = w / 2 - (4 * cw + 3 * cg) / 2, y0 = h * 0.36f;
        for (int i = 0; i < 12; i++) {
            const bool lit = std::find(g_pick_set.begin(), g_pick_set.end(), k_names[i].b) != g_pick_set.end();
            g_ui.push_back({{x0 + (i % 4) * (cw + cg), y0 + (i / 4) * (ch + cg), cw, ch}, k_names[i].name, (Act)(A_PICK_BASE + i), lit, i});
        }
        const float by = y0 + 3 * (ch + cg) + 0.02f * h;
        g_ui.push_back({{w / 2 - bw - gap / 2, by, bw, bh}, "OK", A_PICK_OK, false});
        g_ui.push_back({{w / 2 + gap / 2, by, bw, bh}, "CANCEL", A_PICK_CANCEL, false});
        return;
    }
    row(0.31f * h, {{"DONE", A_DONE}, {"RESET", A_RESET}, {"ADD COMBO", A_ADD}, {g_float ? "STICK: FLOATING" : "STICK: FIXED", A_STICKMODE}});
    if (g_sel >= 0 && g_sel < (int)g_ctl.size()) {
        const Ctl& c = g_ctl[(size_t)g_sel];
        row(0.55f * h, {{"SIZE -", A_SIZE_DN}, {"SIZE +", A_SIZE_UP}, {"OPACITY -", A_OPAC_DN}, {"OPACITY +", A_OPAC_UP}});
        if (c.custom())
            row(0.65f * h, {{c.on ? "SHOWN" : "HIDDEN", A_TOGGLE_ON}, {c.latch ? "HOLD: TOGGLE" : "HOLD: WHILE TOUCHED", A_LATCH}, {"BUTTONS", A_BUTTONS}, {"DELETE", A_DELETE}});
        else
            row(0.65f * h, {{c.on ? "SHOWN" : "HIDDEN", A_TOGGLE_ON}});
    }
}

void close_picker(bool apply) {
    if (apply && !g_pick_set.empty()) {
        if (g_pick_new) {
            Ctl c;
            c.id = new_combo_id(); c.type = T_COMBO; c.x = 0.5f; c.y = 0.45f; c.btns = g_pick_set;
            g_ctl.push_back(c);
            g_sel = (int)g_ctl.size() - 1;
        } else if (g_sel >= 0 && g_ctl[(size_t)g_sel].custom()) {
            g_ctl[(size_t)g_sel].btns = g_pick_set;
        }
        save_layout();
    }
    g_pick = false;
}

void do_action(int act) {
    Ctl* c = (g_sel >= 0 && g_sel < (int)g_ctl.size()) ? &g_ctl[(size_t)g_sel] : nullptr;
    if (act >= A_PICK_BASE) {
        const Btn b = k_names[act - A_PICK_BASE].b;
        auto it = std::find(g_pick_set.begin(), g_pick_set.end(), b);
        if (it != g_pick_set.end()) g_pick_set.erase(it); else g_pick_set.push_back(b);
        return;
    }
    switch (act) {
    case A_DONE: g_edit = false; g_sel = -1; g_drags.clear(); save_layout(); release_all(); break;
    case A_RESET:
        default_controls(g_ctl, (float)g_win_w, (float)g_win_h);
        g_sel = -1;
        save_layout();
        break;
    case A_STICKMODE: g_float = !g_float; for (auto& k : g_ctl) if (k.type == T_STICK) k.on = true; break;
    case A_ADD: g_pick = true; g_pick_new = true; g_pick_set.clear(); break;
    case A_SIZE_DN: if (c) c->s = std::max(0.4f, c->s - 0.1f); break;
    case A_SIZE_UP: if (c) c->s = std::min(3.0f, c->s + 0.1f); break;
    case A_OPAC_DN: if (c) c->a = std::max(0.2f, c->a - 0.2f); break;
    case A_OPAC_UP: if (c) c->a = std::min(2.5f, c->a + 0.2f); break;
    case A_TOGGLE_ON: if (c) c->on = !c->on; break;
    case A_LATCH: if (c) { c->latch = !c->latch; c->latched = false; } break;
    case A_BUTTONS: if (c && c->custom()) { g_pick = true; g_pick_new = false; g_pick_set = c->btns; } break;
    case A_DELETE:
        if (c && c->custom()) { g_ctl.erase(g_ctl.begin() + g_sel); g_sel = -1; }
        break;
    case A_PICK_OK: close_picker(true); break;
    case A_PICK_CANCEL: close_picker(false); break;
    default: break;
    }
    if (act != A_ADD && act != A_BUTTONS && act < A_PICK_BASE) save_layout();
}

void edit_finger(const SDL_Event& ev, float x, float y) {
    const SDL_FingerID id = ev.tfinger.fingerId;
    build_ui();
    if (ev.type == SDL_FINGERUP) {
        g_drags.erase(id);
        g_ui_fingers.erase(id);
        return;
    }
    if (ev.type == SDL_FINGERDOWN) {
        for (auto& b : g_ui) {
            if (in_rect(b.r, x, y)) { g_ui_fingers[id] = true; do_action((int)b.act); return; }
        }
        if (g_pick) return;
        int found = -1;
        for (int i = (int)g_ctl.size() - 1; i >= 0; i--) {   // topmost (last drawn) first
            if (hit(g_ctl[(size_t)i], x, y)) { found = i; break; }
        }
        g_sel = found;
        if (found >= 0) g_drags[id] = {found, g_ctl[(size_t)found].x * g_win_w - x, g_ctl[(size_t)found].y * g_win_h - y};
        return;
    }
    // motion: drag
    auto it = g_drags.find(id);
    if (it != g_drags.end() && !g_ui_fingers.count(id)) {
        Ctl& c = g_ctl[(size_t)it->second.ctl];
        c.x = std::clamp((x + it->second.dx) / g_win_w, 0.02f, 0.98f);
        c.y = std::clamp((y + it->second.dy) / g_win_h, 0.02f, 0.98f);
    }
}

}  // namespace

bool psp_touch_enabled() { return resolve_mode() != 2 || g_edit; }
int psp_touch_mode() { return resolve_mode(); }
void psp_touch_set_mode(int m) {
    g_mode = std::clamp(m, 0, 2);
    if (g_mode == 2) release_all();
}
bool psp_touch_editing() { return g_edit; }
void psp_touch_edit_begin() {
    if (resolve_mode() == 2) g_mode = 0;
    g_visible = true;
    release_all();
    load_layout();
    g_edit = true;
    g_sel = -1;
    g_pick = false;
    g_drags.clear();
    g_ui_fingers.clear();
}

void psp_touch_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.which != PSP_TOUCH_PAD_ID && !g_edit) g_visible = false;   // a real pad button: auto mode hides the controls
        return;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.which != PSP_TOUCH_PAD_ID && std::abs((int)ev.caxis.value) > 16000 && !g_edit) g_visible = false;
        return;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        break;
    default:
        return;
    }
    if (!psp_touch_enabled()) return;
    if (g_win_w <= 0 || g_win_h <= 0) return;   // not drawn yet: the layout is unknown
    load_layout();
    const float x = ev.tfinger.x * g_win_w, y = ev.tfinger.y * g_win_h;
    if (g_edit) { edit_finger(ev, x, y); return; }
    const SDL_FingerID id = ev.tfinger.fingerId;
    if (ev.type == SDL_FINGERUP) {
        g_fingers.erase(id);
    } else {
        if (!shown()) {                           // the first touch only shows the controls
            if (resolve_mode() == 0) g_visible = true;
            return;
        }
        auto it = g_fingers.find(id);
        if (it == g_fingers.end()) {
            bool is_stick = false;
            if (g_float) {   // floating stick: a touch that is on no control starts the stick there (one at a time)
                bool on_ctl = false, have = false;
                for (auto& c : g_ctl) if (c.on && c.type != T_STICK && hit(c, x, y)) on_ctl = true;
                for (auto& kv : g_fingers) if (kv.second.stick) have = true;
                is_stick = !on_ctl && !have;
            } else {
                for (auto& c : g_ctl) if (c.type == T_STICK && c.on && hit(c, x, y)) is_stick = true;
            }
            g_fingers[id] = {x, y, x, y, is_stick};
            if (!is_stick)
                for (size_t ci = 0; ci < g_ctl.size(); ci++)
                    if (g_ctl[ci].on && g_ctl[ci].type == T_DPAD && hit(g_ctl[ci], x, y)) g_fingers[id].dpad = (int)ci;
            static const bool tlog = std::getenv("PSPRECOMP_TOUCH_LOG") != nullptr;
            if (tlog) std::fprintf(stderr, "[TOUCH] finger down %.0f,%.0f stick=%d float=%d\n", x, y, (int)is_stick, (int)g_float);
            for (auto& c : g_ctl)   // a tap on a toggle-hold button flips it
                if (c.on && c.latch && !is_stick && hit(c, x, y)) c.latched = !c.latched;
        } else {
            it->second.x = x; it->second.y = y;
        }
    }
    recompute();
}

// ===================================================================================================================
// Drawing
// ===================================================================================================================
namespace {
struct Vtx { float x, y, r, g, b, a; };
std::vector<Vtx> g_vtx;
float g_white[4] = {1, 1, 1, 0.6f};
float g_shade[4] = {0.02f, 0.02f, 0.08f, 0.45f};
GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
GLint g_u_res = -1;

void tri(float x0, float y0, float x1, float y1, float x2, float y2, const float* c) {
    g_vtx.push_back({x0, y0, c[0], c[1], c[2], c[3]});
    g_vtx.push_back({x1, y1, c[0], c[1], c[2], c[3]});
    g_vtx.push_back({x2, y2, c[0], c[1], c[2], c[3]});
}
void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, const float* c) {
    tri(x0, y0, x1, y1, x2, y2, c);
    tri(x0, y0, x2, y2, x3, y3, c);
}
void fill_rect(const Rect& r, const float* c) { quad(r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h, c); }
void fill_circle(float cx, float cy, float r, const float* c, int seg = 40) {
    for (int i = 0; i < seg; i++) {
        const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
        tri(cx, cy, cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), c);
    }
}
void ring(float cx, float cy, float r, float t, const float* c, int seg = 48) {
    if (c == g_white) ring(cx, cy, r + t * 0.6f, t * 2.2f, g_shade, seg);   // dark halo under a white outline
    for (int i = 0; i < seg; i++) {
        const float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
        const float ro = r, ri = r - t;
        quad(cx + ro * std::cos(a0), cy + ro * std::sin(a0), cx + ro * std::cos(a1), cy + ro * std::sin(a1),
             cx + ri * std::cos(a1), cy + ri * std::sin(a1), cx + ri * std::cos(a0), cy + ri * std::sin(a0), c);
    }
}
void line(float x0, float y0, float x1, float y1, float t, const float* c) {
    const float dx = x1 - x0, dy = y1 - y0, len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
    const float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}
void frame_rect(const Rect& r, float t, const float* c) {
    if (c == g_white) frame_rect({r.x - t * 0.6f, r.y - t * 0.6f, r.w + t * 1.2f, r.h + t * 1.2f}, t * 2.2f, g_shade);
    fill_rect({r.x, r.y, r.w, t}, c); fill_rect({r.x, r.y + r.h - t, r.w, t}, c);
    fill_rect({r.x, r.y, t, r.h}, c); fill_rect({r.x + r.w - t, r.y, t, r.h}, c);
}

// 5x7 bitmap font: A-Z 0-9 and a few symbols (the editor's and the combination labels' only text)
const char* glyph_rows(char ch) {
    static const struct { char c; const char* rows; } tab[] = {
        {'A', "01110" "10001" "10001" "11111" "10001" "10001" "10001"}, {'B', "11110" "10001" "10001" "11110" "10001" "10001" "11110"},
        {'C', "01110" "10001" "10000" "10000" "10000" "10001" "01110"}, {'D', "11110" "10001" "10001" "10001" "10001" "10001" "11110"},
        {'E', "11111" "10000" "10000" "11110" "10000" "10000" "11111"}, {'F', "11111" "10000" "10000" "11110" "10000" "10000" "10000"},
        {'G', "01110" "10001" "10000" "10111" "10001" "10001" "01111"}, {'H', "10001" "10001" "10001" "11111" "10001" "10001" "10001"},
        {'I', "01110" "00100" "00100" "00100" "00100" "00100" "01110"}, {'J', "00111" "00010" "00010" "00010" "00010" "10010" "01100"},
        {'K', "10001" "10010" "10100" "11000" "10100" "10010" "10001"}, {'L', "10000" "10000" "10000" "10000" "10000" "10000" "11111"},
        {'M', "10001" "11011" "10101" "10101" "10001" "10001" "10001"}, {'N', "10001" "11001" "10101" "10011" "10001" "10001" "10001"},
        {'O', "01110" "10001" "10001" "10001" "10001" "10001" "01110"}, {'P', "11110" "10001" "10001" "11110" "10000" "10000" "10000"},
        {'Q', "01110" "10001" "10001" "10001" "10101" "10010" "01101"}, {'R', "11110" "10001" "10001" "11110" "10100" "10010" "10001"},
        {'S', "01111" "10000" "10000" "01110" "00001" "00001" "11110"}, {'T', "11111" "00100" "00100" "00100" "00100" "00100" "00100"},
        {'U', "10001" "10001" "10001" "10001" "10001" "10001" "01110"}, {'V', "10001" "10001" "10001" "10001" "10001" "01010" "00100"},
        {'W', "10001" "10001" "10001" "10101" "10101" "10101" "01010"}, {'X', "10001" "10001" "01010" "00100" "01010" "10001" "10001"},
        {'Y', "10001" "10001" "01010" "00100" "00100" "00100" "00100"}, {'Z', "11111" "00001" "00010" "00100" "01000" "10000" "11111"},
        {'0', "01110" "10001" "10011" "10101" "11001" "10001" "01110"}, {'1', "00100" "01100" "00100" "00100" "00100" "00100" "01110"},
        {'2', "01110" "10001" "00001" "00010" "00100" "01000" "11111"}, {'3', "11110" "00001" "00001" "01110" "00001" "00001" "11110"},
        {'4', "00010" "00110" "01010" "10010" "11111" "00010" "00010"}, {'5', "11111" "10000" "11110" "00001" "00001" "10001" "01110"},
        {'6', "00110" "01000" "10000" "11110" "10001" "10001" "01110"}, {'7', "11111" "00001" "00010" "00100" "01000" "01000" "01000"},
        {'8', "01110" "10001" "10001" "01110" "10001" "10001" "01110"}, {'9', "01110" "10001" "10001" "01111" "00001" "00010" "01100"},
        {'+', "00000" "00100" "00100" "11111" "00100" "00100" "00000"}, {'-', "00000" "00000" "00000" "11111" "00000" "00000" "00000"},
        {'.', "00000" "00000" "00000" "00000" "00000" "01100" "01100"}, {':', "00000" "01100" "01100" "00000" "01100" "01100" "00000"},
        {'/', "00001" "00010" "00010" "00100" "01000" "01000" "10000"},
    };
    for (auto& g : tab) if (g.c == ch) return g.rows;
    return nullptr;
}
float text_width(const std::string& s, float cell) { return s.empty() ? 0 : (s.size() * 6 - 1) * cell; }
void text(const std::string& s, float x, float y, float cell, const float* c) {   // x,y = top-left
    float px = x;
    for (char ch : s) {
        const char* g = glyph_rows((char)std::toupper((unsigned char)ch));
        if (g)
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (g[row * 5 + col] == '1') fill_rect({px + col * cell, y + row * cell, cell, cell}, c);
        px += 6 * cell;
    }
}
void text_center(const std::string& s, float cx, float cy, float maxw, float cell, const float* c) {
    if (text_width(s, cell) > maxw) cell = maxw / ((s.size() * 6 - 1));
    text(s, cx - text_width(s, cell) / 2, cy - 3.5f * cell, cell, c);
}

const char* k_vs = R"GLSL(
in vec2 a_pos;
in vec4 a_col;
uniform vec2 u_res;
out vec4 v_col;
void main() {
    v_col = a_col;
    gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);
}
)GLSL";
const char* k_fs = R"GLSL(
in vec4 v_col;
out vec4 o_col;
void main() { o_col = v_col; }
)GLSL";

GLuint compile(GLenum type, const char* body) {
    const char* src[2] = {PSP_GLSL_VERSION, body};
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[TOUCH] shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
bool gl_init() {
    if (g_prog) return true;
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) return false;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_col");
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { std::fprintf(stderr, "[TOUCH] link failed\n"); glDeleteProgram(g_prog); g_prog = 0; return false; }
    g_u_res = glGetUniformLocation(g_prog, "u_res");
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)(2 * sizeof(float)));
    return true;
}

// translucent fill; neutral controls (r=g=b=1) are dark navy so they show on light screens, face buttons keep their tint
const float* fill_col(bool down, float r, float g, float b, float alpha_mul) {
    static float c[4];
    const bool neutral = r == 1 && g == 1 && b == 1;
    c[0] = neutral ? 0.10f : r; c[1] = neutral ? 0.10f : g; c[2] = neutral ? 0.30f : b;
    c[3] = std::min(1.0f, (down ? (neutral ? 0.60f : 0.65f) : (neutral ? 0.32f : 0.35f)) * alpha_mul);
    return c;
}
const float* line_col(float alpha_mul) {
    static float c[4];
    c[0] = c[1] = c[2] = 1; c[3] = std::min(1.0f, 0.6f * alpha_mul);
    return c;
}

// The shape of a PSP button, centred at cx,cy with half-size s: arrows for the d-pad, the four face symbols in their colours, L / R,
// Start (play triangle) and Select (two bars). Used by the picker and on combination buttons.
void draw_btn_icon(Btn b, float cx, float cy, float s, float alpha) {
    static const float tint[4][3] = {{0.35f, 0.85f, 0.55f}, {0.95f, 0.40f, 0.40f}, {0.45f, 0.65f, 1.0f}, {0.95f, 0.55f, 0.85f}};
    static float c[4];
    const float t = std::max(2.0f, s * 0.22f);
    auto col = [&](float r, float g, float bl) { c[0] = r; c[1] = g; c[2] = bl; c[3] = alpha; return c; };
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:   // arrows (head + stem), so they cannot be mistaken for Start's play triangle
        tri(cx, cy - s, cx + s * 0.8f, cy + s * 0.05f, cx - s * 0.8f, cy + s * 0.05f, col(1, 1, 1));
        fill_rect({cx - s * 0.25f, cy, s * 0.5f, s}, col(1, 1, 1));
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        tri(cx, cy + s, cx - s * 0.8f, cy - s * 0.05f, cx + s * 0.8f, cy - s * 0.05f, col(1, 1, 1));
        fill_rect({cx - s * 0.25f, cy - s, s * 0.5f, s}, col(1, 1, 1));
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        tri(cx - s, cy, cx + s * 0.05f, cy - s * 0.8f, cx + s * 0.05f, cy + s * 0.8f, col(1, 1, 1));
        fill_rect({cx, cy - s * 0.25f, s, s * 0.5f}, col(1, 1, 1));
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        tri(cx + s, cy, cx - s * 0.05f, cy + s * 0.8f, cx - s * 0.05f, cy - s * 0.8f, col(1, 1, 1));
        fill_rect({cx - s, cy - s * 0.25f, s, s * 0.5f}, col(1, 1, 1));
        break;
    case SDL_CONTROLLER_BUTTON_Y: {
        const float* k = col(tint[0][0], tint[0][1], tint[0][2]);
        line(cx, cy - s, cx + s, cy + s * 0.8f, t, k); line(cx + s, cy + s * 0.8f, cx - s, cy + s * 0.8f, t, k); line(cx - s, cy + s * 0.8f, cx, cy - s, t, k);
        break; }
    case SDL_CONTROLLER_BUTTON_B: ring(cx, cy, s, t, col(tint[1][0], tint[1][1], tint[1][2])); break;
    case SDL_CONTROLLER_BUTTON_A: {
        const float* k = col(tint[2][0], tint[2][1], tint[2][2]);
        line(cx - s, cy - s, cx + s, cy + s, t, k); line(cx + s, cy - s, cx - s, cy + s, t, k);
        break; }
    case SDL_CONTROLLER_BUTTON_X: frame_rect({cx - s, cy - s, 2 * s, 2 * s}, t, col(tint[3][0], tint[3][1], tint[3][2])); break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: text_center("L", cx, cy, s * 4, s * 2 / 7.0f * 1.1f, col(1, 1, 1)); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: text_center("R", cx, cy, s * 4, s * 2 / 7.0f * 1.1f, col(1, 1, 1)); break;
    case SDL_CONTROLLER_BUTTON_START:   // a play triangle inside a ring: unmistakable next to the d-pad's Right arrow
        ring(cx, cy, s * 1.15f, t, col(1, 1, 1));
        tri(cx - s * 0.35f, cy - s * 0.6f, cx + s * 0.6f, cy, cx - s * 0.35f, cy + s * 0.6f, col(1, 1, 1));
        break;
    case SDL_CONTROLLER_BUTTON_BACK:
        fill_rect({cx - s, cy - s * 0.55f, 2 * s, s * 0.4f}, col(1, 1, 1));
        fill_rect({cx - s, cy + s * 0.15f, 2 * s, s * 0.4f}, col(1, 1, 1));
        break;
    default: break;
    }
}
// a combination's buttons as a row (or rows) of icons inside a circle of radius r
void draw_combo_icons(const std::vector<Btn>& btns, float cx, float cy, float r, float alpha) {
    const int n = (int)btns.size();
    if (n == 0) return;
    const int rows = (n + 2) / 3;
    const float s = r * (rows == 1 ? (n == 1 ? 0.5f : n == 2 ? 0.36f : 0.28f) : rows == 2 ? 0.25f : 0.19f);
    const float step = s * 2.5f;
    for (int ro = 0; ro < rows; ro++) {
        const int from = ro * 3, cnt = std::min(3, n - from);
        const float y = cy + (ro - (rows - 1) * 0.5f) * step;
        for (int i = 0; i < cnt; i++) draw_btn_icon(btns[(size_t)(from + i)], cx + (i - (cnt - 1) * 0.5f) * step, y, s, alpha);
    }
}

void draw_control(const Ctl& c, float lw) {
    if (c.type == T_STICK && g_float && !g_edit && !g_fl_active) return;   // floating stick: only drawn while in use
    const float am = c.on ? c.a : c.a * 0.55f;   // disabled controls (editor only) are drawn faint
    const float* wl = g_white;
    float white_local[4];
    std::memcpy(white_local, wl, sizeof white_local);
    g_white[3] = std::min(1.0f, 0.6f * am);
    const bool dn = c.pressed;
    const float grow = dn ? 1.16f : 1.0f;   // a pressed control is drawn larger (the touch area stays the same)
    if (c.type == T_STICK) {
        Circle b = circle_of(c);
        if (g_float && !g_edit) { b.x = g_fl_cx; b.y = g_fl_cy; }
        fill_circle(b.x, b.y, b.r, fill_col(false, 1, 1, 1, am));
        ring(b.x, b.y, b.r, lw, g_white);
        const bool act = g_knob_x != 0 || g_knob_y != 0;
        const float kr = b.r * 0.42f * (act ? 1.2f : 1.0f);
        const float kx = b.x + g_knob_x * (b.r - kr), ky = b.y + g_knob_y * (b.r - kr);
        fill_circle(kx, ky, kr, fill_col(act, 1, 1, 1, am));
        ring(kx, ky, kr, lw, g_white);
    } else if (c.type == T_DPAD) {
        // one cross: filled body, an outline with a dark halo, an arrow in every arm; a pressed arm is brighter and drawn larger
        const Rect r = rect_of(c);
        const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, S = r.w / 2, A = S * 0.33f;
        const float px[12] = {-A, A, A, S, S, A, A, -A, -A, -S, -S, -A};
        const float py[12] = {-S, -S, -A, -A, A, A, S, S, A, A, -A, -A};
        const float* body = fill_col(false, 1, 1, 1, am);
        for (int i = 0; i < 12; i++) {
            const int j = (i + 1) % 12;
            tri(cx, cy, cx + px[i], cy + py[i], cx + px[j], cy + py[j], body);
        }
        for (int i = 0; i < 12; i++) {   // outline (dark halo first, then white)
            const int j = (i + 1) % 12;
            line(cx + px[i], cy + py[i], cx + px[j], cy + py[j], lw * 3.2f, g_shade);
        }
        for (int i = 0; i < 12; i++) {
            const int j = (i + 1) % 12;
            line(cx + px[i], cy + py[i], cx + px[j], cy + py[j], lw, g_white);
        }
        const float dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        for (int i = 0; i < 4; i++) {
            const float qx = -dy[i], qy = dx[i];
            if (g_dpad_dir[i]) {   // the pressed arm: brighter and a little larger than the cross
                const float g = 1.12f;
                const float* hl = fill_col(true, 1, 1, 1, am);
                const float x0 = cx + dx[i] * A * 0.4f, y0 = cy + dy[i] * A * 0.4f, x1 = cx + dx[i] * S * g, y1 = cy + dy[i] * S * g;
                quad(x0 + qx * A * g, y0 + qy * A * g, x1 + qx * A * g, y1 + qy * A * g, x1 - qx * A * g, y1 - qy * A * g, x0 - qx * A * g, y0 - qy * A * g, hl);
            }
            const float k = g_dpad_dir[i] ? 1.12f : 1.0f;
            tri(cx + dx[i] * S * 0.82f * k, cy + dy[i] * S * 0.82f * k,
                cx + dx[i] * S * 0.50f * k + qx * A * 0.55f, cy + dy[i] * S * 0.50f * k + qy * A * 0.55f,
                cx + dx[i] * S * 0.50f * k - qx * A * 0.55f, cy + dy[i] * S * 0.50f * k - qy * A * 0.55f, g_white);
        }
    } else if (c.type == T_FACE || c.type == T_COMBO) {
        static const float tint[4][3] = {{0.35f, 0.85f, 0.55f}, {0.95f, 0.40f, 0.40f}, {0.45f, 0.65f, 1.0f}, {0.95f, 0.55f, 0.85f}};
        Circle b = circle_of(c);
        b.r *= grow;
        const bool latched_look = c.latch && c.latched;
        if (c.type == T_FACE) {
            const int i = c.face;
            fill_circle(b.x, b.y, b.r, fill_col(dn, tint[i][0], tint[i][1], tint[i][2], am));
            ring(b.x, b.y, b.r, lw, g_white);
            static float sym[4]; sym[0] = tint[i][0]; sym[1] = tint[i][1]; sym[2] = tint[i][2]; sym[3] = std::min(1.0f, 0.9f * am);
            const float s = b.r * 0.46f, t = lw * 1.3f;
            if (i == 0) {
                line(b.x, b.y - s, b.x + s, b.y + s * 0.8f, t, sym);
                line(b.x + s, b.y + s * 0.8f, b.x - s, b.y + s * 0.8f, t, sym);
                line(b.x - s, b.y + s * 0.8f, b.x, b.y - s, t, sym);
            } else if (i == 1) ring(b.x, b.y, s, t, sym);
            else if (i == 2) { line(b.x - s, b.y - s, b.x + s, b.y + s, t, sym); line(b.x + s, b.y - s, b.x - s, b.y + s, t, sym); }
            else frame_rect({b.x - s, b.y - s, 2 * s, 2 * s}, t, sym);
        } else {
            fill_circle(b.x, b.y, b.r, fill_col(dn || latched_look, 0.55f, 0.45f, 0.95f, am));
            ring(b.x, b.y, b.r, lw, g_white);
            if (c.latch) ring(b.x, b.y, b.r * 0.86f, lw * 0.6f, g_white);   // a second ring marks toggle-hold buttons
            draw_combo_icons(c.btns, b.x, b.y, b.r, std::min(1.0f, 0.95f * am));
        }
    } else if (c.type == T_RECT) {
        Rect r = rect_of(c);
        if (grow != 1.0f) { r.x -= r.w * (grow - 1) / 2; r.y -= r.h * (grow - 1) / 2; r.w *= grow; r.h *= grow; }
        fill_rect(r, fill_col(dn, 1, 1, 1, am));
        frame_rect(r, lw, g_white);
        if (c.glyph == 'L' || c.glyph == 'R') {
            text_center(std::string(1, c.glyph), r.x + r.w / 2, r.y + r.h / 2, r.w, r.h / 9.0f, g_white);
        } else if (c.glyph == 'E') {
            const float sy = r.y + r.h / 2;
            fill_rect({r.x + r.w / 2 - r.h * 0.45f, sy - r.h * 0.22f, r.h * 0.9f, r.h * 0.12f}, g_white);
            fill_rect({r.x + r.w / 2 - r.h * 0.45f, sy + r.h * 0.10f, r.h * 0.9f, r.h * 0.12f}, g_white);
        } else {
            draw_btn_icon(SDL_CONTROLLER_BUTTON_START, r.x + r.w / 2, r.y + r.h / 2, r.h * 0.27f, std::min(1.0f, 0.9f * am));
        }
    } else if (c.type == T_MENU) {
        Circle b = circle_of(c);
        b.r *= grow;
        fill_circle(b.x, b.y, b.r, fill_col(dn, 1, 1, 1, am));
        ring(b.x, b.y, b.r, lw, g_white);
        for (int i = -1; i <= 1; i++) fill_rect({b.x - b.r * 0.5f, b.y + i * b.r * 0.34f - lw * 0.5f, b.r, lw}, g_white);
    }
    std::memcpy(g_white, white_local, sizeof g_white);
}

void draw_editor(float lw) {
    const float w = (float)g_win_w, h = (float)g_win_h;
    static float backdrop[4] = {0.0f, 0.0f, 0.05f, 0.5f};
    fill_rect({0, 0, w, h}, backdrop);
}

void draw_editor_ui(float lw) {
    const float w = (float)g_win_w, h = (float)g_win_h;
    build_ui();
    static float title[4] = {1, 1, 1, 1};
    const float cell = 0.0055f * h;
    text_center(g_pick ? "CHOOSE THE BUTTONS" : "EDIT TOUCH CONTROLS", w / 2, 0.215f * h, 0.7f * w, cell * 1.2f, title);
    if (!g_pick) {
        text_center(g_sel >= 0 ? "DRAG TO MOVE. USE THE BUTTONS BELOW." : "DRAG A CONTROL TO MOVE IT. TAP TO SELECT.", w / 2, 0.265f * h, 0.8f * w, cell * 0.75f, title);
    }
    // selection highlight
    if (!g_pick && g_sel >= 0 && g_sel < (int)g_ctl.size()) {
        static float hl[4] = {1.0f, 0.85f, 0.2f, 0.95f};
        const Ctl& c = g_ctl[(size_t)g_sel];
        if (is_round(c)) { const Circle b = circle_of(c); ring(b.x, b.y, b.r * 1.18f, lw * 1.2f, hl); }
        else { Rect r = rect_of(c); const float g = lw * 3; frame_rect({r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g}, lw * 1.2f, hl); }
    }
    for (auto& b : g_ui) {
        static float f1[4], f2[4] = {1, 1, 1, 0.9f};
        f1[0] = 0.12f; f1[1] = 0.14f; f1[2] = 0.35f; f1[3] = 0.92f;
        if (b.lit) { f1[0] = 0.85f; f1[1] = 0.65f; f1[2] = 0.15f; }
        fill_rect(b.r, f1);
        frame_rect(b.r, lw, f2);
        if (b.icon >= 0) draw_btn_icon(k_names[b.icon].b, b.r.x + b.r.w / 2, b.r.y + b.r.h / 2, std::min(b.r.w, b.r.h) * 0.28f, 1.0f);
        else text_center(b.text, b.r.x + b.r.w / 2, b.r.y + b.r.h / 2, b.r.w * 0.9f, b.r.h / 13.0f, f2);
    }
    if (g_pick) {
        static float c2[4] = {1, 1, 1, 1};
        if (g_pick_set.empty()) text_center("NOTHING YET", w / 2, 0.315f * h, 0.8f * w, cell, c2);
        else {   // the chosen buttons as shapes, joined by plus signs
            const float is = 0.03f * h, stepx = is * 2.6f;
            const float total = (float)g_pick_set.size() * stepx - is * 0.6f;
            float px = w / 2 - total / 2 + is;
            for (size_t i = 0; i < g_pick_set.size(); i++) {
                if (i) text_center("+", px - stepx / 2, 0.315f * h, is * 2, is * 0.25f, c2);
                draw_btn_icon(g_pick_set[i], px, 0.315f * h, is, 1.0f);
                px += stepx;
            }
        }
    }
}
}  // namespace

void psp_touch_draw(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (resolve_mode() == 2 && !g_edit) return;
    if (w != g_win_w || h != g_win_h) {
        g_win_w = w; g_win_h = h;
    }
    load_layout();
    {   // PSPRECOMP_TOUCH_EDIT=1: open the layout editor at start (testing)
        static bool once = false;
        if (!once && g_loaded) { once = true; if (std::getenv("PSPRECOMP_TOUCH_EDIT")) psp_touch_edit_begin(); }
    }
    if (!shown()) return;
    g_vtx.clear();
    const float lw = std::max(2.0f, 0.006f * h);
    if (g_edit) draw_editor(lw);
    for (const Ctl& c : g_ctl) {
        if (!c.on && !g_edit) continue;
        draw_control(c, lw);
    }
    if (g_edit) draw_editor_ui(lw);

    // ---- submit (state saved and restored: the GE tracks its own GL state) ----
    if (!gl_init()) return;
    GLint prev_prog = 0, prev_vao = 0, prev_abo = 0, prev_vp[4], prev_blend_src_rgb, prev_blend_dst_rgb, prev_blend_src_a, prev_blend_dst_a;
    GLboolean prev_blend = glIsEnabled(GL_BLEND), prev_depth = glIsEnabled(GL_DEPTH_TEST), prev_cull = glIsEnabled(GL_CULL_FACE),
              prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean cmask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst_a);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform2f(g_u_res, (float)w, (float)h);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_vtx.size() * sizeof(Vtx)), g_vtx.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_vtx.size());
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_abo);
    glBindVertexArray((GLuint)prev_vao);
    glUseProgram((GLuint)prev_prog);
    glBlendFuncSeparate(prev_blend_src_rgb, prev_blend_dst_rgb, prev_blend_src_a, prev_blend_dst_a);
    if (!prev_blend) glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
}
