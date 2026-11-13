// The OpenGL entry points the runtime uses, implemented on top of the Vulkan core (vkgl_core.cpp) and installed into
// glad's function pointers. Anything the runtime does not call is left null on purpose: a call would crash loudly
// instead of silently doing nothing.
#include "vkgl.h"
#include "../../include/psp_rt.h"
#include "../../include/psp_hdr.h"
#define PSP_GL_TABLE_UNDEF
#include "vkgl_internal.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstdio>
#include <cstring>
#ifndef APIENTRY
#define APIENTRY KHRONOS_APIENTRY
#endif

namespace vkgl {

static bool g_active = false;
static int g_query_next = 1;

extern "C++" {

// ---- state ----
static void APIENTRY f_Enable(GLenum cap) {
    GLState& s = st();
    switch (cap) {
    case GL_BLEND: s.blend = true; break;
    case GL_DEPTH_TEST: s.depth_test = true; break;
    case GL_CULL_FACE: s.cull = true; break;
    case GL_STENCIL_TEST: s.stencil = true; break;
    case GL_SCISSOR_TEST: s.scissor = true; break;
    case GL_RASTERIZER_DISCARD: s.discard = true; break;
    default: break;
    }
}
static void APIENTRY f_Disable(GLenum cap) {
    GLState& s = st();
    switch (cap) {
    case GL_BLEND: s.blend = false; break;
    case GL_DEPTH_TEST: s.depth_test = false; break;
    case GL_CULL_FACE: s.cull = false; break;
    case GL_STENCIL_TEST: s.stencil = false; break;
    case GL_SCISSOR_TEST: s.scissor = false; break;
    case GL_RASTERIZER_DISCARD: s.discard = false; break;
    default: break;
    }
}
static GLboolean APIENTRY f_IsEnabled(GLenum cap) {
    GLState& s = st();
    switch (cap) {
    case GL_BLEND: return s.blend;
    case GL_DEPTH_TEST: return s.depth_test;
    case GL_CULL_FACE: return s.cull;
    case GL_STENCIL_TEST: return s.stencil;
    case GL_SCISSOR_TEST: return s.scissor;
    case GL_RASTERIZER_DISCARD: return s.discard;
    default: return GL_FALSE;
    }
}
static void APIENTRY f_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { GLState& s = st(); s.vp[0] = x; s.vp[1] = y; s.vp[2] = w; s.vp[3] = h; }
static void APIENTRY f_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { int* r = st().sc_rect; r[0] = x; r[1] = y; r[2] = w; r[3] = h; }   // draws ignore it (the runtime disables the test); glClear honours it
#ifndef PSP_GLES
static void APIENTRY f_DepthRange(GLdouble n, GLdouble f) { st().depth_range[0] = n; st().depth_range[1] = f; }
#endif
static void APIENTRY f_DepthRangef(GLfloat n, GLfloat f) { st().depth_range[0] = n; st().depth_range[1] = f; }
static void APIENTRY f_DepthMask(GLboolean m) { st().depth_mask = m != GL_FALSE; }
static void APIENTRY f_DepthFunc(GLenum f) { st().depth_func = f; }
static void APIENTRY f_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { st().color_mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0); }
static void APIENTRY f_BlendFunc(GLenum s, GLenum d) { BlendState& b = st().bl; b.src_rgb = b.src_a = s; b.dst_rgb = b.dst_a = d; }
static void APIENTRY f_BlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) { BlendState& b = st().bl; b.src_rgb = sr; b.dst_rgb = dr; b.src_a = sa; b.dst_a = da; }
static void APIENTRY f_BlendEquation(GLenum e) { st().bl.eq_rgb = st().bl.eq_a = e; }
static void APIENTRY f_BlendEquationSeparate(GLenum r, GLenum a) { st().bl.eq_rgb = r; st().bl.eq_a = a; }
static void APIENTRY f_BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { float* c = st().blend_color; c[0] = r; c[1] = g; c[2] = b; c[3] = a; }
static void APIENTRY f_StencilMask(GLuint m) { st().stencil_wmask = m; }
static void APIENTRY f_StencilFunc(GLenum f, GLint ref, GLuint mask) { st().stencil_func = f; st().stencil_ref = ref; st().stencil_cmask = mask; }
static void APIENTRY f_StencilOp(GLenum sf, GLenum zf, GLenum zp) { st().sfail = sf; st().zfail = zf; st().zpass = zp; }
static void APIENTRY f_CullFace(GLenum m) { st().cull_mode = m; }
static void APIENTRY f_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { float* c = st().clear_color; c[0] = r; c[1] = g; c[2] = b; c[3] = a; }
#ifndef PSP_GLES
static void APIENTRY f_ClearDepth(GLdouble d) { st().clear_depth = d; }
#endif
static void APIENTRY f_ClearDepthf(GLfloat d) { st().clear_depth = d; }
static void APIENTRY f_ClearStencil(GLint s) { st().clear_stencil = s; }
#ifndef PSP_GLES
static void APIENTRY f_PointSize(GLfloat s) { st().point_size = s; st().point_size_set = true; }
#endif
static void APIENTRY f_LineWidth(GLfloat) {}
static void APIENTRY f_PixelStorei(GLenum p, GLint v) {
    if (p == GL_UNPACK_ROW_LENGTH) st().unpack_row = v;
    else if (p == GL_UNPACK_ALIGNMENT) st().unpack_align = v < 1 ? 1 : v;
}
static GLenum APIENTRY f_GetError() { return GL_NO_ERROR; }
static const GLubyte* APIENTRY f_GetString(GLenum n) {
    static std::string renderer;
    switch (n) {
    case GL_VENDOR: return (const GLubyte*)"vkgl (GL on Vulkan)";
    case GL_RENDERER: renderer = "Vulkan: " + std::string(G ? "device" : ""); return (const GLubyte*)renderer.c_str();
    case GL_VERSION: return (const GLubyte*)"3.3.0 vkgl";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"3.30 vkgl";
    default: return (const GLubyte*)"";
    }
}
static const GLubyte* APIENTRY f_GetStringi(GLenum, GLuint) { return (const GLubyte*)""; }
static void APIENTRY f_GetIntegerv(GLenum p, GLint* v) {
    GLState& s = st();
    switch (p) {
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = (GLint)s.draw_fbo; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)s.read_fbo; break;
    case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = s.vp[i]; break;
    case GL_MAX_TEXTURE_SIZE: *v = 8192; break;
    case GL_NUM_EXTENSIONS: *v = 0; break;
    case GL_UNPACK_ROW_LENGTH: *v = s.unpack_row; break;
    case GL_UNPACK_ALIGNMENT: *v = s.unpack_align; break;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)s.unit_tex[s.active_unit]; break;
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)s.array_buffer; break;
    case GL_VERTEX_ARRAY_BINDING: *v = (GLint)s.vao; break;
    case GL_CURRENT_PROGRAM: *v = (GLint)s.program; break;
    case GL_BLEND_SRC_RGB: *v = (GLint)s.bl.src_rgb; break;
    case GL_BLEND_DST_RGB: *v = (GLint)s.bl.dst_rgb; break;
    case GL_BLEND_SRC_ALPHA: *v = (GLint)s.bl.src_a; break;
    case GL_BLEND_DST_ALPHA: *v = (GLint)s.bl.dst_a; break;
    default: *v = 0; break;
    }
}
static void APIENTRY f_GetBooleanv(GLenum p, GLboolean* v) {
    if (p == GL_COLOR_WRITEMASK) for (int i = 0; i < 4; i++) v[i] = (st().color_mask >> i) & 1;
    else *v = GL_FALSE;
}
static void APIENTRY f_GetFloatv(GLenum p, GLfloat* v) {
    if (p == GL_COLOR_CLEAR_VALUE) for (int i = 0; i < 4; i++) v[i] = st().clear_color[i];
    else *v = 0.f;
}

// ---- buffers / vertex arrays ----
static void APIENTRY f_GenBuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_buf(); }
static void APIENTRY f_DeleteBuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; i++) del_buf(ids[i]); }
static GLuint& buffer_slot(GLenum target) {
    GLState& s = st();
    static GLuint other = 0;
    if (target == GL_ARRAY_BUFFER) return s.array_buffer;
    if (target == GL_COPY_WRITE_BUFFER) return s.copy_write_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) { if (Vao* v = get_vao(s.vao)) return v->element; }
    return other;
}
static void APIENTRY f_BindBuffer(GLenum target, GLuint id) { buffer_slot(target) = id; }
static void APIENTRY f_BindBufferBase(GLenum, GLuint, GLuint) {}
static void APIENTRY f_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum) { buf_data(buffer_slot(target), (size_t)size, data); }
static void APIENTRY f_BufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void* data) { buf_subdata(buffer_slot(target), (size_t)off, (size_t)size, data); }
static void* APIENTRY f_MapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield) { return buf_map(buffer_slot(target), (size_t)off, (size_t)len); }
static GLboolean APIENTRY f_UnmapBuffer(GLenum) { return GL_TRUE; }
static void APIENTRY f_FlushMappedBufferRange(GLenum, GLintptr, GLsizeiptr) {}
static void APIENTRY f_GetBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, void* out) {
    if (void* p = buf_map(buffer_slot(target), (size_t)off, 0)) std::memcpy(out, p, (size_t)size);
}
static void APIENTRY f_GenVertexArrays(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_vao(); }
static void APIENTRY f_DeleteVertexArrays(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; i++) del_vao(ids[i]); }
static void APIENTRY f_BindVertexArray(GLuint id) { st().vao = id; }
static void APIENTRY f_VertexAttribPointer(GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
    Vao* v = get_vao(st().vao);
    if (!v || idx >= (GLuint)kMaxAttribs) return;
    Attrib& a = v->a[idx];
    a.size = size; a.type = type; a.normalized = norm != GL_FALSE; a.stride = stride; a.offset = (size_t)(uintptr_t)ptr; a.buffer = st().array_buffer;
}
static void APIENTRY f_EnableVertexAttribArray(GLuint idx) { if (Vao* v = get_vao(st().vao)) if (idx < (GLuint)kMaxAttribs) v->a[idx].enabled = true; }
static void APIENTRY f_DisableVertexAttribArray(GLuint idx) { if (Vao* v = get_vao(st().vao)) if (idx < (GLuint)kMaxAttribs) v->a[idx].enabled = false; }

// ---- textures / samplers ----
static void APIENTRY f_GenTextures(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_tex(); }
static void APIENTRY f_DeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) { for (GLuint& u : st().unit_tex) if (u == ids[i]) u = 0; del_tex(ids[i]); }
}
static void APIENTRY f_ActiveTexture(GLenum t) { int u = (int)t - (int)GL_TEXTURE0; if (u >= 0 && u < kMaxUnits) st().active_unit = u; }
static void APIENTRY f_BindTexture(GLenum, GLuint id) { st().unit_tex[st().active_unit] = id; }
static void set_param(SamplerParams& sp, GLenum pname, float v) {
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: sp.min = (GLenum)v; break;
    case GL_TEXTURE_MAG_FILTER: sp.mag = (GLenum)v; break;
    case GL_TEXTURE_WRAP_S: sp.wrap_s = (GLenum)v; break;
    case GL_TEXTURE_WRAP_T: sp.wrap_t = (GLenum)v; break;
#ifdef GL_TEXTURE_LOD_BIAS
    case GL_TEXTURE_LOD_BIAS: sp.lod_bias = v; break;
#endif
    case GL_TEXTURE_BASE_LEVEL: sp.base_level = (int)v; break;
    case GL_TEXTURE_MAX_LEVEL: sp.max_level = (int)v; break;
    default: break;
    }
}
static void APIENTRY f_TexParameteri(GLenum, GLenum pname, GLint v) { if (Tex* t = get_tex(st().unit_tex[st().active_unit])) set_param(t->sp, pname, (float)v); }
static void APIENTRY f_TexParameterf(GLenum, GLenum pname, GLfloat v) { if (Tex* t = get_tex(st().unit_tex[st().active_unit])) set_param(t->sp, pname, v); }
static void APIENTRY f_GenSamplers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_sampler(); }
static void APIENTRY f_DeleteSamplers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; i++) { for (GLuint& u : st().unit_sampler) if (u == ids[i]) u = 0; del_sampler(ids[i]); } }
static void APIENTRY f_BindSampler(GLuint unit, GLuint id) { if (unit < (GLuint)kMaxUnits) st().unit_sampler[unit] = id; }
static void APIENTRY f_SamplerParameteri(GLuint id, GLenum pname, GLint v) { if (Sampler* s = get_sampler(id)) set_param(s->sp, pname, (float)v); }
static void APIENTRY f_SamplerParameterf(GLuint id, GLenum pname, GLfloat v) { if (Sampler* s = get_sampler(id)) set_param(s->sp, pname, v); }
static void APIENTRY f_TexImage2D(GLenum, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint, GLenum fmt, GLenum type, const void* data) {
    tex_image(st().unit_tex[st().active_unit], level, (GLenum)ifmt, w, h, fmt, type, data);
}
static void APIENTRY f_TexSubImage2D(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* data) {
    tex_subimage(st().unit_tex[st().active_unit], level, x, y, w, h, fmt, type, data);
}
static void APIENTRY f_GenerateMipmap(GLenum) { tex_genmips(st().unit_tex[st().active_unit]); }

// ---- framebuffers ----
static void APIENTRY f_GenFramebuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_fbo(); }
static void APIENTRY f_DeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) { if (st().draw_fbo == ids[i]) st().draw_fbo = 0; if (st().read_fbo == ids[i]) st().read_fbo = 0; del_fbo(ids[i]); }
}
static void APIENTRY f_BindFramebuffer(GLenum target, GLuint id) {
    GLState& s = st();
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) s.draw_fbo = id;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s.read_fbo = id;
}
static Fbo* bound_fbo(GLenum target) { return get_fbo(target == GL_READ_FRAMEBUFFER ? st().read_fbo : st().draw_fbo); }
static void APIENTRY f_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum, GLuint tex, GLint) {
    if (Fbo* f = bound_fbo(target)) {
        if (attachment == GL_COLOR_ATTACHMENT0) { f->color_tex = tex; f->color_rb = 0; }
        else if (attachment == GL_COLOR_ATTACHMENT0 + 1) f->color_tex1 = tex;
        else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT) { f->depth_tex = tex; f->depth_rb = 0; }
    }
}
static void APIENTRY f_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum, GLuint rb) {
    if (Fbo* f = bound_fbo(target)) {
        if (attachment == GL_COLOR_ATTACHMENT0) { f->color_rb = rb; f->color_tex = 0; }
        else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT) { f->depth_rb = rb; f->depth_tex = 0; }
    }
}
static GLenum APIENTRY f_CheckFramebufferStatus(GLenum target) {
    return fbo_complete(target == GL_READ_FRAMEBUFFER ? st().read_fbo : st().draw_fbo) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_UNSUPPORTED;
}
static void APIENTRY f_GenRenderbuffers(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = obj_gen_rbo(); }
static void APIENTRY f_DeleteRenderbuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; i++) del_rbo(ids[i]); }
static void APIENTRY f_BindRenderbuffer(GLenum, GLuint id) { st().renderbuffer = id; }
static void APIENTRY f_RenderbufferStorage(GLenum, GLenum ifmt, GLsizei w, GLsizei h) { rbo_storage(st().renderbuffer, ifmt, w, h); }
static void APIENTRY f_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
static void APIENTRY f_ReadBuffer(GLenum) {}
static void APIENTRY f_DrawBuffers(GLsizei n, const GLenum*) { set_draw_buffers(n); }
static void APIENTRY f_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* px) { read_pixels(x, y, w, h, fmt, type, px); }
static void APIENTRY f_Clear(GLbitfield mask) { clear(mask); }
static void APIENTRY f_Finish() { finish(); }
static void APIENTRY f_Flush() { flush(); }

// ---- shaders / programs / uniforms ----
static GLuint APIENTRY f_CreateShader(GLenum type) { return obj_gen_shader(type); }
static void APIENTRY f_ShaderSource(GLuint id, GLsizei n, const GLchar* const* strs, const GLint* lens) {
    Shader* s = get_shader(id);
    if (!s) return;
    s->src.clear();
    for (GLsizei i = 0; i < n; i++) s->src.append(strs[i], lens && lens[i] >= 0 ? (size_t)lens[i] : std::strlen(strs[i]));
}
static void APIENTRY f_CompileShader(GLuint id) { if (Shader* s = get_shader(id)) s->ok = true; }   // compiled and checked at link time
static void APIENTRY f_GetShaderiv(GLuint id, GLenum p, GLint* v) {
    Shader* s = get_shader(id);
    *v = p == GL_COMPILE_STATUS ? (s && s->ok) : p == GL_INFO_LOG_LENGTH ? (GLint)(s ? s->log.size() : 0) : 0;
}
static void APIENTRY f_GetShaderInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* out) {
    Shader* s = get_shader(id);
    size_t n = s ? std::min<size_t>(s->log.size(), max > 0 ? max - 1 : 0) : 0;
    if (max > 0) { if (n) std::memcpy(out, s->log.data(), n); out[n] = 0; }
    if (len) *len = (GLsizei)n;
}
static void APIENTRY f_DeleteShader(GLuint id) { del_shader(id); }
static GLuint APIENTRY f_CreateProgram() { return obj_gen_program(); }
static void APIENTRY f_AttachShader(GLuint prog, GLuint sh) { if (Program* p = get_prog(prog)) p->shaders.push_back(sh); }
static void APIENTRY f_BindAttribLocation(GLuint, GLuint, const GLchar*) {}
static void APIENTRY f_TransformFeedbackVaryings(GLuint, GLsizei, const GLchar* const*, GLenum) {}
static void APIENTRY f_LinkProgram(GLuint id) {
    Program* p = get_prog(id);
    if (!p) return;
    // The shader objects may be deleted right after linking (the runtime does): take their text now.
    link_program(*p);
}
static void APIENTRY f_GetProgramiv(GLuint id, GLenum pn, GLint* v) {
    Program* p = get_prog(id);
    *v = pn == GL_LINK_STATUS ? (p && p->linked) : pn == GL_INFO_LOG_LENGTH ? (GLint)(p ? p->log.size() : 0) : 0;
}
static void APIENTRY f_GetProgramInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* out) {
    Program* p = get_prog(id);
    size_t n = p ? std::min<size_t>(p->log.size(), max > 0 ? max - 1 : 0) : 0;
    if (max > 0) { if (n) std::memcpy(out, p->log.data(), n); out[n] = 0; }
    if (len) *len = (GLsizei)n;
}
static void APIENTRY f_DeleteProgram(GLuint id) { if (st().program == id) st().program = 0; del_prog(id); }
static void APIENTRY f_UseProgram(GLuint id) { st().program = id; }
static GLint APIENTRY f_GetUniformLocation(GLuint id, const GLchar* name) {
    Program* p = get_prog(id);
    if (!p || !p->linked) return -1;
    std::string n = name;
    const size_t br = n.find('[');
    if (br != std::string::npos) n.resize(br);
    for (size_t i = 0; i < p->conv.samplers.size(); i++) if (p->conv.samplers[i] == n) return 0x10000 + (GLint)i;
    for (size_t i = 0; i < p->conv.uniforms.size(); i++) if (p->conv.uniforms[i].name == n) return (GLint)i;
    return -1;
}
// Writes `n` bytes at the uniform's std140 offset; marks the program dirty only when the value changed.
static void uwrite(GLint loc, const void* data, size_t n, int count = 1) {
    Program* p = get_prog(st().program);
    if (!p || loc < 0) return;
    if (loc >= 0x10000) { const size_t b = (size_t)(loc - 0x10000); if (b < (size_t)kMaxSamplerBindings) { int v; std::memcpy(&v, data, 4); p->unit[b] = v; } return; }
    if ((size_t)loc >= p->conv.uniforms.size()) return;
    const pspvk::UniformMember& u = p->conv.uniforms[loc];
    size_t bytes = n * (size_t)count;
    if (bytes > u.size) bytes = u.size;
    if (std::memcmp(&p->shadow[u.offset], data, bytes) != 0) { std::memcpy(&p->shadow[u.offset], data, bytes); p->dirty = true; }
}
static void APIENTRY f_Uniform1i(GLint l, GLint a) { uwrite(l, &a, 4); }
static void APIENTRY f_Uniform1f(GLint l, GLfloat a) { uwrite(l, &a, 4); }
static void APIENTRY f_Uniform2f(GLint l, GLfloat a, GLfloat b) { float v[2] = {a, b}; uwrite(l, v, 8); }
static void APIENTRY f_Uniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { float v[3] = {a, b, c}; uwrite(l, v, 12); }
static void APIENTRY f_Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { float v[4] = {a, b, c, d}; uwrite(l, v, 16); }
static void APIENTRY f_Uniform3i(GLint l, GLint a, GLint b, GLint c) { int v[3] = {a, b, c}; uwrite(l, v, 12); }
static void APIENTRY f_Uniform4fv(GLint l, GLsizei n, const GLfloat* v) { uwrite(l, v, 16, n); }
static void APIENTRY f_UniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat* v) {
    if (tr) {   // the runtime never transposes; do it right anyway for a single matrix
        float m[16];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[c * 4 + r] = v[r * 4 + c];
        uwrite(l, m, 64, 1);
        return;
    }
    uwrite(l, v, 64, n);
}

// ---- draw ----
static void APIENTRY f_DrawArrays(GLenum mode, GLint first, GLsizei count) { draw_arrays(mode, first, count); }
static void APIENTRY f_MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei n) { draw_multi(mode, first, count, n); }

// ---- queries (diagnostics only: always "nothing drawn / 0 ns") ----
static void APIENTRY f_GenQueries(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; i++) ids[i] = (GLuint)g_query_next++; }
static void APIENTRY f_DeleteQueries(GLsizei, const GLuint*) {}
static void APIENTRY f_BeginQuery(GLenum, GLuint) {}
static void APIENTRY f_EndQuery(GLenum) {}
#ifndef PSP_GLES
static void APIENTRY f_QueryCounter(GLuint, GLenum) {}
static void APIENTRY f_GetQueryObjectui64v(GLuint, GLenum p, GLuint64* v) { *v = p == GL_QUERY_RESULT_AVAILABLE ? 1 : 0; }
#endif
static void APIENTRY f_GetQueryObjectiv(GLuint, GLenum p, GLint* v) { *v = p == GL_QUERY_RESULT_AVAILABLE ? 1 : 0; }
static void APIENTRY f_GetQueryObjectuiv(GLuint, GLenum p, GLuint* v) { *v = p == GL_QUERY_RESULT_AVAILABLE ? 1 : 0; }
static void APIENTRY f_BeginTransformFeedback(GLenum) {}
static void APIENTRY f_EndTransformFeedback() {}

}  // extern "C++"

#ifdef PSP_GLES
#define SET(Name) g_psp_gl.gl##Name = f_##Name
#else
#define SET(Name) glad_gl##Name = f_##Name
#endif

static void install() {
#ifdef PSP_GLES
    SET(DepthRangef); SET(ClearDepthf);
#else
    SET(DepthRange); SET(ClearDepth); SET(PointSize); SET(QueryCounter); SET(GetQueryObjectui64v);
#endif
    SET(Enable); SET(Disable); SET(IsEnabled); SET(Viewport); SET(Scissor); SET(DepthMask); SET(DepthFunc);
    SET(ColorMask); SET(BlendFunc); SET(BlendFuncSeparate); SET(BlendEquation); SET(BlendEquationSeparate); SET(BlendColor);
    SET(StencilMask); SET(StencilFunc); SET(StencilOp); SET(CullFace); SET(ClearColor); SET(ClearStencil);
    SET(LineWidth); SET(PixelStorei); SET(GetError); SET(GetString); SET(GetStringi); SET(GetIntegerv);
    SET(GetBooleanv); SET(GetFloatv);
    SET(GenBuffers); SET(DeleteBuffers); SET(BindBuffer); SET(BindBufferBase); SET(BufferData); SET(BufferSubData);
    SET(MapBufferRange); SET(UnmapBuffer); SET(FlushMappedBufferRange);
#ifndef PSP_GLES
    SET(GetBufferSubData);
#endif
    SET(GenVertexArrays); SET(DeleteVertexArrays); SET(BindVertexArray); SET(VertexAttribPointer);
    SET(EnableVertexAttribArray); SET(DisableVertexAttribArray);
    SET(GenTextures); SET(DeleteTextures); SET(ActiveTexture); SET(BindTexture); SET(TexParameteri); SET(TexParameterf);
    SET(GenSamplers); SET(DeleteSamplers); SET(BindSampler); SET(SamplerParameteri); SET(SamplerParameterf);
    SET(TexImage2D); SET(TexSubImage2D); SET(GenerateMipmap);
    SET(GenFramebuffers); SET(DeleteFramebuffers); SET(BindFramebuffer); SET(FramebufferTexture2D); SET(FramebufferRenderbuffer);
    SET(CheckFramebufferStatus); SET(GenRenderbuffers); SET(DeleteRenderbuffers); SET(BindRenderbuffer); SET(RenderbufferStorage);
    SET(BlitFramebuffer); SET(ReadBuffer); SET(DrawBuffers); SET(ReadPixels); SET(Clear); SET(Finish); SET(Flush);
    SET(CreateShader); SET(ShaderSource); SET(CompileShader); SET(GetShaderiv); SET(GetShaderInfoLog); SET(DeleteShader);
    SET(CreateProgram); SET(AttachShader); SET(BindAttribLocation); SET(TransformFeedbackVaryings); SET(LinkProgram);
    SET(GetProgramiv); SET(GetProgramInfoLog); SET(DeleteProgram); SET(UseProgram); SET(GetUniformLocation);
    SET(Uniform1i); SET(Uniform1f); SET(Uniform2f); SET(Uniform3f); SET(Uniform4f); SET(Uniform3i); SET(Uniform4fv); SET(UniformMatrix4fv);
    SET(DrawArrays);
#ifndef PSP_GLES
    SET(MultiDrawArrays);   // ES: psp_gl.h loops glDrawArrays
#endif
    SET(GenQueries); SET(DeleteQueries); SET(BeginQuery); SET(EndQuery);
#ifndef PSP_GLES
    SET(GetQueryObjectiv);
#endif
    SET(GetQueryObjectuiv); SET(BeginTransformFeedback); SET(EndTransformFeedback);
}

bool available(std::string* why) {
    static int state = 0;   // 0 = not probed, 1 = usable, -1 = not
    static std::string reason;
    if (g_active) return true;
    if (state == 0) {
        state = pspvk::vulkan_available(&reason) ? 1 : -1;
        if (state < 0) std::fprintf(stderr, "[VKGL] Vulkan is not available: %s\n", reason.c_str());
    }
    if (state < 0 && why) *why = reason;
    return state > 0;
}

bool requested() {
    const char* r = std::getenv("PSPRECOMP_RENDERER");
#ifdef __ANDROID__
    const bool by_default = true;    // phones: Vulkan whenever the GPU/driver supports it (PSPRECOMP_RENDERER=gl or the Settings row picks OpenGL)
#else
    const bool by_default = false;
#endif
    const bool want = r && r[0] ? (!std::strcmp(r, "vulkan") || !std::strcmp(r, "vk")) : by_default;
    if (!want) return false;
    std::string why;
    if (available(&why)) return true;
    std::fprintf(stderr, "[VKGL] Vulkan %s but unavailable (%s); using OpenGL\n", r && r[0] ? "requested" : "is the default here", why.c_str());
    return false;
}
bool active() { return g_active; }

bool init(SDL_Window* window, bool vsync, std::string* err) {
    if (!core_init(window, vsync, err)) return false;
    install();
    g_active = true;
    g_psp_rt.unavailable_reason = rt_unavailable_reason;
    g_psp_hdr.available = [] { return core_hdr_supported(); };
    g_psp_hdr.encoding = [] { return core_hdr_encoding(); };
    g_psp_hdr.set_wanted = [](bool on) { core_set_hdr(on); };
    g_psp_hdr.reason = core_hdr_reason;
    if (rt_available()) { g_psp_rt.available = [] { return rt_available(); }; g_psp_rt.set_scene = rt_set_scene; }
    // GL starts with FBO 0 bound and a full-window viewport.
    int w = 0, h = 0;
    core_drawable(&w, &h);
    st().vp[2] = w; st().vp[3] = h;
    std::fprintf(stderr, "[VKGL] GL-on-Vulkan active (%dx%d)\n", w, h);
    return true;
}
void shutdown() { if (g_active) { core_shutdown(); g_active = false; } }
void swap() { if (g_active) core_swap(); }
void set_swap_interval(bool vsync) { core_set_vsync(vsync); }
void drawable_size(SDL_Window* window, int* w, int* h) { core_drawable_query(window, w, h); }

}  // namespace vkgl
