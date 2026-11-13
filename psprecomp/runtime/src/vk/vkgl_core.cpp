// GL-on-Vulkan layer core: frames in flight, command recording, barriers, render passes, pipelines, descriptors, draws.
// See vkgl.h for the conventions (GL-orientation images, inverted front face, clip depth fix in the shader).
#include "vkgl.h"
#include "vkgl_internal.h"
#include "psp_vk_rt.h"
#include "../../include/psp_rt.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>

namespace vkgl {

using namespace pspvk;

// ------------------------------------------------------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------------------------------------------------------

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer main = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cbs;    // every command buffer allocated from `pool`; handed out again after each pool reset (never allocate per submission: they are not freed by a reset)
    size_t cb_next = 0;
    bool main_open = false, upload_open = false;
    VkFence fence = VK_NULL_HANDLE;
    bool fence_pending = false;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    Buffer ubo, stage;
    size_t ubo_off = 0, stage_off = 0;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    VkQueryPool pp = VK_NULL_HANDLE;     // PSPRECOMP_VK_PASSPROF: boundary stamps between render passes / other work
    std::vector<std::string> pp_labels;  // label of the segment that starts at each stamp
    VkQueryPool ts = VK_NULL_HANDLE;     // one timestamp pair per submission (kTsPairs)
    int ts_pairs = 0;                    // pairs written this frame
    bool ts_open = false;                // the current main command buffer has its begin stamp
    std::vector<VkDescriptorPool> pools;
    size_t pool_idx = 0;
    std::unordered_map<uint64_t, VkDescriptorSet> tex_sets;
    std::vector<std::function<void()>> deferred;
    uint64_t end_serial = 0;             // last submission serial of the frame (valid once its fence is pending)
};

struct Globals {
    Context ctx;
    SDL_Window* window = nullptr;
    bool vsync = false;
    VkDevice dev = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;
    Frame frames[kFrames];
    std::vector<VkSemaphore> render_sems;   // one per swapchain image (a semaphore may still be in use by the presentation engine)
    int cur = 0;
    uint64_t serial = 1;                 // current submission serial
    uint64_t frame_epoch = 1;            // bumped at every frame_begin (swap)
    uint64_t completed_serial = 0;       // every submission up to this serial has finished on the GPU
    uint64_t next_gen = 1;
    // object tables
    std::vector<std::unique_ptr<Tex>> tex;
    std::vector<std::unique_ptr<Buffer>> buf;
    std::vector<std::unique_ptr<Fbo>> fbo;
    std::vector<std::unique_ptr<Rbo>> rbo;
    std::vector<std::unique_ptr<Vao>> vao;
    std::vector<std::unique_ptr<Sampler>> smp;
    std::vector<std::unique_ptr<Shader>> shd;
    std::vector<std::unique_ptr<Program>> prg;
    GLState st;
    // default framebuffer (FBO 0) = an offscreen image in GL orientation, flipped into the swapchain on swap
    Img fb0_color, fb0_depth;
    Fbo fb0;
    int fb0_w = 0, fb0_h = 0;
    // pipeline objects
    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE, set1_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkDescriptorPool set0_pool = VK_NULL_HANDLE;
    VkPipelineCache pipe_cache = VK_NULL_HANDLE;
    std::unordered_map<PipeKey, VkPipeline, PipeKeyHash> pipelines;
    std::unordered_map<uint64_t, VkRenderPass> render_passes;
    std::unordered_map<uint64_t, VkSampler> samplers;
    VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    Img dummy;                           // 1x1 white
    Img snap;                            // scratch copy of a render target a draw samples from (feedback)
    uint64_t feedback_draws = 0;
    uint64_t clears_folded = 0, passes = 0;   // glClear turned into a load op / render passes begun
    uint64_t conflict_copies = 0, conflict_bytes = 0;   // buffer stores copied because a write overlapped unfinished reads
    double gpu_ms_sum = 0; uint64_t gpu_ms_n = 0;
    bool passprof = false;
    std::unordered_map<std::string, std::pair<double, uint64_t>> pp_acc;   // label -> (ms, count) since the last stats line
    double t_submit = 0, t_draw = 0, t_wait = 0, t_upload = 0;   // CPU seconds spent in each since the last stats line
    uint64_t n_submit = 0;
    float ts_period = 1.0f;
    bool no_present = false;
    bool multi_draw = false;             // VK_EXT_multi_draw enabled
    PFN_vkCmdDrawMultiEXT cmd_draw_multi = nullptr;
    Buffer dummy_vb;                     // (0,0,0,1) for disabled attributes
    uint32_t ubo_align = 256;
    // current recording state
    bool pass_active = false;
    GLuint pass_fbo = 0;
    int pass_ncolor = 1;
    bool pass_has_depth = false;
    VkRenderPass pass_rp = VK_NULL_HANDLE;
    int pass_w = 0, pass_h = 0;
    VkPipeline bound_pipe = VK_NULL_HANDLE;
    int cache_vp[4] = {-1, -1, -1, -1};
    double cache_dr[2] = {-1, -1};
    float cache_bc[4] = {-1, -1, -1, -1};
    int cache_st[3] = {-1, -1, -1};
    VkBuffer cache_vb[kMaxAttribs] = {};
    uint32_t cache_ubo_off = ~0u;
    VkDescriptorSet cache_set1 = VK_NULL_HANDLE;
    uint64_t draws = 0;
    int draws_since_submit = 0;
    PipeKey last_key; VkPipeline last_pipe = VK_NULL_HANDLE; bool last_key_valid = false;
    bool warned_feedback = false, warned_mask = false;
    bool validation = false;
    // ray-queried shadows (PSPRECOMP_VK_RT=1): one acceleration structure + descriptor set 2 per frame in flight
    bool rt_on = false;
    RtAccel rt[kFrames];
    VkDescriptorSetLayout set2_layout = VK_NULL_HANDLE;
    VkDescriptorPool set2_pool = VK_NULL_HANDLE;
    VkDescriptorSet rt_set[kFrames] = {};
    VkAccelerationStructureKHR rt_set_tlas[kFrames] = {};
    VkBuffer rt_set_cols[kFrames] = {}, rt_set_fx_cols[kFrames] = {};
    uint32_t rt_set_gen[kFrames] = {};   // RtAccel::generation() the descriptors were written for (a freed handle can come back with the same value)
    bool rt_ready[kFrames] = {};
};
static constexpr int kTsPairs = 64;
static bool g_narrow_sync = false;   // PSPRECOMP_VK_SYNC=narrow: barriers/dependencies limited to the stages the renderer really uses
#ifdef __ANDROID__
static int kSubmitEvery = 1 << 30;   // tiled GPUs: one render pass per target and frame; submit at the sync points only
#else
static int kSubmitEvery = 400;
#endif   // PSPRECOMP_VK_SUBMIT_EVERY (draws); 0 = only at swap/sync points
Globals* G = nullptr;
static Globals& g() { return *G; }
GLState& st() { return G->st; }

static void vkcheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) std::fprintf(stderr, "[VKGL] %s failed (VkResult %d)\n", what, (int)r);
}

bool PipeKey::operator==(const PipeKey& o) const { return std::memcmp(this, &o, sizeof(PipeKey)) == 0; }
size_t PipeKeyHash::operator()(const PipeKey& k) const {
    static_assert(sizeof(PipeKey) % 8 == 0, "PipeKey is hashed in 8-byte words");
    const uint64_t* p = (const uint64_t*)&k;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < sizeof(PipeKey) / 8; i++) { h ^= p[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xFF51AFD7ED558CCDull; }
    return (size_t)(h ^ (h >> 32));
}

// ------------------------------------------------------------------------------------------------------------------
// Frames, command buffers, deferred destruction
// ------------------------------------------------------------------------------------------------------------------

static Frame& fr() { return G->frames[G->cur]; }
static constexpr int kPpMax = 256;
// Per-pass GPU profile: a stamp outside render passes whenever the kind of work changes; the time up to the next stamp is
// charged to `label`. Only with PSPRECOMP_VK_PASSPROF=1.
static void pp_mark(VkCommandBuffer cb, const std::string& label) {
    Frame& f = fr();
    if (!f.pp || (int)f.pp_labels.size() >= kPpMax) return;
    if (!f.pp_labels.empty() && f.pp_labels.back() == label && label == "other") return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, f.pp, (uint32_t)f.pp_labels.size());
    f.pp_labels.push_back(label);
}
struct Tmr {
    double* acc; std::chrono::steady_clock::time_point t0;
    explicit Tmr(double* a) : acc(a), t0(std::chrono::steady_clock::now()) {}
    ~Tmr() { *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

static void defer(std::function<void()> f) { fr().deferred.push_back(std::move(f)); }

static VkCommandBuffer new_cb(Frame& f) {
    VkCommandBuffer cb;
    if (f.cb_next < f.cbs.size()) cb = f.cbs[f.cb_next++];
    else {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g().dev, &ai, &cb);
        f.cbs.push_back(cb); f.cb_next = f.cbs.size();
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static VkCommandBuffer main_cb() {
    Frame& f = fr();
    if (!f.main_open) {
        f.main = new_cb(f);
        f.main_open = true;
        if (f.pp && f.pp_labels.empty()) { vkCmdResetQueryPool(f.main, f.pp, 0, kPpMax); pp_mark(f.main, "other"); }
        if (f.ts && f.ts_pairs < kTsPairs) {
            vkCmdResetQueryPool(f.main, f.ts, (uint32_t)f.ts_pairs * 2, 2);
            vkCmdWriteTimestamp(f.main, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.ts, (uint32_t)f.ts_pairs * 2);
            f.ts_open = true;
        }
    }
    return f.main;
}
static VkCommandBuffer upload_cb() { Frame& f = fr(); if (!f.upload_open) { f.upload = new_cb(f); f.upload_open = true; } return f.upload; }

static void destroy_buffer_now(Buffer& b) {
    if (b.buf) vmaDestroyBuffer(g().vma, b.buf, b.alloc);
    b = Buffer{};
}
static void destroy_img_now(Img& im) {
    if (im.att_view && im.att_view != im.view) vkDestroyImageView(g().dev, im.att_view, nullptr);
    if (im.view) vkDestroyImageView(g().dev, im.view, nullptr);
    if (im.image) vmaDestroyImage(g().vma, im.image, im.alloc);
    im = Img{};
}
static void defer_destroy(Img im) { if (im.valid()) defer([im]() mutable { destroy_img_now(im); }); }
static void defer_destroy(Buffer b) { if (b.buf) defer([b]() mutable { destroy_buffer_now(b); }); }

static bool alloc_buffer(Buffer& b, size_t size, VkBufferUsageFlags usage, bool cpu_read = false) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size ? size : 16; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = cpu_read ? VMA_MEMORY_USAGE_GPU_TO_CPU : VMA_MEMORY_USAGE_CPU_TO_GPU;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(g().vma, &bi, &ai, &b.buf, &b.alloc, &info) != VK_SUCCESS) { b = Buffer{}; return false; }
    b.map = info.pMappedData; b.size = bi.size;
    return true;
}

static void frame_begin(int i) {
    Frame& f = g().frames[i];
    g().frame_epoch++;
    if (f.fence_pending) {
        Tmr tw(&g().t_wait);
        vkWaitForFences(g().dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(g().dev, 1, &f.fence);
        if (f.end_serial > g().completed_serial) g().completed_serial = f.end_serial;
        f.fence_pending = false;
    }
    if (f.ts && f.ts_pairs > 0) {
        uint64_t t[kTsPairs * 2];
        if (vkGetQueryPoolResults(g().dev, f.ts, 0, (uint32_t)f.ts_pairs * 2, sizeof t, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ns = 0;
            for (int i = 0; i < f.ts_pairs; i++) if (t[i * 2 + 1] >= t[i * 2]) ns += (double)(t[i * 2 + 1] - t[i * 2]);
            g().gpu_ms_sum += ns * g().ts_period * 1e-6;
            g().gpu_ms_n++;
        }
    }
    if (f.pp && f.pp_labels.size() > 1) {
        uint64_t t[kPpMax];
        const uint32_t n = (uint32_t)f.pp_labels.size();
        if (vkGetQueryPoolResults(g().dev, f.pp, 0, n, sizeof(uint64_t) * n, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (uint32_t i = 0; i + 1 < n; i++) if (t[i + 1] >= t[i]) { auto& a = g().pp_acc[f.pp_labels[i]]; a.first += (double)(t[i + 1] - t[i]) * g().ts_period * 1e-6; a.second++; }
    }
    f.pp_labels.clear();
    f.ts_pairs = 0; f.ts_open = false;
    vkResetCommandPool(g().dev, f.pool, 0);
    f.cb_next = 0;
    f.main_open = f.upload_open = false;
    for (auto& d : f.deferred) d();
    f.deferred.clear();
    for (auto p : f.pools) vkResetDescriptorPool(g().dev, p, 0);
    f.pool_idx = 0;
    f.tex_sets.clear();
    f.ubo_off = f.stage_off = 0;
}

// Submit everything recorded so far. wait: block until the GPU is done with it.
static void submit_now(VkSemaphore wait_sem, VkPipelineStageFlags wait_stage, VkSemaphore signal, VkFence fence_in, bool wait);
static const char* g_end_reason = "?";
static void end_pass();

static void submit_now(VkSemaphore wait_sem, VkPipelineStageFlags wait_stage, VkSemaphore signal, VkFence fence_in, bool wait) {
    g_end_reason = "submit"; end_pass();
    if (fr().main_open && g().passprof) pp_mark(fr().main, "other");
    Tmr tm(&g().t_submit); g().n_submit++;
    Frame& f = fr();
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (f.upload_open) { vkEndCommandBuffer(f.upload); cbs[n++] = f.upload; f.upload_open = false; }
    if (f.main_open) {
        if (f.ts_open) { vkCmdWriteTimestamp(f.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.ts, (uint32_t)f.ts_pairs * 2 + 1); f.ts_pairs++; f.ts_open = false; }
        vkEndCommandBuffer(f.main); cbs[n++] = f.main; f.main_open = false;
    }
    VkFence fence = fence_in;
    VkFence temp = VK_NULL_HANDLE;
    if (wait && !fence) { VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(g().dev, &fi, nullptr, &temp); fence = temp; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n; si.pCommandBuffers = cbs;
    if (wait_sem) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait_sem; si.pWaitDstStageMask = &wait_stage; }
    if (signal) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signal; }
    if (n || wait_sem || signal || fence) vkcheck(vkQueueSubmit(g().ctx.queue(), 1, &si, fence), "vkQueueSubmit");
    g().serial++;
    g().bound_pipe = VK_NULL_HANDLE;
    g().draws_since_submit = 0;
    if (temp) { vkWaitForFences(g().dev, 1, &temp, VK_TRUE, UINT64_MAX); vkDestroyFence(g().dev, temp, nullptr); }
}

void flush() { submit_now(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, false); }
void finish() { submit_now(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, true); g().completed_serial = g().serial - 1; }

// ------------------------------------------------------------------------------------------------------------------
// Formats
// ------------------------------------------------------------------------------------------------------------------

bool fmt_for_internal(GLenum ifmt, FmtInfo* o) {
    switch (ifmt) {
    case GL_RGBA8: case GL_RGBA: case GL_RGB8: case GL_RGB: *o = {VK_FORMAT_R8G8B8A8_UNORM, 4, false}; return true;
    case GL_R8: case GL_RED: *o = {VK_FORMAT_R8_UNORM, 1, false}; return true;
    case GL_RG8: case GL_RG: *o = {VK_FORMAT_R8G8_UNORM, 2, false}; return true;
    case GL_RGBA16F: *o = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, false}; return true;
    case GL_RGBA32F: *o = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, false}; return true;
    case GL_R16F: *o = {VK_FORMAT_R16_SFLOAT, 2, false}; return true;
    case GL_RG16F: *o = {VK_FORMAT_R16G16_SFLOAT, 4, false}; return true;
    case GL_R32F: *o = {VK_FORMAT_R32_SFLOAT, 4, false}; return true;
    case GL_DEPTH24_STENCIL8: case GL_DEPTH_STENCIL: *o = {g().depth_fmt, 4, true}; return true;
    default: return false;
    }
}

static int src_bpp(GLenum fmt, GLenum type) {
    int comps = fmt == GL_RGBA || fmt == GL_BGRA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_RG ? 2 : 1;
    switch (type) {
    case GL_UNSIGNED_BYTE: return comps;
    case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return comps * 2;
    case GL_FLOAT: return comps * 4;
    case GL_UNSIGNED_INT_8_8_8_8_REV: return 4;
    default: return comps;
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Barriers and images
// ------------------------------------------------------------------------------------------------------------------

static void barrier(VkCommandBuffer cb, Img& im, VkImageLayout nl) {
    if (im.layout == nl) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = im.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout; b.newLayout = nl;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {im.aspect, 0, (uint32_t)im.levels, 0, 1};
    VkPipelineStageFlags ss = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ds = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (g_narrow_sync) {
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        ds = ss;
        b.srcAccessMask = im.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    im.layout = nl;
}

static bool create_img(Img& im, VkFormat fmt, int w, int h, int levels, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = (uint32_t)levels; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    Img n;
    if (vmaCreateImage(g().vma, &ii, &ai, &n.image, &n.alloc, nullptr) != VK_SUCCESS) { std::fprintf(stderr, "[VKGL] image %dx%d fmt %d failed\n", w, h, (int)fmt); return false; }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = n.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    const bool ds = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    vi.subresourceRange = {ds ? VK_IMAGE_ASPECT_DEPTH_BIT : aspect, 0, (uint32_t)levels, 0, 1};   // sampling view: depth only for D/S images
    if (vkCreateImageView(g().dev, &vi, nullptr, &n.view) != VK_SUCCESS) { vmaDestroyImage(g().vma, n.image, n.alloc); return false; }
    n.att_view = n.view;
    if (levels > 1 || ds) {
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        if (vkCreateImageView(g().dev, &vi, nullptr, &n.att_view) != VK_SUCCESS) n.att_view = n.view;
    }
    n.fmt = fmt; n.w = w; n.h = h; n.levels = levels; n.aspect = aspect; n.gen = g().next_gen++;
    im = n;
    return true;
}

static int mip_count(int w, int h) { int n = 1; while ((w | h) > 1) { w = std::max(1, w >> 1); h = std::max(1, h >> 1); n++; } return n; }

// ------------------------------------------------------------------------------------------------------------------
// Object tables
// ------------------------------------------------------------------------------------------------------------------

template <class T> static GLuint gen_obj(std::vector<std::unique_ptr<T>>& v) {
    if (v.empty()) v.emplace_back(nullptr);
    v.emplace_back(new T());
    return (GLuint)v.size() - 1;
}
template <class T> static T* get_obj(std::vector<std::unique_ptr<T>>& v, GLuint id) { return id > 0 && id < v.size() ? v[id].get() : nullptr; }

GLuint obj_gen_tex() { return gen_obj(g().tex); }
GLuint obj_gen_buf() { return gen_obj(g().buf); }
GLuint obj_gen_fbo() { return gen_obj(g().fbo); }
GLuint obj_gen_rbo() { return gen_obj(g().rbo); }
GLuint obj_gen_vao() { return gen_obj(g().vao); }
GLuint obj_gen_sampler() { return gen_obj(g().smp); }
GLuint obj_gen_shader(GLenum type) { GLuint id = gen_obj(g().shd); g().shd[id]->type = type; return id; }
GLuint obj_gen_program() { return gen_obj(g().prg); }
Tex* get_tex(GLuint i) { return get_obj(g().tex, i); }
Buffer* get_buf(GLuint i) { return get_obj(g().buf, i); }
Fbo* get_fbo(GLuint i) { return get_obj(g().fbo, i); }
Rbo* get_rbo(GLuint i) { return get_obj(g().rbo, i); }
Vao* get_vao(GLuint i) { return get_obj(g().vao, i); }
Sampler* get_sampler(GLuint i) { return get_obj(g().smp, i); }
Shader* get_shader(GLuint i) { return get_obj(g().shd, i); }
Program* get_prog(GLuint i) { return get_obj(g().prg, i); }

void del_tex(GLuint id) { if (Tex* t = get_tex(id)) { defer_destroy(t->img); g().tex[id].reset(); } }
void del_buf(GLuint id) { if (Buffer* b = get_buf(id)) { defer_destroy(*b); g().buf[id].reset(); } }
void del_rbo(GLuint id) { if (Rbo* r = get_rbo(id)) { defer_destroy(r->img); g().rbo[id].reset(); } }
void del_fbo(GLuint id) {
    if (Fbo* f = get_fbo(id)) {
        if (f->fb) { VkFramebuffer fb = f->fb; defer([fb]() { vkDestroyFramebuffer(G->dev, fb, nullptr); }); }
        if (f->fb_nd) { VkFramebuffer fb = f->fb_nd; defer([fb]() { vkDestroyFramebuffer(G->dev, fb, nullptr); }); }
        g().fbo[id].reset();
    }
}
void del_vao(GLuint id) { if (get_vao(id)) g().vao[id].reset(); }
void del_sampler(GLuint id) { if (get_sampler(id)) g().smp[id].reset(); }
void del_shader(GLuint id) { if (get_shader(id)) g().shd[id].reset(); }
void del_prog(GLuint id) {
    if (Program* p = get_prog(id)) {
        VkShaderModule vs = p->vs, fs = p->fs;
        defer([vs, fs]() { if (vs) vkDestroyShaderModule(G->dev, vs, nullptr); if (fs) vkDestroyShaderModule(G->dev, fs, nullptr); });
        g().prg[id].reset();
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Buffers
// ------------------------------------------------------------------------------------------------------------------

static const VkBufferUsageFlags kBufUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

// glBufferData: always a fresh store (that is also GL's orphaning); the old one lives until the GPU is done with it.
void buf_data(GLuint id, size_t size, const void* data) {
    Buffer* b = get_buf(id);
    if (!b) return;
    Buffer old = *b;
    Buffer nb;
    if (!alloc_buffer(nb, size, kBufUsage)) { std::fprintf(stderr, "[VKGL] buffer %zu bytes failed\n", size); return; }
    if (data) std::memcpy(nb.map, data, size);
    *b = nb;
    defer_destroy(old);
}
// Make [off, off+len) of the store safe to write: if recorded-but-unfinished draws read an overlapping range, move the buffer
// to a fresh copy (the old store lives on until the GPU is done with it).
static void make_writable(Buffer& b, size_t off, size_t len) {
    if (b.read_hi > b.read_lo && b.read_serial > g().completed_serial && off < b.read_hi && off + len > b.read_lo) {
        Buffer nb;
        if (!alloc_buffer(nb, b.size, kBufUsage)) return;
        g().conflict_copies++; g().conflict_bytes += b.size;
        std::memcpy(nb.map, b.map, b.size);
        defer_destroy(b);
        b = nb;
    } else if (b.read_serial <= g().completed_serial) {
        b.read_lo = b.read_hi = 0;
    }
}
void buf_subdata(GLuint id, size_t off, size_t size, const void* data) {
    Buffer* b = get_buf(id);
    if (!b || !b->map || off + size > b->size) return;
    make_writable(*b, off, size);
    std::memcpy((char*)b->map + off, data, size);
}
void* buf_map(GLuint id, size_t off, size_t len) {
    Buffer* b = get_buf(id);
    if (!b || !b->map) return nullptr;
    make_writable(*b, off, len ? len : b->size - off);
    return (char*)b->map + off;
}

// ------------------------------------------------------------------------------------------------------------------
// Textures
// ------------------------------------------------------------------------------------------------------------------

static const VkImageUsageFlags kTexUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

// A staging allocation: either from the frame ring or a one-off buffer freed when the frame retires.
static void* stage_alloc(size_t n, VkBuffer* buf, size_t* off) {
    Frame& f = fr();
    constexpr size_t kRing = 64u << 20;
    if (!f.stage.buf) alloc_buffer(f.stage, kRing, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    size_t a = (f.stage_off + 15) & ~size_t(15);
    if (f.stage.buf && a + n <= f.stage.size) { *buf = f.stage.buf; *off = a; f.stage_off = a + n; return (char*)f.stage.map + a; }
    Buffer one;
    if (!alloc_buffer(one, n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return nullptr;
    *buf = one.buf; *off = 0;
    void* p = one.map;
    defer_destroy(one);
    return p;
}

static void convert_rows(uint8_t* dst, int dst_bpp, const uint8_t* src, GLenum fmt, GLenum type, int w, int h, int row_len, int align) {
    const int sbpp = src_bpp(fmt, type);
    const size_t stride = (((size_t)(row_len ? row_len : w) * sbpp + align - 1) / align) * align;
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + y * stride;
        uint8_t* d = dst + (size_t)y * w * dst_bpp;
        if (fmt == GL_BGRA && type == GL_UNSIGNED_BYTE && dst_bpp == 4) {
            for (int x = 0; x < w; x++) { d[x*4+0] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4+0]; d[x*4+3] = s[x*4+3]; }
        } else if (fmt == GL_RGB && type == GL_UNSIGNED_BYTE && dst_bpp == 4) {
            for (int x = 0; x < w; x++) { d[x*4+0] = s[x*3+0]; d[x*4+1] = s[x*3+1]; d[x*4+2] = s[x*3+2]; d[x*4+3] = 255; }
        } else if (sbpp == dst_bpp) {
            std::memcpy(d, s, (size_t)w * dst_bpp);
        } else {
            std::memset(d, 0, (size_t)w * dst_bpp);
        }
    }
}

// Record a region upload into `cb` (the image must be in TRANSFER_DST).
static void copy_region(VkCommandBuffer cb, Img& im, int level, int x, int y, int w, int h, VkBuffer sb, size_t so) {
    VkBufferImageCopy rg{};
    rg.bufferOffset = so; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1};
    rg.imageOffset = {x, y, 0}; rg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, sb, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

static void upload_region(Tex& t, int level, int x, int y, int w, int h, GLenum fmt, GLenum type, const void* data) {
    Tmr tup(&G->t_upload);
    Img& im = t.img;
    FmtInfo fi{}; fmt_for_internal(t.ifmt, &fi);
    const size_t bytes = (size_t)w * h * fi.bpp;
    VkBuffer sb; size_t so;
    uint8_t* p = (uint8_t*)stage_alloc(bytes, &sb, &so);
    if (!p) return;
    convert_rows(p, fi.bpp, (const uint8_t*)data, fmt, type, w, h, st().unpack_row, st().unpack_align);
    // Not used by the commands recorded so far -> the upload can run before them (no render pass break).
    const bool early = im.used_serial != g().serial;
    if (!early) { g_end_reason = "tex-upload"; end_pass(); }
    VkCommandBuffer cb = early ? upload_cb() : main_cb();
    barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_region(cb, im, level, x, y, w, h, sb, so);
    barrier(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!early) im.used_serial = g().serial;
    if (level + 1 > im.valid_levels) im.valid_levels = level + 1;
}

void tex_image(GLuint id, int level, GLenum ifmt, int w, int h, GLenum fmt, GLenum type, const void* data) {
    Tex* t = get_tex(id);
    if (!t) return;
    FmtInfo fi{};
    if (!fmt_for_internal(ifmt, &fi)) { std::fprintf(stderr, "[VKGL] unsupported texture format 0x%x\n", ifmt); return; }
    if (level == 0) {
        if (!t->img.valid() || t->img.w != w || t->img.h != h || t->img.fmt != fi.vk) {
            defer_destroy(t->img);
            t->img = Img{};
            const bool ok = fi.depth
                ? create_img(t->img, fi.vk, w, h, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : create_img(t->img, fi.vk, w, h, mip_count(w, h), kTexUsage, VK_IMAGE_ASPECT_COLOR_BIT);
            if (!ok) return;
        }
        t->ifmt = ifmt;
        if (!data) { t->img.valid_levels = std::max(t->img.valid_levels, 1); return; }   // storage only (render targets, scratch)
        t->img.valid_levels = 0;
    } else if (!t->img.valid()) return;
    if (data) upload_region(*t, level, 0, 0, std::max(1, w), std::max(1, h), fmt, type, data);
}

void tex_subimage(GLuint id, int level, int x, int y, int w, int h, GLenum fmt, GLenum type, const void* data) {
    Tex* t = get_tex(id);
    if (!t || !t->img.valid() || !data) return;
    upload_region(*t, level, x, y, w, h, fmt, type, data);
}

void tex_genmips(GLuint id) {
    Tex* t = get_tex(id);
    if (!t || !t->img.valid() || t->img.levels < 2) return;
    Img& im = t->img;
    g_end_reason = "genmips"; end_pass();
    VkCommandBuffer cb = main_cb();
    im.used_serial = g().serial;
    // level 0 -> TRANSFER_SRC, the rest -> TRANSFER_DST; blit down the chain; finish all levels in SHADER_READ.
    barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // whole image (levels > 0 are undefined; fine, they are overwritten)
    int w = im.w, h = im.h;
    for (int l = 1; l < im.levels; l++) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const int nw = std::max(1, w >> 1), nh = std::max(1, h >> 1);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(l - 1), 0, 1}; bl.srcOffsets[1] = {w, h, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}; bl.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        w = nw; h = nh;
    }
    barrier(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    im.valid_levels = im.levels;
}

void rbo_storage(GLuint id, GLenum ifmt, int w, int h) {
    Rbo* r = get_rbo(id);
    if (!r) return;
    FmtInfo fi{};
    if (!fmt_for_internal(ifmt, &fi)) { std::fprintf(stderr, "[VKGL] unsupported renderbuffer format 0x%x\n", ifmt); return; }
    defer_destroy(r->img);
    r->img = Img{};
    r->ifmt = ifmt;
    if (fi.depth) create_img(r->img, fi.vk, w, h, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    else create_img(r->img, fi.vk, w, h, 1, kTexUsage, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ------------------------------------------------------------------------------------------------------------------
// Render passes and framebuffers
// ------------------------------------------------------------------------------------------------------------------

static VkRenderPass get_rp(VkFormat color, VkFormat color1, VkFormat depth, unsigned clear = 0) {
    // clear: 1 colour, 2 depth, 4 stencil are cleared by the pass's load op (compatible with the plain LOAD variant: pipelines use that one)
    uint64_t key = 1469598103934665603ull;
    for (uint64_t v : {(uint64_t)color, (uint64_t)color1, (uint64_t)depth, (uint64_t)clear}) { key ^= v + 0x9E3779B97F4A7C15ull; key *= 1099511628211ull; }
    auto it = g().render_passes.find(key);
    if (it != g().render_passes.end()) return it->second;
    VkAttachmentDescription at[3]{};
    VkAttachmentReference cr[2]{}, dr{};
    uint32_t n = 0, ncol = 0;
    auto color_att = [&](VkFormat f) {
        at[n].format = f; at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = (clear & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cr[ncol++] = {n, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        n++;
    };
    if (color) color_att(color);
    if (color1) color_att(color1);
    if (depth) {
        at[n].format = depth; at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = (clear & 2) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = (clear & 4) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].initialLayout = at[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dr = {n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        n++;
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = ncol; sp.pColorAttachments = ncol ? cr : nullptr;
    sp.pDepthStencilAttachment = depth ? &dr : nullptr;
    VkSubpassDependency dep[2]{};
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dep[0].dstStageMask = stages;
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dep[0].dstAccessMask = acc;
    if (g_narrow_sync) { dep[0].srcStageMask = stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT; dep[0].srcAccessMask = acc | VK_ACCESS_TRANSFER_WRITE_BIT; }
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = stages; dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = acc; dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (g_narrow_sync) { dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | stages; dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | acc; }
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = n; ri.pAttachments = at; ri.subpassCount = 1; ri.pSubpasses = &sp;
    ri.dependencyCount = 2; ri.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkcheck(vkCreateRenderPass(g().dev, &ri, nullptr, &rp), "vkCreateRenderPass");
    g().render_passes[key] = rp;
    return rp;
}

struct Target {
    Img* color = nullptr;
    Img* color1 = nullptr;      // second colour attachment, only when two draw buffers are enabled
    Img* depth = nullptr;
    Fbo* fbo = nullptr;
    int w = 0, h = 0;
};

static Target resolve(GLuint fbo_id) {
    Target t;
    if (fbo_id == 0) {
        t.color = &g().fb0_color; t.depth = g().fb0_depth.valid() ? &g().fb0_depth : nullptr; t.fbo = &g().fb0;
    } else {
        Fbo* f = get_fbo(fbo_id);
        if (!f) return t;
        t.fbo = f;
        if (f->color_tex) { if (Tex* x = get_tex(f->color_tex)) t.color = &x->img; }
        else if (f->color_rb) { if (Rbo* r = get_rbo(f->color_rb)) t.color = &r->img; }
        if (f->ndraw >= 2 && f->color_tex1) { if (Tex* x = get_tex(f->color_tex1)) t.color1 = &x->img; }
        if (f->depth_tex) { if (Tex* x = get_tex(f->depth_tex)) t.depth = &x->img; }
        else if (f->depth_rb) { if (Rbo* r = get_rbo(f->depth_rb)) t.depth = &r->img; }
    }
    if (t.color && !t.color->valid()) t.color = nullptr;
    if (t.color1 && !t.color1->valid()) t.color1 = nullptr;
    if (t.depth && !t.depth->valid()) t.depth = nullptr;
    if (t.color) { t.w = t.color->w; t.h = t.color->h; }
    else if (t.depth) { t.w = t.depth->w; t.h = t.depth->h; }   // depth-only framebuffer (blit target)
    return t;
}

bool fbo_complete(GLuint id) { Target t = resolve(id); return t.color != nullptr || t.depth != nullptr; }
void set_draw_buffers(GLsizei n) { if (Fbo* f = get_fbo(st().draw_fbo)) f->ndraw = n >= 2 ? 2 : 1; }

static void end_pass() {
    if (!g().pass_active) return;
    vkCmdEndRenderPass(fr().main);
    g().pass_active = false;
    if (g().passprof) pp_mark(fr().main, "other");
}

struct ClearReq { unsigned flags; float color[4]; float depth; uint32_t stencil; };

// A pass on this target that can take draws needing (want_depth) / not needing depth: a pass that has the depth attachment serves both.
static bool pass_matches(GLuint fbo_id, const Target& t, bool want_depth) {
    Globals& gl = g();
    if (!gl.pass_active || gl.pass_fbo != fbo_id) return false;
    if (want_depth && t.depth && !gl.pass_has_depth) return false;
    const uint64_t cg = t.color->gen, c1g = t.color1 ? t.color1->gen : 0;
    if (gl.pass_has_depth) return t.fbo->built_color == cg && t.fbo->built_color1 == c1g && t.fbo->built_depth == (t.depth ? t.depth->gen : 0);
    return t.fbo->bnd_color == cg && t.fbo->bnd_color1 == c1g;
}

static bool begin_pass(GLuint fbo_id, const Target& t, const ClearReq* clr = nullptr, bool want_depth = true) {
    Globals& gl = g();
    if (pass_matches(fbo_id, t, want_depth)) return true;
    if (gl.pass_active) g_end_reason = gl.pass_fbo != fbo_id ? "fbo-switch" : "needs-depth/attach";
    end_pass();
    const bool use_depth = want_depth && t.depth;
    VkCommandBuffer cb = main_cb();
    barrier(cb, *t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (t.color1) barrier(cb, *t.color1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (use_depth) barrier(cb, *t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    t.color->used_serial = gl.serial;
    if (t.color1) t.color1->used_serial = gl.serial;
    if (use_depth) t.depth->used_serial = gl.serial;
    Fbo& f = *t.fbo;
    const uint64_t cg = t.color->gen, c1g = t.color1 ? t.color1->gen : 0, dg = t.depth ? t.depth->gen : 0;
    VkFramebuffer fb;
    VkRenderPass rp;
    VkImageView views[3]; uint32_t nv = 0;
    views[nv++] = t.color->att_view;
    if (t.color1) views[nv++] = t.color1->att_view;
    const VkFormat f0 = t.color->fmt, f1 = t.color1 ? t.color1->fmt : VK_FORMAT_UNDEFINED;
    if (use_depth) {
        views[nv++] = t.depth->att_view;
        if (!f.fb || f.built_color != cg || f.built_color1 != c1g || f.built_depth != dg) {
            if (f.fb) { VkFramebuffer old = f.fb; defer([old]() { vkDestroyFramebuffer(G->dev, old, nullptr); }); }
            f.rp = get_rp(f0, f1, t.depth->fmt);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = f.rp; fi.attachmentCount = nv; fi.pAttachments = views; fi.width = t.w; fi.height = t.h; fi.layers = 1;
            vkcheck(vkCreateFramebuffer(gl.dev, &fi, nullptr, &f.fb), "vkCreateFramebuffer");
            f.built_color = cg; f.built_color1 = c1g; f.built_depth = dg; f.w = t.w; f.h = t.h;
        }
        fb = f.fb; rp = f.rp;
    } else {
        if (!f.fb_nd || f.bnd_color != cg || f.bnd_color1 != c1g) {
            if (f.fb_nd) { VkFramebuffer old = f.fb_nd; defer([old]() { vkDestroyFramebuffer(G->dev, old, nullptr); }); }
            f.rp_nd = get_rp(f0, f1, VK_FORMAT_UNDEFINED);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = f.rp_nd; fi.attachmentCount = nv; fi.pAttachments = views; fi.width = t.w; fi.height = t.h; fi.layers = 1;
            vkcheck(vkCreateFramebuffer(gl.dev, &fi, nullptr, &f.fb_nd), "vkCreateFramebuffer");
            f.bnd_color = cg; f.bnd_color1 = c1g; f.w = t.w; f.h = t.h;
        }
        fb = f.fb_nd; rp = f.rp_nd;
    }
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea = {{0, 0}, {(uint32_t)t.w, (uint32_t)t.h}};
    VkClearValue cvs[3]{};
    unsigned clear_flags = 0;
    if (clr && clr->flags) {   // the first thing on this target is a glClear: make it the load op (no read of the old contents)
        clear_flags = clr->flags & (use_depth ? 7u : 1u);
        bi.renderPass = get_rp(f0, f1, use_depth ? t.depth->fmt : VK_FORMAT_UNDEFINED, clear_flags);
        uint32_t na = 1;
        for (int i = 0; i < 4; i++) cvs[0].color.float32[i] = clr->color[i];
        if (t.color1) { cvs[1] = cvs[0]; na = 2; }
        if (use_depth) { cvs[na].depthStencil = {clr->depth, clr->stencil}; na++; }
        bi.clearValueCount = na; bi.pClearValues = cvs;
    }
    if (gl.passprof) { char lb[96]; std::snprintf(lb, sizeof lb, "pass fbo%u %dx%d%s%s%s <-%s", fbo_id, t.w, t.h, t.color1 ? " mrt" : "", use_depth ? " d" : "", clear_flags ? " clr" : "", g_end_reason); pp_mark(cb, lb); g_end_reason = "?"; }
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    gl.passes++;
    gl.pass_active = true; gl.pass_fbo = fbo_id; gl.pass_rp = rp; gl.pass_w = t.w; gl.pass_h = t.h; gl.pass_ncolor = t.color1 ? 2 : 1; gl.pass_has_depth = use_depth;
    gl.bound_pipe = VK_NULL_HANDLE;
    for (int& v : gl.cache_vp) v = -1;
    gl.cache_dr[0] = gl.cache_dr[1] = -1; gl.cache_bc[0] = -1;
    gl.cache_st[0] = gl.cache_st[1] = gl.cache_st[2] = -1;
    for (auto& b : gl.cache_vb) b = VK_NULL_HANDLE;
    gl.cache_ubo_off = ~0u; gl.cache_set1 = VK_NULL_HANDLE;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------
// Samplers, pipelines, descriptors
// ------------------------------------------------------------------------------------------------------------------

static VkSamplerAddressMode wrap_mode(GLenum w) {
    switch (w) {
    case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler vk_sampler(const SamplerParams& sp, int valid_levels) {
    const bool mips = sp.min == GL_NEAREST_MIPMAP_NEAREST || sp.min == GL_LINEAR_MIPMAP_NEAREST || sp.min == GL_NEAREST_MIPMAP_LINEAR || sp.min == GL_LINEAR_MIPMAP_LINEAR;
    const float max_lod = mips ? (float)std::max(0, std::min(sp.max_level, valid_levels - 1)) : 0.f;
    const float min_lod = mips ? (float)std::min(sp.base_level, (int)max_lod) : 0.f;
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { key ^= v; key *= 1099511628211ull; };
    uint32_t bias; std::memcpy(&bias, &sp.lod_bias, 4);
    mix(sp.min); mix(sp.mag); mix(sp.wrap_s); mix(sp.wrap_t); mix(bias); mix((uint64_t)(max_lod * 8)); mix((uint64_t)(min_lod * 8));
    auto it = g().samplers.find(key);
    if (it != g().samplers.end()) return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = sp.mag == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.minFilter = (sp.min == GL_NEAREST || sp.min == GL_NEAREST_MIPMAP_NEAREST || sp.min == GL_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = (sp.min == GL_NEAREST_MIPMAP_LINEAR || sp.min == GL_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap_mode(sp.wrap_s); si.addressModeV = wrap_mode(sp.wrap_t); si.addressModeW = si.addressModeU;
    si.mipLodBias = sp.lod_bias; si.minLod = min_lod; si.maxLod = max_lod;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s; vkcheck(vkCreateSampler(g().dev, &si, nullptr, &s), "vkCreateSampler");
    g().samplers[key] = s;
    return s;
}

static VkBlendFactor bfactor(GLenum f) {
    switch (f) {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(GLenum e) {
    switch (e) {
    case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GL_MIN: return VK_BLEND_OP_MIN;
    case GL_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkCompareOp cop(GLenum f) {
    switch (f) {
    case GL_NEVER: return VK_COMPARE_OP_NEVER;
    case GL_LESS: return VK_COMPARE_OP_LESS;
    case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp sop(GLenum o) {
    switch (o) {
    case GL_ZERO: return VK_STENCIL_OP_ZERO;
    case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
    case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case GL_INVERT: return VK_STENCIL_OP_INVERT;
    case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkFormat attr_format(int size, GLenum type, bool norm) {
    static const VkFormat f32[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat u8n[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat u8s[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED};
    static const VkFormat s8n[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM};
    static const VkFormat s8s[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED};
    static const VkFormat u16n[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
    static const VkFormat u16s[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED};
    static const VkFormat s16n[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat s16s[5] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    if (size < 1 || size > 4) size = 4;
    switch (type) {
    case GL_FLOAT: return f32[size];
    case GL_UNSIGNED_BYTE: return norm ? u8n[size] : u8s[size];
    case GL_BYTE: return norm ? s8n[size] : s8s[size];
    case GL_UNSIGNED_SHORT: return norm ? u16n[size] : u16s[size];
    case GL_SHORT: return norm ? s16n[size] : s16s[size];
    default: return f32[size];
    }
}
static uint32_t attr_bytes(int size, GLenum type) {
    switch (type) { case GL_UNSIGNED_BYTE: case GL_BYTE: return size; case GL_UNSIGNED_SHORT: case GL_SHORT: return size * 2; default: return size * 4; }
}

static VkPipeline make_pipeline(const PipeKey& k, Program& p) {
    VkPipelineShaderStageCreateInfo ss[2]{};
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = p.vs; ss[0].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = p.fs; ss[1].pName = "main";

    VkVertexInputBindingDescription vb[kMaxAttribs]{};
    VkVertexInputAttributeDescription va[kMaxAttribs]{};
    for (int i = 0; i < k.nbind; i++) { vb[i].binding = i; vb[i].stride = k.strides[i]; vb[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX; }
    for (int i = 0; i < k.nattr; i++) { va[i].location = k.attr[i][0]; va[i].binding = k.attr[i][1]; va[i].format = (VkFormat)k.attr[i][2]; va[i].offset = k.attr[i][3]; }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = k.nbind; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = k.nattr; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = (VkPrimitiveTopology)k.topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    rs.cullMode = (k.flags & 16) ? (k.cull == 0 ? VK_CULL_MODE_BACK_BIT : k.cull == 1 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_FRONT_AND_BACK) : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;     // GL's CCW, seen through the Vulkan (y down) window convention
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const bool has_depth = k.flags & 32;
    ds.depthTestEnable = has_depth && (k.flags & 1); ds.depthWriteEnable = has_depth && (k.flags & 2);
    ds.depthCompareOp = (VkCompareOp)k.depth_func;
    ds.stencilTestEnable = has_depth && (k.flags & 4);
    ds.front.compareOp = ds.back.compareOp = (VkCompareOp)k.stencil_func;
    ds.front.failOp = ds.back.failOp = (VkStencilOp)k.sfail;
    ds.front.depthFailOp = ds.back.depthFailOp = (VkStencilOp)k.zfail;
    ds.front.passOp = ds.back.passOp = (VkStencilOp)k.zpass;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = (k.flags & 8) != 0;
    cba.srcColorBlendFactor = (VkBlendFactor)k.bsrc_rgb; cba.dstColorBlendFactor = (VkBlendFactor)k.bdst_rgb;
    cba.srcAlphaBlendFactor = (VkBlendFactor)k.bsrc_a; cba.dstAlphaBlendFactor = (VkBlendFactor)k.bdst_a;
    cba.colorBlendOp = (VkBlendOp)k.beq_rgb; cba.alphaBlendOp = (VkBlendOp)k.beq_a;
    cba.colorWriteMask = k.cmask;
    VkPipelineColorBlendAttachmentState cbas[2] = {cba, cba};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.ncolor; cb.pAttachments = cbas;
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0]; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2; pi.pStages = ss; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = has_depth ? &ds : nullptr;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = g().pipe_layout;
    pi.renderPass = (VkRenderPass)(uintptr_t)k.rp; pi.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkcheck(vkCreateGraphicsPipelines(g().dev, g().pipe_cache, 1, &pi, nullptr, &pipe), "vkCreateGraphicsPipelines");
    return pipe;
}

static VkDescriptorSet alloc_set1(VkDescriptorSetLayout layout) {
    Frame& f = fr();
    for (;;) {
        if (f.pool_idx < f.pools.size()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = f.pools[f.pool_idx]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            VkDescriptorSet s;
            if (vkAllocateDescriptorSets(g().dev, &ai, &s) == VK_SUCCESS) return s;
            f.pool_idx++;
            continue;
        }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * kMaxSamplerBindings};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4096; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VkDescriptorPool np; vkcheck(vkCreateDescriptorPool(g().dev, &pi, nullptr, &np), "vkCreateDescriptorPool");
        f.pools.push_back(np);
    }
}

// ------------------------------------------------------------------------------------------------------------------
// Programs
// ------------------------------------------------------------------------------------------------------------------

bool link_program(Program& p) {
    p.linked = false;
    std::string vs, fs;
    for (GLuint sid : p.shaders) {
        Shader* s = get_shader(sid);
        if (!s) continue;
        (s->type == GL_VERTEX_SHADER ? vs : fs) = strip_glsl_version(s->src);
    }
    if (vs.empty() || fs.empty()) { p.log = "missing shader stage"; return false; }
    std::string err;
    ConvertedProgram conv;
    if (!convert_glsl_to_vulkan("", vs, fs, &conv, &err)) { p.log = err; return false; }
    std::vector<uint32_t> vspv, fspv;
    if (!compile_cached(Stage::Vertex, conv.vertex, &vspv, &err)) { p.log = "vertex: " + err; return false; }
    if (!compile_cached(Stage::Fragment, conv.fragment, &fspv, &err)) { p.log = "fragment: " + err; return false; }
    if (conv.ubo_size > kUboRange) { p.log = "uniform block too large"; return false; }
    if (conv.samplers.size() > (size_t)kMaxSamplerBindings) { p.log = "too many samplers"; return false; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule nv, nf;
    mi.codeSize = vspv.size() * 4; mi.pCode = vspv.data(); if (vkCreateShaderModule(g().dev, &mi, nullptr, &nv) != VK_SUCCESS) { p.log = "vkCreateShaderModule"; return false; }
    mi.codeSize = fspv.size() * 4; mi.pCode = fspv.data(); if (vkCreateShaderModule(g().dev, &mi, nullptr, &nf) != VK_SUCCESS) { vkDestroyShaderModule(g().dev, nv, nullptr); p.log = "vkCreateShaderModule"; return false; }
    VkShaderModule ov = p.vs, of = p.fs;
    if (ov || of) defer([ov, of]() { if (ov) vkDestroyShaderModule(G->dev, ov, nullptr); if (of) vkDestroyShaderModule(G->dev, of, nullptr); });
    p.vs = nv; p.fs = nf;
    p.uid = g().next_gen++;
    p.conv = std::move(conv);
    p.shadow.assign(p.conv.ubo_size ? p.conv.ubo_size : 16, 0);
    p.dirty = true;
    p.psize_idx = -1;
    for (size_t i = 0; i < p.conv.uniforms.size(); i++) if (p.conv.uniforms[i].name == "u_psize") p.psize_idx = (int)i;
    p.linked = true;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------
// Draw
// ------------------------------------------------------------------------------------------------------------------

static bool is_attachment_of(const Img* im, const Target& t) { return im == t.color || im == t.color1 || im == t.depth; }

// A draw samples the colour image it renders into (the GE's in-place glow/blur passes). The hardware reads the pre-draw
// contents; Vulkan forbids a feedback loop, so copy the target to a scratch image and sample that.
static Img* snapshot_of(Img& src) {
    Img& sn = g().snap;
    if (!sn.valid() || sn.w != src.w || sn.h != src.h || sn.fmt != src.fmt) {
        defer_destroy(sn);
        sn = Img{};
        create_img(sn, src.fmt, src.w, src.h, 1, kTexUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    g_end_reason = "feedback"; end_pass();
    VkCommandBuffer cb = main_cb();
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, sn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy rg{};
    rg.srcSubresource = rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.extent = {(uint32_t)src.w, (uint32_t)src.h, 1};
    vkCmdCopyImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(cb, sn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sn.valid_levels = 1;
    sn.used_serial = src.used_serial = g().serial;
    g().feedback_draws++;
    return &sn;
}

void draw_arrays(GLenum mode, GLint first, GLsizei count) { draw_multi(mode, &first, &count, 1); }

void draw_multi(GLenum mode, const GLint* firsts, const GLsizei* counts, GLsizei ndraws) {
    Tmr tdraw(&G->t_draw);
    GLState& s = st();
    if (s.discard || ndraws <= 0) return;
    Program* p = get_prog(s.program);
    static const bool trace = std::getenv("PSPRECOMP_VK_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "[VKTRACE] draw prog=%u linked=%d fbo=%u n=%d first=%d count=%d vp=%d,%d,%d,%d blend=%d depth=%d\n", s.program, p ? p->linked : -1, s.draw_fbo, ndraws, firsts[0], counts[0], s.vp[0], s.vp[1], s.vp[2], s.vp[3], s.blend, s.depth_test);
    if (trace && p && p->linked && p->conv.samplers.empty()) {
        Vao* tv = get_vao(s.vao);
        for (int loc = 0; tv && loc < 2; loc++) {
            const Attrib& a = tv->a[loc];
            Buffer* bf = get_buf(a.buffer);
            std::fprintf(stderr, "[VKTRACE]  attr %d en=%d size=%d type=0x%x stride=%d off=%zu buf=%u store=%p map=%p inputs=0x%x\n", loc, a.enabled, a.size, a.type, a.stride, a.offset, a.buffer, bf ? (void*)bf->buf : nullptr, bf ? bf->map : nullptr, p->conv.vertex_inputs);
            if (bf && bf->map && loc == 0) { const float* f = (const float*)((char*)bf->map + a.offset); std::fprintf(stderr, "[VKTRACE]  first floats %g %g | %g %g\n", f[0], f[1], f[6], f[7]); }
        }
        float res[2] = {0, 0};
        if (const pspvk::UniformMember* u = p->conv.find("u_res")) std::memcpy(res, &p->shadow[u->offset], 8);
        std::fprintf(stderr, "[VKTRACE]  u_res %g %g\n", res[0], res[1]);
    }
    if (!p || !p->linked) return;
    Target t = resolve(s.draw_fbo);
    if (!t.color) return;

    uint32_t topo;
    switch (mode) {
    case GL_POINTS: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case GL_LINES: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case GL_LINE_STRIP: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case GL_TRIANGLE_STRIP: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case GL_TRIANGLE_FAN: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case GL_TRIANGLES: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    default: return;
    }

    // 1. sampled images: resolve and make sure each is in SHADER_READ_ONLY (outside a render pass).
    const size_t nsamp = p->conv.samplers.size();
    Img* simg[kMaxSamplerBindings] = {};
    VkSampler ssmp[kMaxSamplerBindings] = {};
    bool need_barrier = false;
    for (size_t b = 0; b < nsamp; b++) {
        const int unit = p->unit[b];
        Tex* tx = get_tex(s.unit_tex[unit]);
        Img* im = tx && tx->img.valid() && tx->img.valid_levels > 0 ? &tx->img : nullptr;
        if (im && is_attachment_of(im, t)) {
            if (im == t.depth || im == t.color1) im = nullptr;   // sampling the depth buffer / second target while drawing: not used by the runtime
            else {
                // reuse the snapshot if an earlier binding of this draw already took one
                Img* prev = nullptr;
                for (size_t q = 0; q < b; q++) if (simg[q] == &g().snap) prev = simg[q];
                im = prev ? prev : snapshot_of(*im);
            }
        }
        SamplerParams def;
        const SamplerParams* sp = &def;
        if (Sampler* so = get_sampler(s.unit_sampler[unit])) sp = &so->sp; else if (tx) sp = &tx->sp;
        if (!im) { im = &g().dummy; }
        simg[b] = im;
        ssmp[b] = vk_sampler(*sp, im->valid_levels);
        if (im->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) need_barrier = true;
    }
    if (need_barrier) {
        g_end_reason = "sample-barrier"; end_pass();
        VkCommandBuffer cb = main_cb();
        for (size_t b = 0; b < nsamp; b++) barrier(cb, *simg[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (size_t b = 0; b < nsamp; b++) simg[b]->used_serial = g().serial;

    // 2. render pass
    // Depth is only needed by draws that test it or the stencil (GL: depth writes need the test). Other passes get no depth attachment.
    begin_pass(s.draw_fbo, t, nullptr, s.depth_test || s.stencil);
    VkCommandBuffer cb = fr().main;

    // 3. pipeline
    PipeKey k;
    std::memset(&k, 0, sizeof k);
    k.prog = p->uid;
    k.rp = (uint64_t)(uintptr_t)g().pass_rp;
    k.ncolor = (uint8_t)g().pass_ncolor;
    k.topology = topo;
    const bool has_depth = g().pass_has_depth;
    k.flags = (s.depth_test ? 1 : 0) | (s.depth_mask ? 2 : 0) | (s.stencil ? 4 : 0) | (s.blend ? 8 : 0) | (s.cull ? 16 : 0) | (has_depth ? 32 : 0);
    k.cmask = s.color_mask;
    k.depth_func = (uint8_t)cop(s.depth_func);
    k.stencil_func = (uint8_t)cop(s.stencil_func); k.sfail = (uint8_t)sop(s.sfail); k.zfail = (uint8_t)sop(s.zfail); k.zpass = (uint8_t)sop(s.zpass);
    k.cull = s.cull_mode == GL_BACK ? 0 : s.cull_mode == GL_FRONT ? 1 : 2;
    k.bsrc_rgb = (uint8_t)bfactor(s.bl.src_rgb); k.bdst_rgb = (uint8_t)bfactor(s.bl.dst_rgb);
    k.bsrc_a = (uint8_t)bfactor(s.bl.src_a); k.bdst_a = (uint8_t)bfactor(s.bl.dst_a);
    k.beq_rgb = (uint8_t)bop(s.bl.eq_rgb); k.beq_a = (uint8_t)bop(s.bl.eq_a);
    Vao* vao = get_vao(s.vao);
    VkBuffer vbufs[kMaxAttribs]; VkDeviceSize voffs[kMaxAttribs] = {};
    GLuint vbuf_gl[kMaxAttribs] = {};
    int nb = 0;
    uint32_t nattr = 0;
    for (int loc = 0; loc < kMaxAttribs; loc++) {
        if (!(p->conv.vertex_inputs & (1u << loc))) continue;   // not consumed by the shader
        const Attrib* a = vao ? &vao->a[loc] : nullptr;
        Buffer* bf = a && a->enabled ? get_buf(a->buffer) : nullptr;
        int binding = -1;
        uint32_t fmt, stride, off;
        if (bf && bf->buf) {
            fmt = attr_format(a->size, a->type, a->normalized);
            stride = a->stride ? (uint32_t)a->stride : attr_bytes(a->size, a->type);
            off = (uint32_t)a->offset;
            for (int i = 0; i < nb; i++) if (vbuf_gl[i] == a->buffer && k.strides[i] == stride) { binding = i; break; }
            if (binding < 0) { binding = nb++; vbuf_gl[binding] = a->buffer; vbufs[binding] = bf->buf; k.strides[binding] = stride; }
        } else {
            fmt = VK_FORMAT_R32G32B32A32_SFLOAT; off = 0;
            for (int i = 0; i < nb; i++) if (vbuf_gl[i] == ~0u && k.strides[i] == 0) { binding = i; break; }
            if (binding < 0) { binding = nb++; vbuf_gl[binding] = ~0u; vbufs[binding] = g().dummy_vb.buf; k.strides[binding] = 0; }
        }
        k.attr[nattr][0] = loc; k.attr[nattr][1] = binding; k.attr[nattr][2] = fmt; k.attr[nattr][3] = off; nattr++;
    }
    k.nattr = (uint8_t)nattr; k.nbind = (uint8_t)nb;
    VkPipeline pipe;
    if (g().last_key_valid && std::memcmp(&k, &g().last_key, sizeof k) == 0) pipe = g().last_pipe;   // same state as the previous draw
    else {
        auto it = g().pipelines.find(k);
        if (it == g().pipelines.end()) { pipe = make_pipeline(k, *p); g().pipelines.emplace(k, pipe); } else pipe = it->second;
        g().last_key = k; g().last_pipe = pipe; g().last_key_valid = pipe != VK_NULL_HANDLE;
    }
    if (!pipe) return;
    if (g().bound_pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); g().bound_pipe = pipe; }

    // 4. dynamic state
    const int vp[4] = {s.vp[0], s.vp[1], s.vp[2], s.vp[3]};
    if (std::memcmp(vp, g().cache_vp, sizeof vp) != 0 || g().cache_dr[0] != s.depth_range[0] || g().cache_dr[1] != s.depth_range[1]) {
        VkViewport v{(float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3], (float)std::clamp(s.depth_range[0], 0.0, 1.0), (float)std::clamp(s.depth_range[1], 0.0, 1.0)};
        vkCmdSetViewport(cb, 0, 1, &v);
        VkRect2D sc{{0, 0}, {(uint32_t)t.w, (uint32_t)t.h}};
        vkCmdSetScissor(cb, 0, 1, &sc);
        std::memcpy(g().cache_vp, vp, sizeof vp); g().cache_dr[0] = s.depth_range[0]; g().cache_dr[1] = s.depth_range[1];
    }
    if (std::memcmp(g().cache_bc, s.blend_color, sizeof s.blend_color) != 0) { vkCmdSetBlendConstants(cb, s.blend_color); std::memcpy(g().cache_bc, s.blend_color, sizeof s.blend_color); }
    if (g().cache_st[0] != (int)s.stencil_cmask || g().cache_st[1] != (int)s.stencil_wmask || g().cache_st[2] != s.stencil_ref) {
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_cmask & 0xFF);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_wmask & 0xFF);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)s.stencil_ref & 0xFF);
        g().cache_st[0] = (int)s.stencil_cmask; g().cache_st[1] = (int)s.stencil_wmask; g().cache_st[2] = s.stencil_ref;
    }

    // 5. uniforms (dynamic UBO) and samplers (descriptor set 1)
    if (p->psize_idx >= 0 && s.point_size_set) {
        const UniformMember& u = p->conv.uniforms[p->psize_idx];
        float cur; std::memcpy(&cur, &p->shadow[u.offset], 4);
        if (cur != s.point_size) { std::memcpy(&p->shadow[u.offset], &s.point_size, 4); p->dirty = true; }
    }
    Frame& f = fr();
    if (p->dirty || p->upload_epoch != g().frame_epoch) {
        p->upload_epoch = g().frame_epoch;
        const size_t sz = (p->shadow.size() + g().ubo_align - 1) & ~size_t(g().ubo_align - 1);
        if (f.ubo_off + sz > f.ubo.size) f.ubo_off = 0;   // exhausted (sized for >10k uniform updates a frame); wraps over finished data
        std::memcpy((char*)f.ubo.map + f.ubo_off, p->shadow.data(), p->shadow.size());
        p->last_offset = (uint32_t)f.ubo_off;
        f.ubo_off += sz;
        p->dirty = false;
    }
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    if (nsamp) {
        uint64_t key = 1469598103934665603ull ^ nsamp;
        for (size_t b = 0; b < nsamp; b++) { key = (key ^ (uint64_t)(uintptr_t)simg[b]->view) * 1099511628211ull; key = (key ^ (uint64_t)(uintptr_t)ssmp[b]) * 1099511628211ull; }
        auto si = f.tex_sets.find(key);
        if (si != f.tex_sets.end()) set1 = si->second;
        else {
            set1 = alloc_set1(g().set1_layout);
            VkDescriptorImageInfo ii[kMaxSamplerBindings]; VkWriteDescriptorSet w[kMaxSamplerBindings];
            // Bindings the program does not use still get a valid descriptor (the layout has all of them).
            for (int b = 0; b < kMaxSamplerBindings; b++) {
                const size_t bb = (size_t)b < nsamp ? (size_t)b : 0;
                ii[b] = {ssmp[bb], simg[bb]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[b].dstSet = set1; w[b].dstBinding = b; w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[b].pImageInfo = &ii[b];
            }
            vkUpdateDescriptorSets(g().dev, kMaxSamplerBindings, w, 0, nullptr);
            f.tex_sets[key] = set1;
        }
    } else {
        // Programs without samplers still use the shared pipeline layout; bind the dummy set once per frame.
        uint64_t key = 0x5a5a;
        auto si = f.tex_sets.find(key);
        if (si != f.tex_sets.end()) set1 = si->second;
        else {
            set1 = alloc_set1(g().set1_layout);
            VkDescriptorImageInfo ii{vk_sampler(SamplerParams(), 1), g().dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w[kMaxSamplerBindings];
            for (int b = 0; b < kMaxSamplerBindings; b++) {
                w[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w[b].dstSet = set1; w[b].dstBinding = b; w[b].descriptorCount = 1;
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[b].pImageInfo = &ii;
            }
            vkUpdateDescriptorSets(g().dev, kMaxSamplerBindings, w, 0, nullptr);
            f.tex_sets[key] = set1;
        }
    }
    if (g().cache_ubo_off != p->last_offset) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g().pipe_layout, 0, 1, &f.set0, 1, &p->last_offset);
        g().cache_ubo_off = p->last_offset;
    }
    if (g().cache_set1 != set1) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g().pipe_layout, 1, 1, &set1, 0, nullptr);
        g().cache_set1 = set1;
    }

    if (p->conv.uses_rt) {
        // ray-query programs need this frame's acceleration structure; without one the draw is skipped (the scene was not built)
        if (!g().rt_on || !g().rt_ready[g().cur]) return;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g().pipe_layout, 2, 1, &g().rt_set[g().cur], 0, nullptr);
    }

    // 6. vertex buffers + draw
    {
        GLint lo = firsts[0], hi = firsts[0] + counts[0];
        for (GLsizei i = 1; i < ndraws; i++) { lo = std::min(lo, firsts[i]); hi = std::max(hi, firsts[i] + counts[i]); }
        for (int loc = 0; loc < kMaxAttribs; loc++) {
            const Attrib* a = vao ? &vao->a[loc] : nullptr;
            if (!a || !a->enabled || !(p->conv.vertex_inputs & (1u << loc))) continue;
            Buffer* bf = get_buf(a->buffer);
            if (!bf) continue;
            const size_t stride = a->stride ? (size_t)a->stride : attr_bytes(a->size, a->type);
            const size_t r0 = a->offset + (size_t)lo * stride, r1 = a->offset + (size_t)(hi - 1) * stride + attr_bytes(a->size, a->type);   // end of the last vertex's attribute, not past it
            if (bf->read_serial <= g().completed_serial || bf->read_hi <= bf->read_lo) { bf->read_lo = r0; bf->read_hi = r1; }
            else { bf->read_lo = std::min(bf->read_lo, r0); bf->read_hi = std::max(bf->read_hi, r1); }
            bf->read_serial = g().serial;
        }
    }
    bool vb_changed = false;
    for (int i = 0; i < nb; i++) if (g().cache_vb[i] != vbufs[i]) vb_changed = true;
    if (vb_changed && nb) {
        vkCmdBindVertexBuffers(cb, 0, nb, vbufs, voffs);
        for (int i = 0; i < nb; i++) g().cache_vb[i] = vbufs[i];
    }
    if (ndraws == 1) {
        if (counts[0] > 0) vkCmdDraw(cb, (uint32_t)counts[0], 1, (uint32_t)firsts[0], 0);
    } else if (g().multi_draw) {
        VkMultiDrawInfoEXT mi[256];
        for (GLsizei base = 0; base < ndraws; base += 256) {
            const uint32_t n = (uint32_t)std::min<GLsizei>(256, ndraws - base);
            for (uint32_t i = 0; i < n; i++) mi[i] = {(uint32_t)firsts[base + i], (uint32_t)counts[base + i]};
            g().cmd_draw_multi(cb, n, mi, 1, 0, sizeof(VkMultiDrawInfoEXT));
        }
    } else {
        for (GLsizei i = 0; i < ndraws; i++) if (counts[i] > 0) vkCmdDraw(cb, (uint32_t)counts[i], 1, (uint32_t)firsts[i], 0);
    }
    g().draws += (uint64_t)ndraws;
    // Let the GPU start on what is recorded while the CPU keeps recording (GL drivers stream like this; a frame submitted
    // only at the end would run the CPU and the GPU one after the other).
    if (++g().draws_since_submit >= kSubmitEvery) { g().draws_since_submit = 0; flush(); }
}

void clear(GLbitfield mask) {
    GLState& s = st();
    Target t = resolve(s.draw_fbo);
    if (!t.color) return;
    if (s.color_mask != 15 && (mask & GL_COLOR_BUFFER_BIT) && !g().warned_mask) { g().warned_mask = true; std::fprintf(stderr, "[VKGL] glClear with a partial colour mask is cleared in full\n"); }
    const bool clr_depth = (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0;
    // A scissored clear (the flash bars along the screen edge) covers only the rectangle: no load-op folding, a clear inside a pass.
    VkClearRect cr{{{0, 0}, {(uint32_t)t.w, (uint32_t)t.h}}, 0, 1};
    bool partial = false;
    if (s.scissor && s.sc_rect[2] > 0 && s.sc_rect[3] > 0) {
        const int x0 = std::clamp(s.sc_rect[0], 0, t.w), y0 = std::clamp(s.sc_rect[1], 0, t.h);
        const int x1 = std::clamp(s.sc_rect[0] + s.sc_rect[2], 0, t.w), y1 = std::clamp(s.sc_rect[1] + s.sc_rect[3], 0, t.h);
        if (x1 <= x0 || y1 <= y0) return;
        if (x0 > 0 || y0 > 0 || x1 < t.w || y1 < t.h) { cr.rect = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}; partial = true; }
    }
    if (partial && !pass_matches(s.draw_fbo, t, clr_depth)) begin_pass(s.draw_fbo, t, nullptr, clr_depth);
    if (!partial && !pass_matches(s.draw_fbo, t, clr_depth)) {
        // Starting a pass on this target: fold the clear into the load op. Aspects not cleared keep LOAD.
        ClearReq c{};
        c.flags = ((mask & GL_COLOR_BUFFER_BIT) ? 1u : 0u) | ((mask & GL_DEPTH_BUFFER_BIT) ? 2u : 0u) | ((mask & GL_STENCIL_BUFFER_BIT) ? 4u : 0u);
        for (int i = 0; i < 4; i++) c.color[i] = s.clear_color[i];
        c.depth = (float)s.clear_depth; c.stencil = (uint32_t)s.clear_stencil & 0xFF;
        begin_pass(s.draw_fbo, t, &c, clr_depth);
        g().clears_folded++;
        return;
    }
    VkClearAttachment ca[3]; uint32_t n = 0;
    if (mask & GL_COLOR_BUFFER_BIT) {
        ca[n] = {}; ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++) ca[n].clearValue.color.float32[i] = s.clear_color[i];
        n++;
        if (t.color1) { ca[n] = ca[n - 1]; ca[n].colorAttachment = 1; n++; }
    }
    if (t.depth && g().pass_has_depth && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) {
        ca[n] = {};
        if (mask & GL_DEPTH_BUFFER_BIT) ca[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (mask & GL_STENCIL_BUFFER_BIT) ca[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        ca[n].clearValue.depthStencil = {(float)s.clear_depth, (uint32_t)s.clear_stencil & 0xFF};
        n++;
    }
    if (!n) return;
    vkCmdClearAttachments(fr().main, n, ca, 1, &cr);
}

void blit(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, GLbitfield mask, GLenum filter) {
    GLState& s = st();
    Target src = resolve(s.read_fbo), dst = resolve(s.draw_fbo);
    if ((mask & GL_COLOR_BUFFER_BIT) && (!src.color || !dst.color)) return;
    if (!(mask & GL_COLOR_BUFFER_BIT) && (!src.depth || !dst.depth)) return;
    g_end_reason = "blit"; end_pass();
    VkCommandBuffer cb = main_cb();
    Img* simg = (mask & GL_COLOR_BUFFER_BIT) ? src.color : src.depth;
    Img* dimg = (mask & GL_COLOR_BUFFER_BIT) ? dst.color : dst.depth;
    if (!simg || !dimg) return;
    if (simg == dimg && sx0 == dx0 && sy0 == dy0 && sx1 == dx1 && sy1 == dy1) return;   // a copy onto itself
    if (simg == dimg) { std::fprintf(stderr, "[VKGL] blit within one image is not supported (read fbo %u, draw fbo %u, mask 0x%x, src %d,%d-%d,%d dst %d,%d-%d,%d)\n", s.read_fbo, s.draw_fbo, mask, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1); return; }
    auto do_blit = [&](Img* si, Img* di, VkImageAspectFlags asp, VkFilter flt) {
        barrier(cb, *si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cb, *di, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        si->used_serial = di->used_serial = g().serial;
        VkImageBlit b{};
        b.srcSubresource = {asp, 0, 0, 1}; b.dstSubresource = {asp, 0, 0, 1};
        b.srcOffsets[0] = {sx0, sy0, 0}; b.srcOffsets[1] = {sx1, sy1, 1};
        b.dstOffsets[0] = {dx0, dy0, 0}; b.dstOffsets[1] = {dx1, dy1, 1};
        vkCmdBlitImage(cb, si->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, di->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, flt);
    };
    if (mask & GL_COLOR_BUFFER_BIT) do_blit(src.color, dst.color, VK_IMAGE_ASPECT_COLOR_BIT, filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && src.depth && dst.depth && src.depth != dst.depth) {
        const VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT | ((mask & GL_STENCIL_BUFFER_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0) {   // same size: a copy (the depth format may not support blits)
            barrier(cb, *src.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(cb, *dst.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            src.depth->used_serial = dst.depth->used_serial = g().serial;
            VkImageCopy rg{};
            rg.srcSubresource = {asp, 0, 0, 1}; rg.dstSubresource = {asp, 0, 0, 1};
            rg.srcOffset = {sx0, sy0, 0}; rg.dstOffset = {dx0, dy0, 0}; rg.extent = {(uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0), 1};
            vkCmdCopyImage(cb, src.depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.depth->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        } else do_blit(src.depth, dst.depth, asp, VK_FILTER_NEAREST);
    }
}

// IEEE half -> float (the fp16 scene targets of HDR Scene are read back as 8-bit screenshots: clamped to 0..1).
static float half_to_float(uint16_t h) {
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? 0.f : 65504.f) : std::ldexp((float)(m | 1024), (int)e - 25);
    return (h & 0x8000) ? -v : v;
}

bool read_pixels(int x, int y, int w, int h, GLenum fmt, GLenum type, void* out) {
    Target src = resolve(st().read_fbo);
    if (!src.color || w <= 0 || h <= 0) return false;
    g_end_reason = "readpix"; end_pass();
    VkCommandBuffer cb = main_cb();
    barrier(cb, *src.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    src.color->used_serial = g().serial;
    Buffer rb;
    const bool f16 = src.color->fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    const size_t bytes = (size_t)w * h * (f16 ? 8 : 4);
    if (!alloc_buffer(rb, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageOffset = {x, y, 0}; rg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(cb, src.color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &rg);
    finish();
    vmaInvalidateAllocation(g().vma, rb.alloc, 0, VK_WHOLE_SIZE);
    const uint8_t* p = (const uint8_t*)rb.map;
    uint8_t* o = (uint8_t*)out;
    const size_t npx = (size_t)w * h;
    if (f16 && type == GL_FLOAT) {   // raw values (HDR Scene statistics): fp16 -> float, not clamped
        const uint16_t* hp = (const uint16_t*)rb.map;
        float* of = (float*)out;
        for (size_t i = 0; i < npx * 4; i++) of[i] = half_to_float(hp[i]);
        destroy_buffer_now(rb);
        return true;
    }
    if (f16) {
        const uint16_t* hp = (const uint16_t*)rb.map;
        const bool bgra = fmt == GL_BGRA;
        for (size_t i = 0; i < npx; i++)
            for (int c = 0; c < 4; c++) {
                float v = half_to_float(hp[i * 4 + c]);
                v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
                o[i * 4 + (bgra && c < 3 ? 2 - c : c)] = (uint8_t)(v * 255.f + 0.5f);
            }
    } else if (fmt == GL_BGRA) for (size_t i = 0; i < npx; i++) { o[i*4+0] = p[i*4+2]; o[i*4+1] = p[i*4+1]; o[i*4+2] = p[i*4+0]; o[i*4+3] = p[i*4+3]; }
    else std::memcpy(o, p, bytes);
    destroy_buffer_now(rb);
    (void)type;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------
// Init / present
// ------------------------------------------------------------------------------------------------------------------

static bool create_fb0(int w, int h) {
    if (g().fb0_color.valid()) {
        defer_destroy(g().fb0_color); defer_destroy(g().fb0_depth);
        g().fb0_color = Img{}; g().fb0_depth = Img{};
        if (g().fb0.fb) { VkFramebuffer old = g().fb0.fb; defer([old]() { vkDestroyFramebuffer(G->dev, old, nullptr); }); g().fb0.fb = VK_NULL_HANDLE; }
        if (g().fb0.fb_nd) { VkFramebuffer old = g().fb0.fb_nd; defer([old]() { vkDestroyFramebuffer(G->dev, old, nullptr); }); g().fb0.fb_nd = VK_NULL_HANDLE; }
    }
    // An HDR swapchain needs a float default framebuffer: the final pass (psp_post.cpp) writes scRGB nits / 80, or the PQ-encoded signal, into it.
    const VkFormat fb0_fmt = g().ctx.swapchain().hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    if (!create_img(g().fb0_color, fb0_fmt, w, h, 1, kTexUsage, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    create_img(g().fb0_depth, g().depth_fmt, w, h, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    g().fb0_color.valid_levels = 1;
    g().fb0_w = w; g().fb0_h = h;
    // start black
    VkCommandBuffer cb = upload_cb();
    barrier(cb, g().fb0_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cv{}; VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g().fb0_color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    barrier(cb, g().fb0_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return true;
}

bool core_init(SDL_Window* w, bool vsync, std::string* err) {
    G = new Globals();
    Globals& gl = *G;
    gl.window = w; gl.vsync = vsync;
    ContextOptions o;
    o.window = w;
    o.validation = std::getenv("PSPRECOMP_VK_VALIDATION") != nullptr;
    // Always ask for ray query when the GPU has it (the settings row decides whether it is used); needs a 1.2 instance.
    // PSPRECOMP_VK_RT=0 (the settings menu saves it while Ray-Traced Shadows is off) only turns the shadows off: ray-traced reflections
    // (PSPRECOMP_VK_RT_REFL=1) still need the device's ray query, which used to be left out of the device then (the row said unavailable).
    const char* rt_env = std::getenv("PSPRECOMP_VK_RT"); const char* rr_env = std::getenv("PSPRECOMP_VK_RT_REFL");
    o.want_ray_query = !(rt_env && !std::strcmp(rt_env, "0") && !(rr_env && rr_env[0] && std::strcmp(rr_env, "0"))) && !std::getenv("PSPRECOMP_VK_NO_RT");
    gl.validation = o.validation;
    if (const char* d = std::getenv("PSPRECOMP_VK_DEVICE")) { if (*d >= '0' && *d <= '9') o.device_index = std::atoi(d); else o.device_name = d; }
    if (!gl.ctx.init(o, err)) { delete G; G = nullptr; return false; }
    gl.dev = gl.ctx.device();
    gl.vma = (VmaAllocator)gl.ctx.allocator();
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(w, &dw, &dh);
    { const char* he = std::getenv("PSPRECOMP_HDR"); gl.ctx.set_hdr_wanted(he && he[0] && he[0] != '0'); }
    if (!gl.ctx.create_swapchain(dw, dh, vsync, err)) { core_shutdown(); return false; }
    gl.ts_period = gl.ctx.props().limits.timestampPeriod;
    gl.passprof = std::getenv("PSPRECOMP_VK_PASSPROF") != nullptr;
    { const char* sy = std::getenv("PSPRECOMP_VK_SYNC"); g_narrow_sync = sy && !std::strcmp(sy, "narrow"); }
    if (gl.ctx.has_multi_draw() && !std::getenv("PSPRECOMP_VK_NO_MULTIDRAW")) {
        gl.cmd_draw_multi = (PFN_vkCmdDrawMultiEXT)vkGetDeviceProcAddr(gl.dev, "vkCmdDrawMultiEXT");
        gl.multi_draw = gl.cmd_draw_multi != nullptr;
    }
    std::fprintf(stderr, "[VKGL] multi-draw: %s\n", gl.multi_draw ? "VK_EXT_multi_draw" : "loop of vkCmdDraw");
    gl.no_present = std::getenv("PSPRECOMP_VK_NO_PRESENT") != nullptr;
    if (const char* e = std::getenv("PSPRECOMP_VK_SUBMIT_EVERY")) kSubmitEvery = std::atoi(e) > 0 ? std::atoi(e) : 1 << 30;
    gl.ubo_align = (uint32_t)std::max<VkDeviceSize>(256, gl.ctx.props().limits.minUniformBufferOffsetAlignment);

    // depth/stencil format
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(gl.ctx.physical(), VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) gl.depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;   // depth "blits" are same-size copies (no blit feature needed)

    std::fprintf(stderr, "[VKGL] depth/stencil format %s, sync mode %s, driver %u, vendor 0x%x\n", gl.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", g_narrow_sync ? "narrow" : "full", gl.ctx.props().driverVersion, gl.ctx.props().vendorID);

    // layouts
    VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo l0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l0.bindingCount = 1; l0.pBindings = &b0;
    vkcheck(vkCreateDescriptorSetLayout(gl.dev, &l0, nullptr, &gl.set0_layout), "set0 layout");
    VkDescriptorSetLayoutBinding b1[kMaxSamplerBindings];
    for (int i = 0; i < kMaxSamplerBindings; i++) b1[i] = {(uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo l1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l1.bindingCount = kMaxSamplerBindings; l1.pBindings = b1;
    vkcheck(vkCreateDescriptorSetLayout(gl.dev, &l1, nullptr, &gl.set1_layout), "set1 layout");
    VkDescriptorSetLayout sl[3] = {gl.set0_layout, gl.set1_layout, VK_NULL_HANDLE};
    gl.rt_on = gl.ctx.has_ray_query();
    if (gl.rt_on) {
        VkDescriptorSetLayoutBinding b2[3] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                              {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},    // per-triangle colours (reflections)
                                              {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};   // the same for the effects layer
        VkDescriptorSetLayoutCreateInfo l2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; l2.bindingCount = 3; l2.pBindings = b2;
        vkcheck(vkCreateDescriptorSetLayout(gl.dev, &l2, nullptr, &gl.set2_layout), "set2 layout");
        sl[2] = gl.set2_layout;
        VkDescriptorPoolSize ps2[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFrames}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFrames}};
        VkDescriptorPoolCreateInfo dp2{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp2.maxSets = kFrames; dp2.poolSizeCount = 2; dp2.pPoolSizes = ps2;
        vkcheck(vkCreateDescriptorPool(gl.dev, &dp2, nullptr, &gl.set2_pool), "set2 pool");
        for (int i = 0; i < kFrames; i++) {
            VkDescriptorSetAllocateInfo a2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; a2.descriptorPool = gl.set2_pool; a2.descriptorSetCount = 1; a2.pSetLayouts = &gl.set2_layout;
            vkAllocateDescriptorSets(gl.dev, &a2, &gl.rt_set[i]);
            if (!gl.rt[i].init(gl.ctx)) gl.rt_on = false;
        }
        std::fprintf(stderr, "[VKGL] ray query: %s\n", gl.rt_on ? "available (settings: Ray-Traced Shadows)" : "init failed");
    } else if (o.want_ray_query) std::fprintf(stderr, "[VKGL] ray query requested but not available on this device\n");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = gl.set2_layout ? 3 : 2; pl.pSetLayouts = sl;
    vkcheck(vkCreatePipelineLayout(gl.dev, &pl, nullptr, &gl.pipe_layout), "pipeline layout");
    VkDescriptorPoolSize ps0{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kFrames};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = kFrames; dp.poolSizeCount = 1; dp.pPoolSizes = &ps0;
    vkcheck(vkCreateDescriptorPool(gl.dev, &dp, nullptr, &gl.set0_pool), "set0 pool");
    VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkcheck(vkCreatePipelineCache(gl.dev, &pc, nullptr, &gl.pipe_cache), "pipeline cache");

    // frames
    for (int i = 0; i < kFrames; i++) {
        Frame& f = gl.frames[i];
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex = gl.ctx.queue_family();
        vkcheck(vkCreateCommandPool(gl.dev, &ci, nullptr, &f.pool), "command pool");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vkCreateFence(gl.dev, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(gl.dev, &si, nullptr, &f.acquire_sem);
        alloc_buffer(f.ubo, 32u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (gl.passprof && gl.ctx.props().limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kPpMax;
            vkCreateQueryPool(gl.dev, &qi, nullptr, &f.pp);
        }
        if (gl.ctx.props().limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kTsPairs * 2;
            vkCreateQueryPool(gl.dev, &qi, nullptr, &f.ts);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = gl.set0_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &gl.set0_layout;
        vkAllocateDescriptorSets(gl.dev, &ai, &f.set0);
        VkDescriptorBufferInfo bi{f.ubo.buf, 0, kUboRange};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = f.set0; wr.dstBinding = 0; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(gl.dev, 1, &wr, 0, nullptr);
    }
    gl.cur = 0;

    // dummy resources
    create_img(gl.dummy, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, kTexUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkBuffer sb; size_t so;
        uint8_t* p = (uint8_t*)stage_alloc(4, &sb, &so);
        std::memset(p, 255, 4);
        VkCommandBuffer cb = upload_cb();
        barrier(cb, gl.dummy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy_region(cb, gl.dummy, 0, 0, 0, 1, 1, sb, so);
        barrier(cb, gl.dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gl.dummy.valid_levels = 1;
    }
    alloc_buffer(gl.dummy_vb, 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    { const float v[4] = {0, 0, 0, 1}; std::memcpy(gl.dummy_vb.map, v, 16); }

    if (!create_fb0(std::max(dw, 1), std::max(dh, 1))) { if (err) *err = "default framebuffer image"; core_shutdown(); return false; }
    return true;
}

bool rt_set_scene(const float* xyz, uint32_t tri_count, const uint32_t* tri_cols, const float* fx_xyz, uint32_t fx_tri_count, const uint32_t* fx_cols) {
    Globals& gl = g();
    const int i = gl.cur;
    gl.rt_ready[i] = false;
    if (!gl.rt_on || !tri_count) return false;
    static const bool dbg = std::getenv("PSPRECOMP_RT_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[RT] set_scene %u tris\n", tri_count);
    end_pass();
    VkCommandBuffer cb = main_cb();
    // earlier draws of this frame may still read the structure being rebuilt (a second scene in one frame)
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR; mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);
    std::string err;
    if (dbg) std::fprintf(stderr, "[RT] record_build\n");
    if (!gl.rt[i].record_build(cb, xyz, tri_count, tri_cols, fx_xyz, fx_tri_count, fx_cols, &err)) { std::fprintf(stderr, "[VKGL] RT scene build failed: %s\n", err.c_str()); return false; }
    const bool realloc = gl.rt_set_gen[i] != gl.rt[i].generation();
    gl.rt_set_gen[i] = gl.rt[i].generation();
    if (realloc || gl.rt_set_tlas[i] != gl.rt[i].tlas()) {   // only after a (re)allocation: the structure is rebuilt in place otherwise
        VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        VkAccelerationStructureKHR t = gl.rt[i].tlas();
        wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &t;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.pNext = &wa; w.dstSet = gl.rt_set[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(gl.dev, 1, &w, 0, nullptr);
        gl.rt_set_tlas[i] = t;
    }
    if (realloc || gl.rt_set_cols[i] != gl.rt[i].cols_buffer()) {
        VkDescriptorBufferInfo bi{gl.rt[i].cols_buffer(), 0, gl.rt[i].cols_size()};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = gl.rt_set[i]; w.dstBinding = 1; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(gl.dev, 1, &w, 0, nullptr);
        gl.rt_set_cols[i] = gl.rt[i].cols_buffer();
    }
    if (realloc || gl.rt_set_fx_cols[i] != gl.rt[i].fx_cols_buffer()) {
        VkDescriptorBufferInfo bi{gl.rt[i].fx_cols_buffer(), 0, gl.rt[i].fx_cols_size()};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = gl.rt_set[i]; w.dstBinding = 2; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(gl.dev, 1, &w, 0, nullptr);
        gl.rt_set_fx_cols[i] = gl.rt[i].fx_cols_buffer();
    }
    if (dbg) std::fprintf(stderr, "[RT] scene ready\n");
    gl.rt_ready[i] = true;
    return true;
}
bool rt_available() { return G && G->rt_on; }
const char* rt_unavailable_reason() {
    static std::string why;
    if (!G) return "";
    if (G->rt_on) return "";
    char name[256]; std::snprintf(name, sizeof name, "%s", G->ctx.device_name().c_str());
    if (std::getenv("PSPRECOMP_VK_NO_RT") || (std::getenv("PSPRECOMP_VK_RT") && !std::strcmp(std::getenv("PSPRECOMP_VK_RT"), "0")))
        why = "Turned off by PSPRECOMP_VK_NO_RT / PSPRECOMP_VK_RT=0 for this run.";
#ifdef __ANDROID__
    else if (!pspvk::using_custom_driver())
        why = std::string("This phone's driver (") + name + ") has no ray query (VK_KHR_ray_query). On an Adreno 740 or newer, a Turnip driver under GPU Driver may add it (restart after choosing one).";
#endif
    else
        why = std::string("This GPU (") + name + ") has no hardware ray query support (VK_KHR_ray_query). It needs a GeForce RTX or Radeon RX 6000 series or newer, or an Adreno 740 or newer with a Turnip driver on Android.";
    return why.c_str();
}

void core_shutdown() {
    if (!G) return;
    Globals& gl = *G;
    if (gl.dev) vkDeviceWaitIdle(gl.dev);
    for (auto& r : gl.rt) r.destroy();
    if (gl.set2_pool) vkDestroyDescriptorPool(gl.dev, gl.set2_pool, nullptr);
    if (gl.set2_layout) vkDestroyDescriptorSetLayout(gl.dev, gl.set2_layout, nullptr);
    for (auto& f : gl.frames) {
        for (auto& d : f.deferred) d();
        f.deferred.clear();
        for (auto p : f.pools) vkDestroyDescriptorPool(gl.dev, p, nullptr);
        destroy_buffer_now(f.ubo); destroy_buffer_now(f.stage);
        if (f.ts) vkDestroyQueryPool(gl.dev, f.ts, nullptr);
        if (f.pp) vkDestroyQueryPool(gl.dev, f.pp, nullptr);
        if (f.pool) vkDestroyCommandPool(gl.dev, f.pool, nullptr);
        if (f.fence) vkDestroyFence(gl.dev, f.fence, nullptr);
        if (f.acquire_sem) vkDestroySemaphore(gl.dev, f.acquire_sem, nullptr);
    }
    // objects the game left behind
    for (auto& t : gl.tex) if (t) destroy_img_now(t->img);
    for (auto& r : gl.rbo) if (r) destroy_img_now(r->img);
    for (auto& b : gl.buf) if (b) destroy_buffer_now(*b);
    for (auto& f : gl.fbo) if (f) { if (f->fb) vkDestroyFramebuffer(gl.dev, f->fb, nullptr); if (f->fb_nd) vkDestroyFramebuffer(gl.dev, f->fb_nd, nullptr); }
    for (auto& p : gl.prg) if (p) { if (p->vs) vkDestroyShaderModule(gl.dev, p->vs, nullptr); if (p->fs) vkDestroyShaderModule(gl.dev, p->fs, nullptr); }
    for (auto& kv : gl.pipelines) vkDestroyPipeline(gl.dev, kv.second, nullptr);
    for (auto& kv : gl.render_passes) vkDestroyRenderPass(gl.dev, kv.second, nullptr);
    for (auto& kv : gl.samplers) vkDestroySampler(gl.dev, kv.second, nullptr);
    destroy_img_now(gl.fb0_color); destroy_img_now(gl.fb0_depth); destroy_img_now(gl.dummy); destroy_img_now(gl.snap);
    if (gl.fb0.fb) vkDestroyFramebuffer(gl.dev, gl.fb0.fb, nullptr);
    if (gl.fb0.fb_nd) vkDestroyFramebuffer(gl.dev, gl.fb0.fb_nd, nullptr);
    destroy_buffer_now(gl.dummy_vb);
    if (gl.pipe_cache) vkDestroyPipelineCache(gl.dev, gl.pipe_cache, nullptr);
    if (gl.pipe_layout) vkDestroyPipelineLayout(gl.dev, gl.pipe_layout, nullptr);
    if (gl.set0_pool) vkDestroyDescriptorPool(gl.dev, gl.set0_pool, nullptr);
    if (gl.set0_layout) vkDestroyDescriptorSetLayout(gl.dev, gl.set0_layout, nullptr);
    if (gl.set1_layout) vkDestroyDescriptorSetLayout(gl.dev, gl.set1_layout, nullptr);
    for (auto sm : gl.render_sems) vkDestroySemaphore(gl.dev, sm, nullptr);
    gl.ctx.destroy();
    delete G;
    G = nullptr;
}

void core_set_vsync(bool v) { if (G) G->vsync = v; }
void core_drawable(int* w, int* h) { if (G) { *w = G->fb0_w; *h = G->fb0_h; } }

void core_drawable_query(SDL_Window* window, int* w, int* h) {
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
    if (G && dw > 0 && dh > 0 && (dw != G->fb0_w || dh != G->fb0_h)) {
        end_pass();
        create_fb0(dw, dh);
    }
    *w = dw; *h = dh;
}

static void log_stats() {
    Globals& gl = *G;
    static uint64_t swaps = 0, last_draws = 0, last_fb = 0;
    if (++swaps % 600 == 0) {
        std::fprintf(stderr, "[VKGL] %llu frames: %llu draws/frame, %llu feedback copies/frame, %zu pipelines, GPU %.2f ms/frame | CPU ms/frame: draw %.2f (incl. submit), submit %.2f in %.1f calls, upload %.2f, fence wait %.2f | buffer conflicts %.2f/frame (%.2f MB/frame) | passes %.1f/frame, clears folded %.1f/frame\n", (unsigned long long)swaps,
                     (unsigned long long)((gl.draws - last_draws) / 600), (unsigned long long)((gl.feedback_draws - last_fb) / 600), gl.pipelines.size(),
                     gl.gpu_ms_n ? gl.gpu_ms_sum / (double)gl.gpu_ms_n : 0.0, gl.t_draw * 1e3 / 600, gl.t_submit * 1e3 / 600, (double)gl.n_submit / 600, gl.t_upload * 1e3 / 600, gl.t_wait * 1e3 / 600, (double)gl.conflict_copies / 600, (double)gl.conflict_bytes / 600 / 1048576.0, (double)gl.passes / 600, (double)gl.clears_folded / 600);
        gl.conflict_copies = gl.conflict_bytes = 0; gl.passes = gl.clears_folded = 0;
        {   // GPU memory this layer holds (VMA), and what objects it is made of: a steady climb here is a leak
            VmaTotalStatistics vs; vmaCalculateStatistics(gl.vma, &vs);
            size_t ntex = 0, nbuf = 0, nrbo = 0, nfbo = 0; uint64_t texpx = 0;
            for (auto& t : gl.tex) if (t) { ntex++; texpx += (uint64_t)t->img.w * (uint64_t)t->img.h; }
            for (auto& b : gl.buf) if (b) nbuf++;
            for (auto& r : gl.rbo) if (r) nrbo++;
            for (auto& f : gl.fbo) if (f) nfbo++;
            size_t ndef = 0; for (auto& f : gl.frames) ndef += f.deferred.size();
            std::fprintf(stderr, "[VKMEM] VMA %.0f MB in %u allocations (%u blocks, %.0f MB reserved) | textures %zu (%.1f Mpx), buffers %zu, renderbuffers %zu, fbos %zu, deferred frees %zu, image table %zu, command buffers %zu\n",
                         (double)vs.total.statistics.allocationBytes / 1048576.0, vs.total.statistics.allocationCount, vs.total.statistics.blockCount,
                         (double)vs.total.statistics.blockBytes / 1048576.0, ntex, (double)texpx / 1e6, nbuf, nrbo, nfbo, ndef, gl.tex.size(), gl.frames[0].cbs.size() + gl.frames[1].cbs.size());
        }
        if (gl.passprof) {
            std::vector<std::pair<std::string, std::pair<double, uint64_t>>> v(gl.pp_acc.begin(), gl.pp_acc.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
            for (size_t i = 0; i < v.size() && i < 8; i++)
                std::fprintf(stderr, "[VKPASS]   %-34s %6.2f ms/frame  (%.1f x/frame)\n", v[i].first.c_str(), v[i].second.first / 600.0, (double)v[i].second.second / 600.0);
            gl.pp_acc.clear();
        }
        gl.gpu_ms_sum = 0; gl.gpu_ms_n = 0; gl.t_draw = gl.t_submit = gl.t_wait = gl.t_upload = 0; gl.n_submit = 0;
        last_draws = gl.draws; last_fb = gl.feedback_draws;
    }
}

static bool g_hdr_dirty = false;
void core_set_hdr(bool on) { if (!G) return; G->ctx.set_hdr_wanted(on); g_hdr_dirty = true; }
bool core_hdr_supported() { return G && G->ctx.hdr_supported(); }
bool core_device_info(DeviceInfo* out) {
    if (!G || !out) return false;
    const VkPhysicalDeviceProperties& p = G->ctx.props();
    char b[96];
    out->name = G->ctx.device_name();
    std::snprintf(b, sizeof b, "Vulkan %u.%u.%u", VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    out->api = b;
    const uint32_t v = p.driverVersion;
    if (p.vendorID == 0x10DE) std::snprintf(b, sizeof b, "%u.%u.%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);   // NVIDIA packs it differently
    else std::snprintf(b, sizeof b, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    out->driver = b;
    out->vendor = p.vendorID;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(G->ctx.physical(), &mp);
    uint64_t best = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > best) best = mp.memoryHeaps[i].size;
    out->vram_total_gb = best ? double(best) / 1073741824.0 : -1;
    return true;
}
int core_hdr_encoding() { return G ? G->ctx.swapchain().hdr : 0; }
const char* core_hdr_reason() { return G ? G->ctx.hdr_reason().c_str() : "The Vulkan renderer is not running."; }

void core_swap() {
    Globals& gl = *G;
    Context& c = gl.ctx;
    end_pass();
    // Window resized: new swapchain and default framebuffer (the content of the old one is dropped, like a GL resize).
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(gl.window, &dw, &dh);
    Frame& f = fr();
    if (g_hdr_dirty && !gl.no_present && dw > 0 && dh > 0) {   // HDR switched in the settings: new swapchain format, new default framebuffer
        g_hdr_dirty = false;
        vkDeviceWaitIdle(gl.dev);
        std::string herr;
        if (c.create_swapchain(dw, dh, gl.vsync, &herr)) create_fb0(dw, dh);
        else std::fprintf(stderr, "[VKGL] HDR swapchain switch failed: %s\n", herr.c_str());
        vkDestroySemaphore(gl.dev, f.acquire_sem, nullptr);
        VkSemaphoreCreateInfo hsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkCreateSemaphore(gl.dev, &hsi, nullptr, &f.acquire_sem);
    }
    if (gl.no_present) {
        // Measurement mode: no swapchain, just run the frame (the default framebuffer is never shown).
        VkCommandBuffer cbn = main_cb();
        (void)cbn;
        submit_now(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, f.fence, false);
        f.end_serial = gl.serial - 1;
        f.fence_pending = true;
        log_stats();
        gl.cur = (gl.cur + 1) % kFrames;
        frame_begin(gl.cur);
        return;
    }
    uint32_t idx = 0;
    static double acq_ms = 0, pres_ms = 0, wait_ms = 0; static int swaps = 0; static double swap_t0 = 0;   // pacing diagnostics (PSPRECOMP_VK_SWAP_LOG=1)
    static const bool swap_log = std::getenv("PSPRECOMP_VK_SWAP_LOG") != nullptr;
    auto nowms = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const double t_acq0 = swap_log ? nowms() : 0;
    swap_t0 = t_acq0;
    VkResult r = c.swapchain().handle ? vkAcquireNextImageKHR(gl.dev, c.swapchain().handle, UINT64_MAX, f.acquire_sem, VK_NULL_HANDLE, &idx) : VK_ERROR_SURFACE_LOST_KHR;
    if (swap_log) acq_ms += nowms() - t_acq0;
    if (r == VK_ERROR_SURFACE_LOST_KHR || c.surface() == VK_NULL_HANDLE) {
        // Android destroyed the window (sleep / app switch) and gave SDL a new one: rebuild surface + swapchain, drop this frame's present.
        std::string err;
        if (!c.recreate_surface(&err) || dw <= 0 || dh <= 0 || !c.create_swapchain(dw, dh, gl.vsync, &err)) {
            std::fprintf(stderr, "[VKGL] surface lost, waiting for a window: %s\n", err.c_str());
            c.destroy_swapchain();
            submit_now(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, f.fence, false);   // nothing to present: run the frame like measurement mode
            f.end_serial = gl.serial - 1;
            f.fence_pending = true;
            gl.cur = (gl.cur + 1) % kFrames;
            frame_begin(gl.cur);
            SDL_Delay(50);   // paused in the background: do not spin
            return;
        }
        std::fprintf(stderr, "[VKGL] surface recreated (%dx%d)\n", dw, dh);
        if (dw != gl.fb0_w || dh != gl.fb0_h) create_fb0(dw, dh);
        vkDestroySemaphore(gl.dev, f.acquire_sem, nullptr);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkCreateSemaphore(gl.dev, &si, nullptr, &f.acquire_sem);
        r = vkAcquireNextImageKHR(gl.dev, c.swapchain().handle, UINT64_MAX, f.acquire_sem, VK_NULL_HANDLE, &idx);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (dw > 0 && dh > 0 && ((uint32_t)dw != c.swapchain().extent.width || (uint32_t)dh != c.swapchain().extent.height))) {
        // acquire semaphore may be signalled if r == SUBOPTIMAL; recreate everything and skip this present
        vkDeviceWaitIdle(gl.dev);
        std::string err;
        if (dw > 0 && dh > 0) {
            c.create_swapchain(dw, dh, gl.vsync, &err);
            if (dw != gl.fb0_w || dh != gl.fb0_h) create_fb0(dw, dh);
        }
        // semaphores can be left signalled by a failed acquire: recreate them
        vkDestroySemaphore(gl.dev, f.acquire_sem, nullptr);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vkCreateSemaphore(gl.dev, &si, nullptr, &f.acquire_sem);
        r = vkAcquireNextImageKHR(gl.dev, c.swapchain().handle, UINT64_MAX, f.acquire_sem, VK_NULL_HANDLE, &idx);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[VKGL] acquire failed (%d)\n", (int)r);
        finish();
        return;
    }
    VkCommandBuffer cb = main_cb();
    Img& src = gl.fb0_color;
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    src.used_serial = gl.serial;
    VkImage simg = c.swapchain().images[idx];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = simg; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    const VkExtent2D e = c.swapchain().extent;
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {0, src.h, 0}; bl.srcOffsets[1] = {src.w, 0, 1};      // GL orientation -> window: flip y
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {(int32_t)e.width, (int32_t)e.height, 1};
    vkCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    while (gl.render_sems.size() < c.swapchain().images.size()) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore sm; vkCreateSemaphore(gl.dev, &sci, nullptr, &sm); gl.render_sems.push_back(sm);
    }
    VkSemaphore render_sem = gl.render_sems[idx];
    submit_now(f.acquire_sem, VK_PIPELINE_STAGE_TRANSFER_BIT, render_sem, f.fence, false);
    f.end_serial = gl.serial - 1;
    f.fence_pending = true;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &render_sem;
    VkSwapchainKHR sc = c.swapchain().handle; pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
    const double t_pr0 = swap_log ? nowms() : 0;
    VkResult pr = vkQueuePresentKHR(c.queue(), &pi);
    if (swap_log) {
        pres_ms += nowms() - t_pr0;
        static double prev_t = 0; static double ivl[64], dur[64]; static int ivn = 0;
        const double tn = nowms();
        if (swaps >= 500 && ivn < 64) { ivl[ivn] = tn - prev_t; dur[ivn++] = tn - swap_t0; }
        prev_t = tn;
        if (swaps == 599) {
            std::fprintf(stderr, "[VKGL] swap intervals ms (swap call ms):");
            for (int k = 0; k < ivn; k++) std::fprintf(stderr, " %.1f(%.1f)", ivl[k], dur[k]);
            std::fprintf(stderr, "\n"); ivn = 0;
        }
        if (++swaps == 600) {
            std::fprintf(stderr, "[VKGL] swap: acquire %.2f ms, present call %.2f ms per frame (600 frames)\n", acq_ms / swaps, pres_ms / swaps);
            acq_ms = pres_ms = wait_ms = 0; swaps = 0;
        }
    }
    if (pr == VK_ERROR_SURFACE_LOST_KHR) { vkDeviceWaitIdle(gl.dev); c.destroy_swapchain(); }   // rebuilt on the next swap
    else if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) std::fprintf(stderr, "[VKGL] present failed (%d)\n", (int)pr);
    log_stats();
    gl.cur = (gl.cur + 1) % kFrames;
    frame_begin(gl.cur);
}

}  // namespace vkgl
