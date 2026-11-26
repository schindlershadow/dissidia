#include <thread>
#include <algorithm>
#include "psp_render_queue.h"
#include "psp_prof.h"
#include "psp_runtime.h"
#include "psp_ge.h"
#include "psp_ge_draw.h"
#include "psp_scheduler.h"
#include "hle/psp_hle.h"
#include "recomp.h"

#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <filesystem>

// ---------------------------------------------------------------------------
// Shared mutex and condvar (used by both blocking single-slot and GE FIFO)
// ---------------------------------------------------------------------------
static std::mutex g_render_mutex;
static std::condition_variable g_render_cv;
// Wakes the main (GL) thread as soon as work is posted (replaces a 1 ms poll).
static std::condition_variable g_work_cv;

// ---------------------------------------------------------------------------
// Single-slot blocking path (FramePresent)
// ---------------------------------------------------------------------------
static RenderRequest g_render_req;

// ---------------------------------------------------------------------------
// Multi-slot non-blocking GE list FIFO
// ---------------------------------------------------------------------------
static std::vector<GePendingList> g_ge_queue;
static std::condition_variable g_ge_done_cv;
// Start at 1: PPSSPP returns 1-based GE list IDs; game passes the
// returned uid back to UpdateStallAddr so uid=0 causes a mismatch.
static int g_ge_uid_counter = 1;
// Progress of the GE interpreter (render thread): g_ge_busy is set while it is inside a list, g_ge_activity counts the lists it has run (or
// stalled). DrawSync(0) uses them to tell a slow GE (a 10-fighter battle, a loaded machine) from a lost list.
static std::atomic<bool> g_ge_busy{false};
static std::atomic<uint64_t> g_ge_activity{0};

// ---------------------------------------------------------------------------
// render_queue_post -- blocking single-slot path for FramePresent
// ---------------------------------------------------------------------------
bool render_queue_async_present_enabled() {
    static const bool on = [] { const char* e = std::getenv("PSPRECOMP_ASYNC_PRESENT"); return e && e[0] && e[0] != '0'; }();
    return on;
}

void render_queue_post_async(RenderRequest& req) {
    psp_prof::seq("post-async-begin");
    std::unique_lock<std::mutex> lock(g_render_mutex);
    // The previous request (if the render thread has not got to it yet) owns the slot.
    g_render_cv.wait(lock, [] { return !g_render_req.pending || g_should_exit.load(); });
    g_render_req.type = req.type;
    g_render_req.rdram = req.rdram;
    g_render_req.list_idx = req.list_idx;
    g_render_req.list_addr = req.list_addr;
    g_render_req.stall_addr = req.stall_addr;
    g_render_req.fb_addr = req.fb_addr;
    g_render_req.fb_stride = req.fb_stride;
    g_render_req.fb_format = req.fb_format;
    g_render_req.pending = true;
    g_render_req.done = false;
    g_render_cv.notify_all();
    g_work_cv.notify_one();
    psp_prof::seq("post-async-end");
}

void render_queue_post(RenderRequest& req) {
    psp_prof::seq("post-begin");
    std::unique_lock<std::mutex> lock(g_render_mutex);
    // A pipelined present may still own the slot.
    g_render_cv.wait(lock, [] { return !g_render_req.pending || g_should_exit.load(); });

    g_render_req.type = req.type;
    g_render_req.rdram = req.rdram;
    g_render_req.list_idx = req.list_idx;
    g_render_req.list_addr = req.list_addr;
    g_render_req.stall_addr = req.stall_addr;
    g_render_req.fb_addr = req.fb_addr;
    g_render_req.fb_stride = req.fb_stride;
    g_render_req.fb_format = req.fb_format;
    g_render_req.pending = true;
    g_render_req.done = false;

    g_render_cv.notify_one();
    g_work_cv.notify_one();

    g_render_cv.wait(lock, [] {
        return g_render_req.done || g_should_exit.load();
    });
    psp_prof::seq("post-end");
}

// ---------------------------------------------------------------------------
// render_queue_enqueue_ge_list -- non-blocking GE list submission
// ---------------------------------------------------------------------------
int render_queue_enqueue_ge_list(
    uint8_t* rdram, uint32_t list_addr, uint32_t stall_addr
) {
    std::unique_lock<std::mutex> lock(g_render_mutex);

    int uid = g_ge_uid_counter++;
    psp_prof::seq("enqueue", uid);

    GePendingList entry;
    entry.uid = uid;
    entry.list_addr = list_addr;
    entry.stall_addr = stall_addr;
    entry.current_pc = list_addr;
    entry.rdram = rdram;
    entry.processed = false;
    entry.stalled = false;
    entry.begun = false;
    entry.ip_tag = ge_interp_current_tag();
    static const bool iptrace2 = [] { const char* e = std::getenv("PSPRECOMP_INTERP_TRACE"); return e && std::atoi(e) >= 2; }();
    if (iptrace2 && entry.ip_tag.seq) std::fprintf(stderr, "[IPQ] enqueue uid %d list %08X stall %08X tag seq %u rec %d\n", uid, list_addr, stall_addr, entry.ip_tag.seq, entry.ip_tag.record ? 1 : 0);

    g_ge_queue.push_back(entry);

    g_render_cv.notify_one();
    g_work_cv.notify_one();

    return uid;
}

// ---------------------------------------------------------------------------
// render_queue_update_stall -- advance stall address for a pending list
// ---------------------------------------------------------------------------
int render_queue_update_stall(int uid, uint32_t new_stall) {
    std::unique_lock<std::mutex> lock(g_render_mutex);

    psp_prof::seq("updatestall", uid);
    for (auto& entry : g_ge_queue) {
        if (entry.uid == uid) {
            entry.stall_addr = new_stall;
            entry.stalled = false;  // Unstall so it gets re-processed
            g_render_cv.notify_one();
            g_work_cv.notify_one();
            return 0;
        }
    }

    // UID not found (already completed or invalid) -- log to surface mismatches
    static int miss_count = 0;
    if (miss_count < 10) {
        std::fprintf(stderr,
            "[GE] UpdateStallAddr: uid=%d not found in queue "
            "(queue size=%zu)\n",
            uid, g_ge_queue.size());
        miss_count++;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// render_queue_draw_sync -- synchronize with GE processing
// ---------------------------------------------------------------------------
int render_queue_draw_sync(int mode) {
    psp_prof::seq("drawsync-begin", mode);
    struct SeqEnd { ~SeqEnd() { psp_prof::seq("drawsync-end"); } } seq_end_;
    // Yield first (no mutex held) so the render thread can process lists
    // before this thread blocks waiting for them to complete.
    sched_yield_point();

    std::unique_lock<std::mutex> lock(g_render_mutex);

    if (mode == 0) {
        // Blocking: wait until all GE lists are drained. Safety valve: 500 ms without any progress of the GE (a stalled list with no further
        // UpdateStallAddr, a lost list) gives up. A GE that is still working (inside a list, or finished lists since the last slice: a battle with
        // many fighters on a slow machine takes more than 500 ms per frame) is waited for, up to 30 s: giving up early let the game free and
        // reuse model / effect memory that a running list still read ("Unknown cmd", "Hit command limit" without END, a GE fault in 10-player
        // 3 v 3 battles). PSPRECOMP_GE_SYNC_OLD=1: the old fixed 500 ms.
        static const bool old_sync = std::getenv("PSPRECOMP_GE_SYNC_OLD") != nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t seen = g_ge_activity.load();
        bool drained = false;
        for (;;) {
            drained = g_ge_done_cv.wait_for(lock, std::chrono::milliseconds(500),
                [] { return g_ge_queue.empty() || g_should_exit.load(); });
            if (drained || old_sync) break;
            const uint64_t now_act = g_ge_activity.load();
            const bool progress = g_ge_busy.load() || now_act != seen;
            seen = now_act;
            if (!progress || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) break;
            static int n = 0; if (n++ < 20) std::fprintf(stderr, "[GE] DrawSync: the GE is still working (%lld ms), waiting\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
        }
        if (!drained) {
            std::fprintf(stderr,
                "[GE] DrawSync timeout: %zu lists still pending\n",
                g_ge_queue.size());
        }
        return 0;
    }

    // mode == 1: poll -- return 0 if done, 1 if still processing
    return g_ge_queue.empty() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// render_queue_all_ge_done / render_queue_has_pending_ge
// ---------------------------------------------------------------------------
bool render_queue_all_ge_done() {
    std::unique_lock<std::mutex> lock(g_render_mutex);
    return g_ge_queue.empty();
}

int render_queue_wait_caught_up(int timeout_ms) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t seen_act = g_ge_activity.load();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_render_mutex);
            bool caught_up = true;
            for (const auto& e : g_ge_queue) if (!e.stalled) { caught_up = false; break; }
            if (caught_up || g_should_exit.load())
                return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            g_work_cv.notify_one();
        }
        // A GE that is still working (inside a list, or lists finished since the last check) is waited for beyond timeout_ms (up to 30 s
        // in all): a 3 s limit let a rebuild free model memory under a running list on a slow machine. PSPRECOMP_GE_SYNC_OLD=1: the plain limit.
        static const bool old_sync = std::getenv("PSPRECOMP_GE_SYNC_OLD") != nullptr;
        const auto el = std::chrono::steady_clock::now() - t0;
        if (el >= std::chrono::milliseconds(timeout_ms)) {
            const uint64_t act = g_ge_activity.load();
            if (old_sync || el >= std::chrono::seconds(30) || !(g_ge_busy.load() || act != seen_act)) return -1;
            seen_act = act;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool render_queue_has_pending_ge() {
    std::unique_lock<std::mutex> lock(g_render_mutex);
    return !g_ge_queue.empty();
}

// ---------------------------------------------------------------------------
// render_queue_process -- main thread: drain both paths
// ---------------------------------------------------------------------------
void render_queue_process() {
    std::unique_lock<std::mutex> lock(g_render_mutex);

    // --- Path 1: Single-slot blocking request (FramePresent) ---
    if (g_render_req.pending) {
        if (g_should_exit.load()) {
            g_render_req.pending = false;
            g_render_req.done = true;
            g_render_cv.notify_one();
        } else {
            switch (g_render_req.type) {
            case RenderRequestType::FramePresent:
                ge_present_frame(
                    g_render_req.rdram,
                    g_render_req.fb_addr,
                    g_render_req.fb_stride,
                    g_render_req.fb_format);
                break;
            case RenderRequestType::DisplayList:
            case RenderRequestType::AllLists:
                ge_process_display_list(
                    g_render_req.rdram,
                    g_render_req.list_addr,
                    g_render_req.stall_addr);
                break;
            case RenderRequestType::None:
            default:
                break;
            }

            g_render_req.pending = false;
            g_render_req.done = true;
            g_render_cv.notify_one();
        }
    }

    // --- Path 2: Non-blocking GE list FIFO ---
    if (!g_ge_queue.empty() && !g_should_exit.load()) {
        // Process lists in-place. Completed lists get removed;
        // stalled lists stay in the queue for later resume.
        // Release lock during GL calls to avoid blocking game threads.
        // Make a copy of entries to process so we can unlock.
        std::vector<GePendingList> to_process;
        std::vector<int> completed_indices;

        for (size_t i = 0; i < g_ge_queue.size(); i++) {
            auto& entry = g_ge_queue[i];
            if (!entry.stalled) {
                to_process.push_back(entry);
            }
        }

        lock.unlock();

        // Test aid PSPRECOMP_GE_SLOW="<ms>:<every>": sleep <ms> after every <every>-th list (a slow GE, to reproduce what a 10-player battle does to DrawSync)
        static int slow_ms = 0, slow_every = 1; static bool slow_init = false;
        if (!slow_init) { slow_init = true; if (const char* e = std::getenv("PSPRECOMP_GE_SLOW")) { std::sscanf(e, "%d:%d", &slow_ms, &slow_every); if (slow_every < 1) slow_every = 1; } }
        static int slow_n = 0;
        g_ge_busy.store(true);
        for (auto& entry : to_process) {
            psp_prof::seq("list-begin", entry.uid);
            ge_interp_list_tag(entry.ip_tag);
            GeListResult result = ge_process_display_list(
                entry.rdram,
                entry.current_pc,
                entry.stall_addr);

            psp_prof::seq(result.completed ? "list-end" : "list-stall", entry.uid);
            entry.current_pc = result.stopped_pc;
            if (result.completed) {
                entry.processed = true;
            } else {
                entry.stalled = true;
            }
            g_ge_activity.fetch_add(1);
            if (slow_ms > 0 && ++slow_n % slow_every == 0) {   // (PSPRECOMP_GE_SLOW_FILE=<path>: only while that file exists, so the loading is not slowed)
                static const char* gate = std::getenv("PSPRECOMP_GE_SLOW_FILE");
                if (!gate || std::filesystem::exists(gate)) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
            }
        }
        g_ge_busy.store(false);

        lock.lock();

        // Update queue entries with results from processing. A list that
        // stalled at a stall address UpdateStallAddr moved meanwhile (it
        // unstalled the entry while the lock was released) is not stalled:
        // keeping the flag lost that unstall, and with no further update the
        // list never ran again (every DrawSync timed out: 2 frames/s from boot).
        for (auto& processed : to_process) {
            for (auto& queued : g_ge_queue) {
                if (queued.uid == processed.uid) {
                    queued.current_pc = processed.current_pc;
                    queued.processed = processed.processed;
                    queued.stalled = processed.stalled && queued.stall_addr == processed.stall_addr;
                    queued.begun = true;
                    break;
                }
            }
        }

        // Remove completed lists
        g_ge_queue.erase(
            std::remove_if(g_ge_queue.begin(), g_ge_queue.end(),
                [](const GePendingList& e) { return e.processed; }),
            g_ge_queue.end());

        // Notify any thread blocked in render_queue_draw_sync.
        // NOTE: the GE finish callback is NOT fired here. On real PSP the
        // finish callback fires per-list at the FINISH command, which is
        // exactly what psp_ge.cpp's GE_CMD_FINISH path does. Firing it
        // again at queue-drain time was a mistimed double-fire (issue #10).
        if (g_ge_queue.empty()) {
            g_ge_done_cv.notify_all();
        }
    } else if (g_should_exit.load() && !g_ge_queue.empty()) {
        g_ge_queue.clear();
        g_ge_done_cv.notify_all();
    }
}

// ---------------------------------------------------------------------------
// render_queue_wait_for_work -- main thread idle wait (<= timeout_ms)
// ---------------------------------------------------------------------------
void render_queue_wait_for_work(int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_render_mutex);
    g_work_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] {
        if (g_render_req.pending || g_should_exit.load()) return true;
        for (const auto& e : g_ge_queue)
            if (!e.stalled) return true;
        return false;
    });
}
