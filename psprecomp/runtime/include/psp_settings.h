// psp_settings.h — in-game settings menu.
//
// An animated screen (SettingsView, drawn by psp_savedata_dialog.cpp / psp_settings_render.inc with the game's theme when its module
// installed one) shown as an overlay over the last game frame: a category sidebar, a scrolling list of rows (toggles, choices, sliders,
// actions, text fields, sub-menus) and a description panel. It works with a pad, the keyboard, a mouse and a touch screen. While it is
// open the game is paused (guest threads wait at their next vblank) and sees no input, and the on-screen touch controls are hidden.
// Choices apply live and are saved to settings.ini in the working directory (PSPRECOMP_SETTINGS overrides the path); on the next start
// the saved values win over PSPRECOMP_FULLSCREEN / PSPRECOMP_SCALE / PSPRECOMP_WINDOW / PSPRECOMP_RENDER_SCALE / PSPRECOMP_FPS.
//
// Categories (L / R shoulder, Q / E or Tab, the sidebar): Display, Graphics, Image (colour; sub-menu HDR), Effects (lighting; sub-menu Ray
// Tracing), Controls (touch screens only), Network (applied at once through psp_adhoc_configure), Cloud, System, Drivers (Android, Adreno)
// and Debug.
//
// Open / close: Esc or F1 on the keyboard; Guide, or both stick buttons (L3 + R3), on a game controller; the touch controls' menu button;
// psp_settings_request_open() from a game module's own menu entry. PSPRECOMP_SETTINGS_MENU=0 disables it.
#pragma once

#include <cstdint>

union SDL_Event;

/// A game module that can run its logic at more than one rate registers this;
/// without it the menu has no Frame Rate row. `set` is called on the main
/// thread while the game is paused and must take effect at the next frame.
struct PspFrameRateHook {
    const int* rates;   // e.g. {30, 60}
    int count;
    int (*get)();
    void (*set)(int fps);
};
void psp_settings_set_frame_rate_hook(const PspFrameRateHook& hook);

/// A game module with an alternative voice language registers this; without it (or while `available`
/// says no, i.e. no voice pack is installed) the menu has no Voice row. The choice is saved as `voice=en|jp`
/// and exported as PSPRECOMP_VOICE at the next start (the game reads it once, while loading).
struct PspVoiceHook {
    bool (*available)();
};
void psp_settings_set_voice_hook(const PspVoiceHook& hook);

/// Same idea for a texture pack: while `available` says a pack is installed, the menu has an HD Textures
/// row (saved as `hd_textures=0|1`, exported as PSPRECOMP_TEX_REPLACE at the next start).
struct PspTexPackHook {
    bool (*available)();
};
void psp_settings_set_texpack_hook(const PspTexPackHook& hook);

/// A game module adds on/off rows to the Debug page (this session only, not saved): `get` says whether it is on, `set` switches it.
/// Up to 6 rows; the label and help text must stay valid (string literals).
struct PspDebugToggle {
    const char* label;
    const char* help;
    bool (*get)();
    void (*set)(bool on);
};
void psp_settings_add_debug_toggle(const PspDebugToggle& toggle);

/// First thing in main(): read settings.ini and export the saved values as
/// the PSPRECOMP_* variables the window, renderer and game module read.
void psp_settings_load_early();

/// Main thread, every SDL event, before the event loop's own handling.
/// Returns true when the event was the menu hotkey (swallow it).
bool psp_settings_handle_event(const SDL_Event& ev);

/// Main thread, every event-loop iteration: menu input repeat, drawing,
/// presenting while the game is paused.
void psp_settings_tick();

bool psp_settings_menu_open();

/// Any thread: ask for the menu to open (the game's own menu entries - title screen, main menu, pause menu - call this). Takes effect
/// on the main thread's next tick; does nothing while the menu is already open or in a multiplayer session.
void psp_settings_request_open();

/// Main thread: true once after the menu was opened or closed from inside psp_settings_tick (not by an event), so the event loop can
/// re-publish the (now gated, or released) input state.
bool psp_settings_take_gate_dirty();

/// Input gate for the game: true while the menu is open, and after it closes
/// until every button is released (`raw_buttons` = the host's PSP button bits).
bool psp_settings_block_input(uint32_t raw_buttons);

/// Guest threads (vblank wait): blocks while the menu is open.
void psp_settings_guest_wait();
