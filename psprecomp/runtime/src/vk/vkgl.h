// GL-on-Vulkan layer (docs/VULKAN_PORT.md). The runtime is written against OpenGL 3.3 core through glad's function
// pointers; with PSPRECOMP_RENDERER=vulkan those pointers are replaced by the implementations in vkgl_api.cpp, which
// translate the GL state machine into Vulkan (pipeline cache, render passes, descriptor sets, upload rings).
// Convention: Vulkan images are stored in GL orientation (row 0 = bottom of the picture), so viewports, blits, read-backs,
// texture coordinates and gl_FragCoord behave exactly as in GL; only the window present flips. Culling front-face is
// inverted accordingly. GL clip depth [-w,w] is mapped to Vulkan [0,w] in the vertex shader (psp_vk_glsl).
#pragma once
#include <string>

struct SDL_Window;

namespace vkgl {

/// PSPRECOMP_RENDERER=vulkan (or "vk").
bool requested();
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

}  // namespace vkgl
