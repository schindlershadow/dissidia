// Unit tests for the wizard's logic that needs no window: progress parsing, ETA, update lines, plan weights, features.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../install.h"

using namespace setup;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "psp_setup_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "game" / "tools");
    { std::ofstream f(tmp / "game" / "features.ini"); f << "hd_textures=1\njp_audio=0\n"; }
    Layout l = find_layout(tmp.string());
    l.python = "python3";
    CHECK(feature_available(l, "hd_textures"));
    CHECK(!feature_available(l, "jp_audio"));
    CHECK(!feature_available(l, "android"));

    Options o;
    auto plan = plan_install(l, "x.iso", "/w", o);
    CHECK(plan.size() == 2);
    double sum = 0;
    for (auto& s : plan) sum += s.weight;
    CHECK(std::fabs(sum - 1.0) < 1e-9);
    CHECK(std::fabs(plan[1].base - plan[0].weight) < 1e-9);
    o.hd_textures = true;   // available in this release: one more step
    o.android = true;       // not available: no step
    plan = plan_install(l, "x.iso", "/w", o);
    CHECK(plan.size() == 3);
    sum = 0;
    for (auto& s : plan) sum += s.weight;
    CHECK(std::fabs(sum - 1.0) < 1e-9);

    // psp_build progress lines + ninja counter
    Progress p;
    p.step = 1;
    feed_line(p, plan, "[psp-build] STEP 2/5 Translating the game to C++ (recompile)", 1000);
    CHECK(p.stage.find("Translating") == 0);
    CHECK(p.fraction > plan[1].base && p.fraction < plan[1].base + plan[1].weight);
    double f0 = p.fraction;
    feed_line(p, plan, "[psp-build] STEP 5/5 Compiling the game", 2000);
    feed_line(p, plan, "[psp-build] PROGRESS 10", 2000);
    CHECK(p.fraction > f0);
    feed_line(p, plan, "[10/474] Building CXX object a/b/batch_0001.cpp.o", 3000);
    CHECK(p.compile_i == 10 && p.compile_n == 474);
    CHECK(p.detail.find("10 of 474") != std::string::npos);
    CHECK(p.detail.find("batch_0001.cpp") != std::string::npos);
    feed_line(p, plan, "[237/474] Building CXX object a/b/batch_0002.cpp.o", 3000 + 120000);
    // 227 files in 120 s -> about 0.53 s each, 237 left: about 125 s -> "About 2 minutes left"
    std::string eta = eta_text(p, 3000 + 120000);
    CHECK(eta.find("2 minutes") != std::string::npos);
    feed_line(p, plan, "[psp-build] ERROR the compiler failed", 4000);
    CHECK(p.error == "the compiler failed");
    CHECK(elapsed_text(p, 1000 + 65000).find("1:05") == 0);

    // update lines
    UpdateInfo u = parse_update_line("UPDATE version=1.2.3 current=1.2.0 size=112233 rebuild=1 relink=0");
    CHECK(u.available && u.version == "1.2.3" && u.current == "1.2.0" && u.size == 112233 && u.rebuild && !u.relink);
    CHECK(!parse_update_line("NOUPDATE current=1.2.0").available);
    CHECK(parse_update_line("ERROR no network").error == "no network");
    auto up = plan_update(l, "/w", Options(), u);
    CHECK(up.size() == 2);
    u.rebuild = false;
    CHECK(plan_update(l, "/w", Options(), u).size() == 1);
    Progress q;
    q.step = 0;
    auto up1 = plan_update(l, "/w", Options(), u);
    feed_line(q, up1, "[update] PROGRESS 50", 10);
    CHECK(q.detail == "Downloading: 50%" && q.fraction > 0.2 && q.fraction < 0.6);

    // settings opt-out
    { std::ofstream f(tmp / "settings.ini"); f << "fps=60\ncheck_updates=0\n"; }
    CHECK(!update_check_enabled(tmp.string()));
    { std::ofstream f(tmp / "settings.ini"); f << "fps=60\n"; }
    CHECK(update_check_enabled(tmp.string()));

    // state round trip
    State s;
    s.workdir = "/a b/c"; s.iso = "/i.iso"; s.version = "1"; s.options.android = true; s.options.undub_iso = "/u.iso"; s.options.check_updates = false;
    save_state((tmp / "state.ini").string(), s);
    State r = load_state((tmp / "state.ini").string());
    CHECK(r.workdir == s.workdir && r.options.android && r.options.undub_iso == "/u.iso" && !r.options.check_updates);

    fs::remove_all(tmp);
    std::printf(failures ? "%d FAILED\n" : "all setup tests passed\n", failures);
    return failures ? 1 : 0;
}
