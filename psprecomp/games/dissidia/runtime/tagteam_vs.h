// games/dissidia/runtime/tagteam_vs.h - the pre-battle "Round N / VS" page in tag modes: the real start-of-battle line-up (docs/tag-team.md "Batch thirty-six").
#pragma once
#include <string>
#include <vector>

struct VsCard {
    int ch = 0;          // charId
    int role = 0;        // 0 = the slot fighter (retail's fighter on the page), 1 = on the field as a partner, 2 = reserve (waits in party order)
    int level = 0;       // 0 = unknown
    int job = 0;         // retail job id (1 Warrior .. ), 0 = none / unknown
    int party_idx = -1;  // position in the party's order list (-1: not a member: a CPU fill)
    bool cpu = true;     // the AI plays it
    std::string who;     // "You", a player's name, "CPU"
};
struct VsLineup {
    bool valid = false;
    bool ffa = false;                 // free-for-all: every fighter for himself (fighters in `ffa_list`, no parties)
    bool assist = true;               // Assist On (partners' call-ins) / Off
    int  round = 0;
    std::vector<VsCard> team[2];      // team 0 = this player's side (left), team 1 = the opponents (right): slot, partners, then the reserve in order
    std::vector<VsCard> party[2];     // each party's order list as the orb row shows it (role per member, party_idx = position)
    std::vector<VsCard> ffa_list;
};
bool dissidia_tagteam_vs_lineup(VsLineup* out);   // tagteam.cpp: false until the early plan has run
void dissidia_tagteam_vs_draw();                  // tagteam_vs.cpp: called every tick of the page
