// GL-on-Vulkan layer (docs/VULKAN_PORT.md). The runtime is written against OpenGL 3.3 core through glad's function
// pointers; with PSPRECOMP_RENDERER=vulkan those pointers are replaced by the implementations in vkgl_api.cpp, which
// translate the GL state machine into Vulkan (pipeline cache, render passes, descriptor sets, upload rings).
// Convention: Vulkan images are stored in GL orientation (row 0 = bottom of the picture), so viewports, blits, read-backs,
// texture coordinates and gl_FragCoord behave exactly as in GL; only the window present flips. Culling front-face is
// inverted accordingly. GL clip depth [-w,w] is mapped to Vulkan [0,w] in the vertex shader (psp_vk_glsl).
#pragma once
#include <string>
#include <vector>

struct SDL_Window;

namespace vkgl {

/// PSPRECOMP_RENDERER=vulkan (or "vk") and Vulkan is usable here; asking for it on a machine without Vulkan falls back to OpenGL.
bool requested();
/// Whether a Vulkan 1.1 device can be used on this machine (probed once, needs SDL video initialised). *why says why not.
bool available(std::string* why);
/// Creates the Vulkan context for `window` (which must have been created with SDL_WINDOW_VULKAN) and installs the GL
/// entry points into glad's pointers. Returns false (with *err) if Vulkan cannot be used; nothing is installed then.
bool init(SDL_Window* window, bool vsync, std::string* err);
void shutdown();
/// SDL_GL_SwapWindow equivalent: submit the frame, flip-blit the default framebuffer to the swapchain, present.
void swap();
void set_swap_interval(bool vsync);
/// SDL_GL_GetDrawableSize equivalent. Also resizes the default framebuffer to the new size (so a resize is never seen
/// half-applied by the renderer).
void drawable_size(SDL_Window* window, int* w, int* h);
bool active();
/// What the Vulkan device is, for the debug overlay (the GL strings of this layer are placeholders).
struct DeviceInfo {
    std::string name;            // VkPhysicalDeviceProperties::deviceName
    std::string api;             // "Vulkan 1.2.xxx"
    std::string driver;          // decoded driverVersion
    unsigned vendor = 0;
    double vram_total_gb = -1;   // largest device-local heap
};
bool device_info(DeviceInfo* out);
/// GPU time, for the performance overlay. take_perf() returns the averages since its last call: gpu_ms = GPU time per frame (timestamp
/// queries around every submission), passes = the most expensive render passes / other GPU work, per frame, largest first.
/// set_profile(true) turns the per-pass recording on (a few timestamps per pass), off again when the overlay is closed.
struct PerfSample {
    double gpu_ms = -1;
    struct Pass { std::string label; double ms; };
    std::vector<Pass> passes;
};
void set_profile(bool on);
bool take_perf(PerfSample* out);
/// "Preparing graphics": compiles the shaders of the shader pack (assets/shaders.pack; PSPRECOMP_SHADER_PACK=<file>) that the SPIR-V cache
/// lacks, on worker threads, with a progress screen. Returns at once (and shows nothing) when the pack is absent or everything is cached.
/// Call after init(), before the game starts.
void warmup_shaders();

}  // namespace vkgl
