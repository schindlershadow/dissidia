// Internal state of the GL-on-Vulkan layer. Single-threaded: everything runs on the render (GL) thread like GL did.
#pragma once
#include "psp_vk_context.h"
#include "psp_vk_glsl.h"
#include "../../third_party/vk_mem_alloc.h"
#include "psp_gl.h"
#include <SDL.h>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vkgl {

constexpr int kFrames = 2;           // frames in flight
constexpr int kMaxUnits = 8;
constexpr int kMaxAttribs = 6;   // the runtime's vertex shaders use locations 0..5
constexpr int kMaxSamplerBindings = 4;
constexpr uint32_t kUboRange = 4096; // descriptor range of the dynamic UBO (>= any program's block)

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView att_view = VK_NULL_HANDLE;   // single-level view for framebuffer attachments (== view when levels == 1)
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int w = 0, h = 0, levels = 1, valid_levels = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t used_serial = 0;        // submission serial of the last command stream use
    uint64_t gen = 0;                // unique per created image (framebuffers compare it; handles can be reused)
    bool valid() const { return image != VK_NULL_HANDLE; }
};

struct SamplerParams {
    GLenum min = GL_NEAREST_MIPMAP_LINEAR, mag = GL_LINEAR, wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
    float lod_bias = 0.f;
    int base_level = 0, max_level = 1000;
};

struct Tex { Img img; SamplerParams sp; GLenum ifmt = 0; };
struct Rbo { Img img; GLenum ifmt = 0; };
struct Sampler { SamplerParams sp; };

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    size_t size = 0;
    // Draws recorded but possibly not executed yet read [read_lo, read_hi) of this store (submission serial read_serial).
    // A write that overlaps them gets a fresh copy of the store first, so earlier draws keep seeing the old bytes (GL semantics).
    uint64_t read_serial = 0;
    size_t read_lo = 0, read_hi = 0;
};

struct Fbo {
    GLuint color_tex = 0, color_rb = 0, depth_rb = 0, depth_tex = 0;
    GLuint color_tex1 = 0;              // second colour attachment (GL_COLOR_ATTACHMENT1: the screen-space effects pass writes two targets)
    int ndraw = 1;                      // draw buffers enabled (glDrawBuffers): 1 or 2
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    uint64_t built_color = 0, built_color1 = 0, built_depth = 0;
    // The same target without its depth attachment (passes that never test depth: no depth load/store traffic on tiled GPUs).
    VkFramebuffer fb_nd = VK_NULL_HANDLE;
    VkRenderPass rp_nd = VK_NULL_HANDLE;
    uint64_t bnd_color = 0, bnd_color1 = 0;   // Img::gen of the attachments the VkFramebuffer was built from
    int w = 0, h = 0;
};

struct Shader { GLenum type = 0; std::string src; bool ok = false; std::string log; };

struct Program {
    std::vector<GLuint> shaders;
    bool linked = false;
    std::string log;
    pspvk::ConvertedProgram conv;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    uint64_t uid = 0;                   // unique per link: shader module handles can be reused after a delete, this cannot
    std::vector<uint8_t> shadow;        // uniform block contents
    bool dirty = true;
    uint32_t last_offset = 0;           // dynamic offset of the last upload of `shadow`
    uint64_t upload_epoch = ~0ull;      // frame epoch of that upload: every frame slot has its own UBO ring, so it is only valid in that frame
    int unit[kMaxSamplerBindings] = {0, 0, 0, 0};
    int psize_idx = -1;                 // index of u_psize in conv.uniforms, if any
};

struct Attrib {
    bool enabled = false;
    int size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    GLsizei stride = 0;
    size_t offset = 0;
    GLuint buffer = 0;
};
struct Vao { Attrib a[kMaxAttribs]; GLuint element = 0; };

struct BlendState {
    GLenum src_rgb = GL_ONE, dst_rgb = GL_ZERO, src_a = GL_ONE, dst_a = GL_ZERO, eq_rgb = GL_FUNC_ADD, eq_a = GL_FUNC_ADD;
};

// Everything the pipeline depends on. Plain bytes so it can be hashed; zero-initialise before filling.
struct PipeKey {
    uint64_t prog = 0;
    uint64_t rp = 0;
    uint32_t topology = 0;
    uint8_t flags = 0;            // 1 depth test, 2 depth write, 4 stencil, 8 blend, 16 cull, 32 pass has depth
    uint8_t cmask = 15;
    uint8_t depth_func = 0, stencil_func = 0, sfail = 0, zfail = 0, zpass = 0, cull = 0;
    uint8_t bsrc_rgb = 0, bdst_rgb = 0, bsrc_a = 0, bdst_a = 0, beq_rgb = 0, beq_a = 0;
    uint8_t nbind = 0, nattr = 0, ncolor = 1, pad_[5] = {};
    uint32_t strides[kMaxAttribs] = {};
    uint32_t attr[kMaxAttribs][4] = {};   // location, binding, format, offset
    bool operator==(const PipeKey& o) const;
};
struct PipeKeyHash { size_t operator()(const PipeKey& k) const; };

struct GLState {
    // enables
    bool blend = false, depth_test = false, cull = false, stencil = false, scissor = false, discard = false;
    BlendState bl;
    int sc_rect[4] = {0, 0, 0, 0};   // glScissor rectangle (GL origin); only glClear honours it (the flash bars along the screen edge)
    float blend_color[4] = {0, 0, 0, 0};
    uint8_t color_mask = 15;
    GLenum depth_func = GL_LESS;
    bool depth_mask = true;
    GLenum stencil_func = GL_ALWAYS;
    int stencil_ref = 0;
    uint32_t stencil_cmask = 0xFFFFFFFFu, stencil_wmask = 0xFFFFFFFFu;
    GLenum sfail = GL_KEEP, zfail = GL_KEEP, zpass = GL_KEEP;
    GLenum cull_mode = GL_BACK;
    int vp[4] = {0, 0, 0, 0};
    double depth_range[2] = {0.0, 1.0};
    float clear_color[4] = {0, 0, 0, 0};
    double clear_depth = 1.0;
    int clear_stencil = 0;
    float point_size = 1.0f;
    bool point_size_set = false;   // glPointSize was called (desktop); on Android the runtime writes u_psize itself
    // bindings
    GLuint program = 0, vao = 0, array_buffer = 0, copy_write_buffer = 0, draw_fbo = 0, read_fbo = 0, renderbuffer = 0;
    int active_unit = 0;
    GLuint unit_tex[kMaxUnits] = {};
    GLuint unit_sampler[kMaxUnits] = {};
    int unpack_row = 0, unpack_align = 4;
};

// ---- core (vkgl_core.cpp) used by the GL entry points (vkgl_api.cpp) ----
struct Frame;
struct Globals;
extern Globals* G;

struct FmtInfo { VkFormat vk; int bpp; bool depth; };
bool fmt_for_internal(GLenum ifmt, FmtInfo* out);

// object tables (names are indices; 0 is "none")
bool core_init(SDL_Window* w, bool vsync, std::string* err);
void core_shutdown();
bool rt_set_scene(const float* xyz, uint32_t tri_count, const uint32_t* tri_cols, const float* fx_xyz, uint32_t fx_tri_count, const uint32_t* fx_cols);   // PSPRECOMP_VK_RT ray-query scene (psp_rt.h)
bool rt_available();
void core_set_hdr(bool on);
bool core_hdr_supported();
int core_hdr_encoding();
const char* core_hdr_reason();
const char* rt_unavailable_reason();
void core_swap();
void core_set_vsync(bool v);
void core_drawable(int* w, int* h);
void core_drawable_query(SDL_Window* window, int* w, int* h);   // current window size; resizes the default framebuffer

GLuint obj_gen_tex();   GLuint obj_gen_buf();   GLuint obj_gen_fbo();  GLuint obj_gen_rbo();
GLuint obj_gen_vao();   GLuint obj_gen_sampler(); GLuint obj_gen_shader(GLenum type); GLuint obj_gen_program();
Tex* get_tex(GLuint); Buffer* get_buf(GLuint); Fbo* get_fbo(GLuint); Rbo* get_rbo(GLuint); Vao* get_vao(GLuint);
Sampler* get_sampler(GLuint); Shader* get_shader(GLuint); Program* get_prog(GLuint);
void del_tex(GLuint); void del_buf(GLuint); void del_fbo(GLuint); void del_rbo(GLuint); void del_vao(GLuint);
void del_sampler(GLuint); void del_shader(GLuint); void del_prog(GLuint);
GLState& st();

void buf_data(GLuint id, size_t size, const void* data);
void buf_subdata(GLuint id, size_t off, size_t size, const void* data);
void* buf_map(GLuint id, size_t off, size_t len);
void tex_image(GLuint id, int level, GLenum ifmt, int w, int h, GLenum fmt, GLenum type, const void* data);
void tex_subimage(GLuint id, int level, int x, int y, int w, int h, GLenum fmt, GLenum type, const void* data);
void tex_genmips(GLuint id);
void rbo_storage(GLuint id, GLenum ifmt, int w, int h);
bool link_program(Program& p);
void draw_arrays(GLenum mode, GLint first, GLsizei count);
void draw_multi(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n);   // state prepared once, n draws
void clear(GLbitfield mask);
void blit(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, GLbitfield mask, GLenum filter);
bool read_pixels(int x, int y, int w, int h, GLenum fmt, GLenum type, void* out);
void finish();
void flush();
bool fbo_complete(GLuint id);
void set_draw_buffers(GLsizei n);

}  // namespace vkgl
