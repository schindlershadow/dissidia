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
/// The game module reports the stage being fought (-1 = unknown / no battle); used for per-stage lighting looks.
void psp_set_stage_id_provider(int (*fn)());
int psp_stage_id();
/// Stage light editing (see psp_screenfx.cpp "Stage lights"). Returns true when the key was consumed.
enum {
    PSP_LIGHT_LEFT = 0, PSP_LIGHT_RIGHT, PSP_LIGHT_FWD, PSP_LIGHT_BACK, PSP_LIGHT_DOWN, PSP_LIGHT_UP,   // held: move the selected light (camera relative)
    PSP_LIGHT_RAD_DOWN, PSP_LIGHT_RAD_UP, PSP_LIGHT_INT_DOWN, PSP_LIGHT_INT_UP,                       // held: radius / intensity
    PSP_LIGHT_FAST,                                                                                  // held: 4x speed (not an action itself)
    PSP_LIGHT_DROP, PSP_LIGHT_REMOVE, PSP_LIGHT_TOGGLE_EDIT, PSP_LIGHT_PREV, PSP_LIGHT_NEXT, PSP_LIGHT_DIST_UP, PSP_LIGHT_DIST_DOWN,
};
bool psp_stage_light_input(int what, bool down);

/// What the scene was drawn with (the last 3D draw of the buffer).
struct ScreenFxScene {
    float proj[16];        // PSP projection matrix, column-major
    float view[12];        // PSP view matrix, 4x3 column-major (3 floats per column, the last column the translation)
    float near_z, far_z;   // glDepthRange of the scene's draws
    float clear_z;         // what the buffer's depth was cleared to (the sky / background)
    bool rt_fx = false;    // psp_rt.h: the scene also has an effects layer (blended draws), traced by the reflection
    GLuint fx_color = 0;   // screen-space effects layer (psp_ge_draw.cpp fxl_*): blended draws, RGBA8 (alpha = coverage) + depth texture; 0 = none
    GLuint fx_depth = 0;
    bool rt_ready = false; // psp_rt.h: this frame's world-space triangles are built; the contact shadow uses ray queries
};

/// Apply the effects to the colour of `fbo` (w x h texels, colour texture `color_tex`, depth renderbuffer attached).
/// Leaves GL state the caller must treat as clobbered: glc::invalidate(), ge_texture_invalidate_bind_cache(),
/// ge_shader_note_external_program(), and the framebuffer binding is the caller's to restore.
void psp_screenfx_run(GLuint fbo, GLuint color_tex, int w, int h, const ScreenFxScene& scene);
