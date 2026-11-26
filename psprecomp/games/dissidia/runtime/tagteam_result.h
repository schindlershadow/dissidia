// games/dissidia/runtime/tagteam_result.h - the Free-for-all result screen of the tag team code (docs/tag-team.md "Batch forty-three"): the data of the battle just decided.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct FfaResRow {
    int place = 0;        // 1..4 (the placing order)
    int ch = 0;           // the character the fighter started with
    int tag = 0;          // the tag-in who came in for him (0 = none came in)
    int level = 0;        // 0 = unknown
    int look = 0;         // the outfit picked in Player Select: 0 = not known / the save's, 1 Normal, 2 Alt 1, 3 Alt 2, 4.. DLC
    int kos = 0;          // knock-outs this fighter scored (KO ledger)
    int falls = 0;        // times a body of his fell
    int dmg = 0;          // HP damage dealt
    int by = 0;           // the character whose hit made his last fall (0 = unknown / still standing)
    int key = 0;          // the stats key (the fighter, not his character: two on one character are two rows)
    int slot = -1;        // 0..3 = P1..P4 in the single player set-up, else the order of the line-up
    bool you = false;     // this machine's player
    bool cpu = true;      // the AI plays it
    bool alive = false;   // still in the fight when the battle was decided
    std::string who;      // "You", a player's name, "CPU"
    std::vector<int> victims;   // the characters he knocked out, in order
};
struct FfaResult {
    bool valid = false;
    uint32_t gen = 0;     // the battle (generation) it belongs to
    bool you_won = false, you_fell = false;
    int n = 0;
    FfaResRow rows[4];    // by placing order
};
bool dissidia_tagteam_ffa_result(FfaResult* out);   // tagteam.cpp: the snapshot of the Free-for-all battle that was just decided (false: none)
// tagteam_result.cpp: the own result screen. The pad filter hands the pad to it every frame (returns what the game may see), the frame hooks (they have a CPU context)
// let it play its cues and keep the game layer. `in_battle`: the battle overlay is loaded (the retail defeat list is the way on), else the menu overlay (the result scene).
uint32_t dissidia_tagteam_result_pad(uint32_t raw);
void dissidia_tagteam_result_frame(uint8_t* rdram, void* ctx);
bool dissidia_tagteam_result_active();               // the screen owns the game layer (the battle HUD must not clear it)
bool dissidia_tagteam_result_mute_scene(uint32_t ra);   // the retail result scene's own sound requests (caller address) are replaced while the screen is up
