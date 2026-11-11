// Screen-space scene effects run on the finished 3D scene of a displayed buffer, before the game's own glow chain and the
// HUD are drawn over it (so neither is darkened or reflected): SSAO, contact shadows, screen-space reflections.
// Render (GL) thread only. Settings: runtime/src/psp_settings.cpp rows SSAO, Contact shadows, Reflections.
#pragma once
#include "psp_gl.h"

int psp_fx_ssao();               // 0 off, 1..3 strength; PSPRECOMP_SSAO / settings 'ssao'
void psp_fx_set_ssao(int level);
int psp_fx_contact();            // 0 off, 1..3 strength; PSPRECOMP_CONTACT_SHADOWS / settings 'contact_shadows'
void psp_fx_set_contact(int level);
int psp_fx_ssr();                // 0 off, 1..3 strength; PSPRECOMP_SSR / settings 'ssr'
void psp_fx_set_ssr(int level);

/// True when any of the effects is on.
bool psp_screenfx_active();

/// What the scene was drawn with (the last 3D draw of the buffer).
struct ScreenFxScene {
    float proj[16];        // PSP projection matrix, column-major
    float view[12];        // PSP view matrix, 4x3 column-major (3 floats per column, the last column the translation)
    float near_z, far_z;   // glDepthRange of the scene's draws
    float clear_z;         // what the buffer's depth was cleared to (the sky / background)
};

/// Apply the effects to the colour of `fbo` (w x h texels, colour texture `color_tex`, depth renderbuffer attached).
/// Leaves GL state the caller must treat as clobbered: glc::invalidate(), ge_texture_invalidate_bind_cache(),
/// ge_shader_note_external_program(), and the framebuffer binding is the caller's to restore.
void psp_screenfx_run(GLuint fbo, GLuint color_tex, int w, int h, const ScreenFxScene& scene);
