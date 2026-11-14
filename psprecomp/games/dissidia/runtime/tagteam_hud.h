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
    bool active = false;    // the one the pad drives (later: the tag swap), marked with an arrow
};
struct TagHudReserve {
    std::vector<std::string> names;   // waiting fighters, in the order they come in; names[0] is next
    int side = 0;
};

// Draws the overlay for these fighters and reserves and hands it to the engine; draws nothing (and clears an earlier overlay)
// when both lists are empty.
void tagteam_hud_draw(const std::vector<TagHudFighter>& fighters, const std::vector<TagHudReserve>& reserves);
