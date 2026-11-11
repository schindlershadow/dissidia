// Function-pointer table for the GLES entry points the renderer uses (Android builds with the Vulkan backend).
// On desktop glad already gives every GL function a replaceable pointer; on Android the GLES functions are real exported symbols,
// so with PSPRECOMP_HAVE_VKGL the renderer's gl* names are macros over this table (psp_gl.h). psp_gl_table_use_gles() points it at
// libGLESv3; the Vulkan layer (src/vk/vkgl_api.cpp) overwrites it. Generated list: keep in step with the install() list there.
#pragma once
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#define PSP_GL_FNS(X) \
    X(glEnable) \
    X(glDisable) \
    X(glIsEnabled) \
    X(glViewport) \
    X(glScissor) \
    X(glDepthMask) \
    X(glDepthFunc) \
    X(glColorMask) \
    X(glBlendFunc) \
    X(glBlendFuncSeparate) \
    X(glBlendEquation) \
    X(glBlendEquationSeparate) \
    X(glBlendColor) \
    X(glStencilMask) \
    X(glStencilFunc) \
    X(glStencilOp) \
    X(glCullFace) \
    X(glClearColor) \
    X(glClearStencil) \
    X(glLineWidth) \
    X(glPixelStorei) \
    X(glGetError) \
    X(glGetString) \
    X(glGetStringi) \
    X(glGetIntegerv) \
    X(glGetBooleanv) \
    X(glGetFloatv) \
    X(glGenBuffers) \
    X(glDeleteBuffers) \
    X(glBindBuffer) \
    X(glBindBufferBase) \
    X(glBufferData) \
    X(glBufferSubData) \
    X(glMapBufferRange) \
    X(glUnmapBuffer) \
    X(glFlushMappedBufferRange) \
    X(glGenVertexArrays) \
    X(glDeleteVertexArrays) \
    X(glBindVertexArray) \
    X(glVertexAttribPointer) \
    X(glEnableVertexAttribArray) \
    X(glDisableVertexAttribArray) \
    X(glGenTextures) \
    X(glDeleteTextures) \
    X(glActiveTexture) \
    X(glBindTexture) \
    X(glTexParameteri) \
    X(glTexParameterf) \
    X(glGenSamplers) \
    X(glDeleteSamplers) \
    X(glBindSampler) \
    X(glSamplerParameteri) \
    X(glSamplerParameterf) \
    X(glTexImage2D) \
    X(glTexSubImage2D) \
    X(glGenerateMipmap) \
    X(glGenFramebuffers) \
    X(glDeleteFramebuffers) \
    X(glBindFramebuffer) \
    X(glFramebufferTexture2D) \
    X(glFramebufferRenderbuffer) \
    X(glCheckFramebufferStatus) \
    X(glGenRenderbuffers) \
    X(glDeleteRenderbuffers) \
    X(glBindRenderbuffer) \
    X(glRenderbufferStorage) \
    X(glBlitFramebuffer) \
    X(glReadBuffer) \
    X(glDrawBuffers) \
    X(glReadPixels) \
    X(glClear) \
    X(glFinish) \
    X(glFlush) \
    X(glCreateShader) \
    X(glShaderSource) \
    X(glCompileShader) \
    X(glGetShaderiv) \
    X(glGetShaderInfoLog) \
    X(glDeleteShader) \
    X(glCreateProgram) \
    X(glAttachShader) \
    X(glBindAttribLocation) \
    X(glTransformFeedbackVaryings) \
    X(glLinkProgram) \
    X(glGetProgramiv) \
    X(glGetProgramInfoLog) \
    X(glDeleteProgram) \
    X(glUseProgram) \
    X(glGetUniformLocation) \
    X(glUniform1i) \
    X(glUniform1f) \
    X(glUniform2f) \
    X(glUniform3f) \
    X(glUniform4f) \
    X(glUniform3i) \
    X(glUniform4fv) \
    X(glUniformMatrix4fv) \
    X(glDrawArrays) \
    X(glGenQueries) \
    X(glDeleteQueries) \
    X(glBeginQuery) \
    X(glEndQuery) \
    X(glGetQueryObjectuiv) \
    X(glBeginTransformFeedback) \
    X(glEndTransformFeedback) \
    X(glDepthRangef) \
    X(glClearDepthf)

struct PspGlTable {
#define PSP_GL_X(n) decltype(&::n) n;
    PSP_GL_FNS(PSP_GL_X)
#undef PSP_GL_X
};
extern PspGlTable g_psp_gl;
void psp_gl_table_use_gles();   // psp_gl_table.cpp

#ifdef PSP_GL_TABLE_MACROS
#define glEnable g_psp_gl.glEnable
#define glDisable g_psp_gl.glDisable
#define glIsEnabled g_psp_gl.glIsEnabled
#define glViewport g_psp_gl.glViewport
#define glScissor g_psp_gl.glScissor
#define glDepthMask g_psp_gl.glDepthMask
#define glDepthFunc g_psp_gl.glDepthFunc
#define glColorMask g_psp_gl.glColorMask
#define glBlendFunc g_psp_gl.glBlendFunc
#define glBlendFuncSeparate g_psp_gl.glBlendFuncSeparate
#define glBlendEquation g_psp_gl.glBlendEquation
#define glBlendEquationSeparate g_psp_gl.glBlendEquationSeparate
#define glBlendColor g_psp_gl.glBlendColor
#define glStencilMask g_psp_gl.glStencilMask
#define glStencilFunc g_psp_gl.glStencilFunc
#define glStencilOp g_psp_gl.glStencilOp
#define glCullFace g_psp_gl.glCullFace
#define glClearColor g_psp_gl.glClearColor
#define glClearStencil g_psp_gl.glClearStencil
#define glLineWidth g_psp_gl.glLineWidth
#define glPixelStorei g_psp_gl.glPixelStorei
#define glGetError g_psp_gl.glGetError
#define glGetString g_psp_gl.glGetString
#define glGetStringi g_psp_gl.glGetStringi
#define glGetIntegerv g_psp_gl.glGetIntegerv
#define glGetBooleanv g_psp_gl.glGetBooleanv
#define glGetFloatv g_psp_gl.glGetFloatv
#define glGenBuffers g_psp_gl.glGenBuffers
#define glDeleteBuffers g_psp_gl.glDeleteBuffers
#define glBindBuffer g_psp_gl.glBindBuffer
#define glBindBufferBase g_psp_gl.glBindBufferBase
#define glBufferData g_psp_gl.glBufferData
#define glBufferSubData g_psp_gl.glBufferSubData
#define glMapBufferRange g_psp_gl.glMapBufferRange
#define glUnmapBuffer g_psp_gl.glUnmapBuffer
#define glFlushMappedBufferRange g_psp_gl.glFlushMappedBufferRange
#define glGenVertexArrays g_psp_gl.glGenVertexArrays
#define glDeleteVertexArrays g_psp_gl.glDeleteVertexArrays
#define glBindVertexArray g_psp_gl.glBindVertexArray
#define glVertexAttribPointer g_psp_gl.glVertexAttribPointer
#define glEnableVertexAttribArray g_psp_gl.glEnableVertexAttribArray
#define glDisableVertexAttribArray g_psp_gl.glDisableVertexAttribArray
#define glGenTextures g_psp_gl.glGenTextures
#define glDeleteTextures g_psp_gl.glDeleteTextures
#define glActiveTexture g_psp_gl.glActiveTexture
#define glBindTexture g_psp_gl.glBindTexture
#define glTexParameteri g_psp_gl.glTexParameteri
#define glTexParameterf g_psp_gl.glTexParameterf
#define glGenSamplers g_psp_gl.glGenSamplers
#define glDeleteSamplers g_psp_gl.glDeleteSamplers
#define glBindSampler g_psp_gl.glBindSampler
#define glSamplerParameteri g_psp_gl.glSamplerParameteri
#define glSamplerParameterf g_psp_gl.glSamplerParameterf
#define glTexImage2D g_psp_gl.glTexImage2D
#define glTexSubImage2D g_psp_gl.glTexSubImage2D
#define glGenerateMipmap g_psp_gl.glGenerateMipmap
#define glGenFramebuffers g_psp_gl.glGenFramebuffers
#define glDeleteFramebuffers g_psp_gl.glDeleteFramebuffers
#define glBindFramebuffer g_psp_gl.glBindFramebuffer
#define glFramebufferTexture2D g_psp_gl.glFramebufferTexture2D
#define glFramebufferRenderbuffer g_psp_gl.glFramebufferRenderbuffer
#define glCheckFramebufferStatus g_psp_gl.glCheckFramebufferStatus
#define glGenRenderbuffers g_psp_gl.glGenRenderbuffers
#define glDeleteRenderbuffers g_psp_gl.glDeleteRenderbuffers
#define glBindRenderbuffer g_psp_gl.glBindRenderbuffer
#define glRenderbufferStorage g_psp_gl.glRenderbufferStorage
#define glBlitFramebuffer g_psp_gl.glBlitFramebuffer
#define glReadBuffer g_psp_gl.glReadBuffer
#define glDrawBuffers g_psp_gl.glDrawBuffers
#define glReadPixels g_psp_gl.glReadPixels
#define glClear g_psp_gl.glClear
#define glFinish g_psp_gl.glFinish
#define glFlush g_psp_gl.glFlush
#define glCreateShader g_psp_gl.glCreateShader
#define glShaderSource g_psp_gl.glShaderSource
#define glCompileShader g_psp_gl.glCompileShader
#define glGetShaderiv g_psp_gl.glGetShaderiv
#define glGetShaderInfoLog g_psp_gl.glGetShaderInfoLog
#define glDeleteShader g_psp_gl.glDeleteShader
#define glCreateProgram g_psp_gl.glCreateProgram
#define glAttachShader g_psp_gl.glAttachShader
#define glBindAttribLocation g_psp_gl.glBindAttribLocation
#define glTransformFeedbackVaryings g_psp_gl.glTransformFeedbackVaryings
#define glLinkProgram g_psp_gl.glLinkProgram
#define glGetProgramiv g_psp_gl.glGetProgramiv
#define glGetProgramInfoLog g_psp_gl.glGetProgramInfoLog
#define glDeleteProgram g_psp_gl.glDeleteProgram
#define glUseProgram g_psp_gl.glUseProgram
#define glGetUniformLocation g_psp_gl.glGetUniformLocation
#define glUniform1i g_psp_gl.glUniform1i
#define glUniform1f g_psp_gl.glUniform1f
#define glUniform2f g_psp_gl.glUniform2f
#define glUniform3f g_psp_gl.glUniform3f
#define glUniform4f g_psp_gl.glUniform4f
#define glUniform3i g_psp_gl.glUniform3i
#define glUniform4fv g_psp_gl.glUniform4fv
#define glUniformMatrix4fv g_psp_gl.glUniformMatrix4fv
#define glDrawArrays g_psp_gl.glDrawArrays
#define glGenQueries g_psp_gl.glGenQueries
#define glDeleteQueries g_psp_gl.glDeleteQueries
#define glBeginQuery g_psp_gl.glBeginQuery
#define glEndQuery g_psp_gl.glEndQuery
#define glGetQueryObjectuiv g_psp_gl.glGetQueryObjectuiv
#define glBeginTransformFeedback g_psp_gl.glBeginTransformFeedback
#define glEndTransformFeedback g_psp_gl.glEndTransformFeedback
#define glDepthRangef g_psp_gl.glDepthRangef
#define glClearDepthf g_psp_gl.glClearDepthf
#endif

#ifdef PSP_GL_TABLE_UNDEF   // the Vulkan layer implements these names itself: drop the call macros
#undef glEnable
#undef glDisable
#undef glIsEnabled
#undef glViewport
#undef glScissor
#undef glDepthMask
#undef glDepthFunc
#undef glColorMask
#undef glBlendFunc
#undef glBlendFuncSeparate
#undef glBlendEquation
#undef glBlendEquationSeparate
#undef glBlendColor
#undef glStencilMask
#undef glStencilFunc
#undef glStencilOp
#undef glCullFace
#undef glClearColor
#undef glClearStencil
#undef glLineWidth
#undef glPixelStorei
#undef glGetError
#undef glGetString
#undef glGetStringi
#undef glGetIntegerv
#undef glGetBooleanv
#undef glGetFloatv
#undef glGenBuffers
#undef glDeleteBuffers
#undef glBindBuffer
#undef glBindBufferBase
#undef glBufferData
#undef glBufferSubData
#undef glMapBufferRange
#undef glUnmapBuffer
#undef glFlushMappedBufferRange
#undef glGenVertexArrays
#undef glDeleteVertexArrays
#undef glBindVertexArray
#undef glVertexAttribPointer
#undef glEnableVertexAttribArray
#undef glDisableVertexAttribArray
#undef glGenTextures
#undef glDeleteTextures
#undef glActiveTexture
#undef glBindTexture
#undef glTexParameteri
#undef glTexParameterf
#undef glGenSamplers
#undef glDeleteSamplers
#undef glBindSampler
#undef glSamplerParameteri
#undef glSamplerParameterf
#undef glTexImage2D
#undef glTexSubImage2D
#undef glGenerateMipmap
#undef glGenFramebuffers
#undef glDeleteFramebuffers
#undef glBindFramebuffer
#undef glFramebufferTexture2D
#undef glFramebufferRenderbuffer
#undef glCheckFramebufferStatus
#undef glGenRenderbuffers
#undef glDeleteRenderbuffers
#undef glBindRenderbuffer
#undef glRenderbufferStorage
#undef glBlitFramebuffer
#undef glReadBuffer
#undef glDrawBuffers
#undef glReadPixels
#undef glClear
#undef glFinish
#undef glFlush
#undef glCreateShader
#undef glShaderSource
#undef glCompileShader
#undef glGetShaderiv
#undef glGetShaderInfoLog
#undef glDeleteShader
#undef glCreateProgram
#undef glAttachShader
#undef glBindAttribLocation
#undef glTransformFeedbackVaryings
#undef glLinkProgram
#undef glGetProgramiv
#undef glGetProgramInfoLog
#undef glDeleteProgram
#undef glUseProgram
#undef glGetUniformLocation
#undef glUniform1i
#undef glUniform1f
#undef glUniform2f
#undef glUniform3f
#undef glUniform4f
#undef glUniform3i
#undef glUniform4fv
#undef glUniformMatrix4fv
#undef glDrawArrays
#undef glGenQueries
#undef glDeleteQueries
#undef glBeginQuery
#undef glEndQuery
#undef glGetQueryObjectuiv
#undef glBeginTransformFeedback
#undef glEndTransformFeedback
#undef glDepthRangef
#undef glClearDepthf
#endif
