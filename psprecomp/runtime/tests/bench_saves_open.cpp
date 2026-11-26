// Times what the UI thread spends when Settings > Saves opens, against a save folder holding many saves and many backups.
// Usage: bench_saves_open <template save folder (a real save: PARAM.SFO + files)> [backups=54] [saves=20]
// Builds its own scratch tree under $TMPDIR (never touches real saves, no cloud files), prints milliseconds, removes it.
#include "psp_saves_page.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
namespace fs = std::filesystem;
using Clk = std::chrono::steady_clock;
static double ms(Clk::time_point a) { return std::chrono::duration<double, std::milli>(Clk::now() - a).count(); }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int nb = argc > 2 ? std::atoi(argv[2]) : 54, ns = argc > 3 ? std::atoi(argv[3]) : 20;
    // BENCH_REUSE=<dir>: use a tree a previous run kept (BENCH_KEEP=1), so strace -c counts only the open.
    const char* reuse = std::getenv("BENCH_REUSE");
    char tmpl[] = "/tmp/savesbenchXXXXXX";
    if (!reuse && !mkdtemp(tmpl)) return 2;
    const fs::path T = reuse ? reuse : tmpl, saves = T / "ms0/PSP/SAVEDATA", bak = T / "ms0/PSP/SAVEDATA_BACKUP";
    char nm[64];
    if (!reuse) fs::create_directories(saves);
    if (!reuse) fs::create_directories(bak);
    for (int i = 0; !reuse && i < ns; i++) { std::snprintf(nm, sizeof nm, "ULES01505GameData%02d", i); fs::copy(argv[1], saves / nm, fs::copy_options::recursive); }
    for (int b = 0; !reuse && b < nb; b++) {
        std::snprintf(nm, sizeof nm, "2026%02d%02d-120000_%s", 1 + b / 28, 1 + b % 28, b % 4 == 0 ? "daily" : "replaced");
        fs::create_directories(bak / nm);
        for (int i = 0; i < ns; i++) { char s[64]; std::snprintf(s, sizeof s, "ULES01505GameData%02d", i); fs::copy(argv[1], bak / nm / s, fs::copy_options::recursive); }
    }
    psp_saves::Env env;
    env.saves_root = saves.string(); env.backup_root = bak.string(); env.scratch_dir = (T / "scratch").string(); env.disc_id = "ULES01505";
    env.detect = [](bool) { return std::vector<psp_savemgr::Source>{}; };
    psp_saves::Page p;
    p.set_env(env);
    double worst = 0;
    auto t0 = Clk::now();
    p.reset();
    double t_reset = ms(t0); worst = std::max(worst, t_reset);
    auto t1 = Clk::now();
    size_t n = p.rows().size();
    double t_rows = ms(t1); worst = std::max(worst, t_rows);
    // frames while it loads: the UI thread only ticks + asks for rows
    int frames = 0; double wf = 0;
    auto t2 = Clk::now();
    bool loading = true;
    while (loading && ms(t2) < 60000) {
        auto f = Clk::now();
        p.tick(0); n = p.rows().size();
        loading = false;
        for (const auto& r : p.rows()) if (r.label.find("Loading") != std::string::npos || r.label.find("Looking") != std::string::npos || r.value.find("Loading") != std::string::npos) loading = true;
        wf = std::max(wf, ms(f)); frames++;
        if (loading) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worst = std::max(worst, wf);
    if (std::getenv("BENCH_ROWS")) for (const auto& r : p.rows()) std::printf("  [%s] = [%s]\n", r.label.c_str(), r.value.c_str());
    std::printf("saves=%d backups=%d rows=%zu\n reset() %.1f ms\n first rows() %.1f ms\n ready after %.1f ms (frames %d, worst frame %.1f ms)\n worst single UI-thread call %.1f ms\n",
                ns, nb, n, t_reset, t_rows, ms(t0), frames, wf, worst);
    { // the second open in the same run (backups already read once)
        auto t3 = Clk::now();
        p.reset();
        double r2 = ms(t3);
        p.wait_idle(60000);
        p.rows();
        std::printf(" second open: reset() %.1f ms, ready after %.1f ms\n", r2, ms(t3));
    }
    if (std::getenv("BENCH_KEEP")) std::printf(" kept %s\n", T.c_str()); else fs::remove_all(T);
}
