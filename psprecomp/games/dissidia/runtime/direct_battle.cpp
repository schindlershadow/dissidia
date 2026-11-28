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
//   PSPRECOMP_TEST_CHAR_SWAP=<from>=<to>[,<from>=<to>]  fight with other characters than the recording picked (tools/fps_parity.py),
//                                     so the Squall vs Lightning walk loads any match-up. The menu fills the battle block at 0x09AFB8A0
//                                     (FUN_088FB1A4 copies it for the battle): per side a 0x1060-byte fighter record that starts with a copy of
//                                     the character's 0x778-byte save record (byte 0 = charId, then level, abilities ...; the CPU side gets a
//                                     generated one), and behind them a team struct per side (+0x272F, stride 0x38: a count byte, five bytes,
//                                     the charIds, 0xFF filled). On every poll a side whose record holds <from> gets <to>'s own save record
//                                     (save records: *(0x089D712C) + 0xF58 + 0x778 x the charId's index in the 31-entry id list at 0x089CBC98)
//                                     and its team charId; the CPU side's generated record only gets the charId. What the loading
//                                     screen (FUN_0888C4C8) loads is the record's +0xF3A halfword: the object table id of the fighter's
//                                     model entry (P_<code>_1P: byte 6 charId, byte 7 costume 1, byte 8 type 0 (Prishe: 16); table *(0x089D4968));
//                                     it is set to <to>'s entry on every poll. The assist (+0xF3E) stays as the menu picked it.
// All are off by default; with none set the filter returns the pad state unchanged.
#include "psp_game_module.h"
#include "hle/psp_hle.h"
#include "psp_platform.h"
#include "psp_runtime.h"
#include "recomp.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace {
uint8_t* g_rdram = nullptr;
std::vector<std::pair<int, int>> g_swap;   // PSPRECOMP_TEST_CHAR_SWAP
uint8_t* g_at(uint32_t a) { return g_rdram + (a & 0x07FFFFFFu); }
uint32_t g_w32(uint32_t a) { uint32_t v; std::memcpy(&v, g_at(a), 4); return v; }
uint32_t save_record(int charId) {   // the save record of a charId (0 = none)
    for (uint32_t i = 0; i < 31; i++) {
        if (*g_at(0x089CBC98u + i) != charId) continue;
        const uint32_t base = g_w32(0x089D712Cu);
        return base >= 0x08800000u && base < 0x0A000000u ? base + 0xF58u + 0x778u * i : 0u;
    }
    return 0;
}
int model_id(int charId, int costume = 1) {   // object table id of a character's player model entry for that LOOK (P_<code>_<n>P, else P_<code>_<n>P_PUB: the DLC looks), -1 = none
    const uint32_t t = g_w32(0x089D4968u);
    if (t < 0x08800000u || t >= 0x0A000000u) return -1;
    const uint32_t h = g_w32(t);
    if (h < 0x08800000u || h >= 0x0A000000u) return -1;
    const uint32_t n = g_w32(h + 4u); int pub = -1;
    for (uint32_t i = 0; i < n && i < 4000u; i++) {
        const uint8_t* r = g_at(h + 8u + 0x44u * i);
        if (r[6] != charId || r[8] == 5 || r[8] == 8 || r[8] == 13) continue;   // not an assist (8 / 13) or manikin (5): Prishe's is type 16
        const char* name = reinterpret_cast<const char*>(r + 36);
        const size_t len = strnlen(name, 16);
        if (len < 6 || std::strncmp(name, "P_", 2) != 0) continue;
        const bool is_pub = len >= 4 && std::strcmp(name + len - 4, "_PUB") == 0;
        const size_t e = is_pub ? len - 4 : len;   // ... _<n>P
        if (e < 3 || name[e - 1] != 'P' || name[e - 3] != '_' || name[e - 2] != '0' + costume) continue;
        uint16_t id; std::memcpy(&id, r, 2);
        if (!is_pub) return id;
        if (pub < 0) pub = id;
    }
    return pub;
}
// PSPRECOMP_TEST_ASSIST_SET=<side>=<charId>[:<object id>]: test aid, the retail assist of that side becomes that character's assist entry
// (record +0xF3E = object id of the first entry with byte 6 = charId and type 8 / 13 (assist), or the id given; +4186 = charId, +4187 = 0).
std::vector<std::pair<int, int>> g_aset; std::vector<int> g_aset_id;
int assist_id(int charId) {
    const uint32_t t = g_w32(0x089D4968u); if (t < 0x08800000u || t >= 0x0A000000u) return -1;
    const uint32_t h = g_w32(t); if (h < 0x08800000u || h >= 0x0A000000u) return -1;
    const uint32_t n = g_w32(h + 4u); int first = -1;
    for (uint32_t i = 0; i < n && i < 4000u; i++) {
        const uint8_t* r = g_at(h + 8u + 0x44u * i);
        if (r[6] != charId || (r[8] != 8 && r[8] != 13)) continue;
        uint16_t id; std::memcpy(&id, r, 2);
        static int logged = 0; if (logged++ < 20) std::fprintf(stderr, "[DIRECT] assist entry candidate char %d id %u costume %u type %u name %.16s\n", charId, id, r[7], r[8], reinterpret_cast<const char*>(r + 36));
        if (first < 0) first = id;
    }
    return first;
}
void assist_set() {
    constexpr uint32_t kBlock = 0x09AFB8A0u, kSide = 0x1060u;
    for (size_t k = 0; k < g_aset.size(); k++) {
        const uint32_t side = static_cast<uint32_t>(g_aset[k].first) & 1u; uint8_t* rec = g_at(kBlock + kSide * side);
        if (rec[1] < 1 || rec[1] > 100) continue;
        int id = g_aset_id[k] > 0 ? g_aset_id[k] : assist_id(g_aset[k].second); if (id <= 0) continue;
        uint16_t m; std::memcpy(&m, rec + 0xF3E, 2);
        if (m == id && rec[4186] == g_aset[k].second) continue;
        const uint16_t w = static_cast<uint16_t>(id); std::memcpy(rec + 0xF3E, &w, 2); rec[4186] = static_cast<uint8_t>(g_aset[k].second); rec[4187] = 0;
        std::fprintf(stderr, "[DIRECT] side %u assist: object id %u -> %u (charId %d)\n", side, m, w, g_aset[k].second);
    }
}
void team_swap() {
    constexpr uint32_t kBlock = 0x09AFB8A0u, kSide = 0x1060u, kTeam = 0x2735u, kTeamStride = 0x38u;
    static int done[2] = {-1, -1};                 // the pair applied to each side (one per side: "15=31,31=15" must not flip a side back)
    static int want[2] = {-2, -2}, from_id[2] = {-2, -2};
    for (uint32_t side = 0; side < 2; side++) {
        uint8_t* rec = g_at(kBlock + kSide * side);
        uint8_t* ids = g_at(kBlock + kTeam + kTeamStride * side);
        if (done[side] >= 0 && rec[0] != g_swap[done[side]].second) done[side] = -1;   // the menu wrote the record again
        if (done[side] < 0) {
            for (size_t k = 0; k < g_swap.size(); k++) {
                const auto& s = g_swap[k];
                if (rec[0] != s.first || rec[1] < 1 || rec[1] > 100) continue;   // a fighter record (charId, level 1..100)
                const uint32_t src = side == 0 ? save_record(s.second) : 0u;
                if (src) std::memcpy(rec, g_at(src), 0x778);
                rec[0] = static_cast<uint8_t>(s.second);
                done[side] = static_cast<int>(k);
                want[side] = model_id(s.second); from_id[side] = model_id(s.first);
                std::fprintf(stderr, "[DIRECT] side %u fighter record: charId %d -> %d (%s)\n", side, s.first, s.second, src ? "its own save record" : "charId only");
                break;
            }
        }
        if (done[side] < 0) continue;
        const auto& s = g_swap[done[side]];
        if (ids[-6] == 1 && ids[0] == s.first && ids[1] == 0xFF) ids[0] = static_cast<uint8_t>(s.second);   // a 1-on-1 team struct
        uint16_t m; std::memcpy(&m, rec + 0xF3A, 2);   // the model the loading screen loads: <to>'s (the menu sets it from its own pick)
        if (want[side] >= 0 && m != want[side] && (m == from_id[side] || from_id[side] < 0)) {
            const uint16_t w = static_cast<uint16_t>(want[side]);
            std::memcpy(rec + 0xF3A, &w, 2);
            std::fprintf(stderr, "[DIRECT] side %u model id %u -> %u (charId %d)\n", side, m, w, s.second);
        }
    }
}
}  // namespace

// Called by the pad filter of framerate.cpp for every poll; returns the buttons to hand the game.
int dissidia_tagteam_ffa_human_chars(uint8_t out[2]);   // tagteam.cpp: the single player Free-for-all's P1 {main, tag-in}, count 0 = not in use
int dissidia_tagteam_ffa_enemy_char();   // tagteam.cpp: the single player Free-for-all's P2 (0 = none)
int dissidia_tagteam_ffa_char_look(int charId);   // tagteam.cpp: the LOOK picked for that fighter (0 = none)
namespace {
// The single player Free-for-all's P2 is the Tournament screen's CPU fighter of round 1: its record's charId, model and 1-on-1 team struct are set to the player's choice
// before the loading screen (same fields as the test swap above).
void force_cpu_char(int to) {
    constexpr uint32_t kBlock = 0x09AFB8A0u, kSide = 0x1060u, kTeam = 0x2735u, kTeamStride = 0x38u;
    if (!g_rdram) return;
    uint8_t* rec = g_at(kBlock + kSide); uint8_t* ids = g_at(kBlock + kTeam + kTeamStride);
    if (rec[1] < 1 || rec[1] > 100 || to <= 0) return;
    const int from = rec[0];
    if (from != to) { if (ids[-6] == 1 && ids[0] == from && ids[1] == 0xFF) ids[0] = static_cast<uint8_t>(to); rec[0] = static_cast<uint8_t>(to); std::fprintf(stderr, "[FFA] enemy fighter of round 1: charId %d -> %d\n", from, to); }
    // 26 Nov: the LOOK picked for P2 in Player Select (Alt 1 / Alt 2 = the 2P / 3P model entry; every look in the record's +0x774, as retail's 1-on-1 record). PSPRECOMP_TAGTEAM_FFA_LOOK_OLD=1: old.
    int want = model_id(to); const int lk = std::getenv("PSPRECOMP_TAGTEAM_FFA_LOOK_OLD") ? 0 : dissidia_tagteam_ffa_char_look(to);
    if (lk >= 2 && lk <= 3) { const int mm = model_id(to, lk); if (mm >= 0) want = mm; }
    if (lk >= 1 && lk <= 8 && rec[0x774] != lk) { rec[0x774] = static_cast<uint8_t>(lk); std::fprintf(stderr, "[FFA] P2 %d: look %d (model id %d)\n", to, lk, want); }
    uint16_t m; std::memcpy(&m, rec + 0xF3A, 2);
    if (want >= 0 && m != want) { const uint16_t w = static_cast<uint16_t>(want); std::memcpy(rec + 0xF3A, &w, 2); }
}
// P1 (the human side, record 0): his main's own save record, model and a team struct of {main, tag-in}: the 5-member Party Select is not needed.
void force_human_chars(const uint8_t* c, int n) {
    constexpr uint32_t kBlock = 0x09AFB8A0u, kTeam = 0x2735u;
    if (!g_rdram || n < 1) return;
    uint8_t* rec = g_at(kBlock); uint8_t* ids = g_at(kBlock + kTeam);
    if (rec[1] < 1 || rec[1] > 100) return;
    const int to = c[0];
    if (rec[0] != to) {
        const uint32_t src = save_record(to); if (src) std::memcpy(rec, g_at(src), 0x778);
        rec[0] = static_cast<uint8_t>(to);
        std::fprintf(stderr, "[FFA] human fighter record: charId -> %d (%s)\n", to, src ? "its own save record" : "charId only");
    }
    // 25 Nov (user: FFA fighters wore the default outfit): the LOOK Player Select shows ("LOOK: Normal / Alt 1 / DLC 1", Square there) is the byte at +0x774 of the character's 0x778-byte save
    // record (found by pressing Square in Quick Battle's Player Select and diffing all 31 records: Lightning 4 -> 1). The retail 1-on-1 battle with Lightning on "DLC 1" has model id 596 (the
    // plain P_GST130_1P entry, not the _PUB one) and the look in the record's +0x774 (4): the loader applies it. So the model stays the first entry and the record's +0x774 is the save's look,
    // also when the record already holds this character (the Party Select lead's look was kept before). PSPRECOMP_TAGTEAM_FFA_COSTUME_OLD=1: not copied.
    int want = model_id(to); uint8_t lkv = 1;
    if (!std::getenv("PSPRECOMP_TAGTEAM_FFA_COSTUME_OLD")) { const uint32_t src = save_record(to); if (src) { const int pl = dissidia_tagteam_ffa_char_look(to); const uint8_t lk = pl ? static_cast<uint8_t>(pl) : *g_at(src + 0x774u); lkv = lk;
        if (lk >= 2 && lk <= 3) { const int mm = model_id(to, lk); if (mm >= 0) want = mm; }   // retail (checked with Lightning in 1-on-1: Alt 1 = look 2 -> model id 597 = P_GST130_2P, DLC 1 = look 4 -> 596 = the 1P entry + look 4 in the record)
        if (lk >= 1 && lk <= 8 && rec[0x774] != lk) { rec[0x774] = lk; static int lg = 0; if (lg++ < 6) std::fprintf(stderr, "[FFA] human main %d: look %d (model id %d)\n", to, lk, want); } } }
    uint16_t m; std::memcpy(&m, rec + 0xF3A, 2);
    if (want >= 0 && m != want) { const uint16_t w = static_cast<uint16_t>(want); std::memcpy(rec + 0xF3A, &w, 2); }
    if (ids[-6] != n || ids[0] != c[0] || (n > 1 && ids[1] != c[1])) { ids[-6] = static_cast<uint8_t>(n); ids[0] = c[0]; ids[1] = n > 1 ? c[1] : 0xFF; for (int k = 2; k < 5; k++) ids[k] = 0xFF; std::fprintf(stderr, "[FFA] human team: %d %d\n", c[0], n > 1 ? c[1] : -1); }
}
}  // namespace
uint32_t dissidia_walk_filter(uint32_t buttons) {
    { uint8_t hc[2]; if (const int hn = dissidia_tagteam_ffa_human_chars(hc)) force_human_chars(hc, hn); }
    if (const int fe = dissidia_tagteam_ffa_enemy_char()) force_cpu_char(fe);
    if (!g_swap.empty()) team_swap();
    if (!g_aset.empty()) assist_set();
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

// FUN_08846E18(table, charId, costume): the object table entry (byte 6 charId, byte 7 costume) of a character's model files.
FuncPtr g_orig_objfind = nullptr;
void objfind_hook(uint8_t* rdram, recomp_context* ctx) {
    const int id = static_cast<int>(ctx->r[5]);
    static int logged = 0;
    if (logged++ < 40) std::fprintf(stderr, "[DIRECT] objfind charId %d costume %d ra=%08X\n", id, static_cast<int>(ctx->r[6]), static_cast<uint32_t>(ctx->r[31]));
    g_orig_objfind(rdram, ctx);
}

void dissidia_direct_battle_install(uint8_t* rdram) {
    g_rdram = rdram;
    if (std::getenv("PSPRECOMP_TEST_OBJFIND_LOG") && (g_orig_objfind = psp_dispatch_probe_lookup(0x08846E18u))) psp_dispatch_register(0x08846E18u, objfind_hook);
    if (const char* sw = std::getenv("PSPRECOMP_TEST_CHAR_SWAP"); sw && sw[0]) {
        for (const char* q = sw; *q;) {
            int a = 0, b = 0, used = 0;
            if (std::sscanf(q, "%d=%d%n", &a, &b, &used) != 2) break;
            if (a > 0 && a < 255 && b > 0 && b < 255) g_swap.push_back({a, b});
            q += used;
            if (*q == ',') q++;
        }
        std::fprintf(stderr, "[DIRECT] character swap: %zu pair(s) from %s\n", g_swap.size(), sw);
    }
    if (const char* as = std::getenv("PSPRECOMP_TEST_ASSIST_SET"); as && as[0]) {
        for (const char* q = as; *q;) { int a = 0, b = 0, used = 0, id = 0; if (std::sscanf(q, "%d=%d%n", &a, &b, &used) != 2) break; q += used;
            if (*q == ':') { int u2 = 0; std::sscanf(q + 1, "%d%n", &id, &u2); q += 1 + u2; }
            g_aset.push_back({a, b}); g_aset_id.push_back(id); if (*q == ',') q++; }
        std::fprintf(stderr, "[DIRECT] assist set: %zu side(s) from %s\n", g_aset.size(), as);
    }
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
    if (g_replay) psp_plat::setenv_("PSPRECOMP_SAVEDATA_AUTO", "1");   // the save list dialog reads the host pad, not the filtered one: load the save without it
    std::fprintf(stderr, "[DIRECT] replaying %zu pad changes from %s\n", g_steps.size(), f);
}
