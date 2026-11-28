#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    float u_sat;
    float u_bright;
    float u_contrast;
    int u_enc;
    float u_peak;
    float u_paper;
    float u_scrgb_nits;
    float u_boost;
    float u_gamut;
    vec2 u_texel;
    int u_hscene;
};





layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 0) uniform sampler2D u_scene;
layout(set = 1, binding = 1) uniform sampler2D u_comp;












vec3 srgb_to_lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }

vec3 pq_encode(vec3 nits) {
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 p = pow(y, vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    vec3 c = texture(u_comp, v_uv).rgb;
    vec3 sc = texture(u_scene, v_uv).rgb;
    // Pixels the overlays changed (HUD, menus, dialogs) stay at SDR level; only the 3D picture is expanded.
    const bool scene_px = max(abs(c.r - sc.r), max(abs(c.g - sc.g), abs(c.b - sc.b))) < (2.5 / 255.0);
    // Grade, in display (gamma) space.
    // With HDR Scene on, values above 1.0 are real light (additive effects) and are kept (extended sRGB); the grade
    // never lifts a value that was at or below 1.0 above it, so the LDR look is unchanged.
    const float hi = u_hscene != 0 ? 64.0 : 1.0;
    c = clamp((c - 0.5) * u_contrast + 0.5, 0.0, hi);
    c = pow(max(c, vec3(0.0)), vec3(1.0 / u_bright));
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(l), c, u_sat), 0.0, hi);
    if (u_enc == 0) { o_color = vec4(c, 1.0); return; }
    // HDR. The game is SDR: linearise, then lift only the bright end (glows, spell flashes, the sky, specular) so that
    // SDR white sits at paper white and the brightest pixels reach the display's peak.
    vec3 lin = srgb_to_lin(c);
    float m = max(lin.r, max(lin.g, lin.b));
    float gain = 1.0;
    if (scene_px && u_boost > 0.0) {
        // Only SMALL bright features (glows, sparks, spell flashes, specular) may rise above paper white. Large bright areas
        // (menus, sky, white stages) are already at their intended level in SDR, so a pixel is lifted only as far as it
        // stands out from the local average around it.
        float lm = 0.0;
        for (int i = 0; i < 8; ++i) {
            float a = 0.785398 * float(i);
            vec3 n = texture(u_comp, v_uv + vec2(cos(a), sin(a)) * u_texel * (i % 2 == 0 ? 6.0 : 11.0)).rgb;
            lm += dot(srgb_to_lin(n), vec3(0.2126, 0.7152, 0.0722));
        }
        lm *= 0.125;
        float lum = dot(lin, vec3(0.2126, 0.7152, 0.0722));
        float stand = smoothstep(0.10, 0.55, lum - lm);       // how much it stands out from its surroundings
        float bright = smoothstep(0.50, 0.95, m);             // and that it is actually bright
        gain = 1.0 + max(u_peak / u_paper - 1.0, 0.0) * u_boost * stand * bright;
    }
    // Colour: a wide-gamut panel shows plain sRGB colour dull next to its own SDR mode (the compositor stretches SDR colour to the
    // panel gamut, but not HDR content). u_gamut stretches the picture the same way, treating the PSP's RGB as Display-P3 by that amount.
    // Matrices are written column by column. scRGB (enc 1) takes BT.709 primaries and may go negative; HDR10 (enc 2) takes BT.2020.
    const mat3 p3_to_709 = mat3(1.2249401, -0.0420569, -0.0196376,  -0.2249404, 1.0420571, -0.0786361,  0.0, 0.0, 1.0982735);
    const mat3 p3_to_2020 = mat3(0.7538330, 0.0457438, -0.0012110,  0.1985970, 0.9417770, 0.0176042,  0.0475700, 0.0124792, 0.9836070);
    const mat3 srgb_to_2020 = mat3(0.6274040, 0.0690970, 0.0163916,  0.3292820, 0.9195400, 0.0880132,  0.0433136, 0.0113612, 0.8955950);
    if (u_enc == 1) lin = mix(lin, p3_to_709 * lin, u_gamut);
    else lin = mix(srgb_to_2020 * lin, p3_to_2020 * lin, u_gamut);
    vec3 nits = lin * u_paper * gain;
    if (u_hscene != 0) nits *= min(1.0, u_peak / max(max(nits.r, max(nits.g, nits.b)), 1.0));   // real HDR data stops at the display peak
    if (u_enc == 1) o_color = vec4(nits / u_scrgb_nits, 1.0);   // scRGB: 1.0 = 80 nits (Android: 1.0 = SDR white, so paper white is the system's)
    else o_color = vec4(pq_encode(nits), 1.0);
}
