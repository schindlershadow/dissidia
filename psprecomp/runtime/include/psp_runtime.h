#pragma once
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/// Global shutdown flag — all threads and the event loop check this.
/// Set by signal handler (SIGTERM/SIGINT) or when all game threads exit.
extern std::atomic<bool> g_should_exit;

/// Count of alive game threads — event loop uses this to detect quiescence.
extern std::atomic<int> g_alive_threads;

/// Main thread ID — set once in main() before any GL init.
/// Used by GL_THREAD_CHECK to assert GL calls happen on the main thread.
extern std::thread::id g_main_thread_id;

/// Host input: PSP button mask driven by SDL keyboard events
/// (set/cleared in psp_event_loop.cpp; ORed in by the sceCtrl HLE).
extern std::atomic<uint32_t> g_host_buttons;
// Packed host analogue stick: lx | (ly << 8), 128 = centre (default 0x8080).
extern std::atomic<uint32_t> g_host_analog;

/// Scripted pad (test driver, games/dissidia/runtime/framerate.cpp): while on, the sceCtrl HLE reports exactly this state
/// (buttons, packed lx | ly << 8) and ignores the keyboard, pad and debug-socket input. psp_ctrl_last_sample gives the state
/// the game was last handed (for recording a play session).
void psp_ctrl_set_script(bool on, uint32_t buttons, uint32_t analog);
void psp_ctrl_last_sample(uint32_t* buttons, uint32_t* analog);
/// A game module can reshape the button state the game is handed (after keyboard / pad / script input): called for every pad poll with the PSP button
/// mask, returns the mask to give the game. Any thread that polls (the game's). nullptr removes it.
void psp_ctrl_set_filter(uint32_t (*filter)(uint32_t buttons));

/// Debug-socket injected button mask + expiry (steady_clock milliseconds).
/// Set by the `B <hexmask> <ms>` debug-socket command; the sceCtrl HLE ORs
/// the mask in only while now < deadline (one-shot timed overlay).
extern std::atomic<uint32_t> g_injected_buttons;
extern std::atomic<int64_t>  g_injected_buttons_deadline_ms;
/// `A <x> <y> <ms>` injected analogue stick (packed lx | ly<<8) + expiry.
extern std::atomic<uint32_t> g_injected_analog;
extern std::atomic<int64_t>  g_injected_analog_deadline_ms;
/// Right stick (a PSP has none): host pad value packed rx | (ry << 8), 128 = centre,
/// and the `A2 <x> <y> <ms>` injected override + expiry.
extern std::atomic<uint32_t> g_host_ranalog;
extern std::atomic<uint32_t> g_injected_ranalog;
extern std::atomic<int64_t>  g_injected_ranalog_deadline_ms;
/// Right stick as a normalised vector: x right+, y UP+ (both -1..1, radial dead
/// zone removed and rescaled). Returns true when outside the dead zone.
bool psp_ctrl_right_stick(float* x, float* y);
/// True once per camera re-center request (R3 on a pad, or the C key); false otherwise.
bool psp_ctrl_take_camera_recenter();
/// A game module pushes this deadline forward while it consumes the right stick itself
/// (analogue camera); until it expires the sceCtrl HLE does not turn the stick into D-pad bits.
extern std::atomic<int64_t> g_rstick_consumed_until_ms;   // steady_clock ms

/// [#35] LOOKUP_MISS counters for the debug socket I command
/// (implemented in psp_dispatch.cpp; relaxed-atomic, any thread).
void psp_dispatch_get_miss_stats(uint32_t* unique_addrs,
                                 uint64_t* total_calls);

/// [#35] Recent dispatched-function ring (cross-thread copy of the
/// PC-TRACE ring in psp_dispatch.cpp), oldest first. Returns count.
int psp_dispatch_get_recent_funcs(uint32_t* out, int max);

/// Debug-only GL thread safety check.
/// In debug builds: aborts if current thread is not the main thread.
/// In release builds: no-op.
#ifndef NDEBUG
#define GL_THREAD_CHECK() do { \
    if (std::this_thread::get_id() != g_main_thread_id) { \
        std::fprintf(stderr, \
            "FATAL: GL call from non-main thread at %s:%d\n", \
            __FILE__, __LINE__); \
        std::abort(); \
    } \
} while(0)
#else
#define GL_THREAD_CHECK() ((void)0)
#endif

/// Initialize SDL2 window and OpenGL 3.3 core context.
/// Must be called from main thread. Returns 0 on success.
/// Implemented in Plan 04.
int psp_runtime_init_sdl();

/// Clean shutdown: signal threads, drain render queue, join threads.
/// Implemented in Plan 05.
void psp_runtime_shutdown();

/// Install signal handlers for SIGTERM and SIGINT that set g_should_exit.
/// Implemented in Plan 05.
void psp_install_signal_handlers();
