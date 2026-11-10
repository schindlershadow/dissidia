// OpenGL entry point for the runtime: desktop OpenGL 3.3 core through glad, Android through OpenGL ES 3.x (libGLESv3).
// The renderer is written against the desktop API; on Android this header maps the few differences (see below).
#pragma once

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

// Names that differ in ES.
#define glDepthRange(n, f) glDepthRangef((GLfloat)(n), (GLfloat)(f))
#define glClearDepth(d) glClearDepthf((GLfloat)(d))
// Desktop-only enums the renderer mentions; ES has no equivalent, so they are inert (the code paths that use them are
// desktop-only or are guarded by PSP_GLES).
#define PSP_GLES 1
#define PSP_GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
// Occlusion-query diagnostics (PSPRECOMP_3D_DIAG*) only need "anything drawn?": the boolean query is the ES equivalent.
#define GL_SAMPLES_PASSED GL_ANY_SAMPLES_PASSED
// glGetBufferSubData (the GE verify harness) through a mapping; glMultiDrawArrays (ES has only the EXT extension) as a loop.
#include <cstring>
inline void glGetBufferSubData(GLenum target, GLintptr off, GLsizeiptr n, void* dst) {
    if (void* p = glMapBufferRange(target, off, n, GL_MAP_READ_BIT)) { std::memcpy(dst, p, (size_t)n); glUnmapBuffer(target); }
}
inline void glMultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n) {
    for (GLsizei i = 0; i < n; i++) glDrawArrays(mode, first[i], count[i]);
}
// ES has no glPointSize: the vertex shader writes gl_PointSize from u_psize, which ge_shader uploads from this value.
extern float g_psp_gles_point_size;
#define glPointSize(x) (g_psp_gles_point_size = (float)(x))
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#else
#include <glad/glad.h>
#define PSP_GLSL_VERSION "#version 330 core\n"
#endif
