// Window-system side of the GL calls the runtime makes: swap and drawable size. With the Vulkan backend
// (PSPRECOMP_RENDERER=vulkan, src/vk/) the window has no GL context, so these go through the Vulkan layer.
#pragma once
#include <SDL.h>
#if defined(PSPRECOMP_HAVE_VKGL)
#include "../src/vk/vkgl.h"
#endif

inline void psp_gl_drawable_size(SDL_Window* w, int* dw, int* dh) {
#if defined(PSPRECOMP_HAVE_VKGL)
    if (vkgl::active()) { vkgl::drawable_size(w, dw, dh); return; }
#endif
    SDL_GL_GetDrawableSize(w, dw, dh);
}
inline void psp_gl_swap(SDL_Window* w) {
#if defined(PSPRECOMP_HAVE_VKGL)
    if (vkgl::active()) { vkgl::swap(); return; }
#endif
    SDL_GL_SwapWindow(w);
}
