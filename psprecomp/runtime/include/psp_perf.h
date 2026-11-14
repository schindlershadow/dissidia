// psp_perf.h: where does the frame time go? (performance overlay, "Load" lines)
//
// Scopes charge the wall-clock time spent inside them to one of a few named parts of the render thread's work. Time is EXCLUSIVE:
// a scope nested in another one is taken away from the outer one, so the parts add up to the busy time of the render thread.
// Whatever a frame has left over is the render thread waiting (for the game's own thread to hand it more work, or for the display).
// Everything is skipped (one relaxed atomic load) unless the performance overlay is on.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace psp_perf {

enum Id { PRIM = 0, FLUSH, TEXTURE, SCREENFX, POST, PRESENT, SWAP, N_IDS };

inline const char* name(int id) {
    static const char* const k[N_IDS] = {
        "Draw setup",         // turning the game's draw commands into GPU draws (vertex decode, state, shaders)
        "GPU submit",         // handing the batched draws to the graphics driver
        "Textures",           // decoding / hashing / uploading the game's textures
        "Shadows + effects",  // ambient occlusion, contact shadows, reflections, ray tracing set-up
        "Post-process",       // anti-aliasing, sharpening, upscale, colour grade, HDR
        "Present",            // copying the picture to the window and drawing overlays
        "Swap / display wait" // the swap call: waits for the display or for the GPU to catch up
    };
    return k[id];
}

inline std::atomic<bool> g_on{false};
inline std::atomic<int64_t> g_ns[N_IDS];
inline thread_local int t_cur = -1;
inline thread_local std::chrono::steady_clock::time_point t_last;

struct Scope {
    int prev = -1;
    bool act = false;
    explicit Scope(int id) {
        if (!g_on.load(std::memory_order_relaxed)) return;
        act = true;
        const auto now = std::chrono::steady_clock::now();
        if (t_cur >= 0) g_ns[t_cur].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t_last).count(), std::memory_order_relaxed);
        prev = t_cur; t_cur = id; t_last = now;
    }
    ~Scope() {
        if (!act) return;
        const auto now = std::chrono::steady_clock::now();
        if (t_cur >= 0) g_ns[t_cur].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t_last).count(), std::memory_order_relaxed);
        t_cur = prev; t_last = now;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace psp_perf
