#include "psp_vk_rt.h"
#include "psp_vk_glsl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace pspvk {

static PFN_vkCreateAccelerationStructureKHR p_create;
static PFN_vkDestroyAccelerationStructureKHR p_destroy;
static PFN_vkGetAccelerationStructureBuildSizesKHR p_sizes;
static PFN_vkCmdBuildAccelerationStructuresKHR p_build;
static PFN_vkGetAccelerationStructureDeviceAddressKHR p_addr;

static bool load_procs(VkDevice d, std::string* err) {
    if (p_create) return true;
#define LOAD(var, name) var = (PFN_##name)vkGetDeviceProcAddr(d, #name); if (!var) { *err = "missing " #name; return false; }
    LOAD(p_create, vkCreateAccelerationStructureKHR) LOAD(p_destroy, vkDestroyAccelerationStructureKHR)
    LOAD(p_sizes, vkGetAccelerationStructureBuildSizesKHR) LOAD(p_build, vkCmdBuildAccelerationStructuresKHR)
    LOAD(p_addr, vkGetAccelerationStructureDeviceAddressKHR)
#undef LOAD
    return true;
}

static const char* kShader = R"(#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, std430) readonly buffer Pts { float p[]; } pts;
layout(set = 0, binding = 2, std430) writeonly buffer Out { uint o[]; } outb;
layout(push_constant) uniform PC { vec4 dir_maxd; float bias; uint n; } pc;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.n) return;
    vec3 o = vec3(pts.p[3u * i], pts.p[3u * i + 1u], pts.p[3u * i + 2u]);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, o, pc.bias, pc.dir_maxd.xyz, pc.dir_maxd.w);
    while (rayQueryProceedEXT(rq)) {}
    outb.o[i] = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 1u : 0u;
}
)";

static double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool RtShadow::make_buffer(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::string* err) {
    VkDevice d = ctx_->device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d, &bi, nullptr, &b->buf) != VK_SUCCESS) { *err = "vkCreateBuffer"; return false; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, b->buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(ctx_->physical(), &mp);
    const VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = (int)i; break; }
    if (type < 0 && !host)   // no device-local type for this buffer: any type will do
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) { type = (int)i; break; }
    if (type < 0) { *err = "no memory type"; return false; }
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.pNext = &fi; mi.allocationSize = mr.size; mi.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d, &mi, nullptr, &b->mem) != VK_SUCCESS) { *err = "vkAllocateMemory"; return false; }
    vkBindBufferMemory(d, b->buf, b->mem, 0);
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ai.buffer = b->buf;
    b->addr = vkGetBufferDeviceAddress(d, &ai);
    b->size = size;
    if (host && vkMapMemory(d, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { *err = "vkMapMemory"; return false; }
    return true;
}

void RtShadow::free_buffer(Buf* b) {
    VkDevice d = ctx_->device();
    if (b->map) vkUnmapMemory(d, b->mem);
    if (b->buf) vkDestroyBuffer(d, b->buf, nullptr);
    if (b->mem) vkFreeMemory(d, b->mem, nullptr);
    *b = Buf{};
}

void RtShadow::free_scene() {
    if (!ctx_) return;
    if (tlas_) { p_destroy(ctx_->device(), tlas_, nullptr); tlas_ = VK_NULL_HANDLE; }
    if (blas_) { p_destroy(ctx_->device(), blas_, nullptr); blas_ = VK_NULL_HANDLE; }
    free_buffer(&verts_); free_buffer(&blas_buf_); free_buffer(&tlas_buf_); free_buffer(&inst_buf_); free_buffer(&scratch_);
    tris_ = 0;
}

bool RtShadow::init(Context& ctx, std::string* err) {
    std::string e; if (!err) err = &e;
    ctx_ = &ctx;
    if (!ctx.has_ray_query()) { *err = "device has no ray query"; return false; }
    VkDevice d = ctx.device();
#define LOAD(var, name) var = (PFN_##name)vkGetDeviceProcAddr(d, #name); if (!var) { *err = "missing " #name; return false; }
    LOAD(p_create, vkCreateAccelerationStructureKHR) LOAD(p_destroy, vkDestroyAccelerationStructureKHR)
    LOAD(p_sizes, vkGetAccelerationStructureBuildSizesKHR) LOAD(p_build, vkCmdBuildAccelerationStructuresKHR)
    LOAD(p_addr, vkGetAccelerationStructureDeviceAddressKHR)
#undef LOAD
    std::vector<uint32_t> spv;
    if (!compile_cached(Stage::Compute, kShader, &spv, err)) {
        // The in-game shadows run in the fragment shader (vkgl_core.cpp) and need only the acceleration structures built here; this compute
        // pass is the stage-1 test path (tests/test_vk_rt.cpp). A build without glslang (Android) and without that shader cached skips it.
        if (err->find("PSPVK_NO_GLSLANG") != std::string::npos || err->find("no precompiled SPIR-V") != std::string::npos) { err->clear(); return true; }
        *err = "shader: " + *err;
        return false;
    }
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = spv.size() * 4; si.pCode = spv.data();
    VkShaderModule sm; if (vkCreateShaderModule(d, &si, nullptr, &sm) != VK_SUCCESS) { *err = "vkCreateShaderModule"; return false; }
    VkDescriptorSetLayoutBinding b[3] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2] = b[1]; b[2].binding = 2;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 3; li.pBindings = b;
    vkCreateDescriptorSetLayout(d, &li, nullptr, &dsl_);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl_; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(d, &pli, nullptr, &pl_);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    ci.layout = pl_;
    VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe_);
    vkDestroyShaderModule(d, sm, nullptr);
    if (r != VK_SUCCESS) { *err = "vkCreateComputePipelines"; return false; }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    vkCreateDescriptorPool(d, &dpi, nullptr, &pool_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = pool_; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl_;
    vkAllocateDescriptorSets(d, &dai, &set_);
    return true;
}

void RtShadow::destroy() {
    if (!ctx_) return;
    VkDevice d = ctx_->device();
    vkDeviceWaitIdle(d);
    free_scene();
    free_buffer(&pts_); free_buffer(&out_);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipe_) vkDestroyPipeline(d, pipe_, nullptr);
    if (pl_) vkDestroyPipelineLayout(d, pl_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(d, dsl_, nullptr);
    pool_ = VK_NULL_HANDLE; pipe_ = VK_NULL_HANDLE; pl_ = VK_NULL_HANDLE; dsl_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
}

bool RtShadow::build_scene(const float* xyz, uint32_t tri_count, std::string* err) {
    std::string e; if (!err) err = &e;
    if (!ctx_ || !tri_count) { *err = "not initialised / empty scene"; return false; }
    const double t0 = now_ms();
    VkDevice d = ctx_->device();
    vkDeviceWaitIdle(d);
    free_scene();
    const VkDeviceSize vbytes = (VkDeviceSize)tri_count * 9 * sizeof(float);
    if (!make_buffer(&verts_, vbytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, err)) return false;
    std::memcpy(verts_.map, xyz, vbytes);

    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    g.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    g.geometry.triangles.vertexData.deviceAddress = verts_.addr;
    g.geometry.triangles.vertexStride = 12;
    g.geometry.triangles.maxVertex = tri_count * 3 - 1;
    g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bg.geometryCount = 1; bg.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR bsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bg, &tri_count, &bsz);

    // TLAS: one identity instance of the BLAS.
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; tg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tg.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tb{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
    const uint32_t one = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &one, &tsz);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(ctx_->physical(), &p2);
    const VkDeviceSize al = asp.minAccelerationStructureScratchOffsetAlignment ? asp.minAccelerationStructureScratchOffsetAlignment : 128;
    const VkDeviceSize scratch_sz = (bsz.buildScratchSize > tsz.buildScratchSize ? bsz.buildScratchSize : tsz.buildScratchSize) + al;

    const VkBufferUsageFlags asu = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    if (!make_buffer(&blas_buf_, bsz.accelerationStructureSize, asu, false, err) || !make_buffer(&tlas_buf_, tsz.accelerationStructureSize, asu, false, err)
        || !make_buffer(&scratch_, scratch_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, err)
        || !make_buffer(&inst_buf_, sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true, err)) return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = blas_buf_.buf; ci.size = bsz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (p_create(d, &ci, nullptr, &blas_) != VK_SUCCESS) { *err = "create BLAS"; return false; }
    ci.buffer = tlas_buf_.buf; ci.size = tsz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (p_create(d, &ci, nullptr, &tlas_) != VK_SUCCESS) { *err = "create TLAS"; return false; }

    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; dai.accelerationStructure = blas_;
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF; inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = p_addr(d, &dai);
    std::memcpy(inst_buf_.map, &inst, sizeof inst);

    const VkDeviceSize sa = (scratch_.addr + al - 1) & ~(al - 1);
    bg.dstAccelerationStructure = blas_; bg.scratchData.deviceAddress = sa;
    tg.geometry.instances.data.deviceAddress = inst_buf_.addr;
    tb.dstAccelerationStructure = tlas_; tb.scratchData.deviceAddress = sa;
    VkAccelerationStructureBuildRangeInfoKHR br{tri_count, 0, 0, 0}, tr{1, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* brp = &br; const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    VkCommandBuffer cb = ctx_->begin_once();
    p_build(cb, 1, &bg, &brp);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the TLAS build reads the BLAS and shares the scratch
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR; mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);
    p_build(cb, 1, &tb, &trp);
    if (!ctx_->end_once(cb)) { *err = "AS build submit"; return false; }
    free_buffer(&scratch_);   // builds are done (end_once waits)

    VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &tlas_;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.pNext = &wa; w.dstSet = set_; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
    tris_ = tri_count;
    build_ms_ = now_ms() - t0;
    return true;
}

bool RtShadow::trace_points(const float* pts, uint32_t n, const float dir[3], float max_dist, float bias, uint8_t* out, std::string* err) {
    std::string e; if (!err) err = &e;
    if (!ctx_ || !tris_) { *err = "no scene"; return false; }
    if (!pipe_) { *err = "the compute trace shader is not available in this build"; return false; }
    const double t0 = now_ms();
    VkDevice d = ctx_->device();
    if (pts_.size < (VkDeviceSize)n * 12) { free_buffer(&pts_); if (!make_buffer(&pts_, (VkDeviceSize)n * 12, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, err)) return false; }
    if (out_.size < (VkDeviceSize)n * 4) { free_buffer(&out_); if (!make_buffer(&out_, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, err)) return false; }
    std::memcpy(pts_.map, pts, (size_t)n * 12);
    VkDescriptorBufferInfo bi[2] = {{pts_.buf, 0, VK_WHOLE_SIZE}, {out_.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set_; w[i].dstBinding = 1 + i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i]; }
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    struct { float dir[3], maxd, bias; uint32_t n, pad[2]; } pc = {{dir[0], dir[1], dir[2]}, max_dist, bias, n, {0, 0}};
    VkCommandBuffer cb = ctx_->begin_once();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cb, pl_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if (!ctx_->end_once(cb)) { *err = "trace submit"; return false; }
    const uint32_t* o = (const uint32_t*)out_.map;
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)o[i];
    trace_ms_ = now_ms() - t0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// RtAccel
// ---------------------------------------------------------------------------------------------------------------------

bool RtAccel::make_buffer(Buf* b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, std::string* err) {
    VkDevice d = ctx_->device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size; bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d, &bi, nullptr, &b->buf) != VK_SUCCESS) { *err = "vkCreateBuffer"; return false; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, b->buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(ctx_->physical(), &mp);
    const VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = (int)i; break; }
    if (type < 0 && !host)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) { type = (int)i; break; }
    if (type < 0) { *err = "no memory type"; return false; }
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mi.pNext = &fi; mi.allocationSize = mr.size; mi.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d, &mi, nullptr, &b->mem) != VK_SUCCESS) { *err = "vkAllocateMemory"; return false; }
    vkBindBufferMemory(d, b->buf, b->mem, 0);
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; ai.buffer = b->buf;
    b->addr = vkGetBufferDeviceAddress(d, &ai);
    b->size = size;
    if (host && vkMapMemory(d, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) { *err = "vkMapMemory"; return false; }
    return true;
}

void RtAccel::free_buffer(Buf* b) {
    VkDevice d = ctx_->device();
    if (b->map) vkUnmapMemory(d, b->mem);
    if (b->buf) vkDestroyBuffer(d, b->buf, nullptr);
    if (b->mem) vkFreeMemory(d, b->mem, nullptr);
    *b = Buf{};
}

void RtAccel::free_all() {
    if (!ctx_) return;
    if (tlas_) { p_destroy(ctx_->device(), tlas_, nullptr); tlas_ = VK_NULL_HANDLE; }
    if (blas_) { p_destroy(ctx_->device(), blas_, nullptr); blas_ = VK_NULL_HANDLE; }
    if (blas2_) { p_destroy(ctx_->device(), blas2_, nullptr); blas2_ = VK_NULL_HANDLE; }
    free_buffer(&verts_); free_buffer(&blas_buf_); free_buffer(&tlas_buf_); free_buffer(&inst_buf_); free_buffer(&scratch_); free_buffer(&cols_); free_buffer(&verts2_); free_buffer(&blas2_buf_); free_buffer(&cols2_);
    cap_ = 0; cap2_ = 0; fx_tris_ = 0; tris_ = 0;
}

bool RtAccel::init(Context& ctx) {
    std::string e;
    if (!ctx.has_ray_query() || !load_procs(ctx.device(), &e)) return false;
    ctx_ = &ctx;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(ctx.physical(), &p2);
    scratch_align_ = asp.minAccelerationStructureScratchOffsetAlignment ? asp.minAccelerationStructureScratchOffsetAlignment : 128;
    return true;
}

void RtAccel::destroy() {
    if (!ctx_) return;
    vkDeviceWaitIdle(ctx_->device());
    free_all();
    ctx_ = nullptr;
}

bool RtAccel::allocate(uint32_t cap, uint32_t cap2, std::string* err) {
    VkDevice d = ctx_->device();
    if (std::getenv("PSPRECOMP_RT_DEBUG")) std::fprintf(stderr, "[RT] allocate: create=%p sizes=%p build=%p addr=%p destroy=%p dev=%p\n", (void*)p_create, (void*)p_sizes, (void*)p_build, (void*)p_addr, (void*)p_destroy, (void*)d);
    vkDeviceWaitIdle(d);   // the previous structures may still be read by frames in flight
    free_all();
    auto blas_geom = [](uint32_t c) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; g.geometry.triangles.vertexStride = 12;
        g.geometry.triangles.maxVertex = c * 3 - 1; g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        return g;
    };
    auto blas_size = [&](uint32_t c, VkAccelerationStructureBuildSizesInfoKHR* out) {
        VkAccelerationStructureGeometryKHR g = blas_geom(c);
        VkAccelerationStructureBuildGeometryInfoKHR bg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;   // rebuilt every frame
        bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bg.geometryCount = 1; bg.pGeometries = &g;
        *out = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        p_sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bg, &c, out);
    };
    VkAccelerationStructureBuildSizesInfoKHR bsz, bsz2;
    blas_size(cap, &bsz); blas_size(cap2, &bsz2);
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; tg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tg.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tb{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
    const uint32_t two = 2;
    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &two, &tsz);
    VkDeviceSize scratch_sz = std::max(std::max(bsz.buildScratchSize, bsz2.buildScratchSize), tsz.buildScratchSize) + scratch_align_;
    const VkBufferUsageFlags asu = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    const VkBufferUsageFlags bi = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if (!make_buffer(&verts_, (VkDeviceSize)cap * 36, bi, true, err) || !make_buffer(&verts2_, (VkDeviceSize)cap2 * 36, bi, true, err)
        || !make_buffer(&blas_buf_, bsz.accelerationStructureSize, asu, false, err) || !make_buffer(&blas2_buf_, bsz2.accelerationStructureSize, asu, false, err)
        || !make_buffer(&tlas_buf_, tsz.accelerationStructureSize, asu, false, err)
        || !make_buffer(&scratch_, scratch_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, err)
        || !make_buffer(&cols_, (VkDeviceSize)cap * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, err)
        || !make_buffer(&cols2_, (VkDeviceSize)cap2 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, err)
        || !make_buffer(&inst_buf_, 2 * sizeof(VkAccelerationStructureInstanceKHR), bi, true, err)) { free_all(); return false; }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = blas_buf_.buf; ci.size = bsz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (p_create(d, &ci, nullptr, &blas_) != VK_SUCCESS) { *err = "create BLAS"; free_all(); return false; }
    ci.buffer = blas2_buf_.buf; ci.size = bsz2.accelerationStructureSize;
    if (p_create(d, &ci, nullptr, &blas2_) != VK_SUCCESS) { *err = "create effects BLAS"; free_all(); return false; }
    ci.buffer = tlas_buf_.buf; ci.size = tsz.accelerationStructureSize; ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (p_create(d, &ci, nullptr, &tlas_) != VK_SUCCESS) { *err = "create TLAS"; free_all(); return false; }
    cap_ = cap; cap2_ = cap2; gen_++;
    if (std::getenv("PSPRECOMP_RT_DEBUG")) std::fprintf(stderr, "[RT] allocated cap %u + effects %u\n", cap, cap2);
    return true;
}

bool RtAccel::record_build(VkCommandBuffer cb, const float* xyz, uint32_t n, const uint32_t* tri_cols, const float* fx_xyz, uint32_t fx_n, const uint32_t* fx_cols, std::string* err) {
    std::string e; if (!err) err = &e;
    if (!ctx_ || !n) return false;
    if (std::getenv("PSPRECOMP_RT_DEBUG")) std::fprintf(stderr, "[RT] record_build n=%u cap=%u fx=%u\n", n, cap_, fx_n);
    static const uint32_t fx_min = std::getenv("PSPRECOMP_RT_FX_CAP") ? (uint32_t)std::atoi(std::getenv("PSPRECOMP_RT_FX_CAP")) : 8192u;   // test knob: a small start forces re-allocations when an effect spawns
    if ((n > cap_ || fx_n > cap2_) && !allocate(n > cap_ ? std::max<uint32_t>(n + n / 2, 30000u) : cap_, fx_n > cap2_ ? std::max<uint32_t>(fx_n + fx_n / 2, fx_min) : (cap2_ ? cap2_ : fx_min), err)) return false;
    std::memcpy(verts_.map, xyz, (size_t)n * 36);
    if (tri_cols) std::memcpy(cols_.map, tri_cols, (size_t)n * 4);
    else for (uint32_t i = 0; i < n; i++) static_cast<uint32_t*>(cols_.map)[i] = 0xFF808080u;
    if (fx_n) { std::memcpy(verts2_.map, fx_xyz, (size_t)fx_n * 36); if (fx_cols) std::memcpy(cols2_.map, fx_cols, (size_t)fx_n * 4); }
    const VkDeviceSize sa = (scratch_.addr + scratch_align_ - 1) & ~(scratch_align_ - 1);
    auto build_blas = [&](VkAccelerationStructureKHR dst, const Buf& vb, uint32_t cnt) {
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR; g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; g.geometry.triangles.vertexData.deviceAddress = vb.addr;
        g.geometry.triangles.vertexStride = 12; g.geometry.triangles.maxVertex = cnt * 3 - 1; g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR bg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bg.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR; bg.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        bg.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; bg.geometryCount = 1; bg.pGeometries = &g;
        bg.dstAccelerationStructure = dst; bg.scratchData.deviceAddress = sa;
        VkAccelerationStructureBuildRangeInfoKHR br{cnt, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* brp = &br;
        p_build(cb, 1, &bg, &brp);
    };
    auto as_barrier = [&](VkAccessFlags dst, VkPipelineStageFlags dst_stage) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR; mb.dstAccessMask = dst;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, dst_stage, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    const VkAccessFlags asrw = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    build_blas(blas_, verts_, n);
    as_barrier(asrw, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);   // the scratch buffer is reused
    if (fx_n) { build_blas(blas2_, verts2_, fx_n); as_barrier(asrw, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR); }
    // two instances: mask 1 = the opaque scene, mask 2 = the effects layer (reference 0 = inactive when there are none)
    VkAccelerationStructureInstanceKHR inst[2] = {};
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    for (int k = 0; k < 2; k++) {
        inst[k].transform.matrix[0][0] = inst[k].transform.matrix[1][1] = inst[k].transform.matrix[2][2] = 1.0f;
        inst[k].mask = k ? 0x2 : 0x1; inst[k].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        dai.accelerationStructure = k ? blas2_ : blas_;
        inst[k].accelerationStructureReference = (k && !fx_n) ? 0 : p_addr(ctx_->device(), &dai);
    }
    std::memcpy(inst_buf_.map, inst, sizeof inst);
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; tg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tg.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    tg.geometry.instances.data.deviceAddress = inst_buf_.addr;
    VkAccelerationStructureBuildGeometryInfoKHR tb{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; tb.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tb.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; tb.geometryCount = 1; tb.pGeometries = &tg;
    tb.dstAccelerationStructure = tlas_; tb.scratchData.deviceAddress = sa;
    VkAccelerationStructureBuildRangeInfoKHR tr{2, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* trp = &tr;
    p_build(cb, 1, &tb, &trp);
    as_barrier(VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    tris_ = n; fx_tris_ = fx_n;
    return true;
}

}  // namespace pspvk
