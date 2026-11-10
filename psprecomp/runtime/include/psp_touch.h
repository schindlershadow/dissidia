// On-screen touch controls (Android by default; PSPRECOMP_TOUCH=on|off|auto, settings.ini "touch").
//
// Fingers are turned into synthetic SDL game-controller events (button / left-stick axis, marked with
// PSP_TOUCH_PAD_ID), so everything that listens to a pad (the game's input, the settings menu, the dialogs) works with touch
// unchanged. The controls are drawn as translucent shapes over the finished frame (psp_touch_draw, GL thread).
// Layout: left stick bottom-left, d-pad above it, face buttons bottom-right, L / R top corners, Select / Start bottom centre,
// a menu button (opens the settings menu) top centre. Auto mode: hidden until the first touch, hidden again when a real
// pad button is pressed.
#pragma once
#include <SDL.h>

constexpr SDL_JoystickID PSP_TOUCH_PAD_ID = -77;   // `which` of the synthetic controller events

bool psp_touch_enabled();                     // false = no controls and touch events are ignored
void psp_touch_event(const SDL_Event& ev);    // finger events; also notes real pad activity (auto-hide). Main thread.
void psp_touch_draw(int w, int h);            // draw over the bound default framebuffer (GL thread, after the frame)
int psp_touch_mode();                         // 0 auto, 1 on, 2 off
void psp_touch_set_mode(int m);               // runtime change (settings menu)
