// games/dissidia/runtime/direct_battle.cpp - test-only: get from boot into the Quick Battle without the harness's
// picture-by-picture menu walk (docs/testing.md "Skipping the menu walk").
//
// The game polls the pad once per frame, and a poll is the one clock that does not depend on fast-forward or on the
// harness, so the walk is replayed by poll number. The title and the save list take a varying number of frames, so
// the first part is rule-driven and the rest is replayed relative to the moment the save finished loading:
//   1. title:      CROSS (3 polls of every 15) until the save has loaded (PSPRECOMP_SAVEDATA_AUTO=1 is set for the replay: the
//                  save list dialog reads the host pad directly, so a replay could not confirm it)
//   2. (without SAVEDATA_AUTO: the list dialog: after a short settle one CROSS)
//   3. save loaded: the recorded rows ("<polls since the save loaded> <pad mask hex>") are replayed, the pad being
//                   the replay's alone, until the last row; then the pad is handed back.
//   PSPRECOMP_TEST_PAD_RECORD=1       log the pad of a normal (harness) walk: "[PADREC] <poll> <mask hex>", and a
//                                     "[PADREC] loaded <poll>" line when the save finishes loading
//   PSPRECOMP_TEST_DIRECT_BATTLE=<f>  replay the recording <f> (tools/battle_test.py --direct, tests/replays/quick_battle_walk.txt)
// Both are off by default; with neither set the filter returns the pad state unchanged.
#include "psp_game_module.h"
#include "psp_runtime.h"
#include "recomp.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern std::atomic<uint32_t> g_savedata_list_opened, g_savedata_completed;   // runtime/src/hle/psp_hle_utility.cpp

namespace {

constexpr uint32_t CROSS = 0x4000u;

struct Step { uint64_t rel; uint32_t mask; };
std::vector<Step> g_steps;
bool g_replay = false, g_record = false;
std::atomic<uint64_t> g_poll{0};
uint32_t g_last_logged = 0xFFFFFFFFu;
uint32_t g_seen_loaded = 0;
int g_state = 0;                  // 0 title, 1 save list, 2 loading, 3 replaying, 4 done
uint64_t g_t1 = 0, g_press_at = 0, g_base = 0;
size_t g_idx = 0;
uint32_t g_cur = 0;

}  // namespace

// Called by the pad filter of framerate.cpp for every poll; returns the buttons to hand the game.
uint32_t dissidia_walk_filter(uint32_t buttons) {
    if (!g_replay && !g_record) return buttons;
    const uint64_t n = ++g_poll;
    if (g_record) {
        if (g_savedata_completed.load() != g_seen_loaded) {
            g_seen_loaded = g_savedata_completed.load();
            std::fprintf(stderr, "[PADREC] loaded %llu\n", static_cast<unsigned long long>(n));
        }
        if (buttons != g_last_logged) {
            g_last_logged = buttons;
            std::fprintf(stderr, "[PADREC] %llu %X\n", static_cast<unsigned long long>(n), buttons);
        }
        return buttons;
    }
    switch (g_state) {
    case 0:
        if (g_savedata_completed.load() > 0) { g_state = 3; g_base = n; g_idx = 0; g_cur = 0; return 0; }   // (SAVEDATA_AUTO: no list dialog)
        if (g_savedata_list_opened.load() > 0) { g_state = 1; g_t1 = n; g_press_at = n + 20; return 0; }
        return (n % 15) < 3 ? CROSS : 0;
    case 1:
        if (g_savedata_completed.load() > 0) { g_state = 3; g_base = n; g_idx = 0; g_cur = 0; return 0; }
        if (n >= g_press_at && n < g_press_at + 3) return CROSS;
        if (n >= g_press_at + 200) g_press_at = n;      // the press was lost: again
        return 0;
    case 3: {
        const uint64_t rel = n - g_base;
        while (g_idx < g_steps.size() && g_steps[g_idx].rel <= rel) g_cur = g_steps[g_idx++].mask;
        if (g_idx >= g_steps.size()) {
            g_state = 4;
            std::fprintf(stderr, "[DIRECT] replay finished at poll %llu (save loaded at %llu)\n", static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(g_base));
        }
        return g_cur;
    }
    default: return buttons;
    }
}

void dissidia_direct_battle_install(uint8_t* rdram) {
    (void)rdram;
    if (const char* r = std::getenv("PSPRECOMP_TEST_PAD_RECORD"); r && r[0] && r[0] != '0') {
        g_record = true;
        std::fprintf(stderr, "[DIRECT] recording the pad (PADREC lines)\n");
    }
    const char* f = std::getenv("PSPRECOMP_TEST_DIRECT_BATTLE");
    if (!f || !f[0] || f[0] == '0') return;
    FILE* fp = std::fopen(f, "r");
    if (!fp) { std::fprintf(stderr, "[DIRECT] cannot open %s\n", f); return; }
    unsigned long long rel; unsigned mask;
    while (std::fscanf(fp, "%llu %x", &rel, &mask) == 2) g_steps.push_back({rel, mask});
    std::fclose(fp);
    g_replay = !g_steps.empty();
    g_record = false;
    if (g_replay) setenv("PSPRECOMP_SAVEDATA_AUTO", "1", 1);   // the save list dialog reads the host pad, not the filtered one: load the save without it
    std::fprintf(stderr, "[DIRECT] replaying %zu pad changes from %s\n", g_steps.size(), f);
}
