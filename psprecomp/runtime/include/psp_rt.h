#pragma once
// Ray-queried shadows hook between the renderer-independent runtime and the Vulkan layer (docs/VULKAN_PORT.md).
// The Vulkan layer fills this in at start when PSPRECOMP_VK_RT=1 and the device has ray query; on GL it stays empty.
#include <cstdint>

struct PspRtHooks {
    /// True when a scene can be set (ray query device, layer active).
    bool (*available)() = nullptr;
    /// Build this frame's acceleration structure from world-space triangles (3 floats per vertex, 9 per triangle) into the
    /// layer's current command stream. The next draw of a program that declares `uniform accelerationStructureEXT` sees it.
    /// `tri_cols` (optional, one RGBA8 per triangle, R in the low byte) is what a ray-traced reflection shows where it hits that
    /// triangle: the shader reads it as `rt_tri_col[gl_PrimitiveID]` (storage buffer, set 2 binding 1).
    /// The optional effects layer (fx_*: blended, depth-tested, non-depth-writing draws, alpha byte = coverage) is a second instance the
    /// reflection traces separately (`rt_fx_col[]`, binding 2) and blends over the opaque hit; it casts no shadows.
    bool (*set_scene)(const float* xyz, uint32_t tri_count, const uint32_t* tri_cols, const float* fx_xyz, uint32_t fx_tri_count, const uint32_t* fx_cols) = nullptr;
    /// Set by the Vulkan layer whenever it is active: why ray tracing is not available on this GPU (null / empty = available).
    const char* (*unavailable_reason)() = nullptr;
};
extern PspRtHooks g_psp_rt;
/// User switch (settings row "Ray-Traced Shadows", env PSPRECOMP_VK_RT=1 / saved `rt_shadows`). Takes effect at once: the layer always
/// creates the ray-query device when the GPU has it (desktop), this only decides whether scenes are captured and used.
bool psp_rt_enabled();
void psp_rt_set_enabled(bool on);   // on = Medium
int psp_rt_quality();               // 0 off, 1 low (1 hard ray), 2 medium (3 soft rays), 3 high (6 soft rays)
void psp_rt_set_quality(int q);
/// Text for the settings row when ray tracing cannot be used (never empty then).
const char* psp_rt_unavailable_reason();
/// Ray tracing is available and switched on: contact shadows are replaced by it.
inline bool psp_rt_active();
/// User switch "Ray-Traced Reflections" (env PSPRECOMP_VK_RT_REFL=1 / saved `rt_reflections`): the floor reflection is a ray query along the
/// reflected view ray instead of the screen-space march, so it also shows what is off screen or hidden. Replaces the Reflections row.
bool psp_rt_refl_enabled();
void psp_rt_refl_set_enabled(bool on);   // on = Medium
int psp_rt_refl_quality();               // 0 off, 1 low (opaque geometry only), 2 medium (+ effects layer), 3 high (+ 3 jittered rays, a glossy blur)
void psp_rt_refl_set_quality(int q);
inline bool psp_rt_available() { return g_psp_rt.available && g_psp_rt.available(); }
inline bool psp_rt_active() { return psp_rt_available() && psp_rt_enabled(); }
inline bool psp_rt_refl_active() { return psp_rt_available() && psp_rt_refl_enabled(); }
/// Either ray-traced effect is on: the draw path captures this frame's triangles.
void psp_rt_request_dump();   // F9: log every 3D draw of the next frame ([RTDUMP] lines on stderr)
inline bool psp_rt_scene_wanted() { return psp_rt_available() && (psp_rt_enabled() || psp_rt_refl_enabled()); }
