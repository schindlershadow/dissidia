// Savedata list dialog (sceUtilitySavedata LISTLOAD / LISTSAVE).
//
// Pure logic + a software renderer (FreeType text, SDF shapes): no dependency
// on the runtime beyond FreeType, so it is unit-testable on the host
// (tests/test_savedata_dialog.cpp). Without a theme the look is a generic
// recreation of a PSP-menu style (lavender background, bevelled white panel,
// navy header, royal-blue chevron selection bar, pointing hand). A game
// module can instead install an SdTheme (below) built from the game's own UI
// textures (see games/dissidia/runtime/savedata_theme.cpp); the same layout
// and scaling then draw those sprites. The layout is in
// 480x272 design units and the canvas is rendered at an integer multiple of
// that (the output resolution) so it stays crisp when upscaled. The HLE
// (psp_hle_utility.cpp) feeds it the save directories it found, polls the
// pad each Update/GetStatus and, when the user picks a row or cancels,
// completes the utility call. The rendered frame is published through
// overlay_publish()/overlay_fetch(); the GL presenter
// (psp_ge_draw.cpp present_blit) blits it over the game frame while a
// dialog is active.
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace psp_sddialog {

constexpr int SCREEN_W = 480;   // design units
constexpr int SCREEN_H = 272;
constexpr int MAX_SCALE = 4;

// The presenter publishes the integer scale (1..MAX_SCALE) the overlay should
// be rendered at (output width / 480, rounded up); the HLE reads it each frame.
void set_output_scale(int s);
int output_scale();

// PSP pad bits (sceCtrl).
constexpr uint32_t BTN_UP = 0x0010, BTN_RIGHT = 0x0020, BTN_DOWN = 0x0040,
                   BTN_LEFT = 0x0080, BTN_LTRIGGER = 0x0100,
                   BTN_RTRIGGER = 0x0200, BTN_CIRCLE = 0x2000,
                   BTN_CROSS = 0x4000, BTN_TRIANGLE = 0x1000;

struct SaveEntry {
    std::string dirname;     // full directory name, e.g. ULES01505GameData00
    std::string slot;        // the saveName part ("GameData00")
    std::string title;       // PARAM.SFO TITLE
    std::string savetitle;   // PARAM.SFO SAVEDATA_TITLE
    std::string detail;      // PARAM.SFO SAVEDATA_DETAIL (first line shown)
    time_t mtime = 0;
    uint64_t size_bytes = 0; // total size of the save directory's files
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> icon_rgba;  // decoded ICON0.PNG (RGBA8), empty = none
    bool exists = true;      // false = empty slot ("New Data")
};

// ---------------------------------------------------------------------------
// Theme: the game's own UI art, supplied by the game module.
// ---------------------------------------------------------------------------
// Straight-alpha RGBA8 image (uint32 = 0xAABBGGRR on little endian).
struct SdImage {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

enum class SdMirror { None, LeftFromRight, RightFromLeft };

// A rectangle of an image drawn with 9-slice scaling. (x,y,w,h) is the source
// rectangle; l/r/t/b are the fixed border widths in source pixels (all zero =
// the whole rectangle is stretched). With `mirror` set, only one end cap is
// stored in the image and the other is its horizontal mirror (the game draws
// its chevron bars that way). Source pixels map 1:1 to design units
// (480x272); the canvas scale multiplies both.
struct SdSprite {
    std::shared_ptr<const SdImage> img;
    int x = 0, y = 0, w = 0, h = 0;
    int l = 0, r = 0, t = 0, b = 0;
    SdMirror mirror = SdMirror::None;
    bool valid() const { return img && w > 0 && h > 0; }
};

struct SdTheme {
    SdSprite background;                // stretched over the whole 480x272 screen
    SdSprite tab;                       // header tab and bottom hint bar (horizontal 3-slice)
    SdSprite row_selected;              // royal-blue chevron bar
    SdSprite row_normal;                // silver chevron bar
    SdSprite cursor;                    // pointing hand, drawn at native size
    SdSprite corner[4];                 // frame corner ornaments: TL, TR, BL, BR
    SdSprite glyph_cross, glyph_circle;     // face-button glyphs
    SdSprite dpad_up, dpad_down;
    // Settings menu (optional): d-pad left/right glyphs for the hint bar, and the
    // value arrows in two colours (on the silver rows / on the selected bar).
    SdSprite dpad_left, dpad_right;
    SdSprite arrow_left, arrow_right, arrow_left_hi, arrow_right_hi;
    // Colours (RGBA8, same packing as SdImage).
    uint32_t text = 0xFF56362E;             // row title
    uint32_t text_dim = 0xFF8C6460;         // row detail
    uint32_t text_selected = 0xFFFFFFFF;
    uint32_t text_selected_dim = 0xFFFFE6D6;
    uint32_t hint_text = 0xFFFFFFFF;
    uint32_t title = 0xFFFFF4F0;            // screen title fill
    uint32_t title_edge = 0xFF5A2E1C;       // and its outline
    uint32_t panel_fill = 0xD8EEE4DC;
    uint32_t panel_edge = 0xFFFFFFFF;
    uint32_t panel_text = 0xFF5A4A40;
    // Optional TrueType data for the body text (the game's text font); empty
    // = the embedded fallback faces. Regular and bold.
    std::shared_ptr<const std::vector<uint8_t>> font_regular, font_bold;
    float font_scale = 1.0f;                // body text size multiplier
    bool valid() const { return background.valid() && row_selected.valid() && tab.valid(); }
};

// Installs / removes the theme (null = built-in look). The provider form lets a
// game module defer building the theme (which reads game data) until the first
// time the dialog is drawn; it is called at most once per install.
void set_theme(std::shared_ptr<const SdTheme> t);
void set_theme_provider(std::shared_ptr<const SdTheme> (*provider)());
std::shared_ptr<const SdTheme> theme();

enum class Kind { Load, Save };
enum class Result { Running, Selected, Cancelled };

// RGBA8 canvas of (SCREEN_W*scale) x (SCREEN_H*scale) pixels (byte order
// R,G,B,A in memory; uint32 = 0xAABBGGRR on LE). Always opaque once rendered.
struct Canvas {
    int scale, w, h;
    std::vector<uint32_t> px;
    explicit Canvas(int s = 1)
        : scale(s < 1 ? 1 : (s > MAX_SCALE ? MAX_SCALE : s)),
          w(SCREEN_W * scale), h(SCREEN_H * scale),
          px(size_t(w) * size_t(h), 0) {}
    static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
    }
    void clear(uint32_t c) { px.assign(px.size(), c); }
};

class ListDialog {
public:
    // confirm_cross: true = CROSS confirms / CIRCLE cancels (the default of
    // the task + Western PSPs); false = CIRCLE confirms (utility
    // common.buttonSwap == 0 on Japanese units).
    void begin(Kind kind, std::vector<SaveEntry> entries, bool confirm_cross,
               int initial_selection = 0);
    // buttons = current pad state; now_ms = monotonic ms (for key repeat).
    Result update(uint32_t buttons, int64_t now_ms);
    int selected() const { return sel_; }
    const SaveEntry* selected_entry() const;
    const std::vector<SaveEntry>& entries() const { return entries_; }
    Kind kind() const { return kind_; }
    // Bumped whenever what render() would draw changes (selection/scroll/begin).
    uint32_t revision() const { return rev_; }
    void render(Canvas& c) const;
    // Shows the "Triangle: Manage saves" hint (the HLE opens psp_savemgr_ui on that button).
    void set_manage_hint(bool on) { if (manage_ != on) { manage_ = on; ++rev_; } }

private:
    bool manage_ = false;
    Kind kind_ = Kind::Load;
    std::vector<SaveEntry> entries_;
    bool confirm_cross_ = true;
    int sel_ = 0, top_ = 0;
    uint32_t prev_ = 0;
    uint32_t held_dir_ = 0;
    int64_t next_repeat_ = 0;
    uint32_t rev_ = 0;
    bool armed_ = false;     // ignore buttons held when the dialog opened
    void move(int d);
};

// ---------------------------------------------------------------------------
// Settings menu page (driven by psp_settings.cpp): a title, a tab label and up
// to MENU_ROWS rows of label + value in the save dialog's style (the game's
// theme when one is installed), with a help line in the information panel.
// ---------------------------------------------------------------------------
constexpr int MENU_ROWS = 5;   // more than 4 are drawn closer together
struct MenuRow {
    std::string label;
    std::string value;          // shown right-aligned; empty for an action row
    std::string help;           // information panel text while selected
    bool can_dec = false, can_inc = false;   // draws the left / right arrows
    bool disabled = false;      // drawn greyed out (the help text says why); the row still scrolls and shows its help
};
// `tabs` (optional, with more than one name): the pages of a multi-page menu drawn as tabs, `tab_active` the shown one, the others
// dimmed; the hint bar then also shows the L / R page keys. Without it `tab` is drawn as the single header tab.
// `anim` (optional): a tab switch in progress. p runs 0 -> 1 (already eased): tab `from_tab` fades down while `tab_active` rises, and the
// rows slide in from the side the switch came from (dir +1 = from the right) while fading in.
struct MenuAnim { int from_tab = -1; float p = 1.f; int dir = 0; };
void render_menu(Canvas& c, const std::string& title, const std::string& tab,
                 const std::vector<MenuRow>& rows, int sel, bool confirm_cross,
                 const std::vector<std::string>* tabs = nullptr, int tab_active = 0, const MenuAnim* anim = nullptr);

// ---------------------------------------------------------------------------
// Settings screen (driven by psp_settings.cpp): a category sidebar, a scrolling list of rows (toggles, choices, sliders, actions,
// sub-menus, text fields), a description panel and a hint bar, with animation. It is drawn from a SetScene (what to show) by a
// SettingsView (which owns the animation state and remembers where everything was drawn, so the mouse / touch code can hit-test
// against the same geometry). All coordinates are design units (480x272).
// ---------------------------------------------------------------------------
enum class SetKind { Choice, Toggle, Slider, Action, Danger, Submenu, Text, Info, Header };
struct SetRow {
    SetKind kind = SetKind::Choice;
    int id = 0;                 // stable per row (animation memory); < SET_MAX_IDS
    std::string label, value;
    std::string help;           // "Title\nBody": the description panel
    std::string badge;          // small chip beside the title in the description panel ("Saved", "Next start", ...)
    bool can_dec = false, can_inc = false;   // Choice / Slider: the arrows
    bool disabled = false;      // greyed out (the help text says why); still selectable
    bool on = false;            // Toggle
    int steps = 0, cur = 0;     // Slider: number of positions (>= 2) and the current one
    bool selectable() const { return kind != SetKind::Header; }
};
constexpr int SET_MAX_IDS = 192;
struct SetCat { std::string name; int icon = 0; };   // icon: see settings_icon()
struct SetKeyboardState {    // the text-entry modal
    bool active = false;
    std::string title, text;
    bool secret = false, shift = false, symbols = false;
    int row = 0, col = 0;    // focused key (pad / keyboard)
};
struct SetDialog {                            // a question over the settings screen (e.g. restart now / later)
    bool active = false;
    std::string title, text;                  // text: lines separated by \n
    std::vector<std::string> buttons;
    int sel = 0;
    int hover = -1;
};
struct SetScene {
    std::string title = "Settings";
    std::vector<std::string> crumbs;          // breadcrumb after the title, e.g. {"Image", "HDR"}
    std::vector<SetCat> cats;
    int cat = 0;                              // the active category
    std::vector<SetRow> rows;
    int sel = 0;                              // the selected row (never a Header)
    int hover_row = -1, hover_cat = -1;       // under the pointer, -1 = none
    bool hover_back = false;
    int hover_part = 0;                       // what of hover_row the pointer is on: 1 left arrow, 2 right arrow, 3 slider track
    int press_row = -1;                       // a row held down by the pointer
    int device = 0;                           // 0 pad, 1 keyboard / mouse, 2 touch: which hints and cursors to show
    bool confirm_cross = true;
    bool in_sub = false;                      // the page is a sub-menu: Back goes up one level instead of closing
    bool focus_tabs = false;                  // the category list (left) has the focus; Back there closes
    std::string brand = "Dissidia 012 Recompiled";   // the plate in the top right corner (a Back button inside a sub-menu)
    SetKeyboardState kb;
    SetDialog dlg;
};
struct SetRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool has(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
struct SetKey { std::string label; int kind = 0; char ch = 0; SetRect r; };   // kind: 0 character, 1 shift, 2 symbols, 3 space, 4 delete, 5 done, 6 cancel
struct SetGeom {
    SetRect back, list, scroll_track, scroll_thumb, detail, side;
    std::vector<SetRect> dlg_btns;            // the dialog's buttons while it is open
    std::vector<SetRect> cats;
    struct Row { int idx = 0; SetRect r, dec, inc, track; };
    std::vector<Row> rows;                    // the rows at least partly inside the list
    std::vector<std::vector<SetKey>> keys;    // the keyboard modal's keys by row, while it is open
    SetRect kb_field;
};
class SettingsView {
public:
    // The screen opens (open = true) or closes with an animation; closed() is true once a close has finished.
    void set_open(bool open);
    bool closed() const { return !open_ && p_open_ <= 0.001f; }
    bool opening_or_closing() const { return (open_ && p_open_ < 0.999f) || (!open_ && p_open_ > 0.001f); }
    void page_changed(int dir);               // the active category changed (dir +1 = to a later one, -1 = earlier, 0 = sub-menu level)
    void rows_changed() { rows_reset_ = true; }   // a different list is shown: jump the scroll instead of gliding
    void bump(int row_id);                    // a value was changed by the user: pulse that row
    // Draws a frame (advances the animation by the time since the previous call).
    void render(Canvas& c, const SetScene& s, double now_ms);
    const SetGeom& geom() const { return g_; }
    // True while something moves (the caller keeps drawing at the frame rate); false once everything has settled.
    bool animating() const { return animating_; }
    // Scrolling by hand (wheel, a dragged finger): pixels in design units, positive = down. `momentum` = a fling speed to coast on.
    void scroll_by(float dy) { scroll_target_ += dy; manual_scroll_ = true; }
    void fling(float v) { fling_v_ = v; }
    void follow_selection() { manual_scroll_ = false; }
    void scroll_cats(float dy) { cat_scroll_t_ += dy; cat_manual_ = true; }   // the category list (left)
    float scroll_max() const { return scroll_max_; }
    float scroll_pos() const { return scroll_; }
    void set_scroll_pos(float p) { scroll_ = scroll_target_ = p; manual_scroll_ = true; }
    // state; public only so the renderer (psp_settings_render.inc) can drive it
    SetGeom g_;
    bool open_ = false, animating_ = false, rows_reset_ = true, manual_scroll_ = false;
    float p_open_ = 0.f;
    double last_ms_ = -1, t_ = 0;
    float scroll_ = 0, scroll_target_ = 0, scroll_max_ = 0, fling_v_ = 0;
    float cat_scroll_ = 0, cat_scroll_t_ = 0, cat_scroll_max_ = 0; bool cat_manual_ = false; int cat_follow_ = -1;   // category list scrolling
    float dlg_p_ = 0.f;                       // dialog fade
    float tab_focus_ = 1.f;                   // eases to 1 while the category list has the focus
    float sel_y_ = 0, cat_y_ = 0;             // gliding highlight positions (design units)
    bool glide_init_ = false;
    int last_cat_ = -1;
    float page_p_ = 1.f; int page_dir_ = 0;
    float knob_[SET_MAX_IDS] = {}, fill_[SET_MAX_IDS] = {}, pulse_[SET_MAX_IDS] = {};
    bool knob_init_[SET_MAX_IDS] = {};
    std::string last_val_[SET_MAX_IDS];
    float help_t_ = 0; std::string help_key_; std::vector<std::string> help_lines_;   // description panel: auto-scroll and wrapped text
    float kb_p_ = 0.f, kb_fx_ = 0, kb_fy_ = 0, kb_fw_ = 30.f;
    int ring_ = 0;
};
// The keys of the text-entry modal (also used for navigation). Rect coordinates are filled in by SettingsView::render.
std::vector<std::vector<SetKey>> settings_keyboard(bool shift, bool symbols);

// Cross-thread overlay hand-off (game thread publishes, GL thread fetches).
void overlay_publish(const Canvas& c);   // activates the overlay
void overlay_clear();                    // deactivates it
// Copies the current overlay into `out` (w*h uint32, top row first). Returns
// false when no dialog is active. The menu overlay (below) wins while active.
bool overlay_fetch(std::vector<uint32_t>& out, int* w = nullptr, int* h = nullptr);
// Settings-menu overlay: a second slot drawn over (instead of) the dialog one,
// so opening the menu during a save dialog does not disturb it.
void menu_overlay_publish(const Canvas& c);
void menu_overlay_clear();

// ---------------------------------------------------------------------------
// On-screen keyboard (sceUtilityOsk, e.g. naming the player): a text field and
// a grid of keys in the save dialog's style. Pad: d-pad / stick moves, the
// confirm button types the key, the cancel button deletes, Triangle = space,
// Square = upper / lower case, Start = done. The host keyboard types directly
// (the event loop forwards its text through osk_push_key while it is open).
// ---------------------------------------------------------------------------
class KeyboardDialog {
public:
    void begin(const std::u16string& title, const std::u16string& initial, int max_chars, bool confirm_cross);
    Result update(uint32_t buttons, int64_t now_ms);   // Selected = Done (never Cancelled)
    const std::u16string& text() const { return text_; }
    uint32_t revision() const { return rev_; }
    void render(Canvas& c) const;

private:
    std::u16string title_, text_;
    int max_ = 16;
    bool confirm_cross_ = true, armed_ = false, lower_ = false;
    int row_ = 1, col_ = 0;
    uint32_t prev_ = 0, held_dir_ = 0;
    int64_t next_repeat_ = 0;
    uint32_t rev_ = 0;
    bool done_ = false;
    void move(int dr, int dc);
    void press();
    void type(char16_t ch);
    void erase();
};
// Host keyboard -> open keyboard dialog (event loop thread -> HLE thread).
void osk_set_active(bool on);
bool osk_active();
void osk_push_key(uint32_t cp);   // printable code point; 8 = delete; 13 = done

// Performance overlay box: `text` in white on a dark box, rendered at `scale`
// (1..MAX_SCALE) into `out` (w*h, top row first, opaque). Uses its own FreeType
// instance, so the GL thread can call it while a dialog renders elsewhere.
// With `graph_ms` (graph_n >= 2 frame times in ms, oldest first) a frame-time graph is drawn under the text.
bool render_hud_box(const std::string& text, int scale, std::vector<uint32_t>& out, int* w, int* h,
                    const float* graph_ms = nullptr, int graph_n = 0);
// Same font, but a transparent RGBA image (white text, soft shadow, alpha = coverage) for the simple fps counter.
bool render_hud_text(const std::string& text, int scale, std::vector<uint32_t>& out, int* w, int* h);

}  // namespace psp_sddialog

// Convenience wrapper with the plain-pointer signature (copies the theme).
void psp_sddialog_set_theme(const psp_sddialog::SdTheme* t);
