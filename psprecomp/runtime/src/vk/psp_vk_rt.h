// Ray-queried shadows, prototype (docs/VULKAN_PORT.md "Ray tracing hook").
// A world-space triangle soup becomes one bottom-level + one top-level acceleration structure; a compute shader casts
// a ray per query point toward the light with VK_KHR_ray_query. Stage 1 (this file, tests/test_vk_rt.cpp): the build and
// the query on arbitrary points, checked against a CPU ray caster. Stage 2 (not done): feed it the game's geometry from the
// draw path and the depth-reconstructed world positions of psp_screenfx, replacing the contact-shadow term.
// Needs a device that exposes ray query (Context::has_ray_query()); everything else keeps the raster path.
#pragma once
#include "psp_vk_context.h"
#include <string>

namespace pspvk {

class RtShadow {
public:
    ~RtShadow() { destroy(); }
    bool init(Context& ctx, std::string* err);
    void destroy();

    /// Replace the scene. xyz: 3 floats per vertex, 3 vertices per triangle (no index buffer). Blocks until built.
    bool build_scene(const float* xyz, uint32_t tri_count, std::string* err);

    /// For each query point: shadowed_out[i] = 1 when a ray from the point along `dir` (toward the light, unit length)
    /// hits scene geometry between `bias` and `max_dist`, else 0. Blocks until the GPU is done.
    bool trace_points(const float* pts_xyz, uint32_t n, const float dir[3], float max_dist, float bias,
                      uint8_t* shadowed_out, std::string* err);

    double last_build_ms() const { return build_ms_; }
    double last_trace_ms() const { return trace_ms_; }

private:
    struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; void* map = nullptr; VkDeviceSize size = 0; };
    bool make_buffer(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::string* err);
    void free_buffer(Buf* b);
    void free_scene();

    Context* ctx_ = nullptr;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout pl_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE, tlas_ = VK_NULL_HANDLE;
    Buf verts_, blas_buf_, tlas_buf_, inst_buf_, scratch_;
    Buf pts_, out_;
    uint32_t tris_ = 0;
    double build_ms_ = 0, trace_ms_ = 0;
};

/// Per-frame scene acceleration structure for the renderer (stage 2): same build as RtShadow::build_scene, but recorded into the
/// caller's command buffer (no wait), rebuilt in place every frame (one instance per frame in flight: the previous use of this
/// instance has finished when the layer starts recording into it again). Grows (with a device wait) when the scene outgrows it.
class RtAccel {
public:
    ~RtAccel() { destroy(); }
    bool init(Context& ctx);
    void destroy();
    /// Record "upload vertices, build BLAS, build TLAS, barrier to fragment/compute shader reads" into cb (outside a render pass).
    /// `tri_cols` (optional): one RGBA8 per triangle, read back by the shader as `rt_tri_col[]` (cols_buffer()); grey when null.
    bool record_build(VkCommandBuffer cb, const float* xyz, uint32_t tri_count, const uint32_t* tri_cols, const float* fx_xyz, uint32_t fx_tri_count, const uint32_t* fx_cols, std::string* err);
    /// The effects layer (second instance, mask 2: blended, depth-tested draws): its per-triangle RGBA8 (alpha = coverage).
    VkBuffer fx_cols_buffer() const { return cols2_.buf; }
    VkDeviceSize fx_cols_size() const { return cols2_.size; }
    VkBuffer cols_buffer() const { return cols_.buf; }
    VkDeviceSize cols_size() const { return cols_.size; }
    VkAccelerationStructureKHR tlas() const { return tlas_; }
    uint32_t generation() const { return gen_; }   // +1 per (re)allocation: handles can be reused by the driver, so compare this, not the handles
    uint32_t tris() const { return tris_; }
private:
    struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; void* map = nullptr; VkDeviceSize size = 0; };
    bool make_buffer(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::string* err);
    void free_buffer(Buf* b);
    bool allocate(uint32_t cap, uint32_t cap2, std::string* err);
    void free_all();
    Context* ctx_ = nullptr;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE, blas2_ = VK_NULL_HANDLE, tlas_ = VK_NULL_HANDLE;
    Buf verts_, blas_buf_, tlas_buf_, inst_buf_, scratch_, cols_, verts2_, blas2_buf_, cols2_;
    VkDeviceSize scratch_align_ = 128;
    uint32_t cap_ = 0, cap2_ = 0, tris_ = 0, fx_tris_ = 0, gen_ = 0;
};

}  // namespace pspvk
