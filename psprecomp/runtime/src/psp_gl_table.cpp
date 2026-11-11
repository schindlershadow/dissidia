// Android + Vulkan backend: the GL function table starts out as the real GLES entry points.
#if defined(__ANDROID__) && defined(PSPRECOMP_HAVE_VKGL)
#include "psp_gl_table.h"
PspGlTable g_psp_gl;
void psp_gl_table_use_gles() {
#define PSP_GL_X(n) g_psp_gl.n = ::n;
    PSP_GL_FNS(PSP_GL_X)
#undef PSP_GL_X
}
#endif
