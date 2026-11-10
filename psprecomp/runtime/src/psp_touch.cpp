// On-screen touch controls, see include/psp_touch.h.
#include "psp_touch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "psp_gl.h"

namespace {

// ---- layout (pixels, recomputed from the window size) -------------------------------------------------------------------
struct Circle { float x, y, r; };
struct Rect { float x, y, w, h; };
struct Layout {
    float w = 0, h = 0;
    Circle stick, face[4], menu;            // face: 0 triangle (up), 1 circle (right), 2 cross (down), 3 square (left)
    Rect dpad;                              // square; 8 sectors
    Rect l, r, start, select;
};
enum Ctl { C_UP, C_RIGHT, C_DOWN, C_LEFT, C_TRI, C_CIRCLE, C_CROSS, C_SQUARE, C_L, C_R, C_START, C_SELECT, C_MENU, C_N };
constexpr SDL_GameControllerButton k_btn[C_N] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X,   // Triangle, Circle, Cross, Square
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE};

Layout make_layout(float w, float h) {
    Layout L;
    L.w = w; L.h = h;
    const float u = h, m = 0.05f * u;
    L.stick = {m + 0.20f * u, h - m - 0.20f * u, 0.17f * u};
    L.dpad = {L.stick.x - 0.16f * u, L.stick.y - L.stick.r - 0.04f * u - 0.32f * u, 0.32f * u, 0.32f * u};
    const float fx = w - m - 0.26f * u, fy = h - m - 0.27f * u, d = 0.115f * u, fr = 0.075f * u;
    L.face[0] = {fx, fy - d, fr};
    L.face[1] = {fx + d, fy, fr};
    L.face[2] = {fx, fy + d, fr};
    L.face[3] = {fx - d, fy, fr};
    L.l = {m, m, 0.26f * u, 0.11f * u};
    L.r = {w - m - 0.26f * u, m, 0.26f * u, 0.11f * u};
    L.select = {w * 0.5f - 0.27f * u, h - m - 0.075f * u, 0.22f * u, 0.075f * u};
    L.start = {w * 0.5f + 0.05f * u, h - m - 0.075f * u, 0.22f * u, 0.075f * u};
    L.menu = {w * 0.5f, m + 0.04f * u, 0.04f * u};
    return L;
}
bool in_circle(const Circle& c, float x, float y, float grow = 1.25f) {
    const float dx = x - c.x, dy = y - c.y;
    return dx * dx + dy * dy <= c.r * grow * c.r * grow;
}
bool in_rect(const Rect& r, float x, float y, float grow = 0.0f) {
    return x >= r.x - grow && x <= r.x + r.w + grow && y >= r.y - grow && y <= r.y + r.h + grow;
}

// ---- state ---------------------------------------------------------------------------------------------------------------
struct Finger { float x, y; bool stick; };   // pixels; `stick` = captured by the analog stick
std::map<SDL_FingerID, Finger> g_fingers;
bool g_down[C_N] = {};
int16_t g_ax = 0, g_ay = 0;                  // synthetic left stick
float g_knob_x = 0, g_knob_y = 0;            // stick knob offset in units of the stick radius (-1..1) for drawing
bool g_visible = false;                      // auto mode: shown after a touch
int g_mode = -1;                             // 0 auto, 1 on, 2 off (resolved lazily from PSPRECOMP_TOUCH)
Layout g_layout;
int g_win_w = 0, g_win_h = 0;

int resolve_mode() {
    if (g_mode >= 0) return g_mode;
    const char* e = std::getenv("PSPRECOMP_TOUCH");
#ifdef __ANDROID__
    g_mode = 0;
#else
    g_mode = 2;   // desktop: off unless asked for
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
    return m == 1 || (m == 0 && g_visible);
}

void push_button(SDL_GameControllerButton b, bool down) {
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

// Recompute every control from the fingers, and push the differences as controller events.
void recompute() {
    bool now[C_N] = {};
    float sx = 0, sy = 0;
    const Layout& L = g_layout;
    for (auto& kv : g_fingers) {
        const Finger& f = kv.second;
        if (f.stick) {
            float dx = (f.x - L.stick.x) / L.stick.r, dy = (f.y - L.stick.y) / L.stick.r;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) { dx /= len; dy /= len; }
            sx = dx; sy = dy;
            continue;
        }
        if (in_rect(L.dpad, f.x, f.y, 0.03f * L.h)) {
            const float cx = L.dpad.x + L.dpad.w / 2, cy = L.dpad.y + L.dpad.h / 2;
            const float dx = f.x - cx, dy = f.y - cy;
            if (dx * dx + dy * dy > (0.04f * L.h) * (0.04f * L.h)) {   // dead centre presses nothing
                const float ang = std::atan2(dy, dx);                  // 0 = right, +pi/2 = down
                const float t = 0.38f;                                  // diagonal band width: sin(22.5 deg)
                const float c = std::cos(ang), s = std::sin(ang);
                if (s < -t) now[C_UP] = true;
                if (s > t) now[C_DOWN] = true;
                if (c > t) now[C_RIGHT] = true;
                if (c < -t) now[C_LEFT] = true;
            }
        }
        const Ctl face_ctl[4] = {C_TRI, C_CIRCLE, C_CROSS, C_SQUARE};
        for (int i = 0; i < 4; i++) if (in_circle(L.face[i], f.x, f.y)) now[face_ctl[i]] = true;
        if (in_rect(L.l, f.x, f.y, 0.02f * L.h)) now[C_L] = true;
        if (in_rect(L.r, f.x, f.y, 0.02f * L.h)) now[C_R] = true;
        if (in_rect(L.start, f.x, f.y, 0.02f * L.h)) now[C_START] = true;
        if (in_rect(L.select, f.x, f.y, 0.02f * L.h)) now[C_SELECT] = true;
        if (in_circle(L.menu, f.x, f.y, 1.4f)) now[C_MENU] = true;
    }
    for (int i = 0; i < C_N; i++) {
        if (now[i] != g_down[i]) { g_down[i] = now[i]; push_button(k_btn[i], now[i]); }
    }
    g_knob_x = sx; g_knob_y = sy;
    // radial dead zone 12%, then rescale so the full range is still reachable
    float mag = std::sqrt(sx * sx + sy * sy);
    int16_t ax = 0, ay = 0;
    if (mag > 0.12f) {
        const float k = (mag - 0.12f) / (1.0f - 0.12f) / mag;
        ax = (int16_t)std::lround(std::clamp(sx * k, -1.0f, 1.0f) * 32767.0f);
        ay = (int16_t)std::lround(std::clamp(sy * k, -1.0f, 1.0f) * 32767.0f);
    }
    if (ax != g_ax) { g_ax = ax; push_axis(SDL_CONTROLLER_AXIS_LEFTX, ax); }
    if (ay != g_ay) { g_ay = ay; push_axis(SDL_CONTROLLER_AXIS_LEFTY, ay); }
}

void release_all() {
    g_fingers.clear();
    recompute();
}

}  // namespace

bool psp_touch_enabled() { return resolve_mode() != 2; }
int psp_touch_mode() { return resolve_mode(); }
void psp_touch_set_mode(int m) {
    g_mode = std::clamp(m, 0, 2);
    if (g_mode == 2) release_all();
}

void psp_touch_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev.cbutton.which != PSP_TOUCH_PAD_ID) g_visible = false;   // a real pad button: auto mode hides the controls
        return;
    case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.which != PSP_TOUCH_PAD_ID && std::abs((int)ev.caxis.value) > 16000) g_visible = false;
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
    const float x = ev.tfinger.x * g_win_w, y = ev.tfinger.y * g_win_h;
    const SDL_FingerID id = ev.tfinger.fingerId;
    if (ev.type == SDL_FINGERUP) {
        g_fingers.erase(id);
    } else {
        const bool was_visible = shown();
        if (!was_visible) {                       // the first touch only shows the controls
            if (resolve_mode() == 0) g_visible = true;
            return;
        }
        auto it = g_fingers.find(id);
        if (it == g_fingers.end()) {
            Finger f{x, y, in_circle(g_layout.stick, x, y, 1.5f)};
            g_fingers[id] = f;
        } else {
            it->second.x = x; it->second.y = y;
        }
    }
    recompute();
}

// ---- drawing ------------------------------------------------------------------------------------------------------------
namespace {
struct Vtx { float x, y, r, g, b, a; };
float g_white[4] = {1, 1, 1, 0.6f};      // outlines and symbols
float g_shade[4] = {0.02f, 0.02f, 0.08f, 0.45f};   // dark underlay so white reads on light screens
std::vector<Vtx> g_vtx;
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
// 3x5 bitmap letters for the shoulder buttons
void letter(char ch, float x, float y, float cell, const float* c) {
    static const char* L5[5] = {"X..", "X..", "X..", "X..", "XXX"};
    static const char* R5[5] = {"XX.", "X.X", "XX.", "X.X", "X.X"};
    const char** g = ch == 'L' ? L5 : R5;
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
            if (g[row][col] == 'X') fill_rect({x + col * cell, y + row * cell, cell, cell}, c);
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
}  // namespace

void psp_touch_draw(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (resolve_mode() == 2) return;
    if (w != g_win_w || h != g_win_h) {
        g_win_w = w; g_win_h = h;
        g_layout = make_layout((float)w, (float)h);
    }
    if (!shown()) return;
    const Layout& L = g_layout;
    g_vtx.clear();
    const float line_a = 0.55f;
    // translucent fill; neutral controls (r=g=b=1) are dark navy so they show on light screens, face buttons keep their tint
    auto col = [&](bool down, float r, float g, float b) {
        static float c[4];
        const bool neutral = r == 1 && g == 1 && b == 1;
        c[0] = neutral ? 0.10f : r; c[1] = neutral ? 0.10f : g; c[2] = neutral ? 0.30f : b;
        c[3] = down ? (neutral ? 0.60f : 0.65f) : (neutral ? 0.32f : 0.35f);
        return c;
    };
    float* white = g_white;
    (void)line_a;
    const float lw = std::max(2.0f, 0.006f * h);

    // analog stick: base ring + knob
    fill_circle(L.stick.x, L.stick.y, L.stick.r, col(false, 1, 1, 1));
    ring(L.stick.x, L.stick.y, L.stick.r, lw, white);
    {
        const float kr = L.stick.r * 0.42f;
        const bool act = g_knob_x != 0 || g_knob_y != 0;
        fill_circle(L.stick.x + g_knob_x * (L.stick.r - kr), L.stick.y + g_knob_y * (L.stick.r - kr), kr, col(act, 1, 1, 1));
        ring(L.stick.x + g_knob_x * (L.stick.r - kr), L.stick.y + g_knob_y * (L.stick.r - kr), kr, lw, white);
    }
    // d-pad: four arrow wedges
    {
        const float cx = L.dpad.x + L.dpad.w / 2, cy = L.dpad.y + L.dpad.h / 2, s = L.dpad.w / 2, n = s * 0.40f;
        const bool dn[4] = {g_down[C_UP], g_down[C_RIGHT], g_down[C_DOWN], g_down[C_LEFT]};
        const float dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
        for (int i = 0; i < 4; i++) {
            const float px = -dy[i], py = dx[i];   // perpendicular
            const float* c = col(dn[i], 1, 1, 1);
            // trapezoid from the centre area out to the edge, wedge-shaped arm
            quad(cx + dx[i] * n * 0.5f + px * n * 0.5f, cy + dy[i] * n * 0.5f + py * n * 0.5f,
                 cx + dx[i] * s + px * n * 0.5f, cy + dy[i] * s + py * n * 0.5f,
                 cx + dx[i] * s - px * n * 0.5f, cy + dy[i] * s - py * n * 0.5f,
                 cx + dx[i] * n * 0.5f - px * n * 0.5f, cy + dy[i] * n * 0.5f - py * n * 0.5f, c);
            // arrow head
            tri(cx + dx[i] * s * 0.82f, cy + dy[i] * s * 0.82f,
                cx + dx[i] * s * 0.50f + px * n * 0.22f, cy + dy[i] * s * 0.50f + py * n * 0.22f,
                cx + dx[i] * s * 0.50f - px * n * 0.22f, cy + dy[i] * s * 0.50f - py * n * 0.22f, white);
        }
    }
    // face buttons with their symbols
    {
        const bool dn[4] = {g_down[C_TRI], g_down[C_CIRCLE], g_down[C_CROSS], g_down[C_SQUARE]};
        static const float tint[4][3] = {{0.35f, 0.85f, 0.55f}, {0.95f, 0.40f, 0.40f}, {0.45f, 0.65f, 1.0f}, {0.95f, 0.55f, 0.85f}};
        for (int i = 0; i < 4; i++) {
            const Circle& b = L.face[i];
            fill_circle(b.x, b.y, b.r, col(dn[i], tint[i][0], tint[i][1], tint[i][2]));
            ring(b.x, b.y, b.r, lw, white);
            static float sym[4]; sym[0] = tint[i][0]; sym[1] = tint[i][1]; sym[2] = tint[i][2]; sym[3] = 0.9f;
            const float s = b.r * 0.46f, t = lw * 1.3f;
            if (i == 0) {          // triangle
                line(b.x, b.y - s, b.x + s, b.y + s * 0.8f, t, sym);
                line(b.x + s, b.y + s * 0.8f, b.x - s, b.y + s * 0.8f, t, sym);
                line(b.x - s, b.y + s * 0.8f, b.x, b.y - s, t, sym);
            } else if (i == 1) {   // circle
                ring(b.x, b.y, s, t, sym);
            } else if (i == 2) {   // cross
                line(b.x - s, b.y - s, b.x + s, b.y + s, t, sym);
                line(b.x + s, b.y - s, b.x - s, b.y + s, t, sym);
            } else {               // square
                frame_rect({b.x - s, b.y - s, 2 * s, 2 * s}, t, sym);
            }
        }
    }
    // shoulders, start / select, menu
    {
        fill_rect(L.l, col(g_down[C_L], 1, 1, 1)); frame_rect(L.l, lw, white);
        fill_rect(L.r, col(g_down[C_R], 1, 1, 1)); frame_rect(L.r, lw, white);
        const float cell = L.l.h / 9.0f;
        letter('L', L.l.x + L.l.w / 2 - 1.5f * cell, L.l.y + L.l.h / 2 - 2.5f * cell, cell, white);
        letter('R', L.r.x + L.r.w / 2 - 1.5f * cell, L.r.y + L.r.h / 2 - 2.5f * cell, cell, white);
        fill_rect(L.select, col(g_down[C_SELECT], 1, 1, 1)); frame_rect(L.select, lw, white);
        fill_rect(L.start, col(g_down[C_START], 1, 1, 1)); frame_rect(L.start, lw, white);
        // select: two bars; start: a play triangle
        const float sy = L.select.y + L.select.h / 2;
        fill_rect({L.select.x + L.select.w / 2 - L.select.h * 0.45f, sy - L.select.h * 0.22f, L.select.h * 0.9f, L.select.h * 0.12f}, white);
        fill_rect({L.select.x + L.select.w / 2 - L.select.h * 0.45f, sy + L.select.h * 0.10f, L.select.h * 0.9f, L.select.h * 0.12f}, white);
        const float tx = L.start.x + L.start.w / 2, ty = L.start.y + L.start.h / 2, ts = L.start.h * 0.32f;
        tri(tx - ts * 0.7f, ty - ts, tx + ts, ty, tx - ts * 0.7f, ty + ts, white);
        fill_circle(L.menu.x, L.menu.y, L.menu.r, col(g_down[C_MENU], 1, 1, 1));
        ring(L.menu.x, L.menu.y, L.menu.r, lw, white);
        for (int i = -1; i <= 1; i++)
            fill_rect({L.menu.x - L.menu.r * 0.5f, L.menu.y + i * L.menu.r * 0.34f - lw * 0.5f, L.menu.r, lw}, white);
    }

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
    // restore
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
