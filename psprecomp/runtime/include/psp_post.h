// Present-time post-processing: FXAA anti-aliasing and contrast-adaptive sharpening, and the sharp upscale
// filters, applied while the picture is drawn into the window (so the game's own glow / blur chain is untouched).
// Render (GL) thread only. Settings: runtime/src/psp_settings.cpp rows Post AA, Sharpen, Upscale.
#pragma once
#include "psp_gl.h"
#include "psp_hdr.h"

enum PspPostAA { POST_AA_OFF = 0, POST_AA_FXAA = 1, POST_AA_SMAA = 2 };   // SMAA: morphological, 3 passes (SMAA 1x, 'high' preset)
enum PspPostUpscale { POST_UP_BILINEAR = 0, POST_UP_SHARP = 1, POST_UP_BICUBIC = 2 };   // 0 = the old blit

int psp_post_aa();               // PSPRECOMP_POST_AA=0|1|2 (off, FXAA, SMAA) / settings 'post_aa'
void psp_post_set_aa(int mode);
int psp_post_sharpen();          // 0 off, 1..4 strength; PSPRECOMP_SHARPEN / settings 'sharpen'
void psp_post_set_sharpen(int level);
int psp_post_upscale();          // PSPRECOMP_UPSCALE=bilinear|sharp|bicubic / settings 'upscale'
void psp_post_set_upscale(int mode);

// ---- Final pass: colour grade and HDR ---------------------------------------------------------------------------------
// Runs once per present, after the HUD / menus / overlays, on the whole window. It keeps a copy of the scene taken before
// the overlays (psp_post_scene_snapshot) so that HDR highlight expansion only touches the 3D picture: a pixel the overlays
// changed (HUD text, menus, dialogs) is left at its SDR level, otherwise white UI text would be pushed to peak brightness.
int psp_post_saturation();       // percent, 100 = unchanged, 0..200; PSPRECOMP_SATURATION / settings 'saturation'
void psp_post_set_saturation(int pct);
int psp_post_brightness();       // gamma percent, 100 = unchanged, 50..150 (above 100 = brighter midtones); PSPRECOMP_BRIGHTNESS
void psp_post_set_brightness(int pct);
int psp_post_contrast();         // percent, 100 = unchanged, 50..150; PSPRECOMP_CONTRAST
void psp_post_set_contrast(int pct);

/// HDR output. `want` is the user's choice (PSPRECOMP_HDR=1 / settings 'hdr'); it takes effect only when the Vulkan
/// backend found an HDR swapchain (g_psp_hdr.active()), otherwise the picture stays SDR and the expansion is not applied.
// (hooks: psp_hdr.h)

bool psp_hdr_wanted();
void psp_hdr_set_wanted(bool on);
bool psp_hdr_available();
int psp_hdr_encoding();
bool psp_hdr_scene_wanted();     // HDR Scene (PSPRECOMP_HDR_SCENE, default off): render the scene in fp16 so additive effects keep values above 1.0
void psp_hdr_scene_set_wanted(bool on);
bool psp_hdr_scene_live();       // wanted AND an HDR swapchain is live (Vulkan only): the scene targets are fp16
int psp_hdr_peak();              // nits the brightest expanded highlight reaches, 400..4000; PSPRECOMP_HDR_PEAK (default 1000)
void psp_hdr_set_peak(int nits);
int psp_hdr_paper();             // nits SDR white is shown at, 80..1000; PSPRECOMP_HDR_PAPER (default 203, the BT.2408 reference)
void psp_hdr_set_paper(int nits);
int psp_hdr_boost();             // 0 off, 1 low (default), 2 medium, 3 high: how far small bright features rise above paper white; PSPRECOMP_HDR_BOOST
void psp_hdr_set_boost(int level);
int psp_hdr_gamut();             // 0 off (plain sRGB), 1 low, 2 medium (default), 3 high: stretch of the colours towards Display-P3; PSPRECOMP_HDR_GAMUT
void psp_hdr_set_gamut(int level);

/// Copy the finished scene (before the HUD / overlays) for the final pass. No-op when the final pass is not needed.
void psp_post_scene_snapshot(int dw, int dh);
/// Grade (and HDR-expand + encode) the whole window. Same GL-state caveat as psp_post_draw. Returns false when nothing was drawn.
bool psp_post_final(int dw, int dh);
/// True when the final pass has work (a grade is set, or HDR output is live).
bool psp_post_final_active();

/// True when any post effect is on (the present then goes through psp_post_draw instead of glBlitFramebuffer).
bool psp_post_active();

/// Draw `tex` (sw x sh texels, GL origin bottom-left) into the default framebuffer rectangle [x0,x1) x [y0,y1).
/// `nearest` forces nearest sampling (integer scale); otherwise the upscale mode decides. Leaves GL state that the
/// caller must treat as clobbered: call glc::invalidate() and ge_shader_note_external_program() afterwards.
/// Returns false (nothing drawn) if the shader failed to build; the caller then falls back to the plain blit.
bool psp_post_draw(GLuint tex, int sw, int sh, int x0, int y0, int x1, int y1, bool nearest);
