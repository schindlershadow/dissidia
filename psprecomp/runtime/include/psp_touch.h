// On-screen touch controls (Android by default; PSPRECOMP_TOUCH=on|off|auto, settings.ini "touch").
//
// Fingers are turned into synthetic SDL game-controller events (button / left-stick axis, marked with
// PSP_TOUCH_PAD_ID), so everything that listens to a pad (the game's input, the settings menu, the dialogs) works with touch
// unchanged. The controls are drawn as translucent shapes over the finished frame (psp_touch_draw, GL thread).
// Layout: left stick bottom-left, d-pad above it, face buttons bottom-right, L / R top corners, Select / Start bottom centre,
// a menu button (opens the settings menu) top centre. The right (camera) stick is always floating on the right half of the screen (a touch on
// no control starts it there); a floating left stick uses the left half only. Auto mode: hidden until the first touch, hidden again when a real
// pad button is pressed.
#pragma once
#include <SDL.h>

constexpr SDL_JoystickID PSP_TOUCH_PAD_ID = -77;   // `which` of the synthetic controller events

bool psp_touch_enabled();                     // false = no controls and touch events are ignored
void psp_touch_event(const SDL_Event& ev);    // finger events; also notes real pad activity (auto-hide). Main thread.
void psp_touch_draw(int w, int h);            // draw over the bound default framebuffer (GL thread, after the frame)
bool psp_touch_editing();                     // the layout editor is open (it takes all touches)
void psp_touch_edit_begin();                  // open the layout editor (Settings > System > Edit Touch Controls)
int psp_touch_mode();                         // 0 auto, 1 on, 2 off
void psp_touch_set_mode(int m);               // runtime change (settings menu)

// Layout editor: every control (stick, d-pad, the four face buttons, L, R, Start, Select, menu) can be dragged anywhere, resized, made
// more or less opaque and hidden; "Add combo" creates a button that presses several PSP buttons at once (for example L+R, or R+Triangle),
// optionally as a toggle-hold button (a tap presses it, the next tap releases it). The layout is saved next to settings.ini in
// touch_layout.txt (one line per control, positions as fractions of the window).
