// Present-time post-processing: FXAA anti-aliasing and contrast-adaptive sharpening, and the sharp upscale
// filters, applied while the picture is drawn into the window (so the game's own glow / blur chain is untouched).
// Render (GL) thread only. Settings: runtime/src/psp_settings.cpp rows Post AA, Sharpen, Upscale.
#pragma once
#include "psp_gl.h"

enum PspPostAA { POST_AA_OFF = 0, POST_AA_FXAA = 1 };
enum PspPostUpscale { POST_UP_BILINEAR = 0, POST_UP_SHARP = 1, POST_UP_BICUBIC = 2 };   // 0 = the old blit

int psp_post_aa();               // PSPRECOMP_POST_AA=0|1 / settings 'post_aa'
void psp_post_set_aa(int mode);
int psp_post_sharpen();          // 0 off, 1..4 strength; PSPRECOMP_SHARPEN / settings 'sharpen'
void psp_post_set_sharpen(int level);
int psp_post_upscale();          // PSPRECOMP_UPSCALE=bilinear|sharp|bicubic / settings 'upscale'
void psp_post_set_upscale(int mode);

/// True when any post effect is on (the present then goes through psp_post_draw instead of glBlitFramebuffer).
bool psp_post_active();

/// Draw `tex` (sw x sh texels, GL origin bottom-left) into the default framebuffer rectangle [x0,x1) x [y0,y1).
/// `nearest` forces nearest sampling (integer scale); otherwise the upscale mode decides. Leaves GL state that the
/// caller must treat as clobbered: call glc::invalidate() and ge_shader_note_external_program() afterwards.
/// Returns false (nothing drawn) if the shader failed to build; the caller then falls back to the plain blit.
bool psp_post_draw(GLuint tex, int sw, int sh, int x0, int y0, int x1, int y1, bool nearest);
