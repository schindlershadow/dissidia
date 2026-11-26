// games/dissidia/runtime/tagteam_hud.h - the tag team battle's own HUD: a panel for each extra fighter and the reserve
// roster of each side, drawn into the engine's game overlay canvas (ge_draw_set_game_overlay).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TagHudFighter {
    std::string name;       // short label (the character's code, e.g. "SQU")
    int side = 0;           // 0 = the human's team (left), 1 = the enemy team (right)
    int hp = 0, hp_max = 1;
    int brv = -1;           // bravery (-1: not shown)
    bool active = false;    // the one the pad drives (later: the tag swap), marked with an arrow
    std::string code;       // file code of the character ("gst120"): its face strip
    float break_flash = -1.f;   // 0..1: a BREAK flash at this panel's BRV number (its fighter was just broken); < 0: none
    float ex = -1.f;            // EX gauge 0..1 (status +0x18 / 10000); < 0: not shown
    float assist = -1.f;        // assist gauge 0..1 (status +0x1C / 10000, two levels of 0.5); < 0: not shown
    bool ex_mode = false;       // in EX Mode (actor +0x5B2 bit 2)
    uint32_t id = 0;            // the actor this panel shows (HP hit indicator / trail key)
    bool lethal = false;        // BRV >= the HP of this fighter's own target: the BRV number glows purple (retail's lethal-BRV look)
    int stone = -1;             // the member's summonstone id (-1: none); drawn as the retail summon orb
    bool stone_auto = false;    // an AUTO stone (label "A") rather than a manual one ("R+O")
    bool stone_used = false;    // its use is spent (the orb is greyed)
    bool has_assist = true;     // the fighter has an assist (actor +0x1108); without one retail never fills the gauge: drawn dim
};
struct TagHudReserve {
    std::vector<std::string> names;   // waiting fighters, in the order they come in; names[0] is next
    std::vector<std::string> codes;   // their file codes (face strips), same order
    int side = 0;
    float countdown = -1.f;           // seconds until this side's next member is called in (< 0: nobody is waiting)
};

// An HP bar over a fighter's head (Settings > Debug > Overhead HP Bars): the enemies of the fighter this machine controls. The position is
// read from the actor (+0x80, the feet) every frame, so the bar follows the fighter between HUD ticks.
struct TagHudOverhead {
    uint32_t actor = 0;
    int hp = 0, hp_max = 1;
    int brv = -1;           // bravery (-1: not shown)
    bool target = false;    // the lock target: brighter, wider, with its bravery
    std::string name;       // short label (mini panel styles)
    std::string code;       // file code of the character: its face strip (style 3)
};
// The bars for the next frames (once per HUD tick); an empty list removes them. style: 1 = a thin HP bar (the target's with its Bravery),
// 2 = a mini status panel (name, Bravery, HP bar and number), 3 = the PPSSPP plugin's HUD (v0.52-v0.61): half-size panels with the face over the
// enemies, the ally and the other enemy on the assist-gauge row at the bottom; 0 = off. With 1, 2 or 3 the side panels are not drawn.
void tagteam_hud_overhead(const std::vector<TagHudOverhead>& bars, int style = 1, bool hide_panels = false);
// Lock-on indicator (presentation only): the enemies that currently have the local player's fighter as their target. `me` = the player's actor,
// `by` = the locked-on enemies' actors (empty: nobody), `fresh` = those among them that locked on just now (flash + optional cue).
// Called every HUD tick; an empty `by` (or me == 0) removes the markers. oh_style = the overhead bar style (0..3), to place the eye above the bar.
struct TagHudLockInd { uint32_t me = 0; std::vector<uint32_t> by, fresh; int oh_style = 0; };
void tagteam_hud_lockind(const TagHudLockInd& li);
// Height of a fighter's top (head / horns, metres above its feet, margin included), from tagteam.cpp; every marker drawn above a fighter is anchored with it.
void tagteam_hud_set_top_fn(float (*fn)(uint32_t actor));
// true until the fight has started (battle-start banters / retail intro): the side panels are not drawn.
void tagteam_hud_set_intro(bool intro_running);
// Guest memory, for the bars' positions (tagteam.cpp, every frame before tagteam_layer_frame).
void tagteam_hud_set_ram(const uint8_t* rdram);

// Draws the overlay for these fighters and reserves and hands it to the engine; draws nothing (and clears an earlier overlay)
// when both lists are empty.
void tagteam_hud_draw(const std::vector<TagHudFighter>& fighters, const std::vector<TagHudReserve>& reserves);

// A line of text centred near the top of the screen ("NAME JOINS THE BATTLE"); empty string removes it. Shown with the next draw.
void tagteam_hud_set_banner(const std::string& text);

// Once per frame (tagteam.cpp's frame end): draws the HUD and the effects (tagteam_fx.cpp) on the game layer; in_battle = false clears it.
void tagteam_layer_frame(bool in_battle);

// A short message at the top of the screen that clears by itself ("NAME IS DOWN", the job powers a fall gained): kind 0 = a fall, 1 = job power,
// 2 = advanced job. Shown for `secs` seconds (wall clock), newest last; tagteam_hud_clear() removes them all (a new battle, the teardown).
void tagteam_hud_notice(const std::string& text, int kind, float secs = 4.0f);
void tagteam_hud_clear();

// HP damage indicator (presentation only): one call per HP hit on any fighter. side = the victim's / attacker's HUD side (0 = the human's team, left).
// pos = the victim's world position (for the optional floating number, PSPRECOMP_TAGTEAM_HPDMG=2), may be null.
struct TagHudHit { uint32_t victim = 0, attacker = 0; int vside = 0, aside = 0, dmg = 0, hp_before = 0, hp_after = 0, hp_max = 1; bool ko = false; std::string vname, aname; float pos[3] = {0, 0, 0}; bool have_pos = false; };
void tagteam_hud_hp_hit(const TagHudHit& h);
// Which actor each big (retail) panel shows, by HUD side (0 = left); 0 = unknown. Called every tick with the fighters.
void tagteam_hud_big_panels(uint32_t left_actor, uint32_t right_actor);
bool tagteam_fx_project(const float p[3], float& sx, float& sy);   // tagteam_fx.cpp

// View smoothing (26 Nov): called once per frame before tagteam_hud_pos / tagteam_layer_frame. on = 30 Hz logic interpolation is running, gen = the gameplay
// frame the frame that just ran belongs to, slow_rate = the battle's slow-motion rate (1 = normal; the HUD's pulse clock follows it).
void tagteam_hud_view(bool on, uint32_t gen, float slow_rate);
// A fighter's feet (actor + 0x80) as the renderer's interpolated scene has them (the position to project with ge_draw_last_view_proj); false = no actor / memory.
bool tagteam_hud_pos(uint32_t actor, float p[3]);
// The actor the retail lock-on ring is on (0 = none): its position goes to the renderer so the ring is moved with the interpolated scene (call after tagteam_hud_view).
void tagteam_hud_follow_target(uint32_t actor);
