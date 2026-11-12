#pragma once
// Ray-queried shadows hook between the renderer-independent runtime and the Vulkan layer (docs/VULKAN_PORT.md).
// The Vulkan layer fills this in at start when PSPRECOMP_VK_RT=1 and the device has ray query; on GL it stays empty.
#include <cstdint>

struct PspRtHooks {
    /// True when a scene can be set (ray query device, layer active).
    bool (*available)() = nullptr;
    /// Build this frame's acceleration structure from world-space triangles (3 floats per vertex, 9 per triangle) into the
    /// layer's current command stream. The next draw of a program that declares `uniform accelerationStructureEXT` sees it.
    bool (*set_scene)(const float* xyz, uint32_t tri_count) = nullptr;
    /// Set by the Vulkan layer whenever it is active: why ray tracing is not available on this GPU (null / empty = available).
    const char* (*unavailable_reason)() = nullptr;
};
extern PspRtHooks g_psp_rt;
/// User switch (settings row "Ray-Traced Shadows", env PSPRECOMP_VK_RT=1 / saved `rt_shadows`). Takes effect at once: the layer always
/// creates the ray-query device when the GPU has it (desktop), this only decides whether scenes are captured and used.
bool psp_rt_enabled();
void psp_rt_set_enabled(bool on);
/// Text for the settings row when ray tracing cannot be used (never empty then).
const char* psp_rt_unavailable_reason();
/// Ray tracing is available and switched on: contact shadows are replaced by it.
inline bool psp_rt_active();
inline bool psp_rt_available() { return g_psp_rt.available && g_psp_rt.available(); }
inline bool psp_rt_active() { return psp_rt_available() && psp_rt_enabled(); }
