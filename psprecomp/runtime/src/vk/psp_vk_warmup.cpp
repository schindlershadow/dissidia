// "Preparing graphics": compile the shader pack ahead of play (vk/vkgl.h warmup_shaders). See psp_vk_glsl.h for the pack format.
#include "vkgl.h"
#include "psp_vk_glsl.h"
#include "../hle/psp_savedata_dialog.h"
#include "psp_ge_draw.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace vkgl {
namespace {

bool read_pack(std::vector<uint8_t>* out) {
    const char* e = std::getenv("PSPRECOMP_SHADER_PACK");
    SDL_RWops* rw = SDL_RWFromFile(e && e[0] ? e : "shaders.pack", "rb");   // Android: the APK's assets/shaders.pack
    if (!rw) return false;
    const Sint64 n = SDL_RWsize(rw);
    if (n > 8 && n < (256 << 20)) {
        out->resize(size_t(n));
        if (SDL_RWread(rw, out->data(), 1, size_t(n)) != size_t(n)) out->clear();
    }
    SDL_RWclose(rw);
    return !out->empty();
}

void draw_progress(int done, int total) {
    using psp_sddialog::Canvas;
    Canvas c(psp_sddialog::output_scale());
    psp_sddialog::MenuRow row;
    row.label = "Compiling shaders";
    row.value = std::to_string(done) + " / " + std::to_string(total);
    row.help = "Preparing graphics\nThe game's shaders are compiled for this phone once, after a first start or an update. Later starts skip this.";
    psp_sddialog::render_menu(c, "Preparing graphics", "Please wait", {row}, 0, true);
    // the progress bar, in the area between the row and the information panel
    const int x0 = c.w / 10, x1 = c.w - c.w / 10, y0 = c.h * 52 / 100, y1 = y0 + std::max(c.h / 22, 6);
    const int fill = total > 0 ? int(int64_t(x1 - x0) * done / total) : 0;
    const uint32_t edge = Canvas::rgba(255, 255, 255), back = Canvas::rgba(46, 40, 98), bar = Canvas::rgba(104, 112, 236);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const bool border = y < y0 + 2 || y >= y1 - 2 || x < x0 + 2 || x >= x1 - 2;
            c.px[size_t(y) * size_t(c.w) + size_t(x)] = border ? edge : (x - x0 < fill ? bar : back);
        }
    psp_sddialog::menu_overlay_publish(c);
    ge_draw_present_overlay_now();
}

}  // namespace

void warmup_shaders() {
    std::vector<uint8_t> data;
    std::vector<pspvk::PackShader> all;
    if (!read_pack(&data) || !pspvk::parse_shader_pack(data, &all)) return;
    data = std::vector<uint8_t>();
    std::vector<const pspvk::PackShader*> todo;
    for (const auto& s : all) if (!pspvk::cache_has(s.stage, s.glsl)) todo.push_back(&s);
    std::fprintf(stderr, "[VKGL] shader pack: %zu shaders, %zu to compile\n", all.size(), todo.size());
    if (todo.empty()) return;

    const int total = int(todo.size());
    std::atomic<int> next{0}, done{0}, failed{0};
    const char* dl = std::getenv("PSPRECOMP_WARMUP_DELAY_MS");   // testing: slow each shader down so the screen can be looked at
    const int delay_ms = dl && dl[0] ? std::atoi(dl) : 0;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int nthreads = int(std::min(4u, hw));
    std::vector<std::thread> pool;
    const Uint32 t0 = SDL_GetTicks();
    for (int t = 0; t < nthreads; t++)
        pool.emplace_back([&] {
            for (;;) {
                const int i = next.fetch_add(1);
                if (i >= total) return;
                std::vector<uint32_t> spv;
                std::string err;
                if (!pspvk::compile_cached(todo[size_t(i)]->stage, todo[size_t(i)]->glsl, &spv, &err)) {
                    failed.fetch_add(1);
                    std::fprintf(stderr, "[VKGL] shader pack: compile failed: %s\n", err.c_str());
                }
                if (delay_ms > 0) SDL_Delay(Uint32(delay_ms));
                done.fetch_add(1);
            }
        });
    int shown = -1;
    Uint32 last = 0;
    while (done.load() < total) {
        SDL_PumpEvents();   // Android stops waiting for an app that does not read its events
        const Uint32 now = SDL_GetTicks();
        const int d = done.load();
        if (d != shown && now - last >= 80) { draw_progress(d, total); shown = d; last = now; }
        SDL_Delay(15);
    }
    for (auto& th : pool) th.join();
    draw_progress(total, total);
    psp_sddialog::menu_overlay_clear();
    std::fprintf(stderr, "[VKGL] shader pack: compiled %d shaders (%d failed) in %u ms\n", total, failed.load(), unsigned(SDL_GetTicks() - t0));
}

}  // namespace vkgl
