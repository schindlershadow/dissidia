#version 460
#extension GL_EXT_ray_query : require
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    vec2 u_step;
    mat4 u_proj;
    mat4 u_inv;
    vec2 u_dr;
    float u_clearz;
    float u_sky;
    vec2 u_fade;
    int u_depth_snap;
    int u_snap_split;
    float u_radius;
    int u_n_ao;
    int u_n_cs;
    int u_n_ssr;
    vec3 u_light;
    vec3 u_up;
    float u_cs_len;
    float u_cs_thick;
    float u_ssr_len;
    float u_ssr_amt;
    int u_have_fx;
    int u_fx_dbg;
    float u_fx_gain;
    float u_fx_thick;
    float u_lscale;
    int u_mask_on;
    int u_mask_dbg;
    float u_wave;
    float u_wave_max;
    int u_char_mode;
    int u_fmask;
    int u_use_rt;
    int u_sun;
    mat4 u_view_inv;
    vec3 u_light_w;
    float u_rt_len;
    int u_fmask_dbg;
    int u_char_wrap;
    float u_char_amt;
    float u_rt_self_len;
    int u_rt_rays;
    float u_rt_soft;
    int u_use_rt_refl;
    int u_rt_dbg;
    int u_rt_refl_rays;
    float u_rt_rough;
    float u_rough_jit;
    int u_rt_fx;
    float u_fx_minh;
    float u_rt_hidden;
    float u_rt_sky_amt;
    float u_rt_sky_dist;
    int u_refl_split;
    float u_rt_refl_len;
    vec3 u_rt_sky;
    int u_n_lights;
    vec4 u_lp[24];
    vec4 u_lc[24];
    vec4 u_lvp[24];
    int u_edit;
    int u_sel;
    int u_fx0;
    int u_fx_split;
    float u_fx_alb;
    int u_mark0;
    int u_mark1;
    int u_env_on;
};





layout(location = 0) in vec2 v_uv;
layout(set = 1, binding = 0) uniform sampler2D u_depth;










// The depth at uv, from one definite full-resolution texel. The passes run at half resolution, where a pixel's centre lies exactly on
// the border between two depth texels: nearest sampling then picks either one depending on the GPU's rounding (Adreno / Turnip did
// not do it consistently), so the normals rebuilt from neighbouring depths jumped in bands along pixel columns and depth contours,
// and AO and the ray-traced shadow's facing term drew evenly spaced lines on walls (RP6, render scale 2). The quarter-texel pull-back
// makes every read land inside a texel.
// The position is rebuilt at the same texel centre the depth came from (vpos): a depth from one place and a screen position from another
// put a point off its surface by up to half a texel's depth change, which is large on a floor seen at a low angle (lines across floors).

vec2 snap_uv(vec2 uv) { return u_depth_snap == 0 ? uv : (floor(uv * u_size - 0.25) + 0.5) / u_size; }
float wd_at(vec2 uv) { return texture(u_depth, snap_uv(uv)).r; }
float depth_t(float wd) { return (wd - u_dr.x) / (u_dr.y - u_dr.x); }   // 0 at the near plane, 1 at the far plane
bool is_far(float wd) { return abs(wd - u_clearz) < 3.0 / 65535.0 || depth_t(wd) > u_sky; }
vec3 vpos(vec2 uv, float wd) {
    float z = depth_t(wd) * 2.0 - 1.0;
    vec2 ps = (u_snap_split != 0 && v_uv.x < 0.5) ? uv : snap_uv(uv);   // debug split: the left half rebuilds at the unsnapped point
    vec4 p = u_inv * vec4(ps * 2.0 - 1.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 vpos(vec2 uv) { return vpos(uv, wd_at(uv)); }
// View-space z straight from the projection (no matrix product): used where only the depth along the view axis matters.
float vz(float wd) {
    float ndc = depth_t(wd) * 2.0 - 1.0;
    return (u_proj[3][2] - ndc * u_proj[3][3]) / (ndc * u_proj[2][3] - u_proj[2][2]);
}
float dist_fade(float lp) { return 1.0 - smoothstep(u_fade.x, u_fade.y, lp); }
// Surface normal from the depth neighbours (the side with the smaller jump on each axis), facing the camera.
vec3 normal_at(vec2 uv, vec3 P) {
    vec2 px = u_step;
    vec3 a = vpos(uv + vec2(px.x, 0.0)), b = vpos(uv - vec2(px.x, 0.0));
    vec3 c = vpos(uv + vec2(0.0, px.y)), d = vpos(uv - vec2(0.0, px.y));
    vec3 dx = abs(length(a) - length(P)) < abs(length(b) - length(P)) ? a - P : P - b;
    vec3 dy = abs(length(c) - length(P)) < abs(length(d) - length(P)) ? c - P : P - d;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, P) > 0.0) n = -n;      // the camera is at the origin
    return n;
}
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

layout(location = 0) out vec4 o_fx;
layout(location = 1) out vec4 o_refl;
layout(set = 1, binding = 1) uniform sampler2D u_color;











layout(set = 1, binding = 2) uniform sampler2D u_env;
#define u_fxc u_env
#define u_fxd u_env    // effects depth: the nearest effect fragment per pixel










// The mask byte is a fighter (1). Read with linear filtering where a march sample lands, so a range rather than an exact byte.
bool fighter_at(vec2 uv) { float b = texture(u_color, uv).a * 255.0; return b > 0.5 && b < 1.5; }
// The surface's material from the mask byte (psp_refl_table.h psp_refl_encode): x reflectivity, y roughness, z metal, w water.
vec4 mask_material(float a) {
    float b = floor(a * 255.0 + 0.5);
    return vec4(floor(b / 32.0) / 7.0, mod(floor(b / 4.0), 8.0) / 7.0, mod(floor(b / 2.0), 2.0), mod(b, 2.0));
}
// Water: the game animates its water by scrolling a ripple pattern, the geometry stays flat, so a reflection from the depth normal sat
// on it as a still mirror. The ripples' brightness gradient (the scene copy, before the reflection is added) tilts the normal along the
// surface, so the reflection bends and flows with the pattern. Limited, so an edge next to the water (a fighter's feet) cannot fold it.
vec3 wave_normal(vec2 uv, vec3 P, vec3 n) {
    vec2 s = u_step * 4.0;      // a wide span: the swell of the ripples, not their half-resolution texels (those gave a harsh speckle)
    const vec3 lw = vec3(0.299, 0.587, 0.114);
    float lx = dot(texture(u_color, uv + vec2(s.x, 0.0)).rgb - texture(u_color, uv - vec2(s.x, 0.0)).rgb, lw);
    float ly = dot(texture(u_color, uv + vec2(0.0, s.y)).rgb - texture(u_color, uv - vec2(0.0, s.y)).rgb, lw);
    vec3 dx = vpos(uv + vec2(s.x, 0.0)) - P, dy = vpos(uv + vec2(0.0, s.y)) - P;
    vec3 tx = dx - n * dot(dx, n), ty = dy - n * dot(dy, n);
    tx /= max(length(tx), 1e-5);
    ty /= max(length(ty), 1e-5);
    vec3 t = (tx * lx + ty * ly) * u_wave;
    float tl = length(t);
    if (tl > u_wave_max) t *= u_wave_max / tl;
    return normalize(n - t);
}
// Schlick's Fresnel with the roughness damping of the grazing-angle boost: a rough surface does not turn into a mirror at a low
// angle the way water or glass does. Metals reflect strongly head-on as well (base 0.5 instead of 0.04).
float fresnel_r(float cosv, float rough, float metal) {
    float f0 = mix(0.04, 0.5, metal);
    return f0 + (max(1.0 - rough, f0) - f0) * pow(1.0 - clamp(cosv, 0.0, 1.0), 5.0);
}

layout(set = 2, binding = 0) uniform accelerationStructureEXT u_tlas;
layout(set = 2, binding = 1, std430) readonly buffer RtTriColBlock { uint rt_tri_col[]; };
layout(set = 2, binding = 2, std430) readonly buffer RtFxColBlock { uint rt_fx_col[]; };





































// The sky seen in world direction d. A direction never on screen yet (overhead, behind the camera) falls back to the same
// bearing nearer the horizon, which the camera sees most. rgb is the colour, a how sure (0: not seen at all).
vec4 sky_map(vec3 d) {
    if (u_env_on == 0) return vec4(0.0);
    float az = atan(d.z, d.x);
    float el = asin(clamp(d.y, -1.0, 1.0));
    vec3 acc = vec3(0.0);
    float aa = 0.0;
    for (int i = 0; i < 4; ++i) {
        float e = el * (1.0 - float(i) / 3.0 * 0.85);
        vec4 s = texture(u_env, vec2(az / 6.2831853 + 0.5, e / 3.14159265 + 0.5));
        if (s.a > 0.02) {
            float c = min(1.0, s.a * 1.5) * (1.0 - aa);
            acc += s.rgb / s.a * c;
            aa += c;
        }
        if (aa > 0.95) break;
    }
    return aa > 0.0 ? vec4(acc / aa, aa) : vec4(0.0);
}

// The effect at uv floats in front of the scene there: its depth is clearly nearer than the scene's. A blended sheet lying on a
// surface (the water layer of the floor, decals, shadows) is at the scene's own depth and is not an effect to reflect.
bool fx_floats(vec2 uv, float wd_scene, float efd) {
    return length(vpos(uv, wd_scene)) - length(vpos(uv, efd)) > 0.2 + 0.01 * length(vpos(uv, wd_scene));
}

void main() {
    o_fx = vec4(1.0, 0.0, 0.0, 1.0);
    o_refl = vec4(0.0);
    float wd = wd_at(v_uv);
    if (u_fx_dbg == 3 && u_have_fx != 0) {   // debug: where the effects layer holds anything (sky included), no floor test
        vec4 fc = texture(u_fxc, v_uv);
        if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) { o_refl = vec4(fc.rgb * 0.5 + vec3(0.5, 0.0, 0.5), 1.0); return; }
    }
    vec3 mk = vec3(0.0);

    if (u_edit != 0 && u_n_lights > 0) {
        vec4 q = u_inv * vec4(v_uv * 2.0 - 1.0, 1.0, 1.0);
        vec3 dir = normalize(q.xyz / q.w);
        float surf = is_far(wd) ? 1e9 : length(vpos(v_uv, wd));
        for (int li = u_mark0; li < min(u_mark1, u_n_lights); ++li) {
            vec3 Lv = u_lvp[li].xyz;
            float t = dot(Lv, dir);
            if (t <= 0.0) continue;
            float d = length(Lv - dir * t);
            float core = smoothstep(0.3, 0.12, d), halo = 0.35 * smoothstep(1.4, 0.0, d), ring = (li == u_sel) ? smoothstep(0.06, 0.0, abs(d - 0.5)) : 0.0;
            vec3 col = (li == u_sel) ? vec3(1.0) : normalize(u_lc[li].rgb + 1e-4) ;
            float vis = t > surf + 0.3 ? 0.25 : 1.0;                 // behind a wall: dimmed, so it can still be found
            mk += (col * (core + halo) + vec3(1.0, 0.9, 0.2) * ring) * vis;
        }
    }

    if (is_far(wd)) { o_refl.rgb = mk * u_lscale; return; }
    vec3 P = vpos(v_uv, wd);
    float lp = length(P);
    float fd = dist_fade(lp);
    if (fd <= 0.0) { o_refl.rgb = mk * u_lscale; return; }
    vec3 n = normal_at(v_uv, P);
    float noise = ign(gl_FragCoord.xy);
    float ao = 1.0;
    if (u_n_ao > 0) {
        float rad = u_radius * lp;
        vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
        float occ = 0.0;
        for (int i = 0; i < u_n_ao; ++i) {
            float t = (float(i) + 0.5) / float(u_n_ao);
            float a = noise * 6.2831853 + float(i) * 2.39996323;
            float c = mix(0.12, 1.0, fract(float(i) * 0.618034 + noise));
            float s = sqrt(1.0 - c * c);
            vec3 dir = tx * (cos(a) * s) + ty * (sin(a) * s) + n * c;
            vec3 S = P + dir * (rad * mix(0.15, 1.0, t * t));
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) continue;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) continue;
            float wd2 = wd_at(uv2);
            if (is_far(wd2)) continue;
            float dq = length(vpos(uv2, wd2)), ds = length(S);
            if (ds - dq > 0.02 * rad) occ += smoothstep(0.0, 1.0, rad / max(abs(lp - dq), 1e-4));
        }
        ao = mix(1.0, 1.0 - occ / float(u_n_ao), fd);
    }
    float sh = 0.0;
    if (u_n_cs > 0) {
        float facing = smoothstep(0.15, 0.5, dot(n, u_light));   // surfaces turned away from the light are dark already
        if (facing > 0.0) {
            float jit = ign(gl_FragCoord.xy + 17.0);
            vec3 O = P + n * (0.01 + 0.004 * lp);
            // A fighter's own body (and a fighter right next to it) does not shadow it: the low-poly models and their solid cards of
            // hair / fur / fringes blotched the whole body. The stage and other objects still do.
            bool self_rx = u_fmask != 0 && u_char_mode != 2 && fighter_at(v_uv);   // Sharp: a fighter is shadowed like the stage

            if (u_use_rt != 0) {
              if (u_sun != 0 && !(self_rx && u_char_mode == 0)) {   // Off: no sun shadow on a fighter
                vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
                vec3 lw = u_light_w;
                vec3 lu = normalize(abs(lw.y) < 0.99 ? cross(lw, vec3(0.0, 1.0, 0.0)) : cross(lw, vec3(1.0, 0.0, 0.0))), lv = cross(lw, lu);
                float blocked = 0.0;
                for (int r = 0; r < u_rt_rays; ++r) {
                    vec3 ld = lw;
                    if (u_rt_rays > 1) {
                        float a = (float(r) + jit) / float(u_rt_rays) * 6.2831853 + jit * 6.2831853, rad = sqrt(fract(jit * 7.31 + float(r) * 0.618034)) * u_rt_soft;
                        ld = normalize(lw + lu * (cos(a) * rad) + lv * (sin(a) * rad));
                    }
                    rayQueryEXT rq;
                    if (self_rx) {
                        // on a fighter: every triangle is a candidate; a fighter's triangle (low bit of its coverage, psp_ge_draw.cpp
                        // rt_capture) within u_rt_self_len is its own body: passed through (Off) or taking part of the light (Soft:
                        // coverage x u_char_amt, hits right at the start skipped: the faceted body's own surface), anything else blocks
                        float trans = 1.0;
                        rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsNoOpaqueEXT, 0x1, Ow, 0.0, ld, u_rt_len);
                        while (rayQueryProceedEXT(rq)) {
                            if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
                            uint pc = rt_tri_col[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false) + rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)];
                            float th = rayQueryGetIntersectionTEXT(rq, false);
                            if ((pc & 0x01000000u) != 0u && th < u_rt_self_len) {
                                if (u_char_mode == 1 && th > 0.12) trans *= 1.0 - u_char_amt * float(pc >> 24) / 255.0;
                                continue;
                            }
                            rayQueryConfirmIntersectionEXT(rq);
                            rayQueryTerminateEXT(rq);
                        }
                        if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) { blocked += 1.0 - trans; continue; }
                    } else {
                        rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x1, Ow, 0.0, ld, u_rt_len);
                        while (rayQueryProceedEXT(rq)) {}
                    }
                    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) blocked += 1.0;
                }
                float fc = (self_rx && u_char_mode == 1) ? smoothstep(0.05, 0.75, dot(n, u_light)) : facing;   // Soft: a gentler terminator on the low-poly body
                if (blocked > 0.0) sh = fc * fd * blocked / float(u_rt_rays);
              }
            } else

            for (int j = 0; j < u_n_cs; ++j) {
                float t = (float(j) + jit) / float(u_n_cs);
                vec3 S = O + u_light * (t * t * u_cs_len);      // denser near the surface
                vec4 cl = u_proj * vec4(S, 1.0);
                if (cl.w <= 1e-5) break;
                vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
                if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
                float wd2 = wd_at(uv2);
                if (is_far(wd2)) continue;
                if (self_rx && (u_char_mode == 0 || fighter_at(uv2))) continue;   // Off: no contact shadow on a fighter at all
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.01 + 0.004 * lp && diff < u_cs_thick) { sh = facing * fd; break; }
            }
        }
    }
    o_fx = vec4(ao, sh, 0.0, 1.0);
    if (u_fx_dbg == 2 && u_have_fx != 0) {   // debug: the effects layer itself, wherever the layer holds something
        vec4 fc = texture(u_fxc, v_uv);
        if ((fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) && fx_floats(v_uv, wd, texture(u_fxd, snap_uv(v_uv)).r)) { o_refl = vec4(fc.rgb + vec3(0.4, 0.0, 0.4), 1.0); return; }
    }

    // How reflective this surface is (issue #2): the material's reflectivity from the mask, in any orientation (walls too). Without a
    // reflectivity table (another game) the old rule: every upward-facing surface is a mirror floor.
    vec4 mat = u_mask_on != 0 ? mask_material(texture(u_color, v_uv).a) : vec4(smoothstep(0.7, 0.95, dot(n, u_up)), 0.0, 0.0, 0.0);
    float surf_k = mat.x, m_rough = mat.y, m_metal = mat.z;
    vec3 n_geo = n;                                   // the reflection alone sees the water's ripples; the stage lights keep the flat normal
    if (mat.w > 0.5 && u_wave > 0.0 && surf_k > 0.004 && u_n_ssr > 0) n = wave_normal(v_uv, P, n);
    if (u_n_ssr > 0 && surf_k > 0.004) {

        if (u_use_rt_refl != 0 && !(u_refl_split != 0 && v_uv.x < 0.5)) {
            float upness = dot(n, u_up);
            vec3 V = normalize(P);
            vec3 R0 = reflect(V, n);
            float el0 = max(dot(R0, n), 0.0);
            vec4 racc = vec4(0.0);
            for (int ri = 0; ri < u_rt_refl_rays; ++ri) {
            vec3 R = R0;
            if (u_rt_refl_rays > 1 || m_rough > 0.0) {
                // Glossy jitter. The part along the normal is limited to half the ray's own elevation: at a grazing angle a fixed
                // angular jitter bent rays up by many times their elevation and smeared the horizon over the whole floor (High looked blurry).
                float ja = ign(gl_FragCoord.xy + 31.0 * float(ri + 1)) * 6.2831853, jb = ign(gl_FragCoord.yx + 57.0 * float(ri + 1)) * 2.0 - 1.0, jr = sqrt(1.0 - jb * jb);
                vec3 J = vec3(cos(ja) * jr, jb, sin(ja) * jr) * (u_rt_rough + m_rough * u_rough_jit);   // the material's roughness widens the lobe (the blur pass smooths the noise)
                float jn = dot(J, n);
                J += n * (clamp(jn, -0.5 * el0, 0.5 * el0) - jn);
                R = normalize(R0 + J);
            }
            vec3 O = P + n * (0.01 + 0.004 * lp);
            vec3 Ow = (u_view_inv * vec4(O, 1.0)).xyz;
            vec3 Rw = normalize((u_view_inv * vec4(R, 0.0)).xyz);
            vec3 col = u_rt_sky;
            float hit_t = u_rt_refl_len;
            bool hit = false;
            bool hidden = false;                 // the hit shows a flat triangle colour (not visible on screen)
            vec3 dbg = vec3(0.0, 0.0, 1.0);      // miss
            vec3 cut = vec3(0.0);                // cut-out cards passed through off screen, weighted by their coverage
            float trans = 1.0;
            float tmin = 0.0;
            // Up to 4 hits: a hit on a see-through texel of a cut-out card (tree, spire, fence: the triangle is a whole quad) is not
            // a reflector. On screen that shows as a hit point the picture does not draw (the picture is farther there, or sky):
            // trace on past it. Off screen the triangle's mean texture alpha (coverage) blends it with what lies behind.
            for (int pass = 0; pass < 4; ++pass) {
                rayQueryEXT rq;
                rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsOpaqueEXT, 0x1, Ow, tmin, Rw, u_rt_refl_len);
                while (rayQueryProceedEXT(rq)) {}
                hit = rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
                if (!hit) { hit_t = u_rt_refl_len; break; }
                hit_t = rayQueryGetIntersectionTEXT(rq, true);
                uint pc = rt_tri_col[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)];
                col = vec3(float(pc & 255u), float((pc >> 8) & 255u), float((pc >> 16) & 255u)) / 255.0;
                float cov = float(pc >> 24) / 255.0;
                dbg = vec3(1.0, 0.0, 0.0);       // hit, shaded by the triangle's colour
                hidden = true;
                vec3 H = O + R * hit_t;
                float lh = length(H);
                if (lh >= u_rt_sky_dist) break;  // the sky dome: shown from the sky map below
                vec4 ch = u_proj * vec4(H, 1.0);
                if (ch.w > 1e-5) {
                    vec2 uvh = ch.xy / ch.w * 0.5 + 0.5;
                    if (uvh.x > 0.0 && uvh.x < 1.0 && uvh.y > 0.0 && uvh.y < 1.0) {
                        float wdh = wd_at(uvh);
                        float ls = is_far(wdh) ? 1e9 : length(vpos(uvh, wdh));
                        if (abs(ls - lh) < 0.03 * lh + 0.05) { col = texture(u_color, uvh).rgb; dbg = vec3(0.0, 1.0, 0.0); hidden = false; break; }   // visible: the real shaded colour
                        if (ls > lh * 1.04 + 0.3) { tmin = hit_t + 0.05; hit = false; continue; }   // the picture shows what is behind it: a see-through texel
                    }
                }
                if (cov > 0.97 || pass == 3) break;
                cut += trans * cov * col * u_rt_hidden;
                trans *= 1.0 - cov;
                tmin = hit_t + 0.05;
                hit = false;
            }
            vec3 H = O + R * (hit ? hit_t : 500.0);
            bool sky = !hit || length(H) >= u_rt_sky_dist;    // the sky dome (or nothing): the floor already carries a baked sky, so it counts less
            if (sky) {
                vec4 sm = sky_map(Rw);                         // the stage's sky as seen so far, by world direction
                vec3 scr = u_rt_sky;
                vec4 ch = u_proj * vec4(H, 1.0);
                if (ch.w > 1e-5) {                              // old fallback: the picture's sky in that direction, if it is the sky there
                    vec2 uvs = clamp(ch.xy / ch.w * 0.5 + 0.5, vec2(0.02), vec2(0.98));
                    if (is_far(wd_at(uvs))) scr = texture(u_color, uvs).rgb;
                }
                col = mix(scr, sm.rgb, sm.a);
                hidden = false;
                dbg = vec3(1.0, 1.0, 0.0);
                hit_t = 0.25 * u_rt_refl_len;
            }
            if (hidden) col *= u_rt_hidden;      // a surface the camera cannot see (an underside, a back): its flat colour is unlit, so show it dim
            col = cut + trans * col;
            // The effects layer (orbs, auras, glass: blended draws that write no depth): the closest one in front of the opaque hit,
            // shown with its real on-screen colour when it is visible there, else its triangle colour, blended over the opaque reflection.
            vec3 fxc = vec3(0.0);   // summed effect colour x coverage
            float fxa = 0.0, fx_t = 0.0;
            int nfx = 0;
            if (u_rt_fx != 0 && !(u_rt_fx == 2 && v_uv.x < 0.5)) {   // 2 = debug split: no effects layer in the left half
                // The closest effect hit that is clearly above the reflecting surface: flat sheets lying on or just above the floor (Watera's
                // splash ring, Thundaga's ground ripples, decals) are hit at once by every ray leaving the floor and washed the whole area out.
                // Shaded with the effect triangle's own colour (texture average x vertex colour, coverage in alpha): most effects are drawn after
                // the 3D run, so the scene copy this pass reads does not hold them, and sampling it showed the floor behind the effect instead.
                // Effects are additive glows drawn in layers (a rail beam: a white core inside a green ribbon), so every effect triangle along
                // the ray adds its colour x coverage (up to 4), in ONE traversal: the effect instance is traced as non-opaque, each triangle the
                // traversal meets is a candidate that is summed and never committed. (Separate closest-hit queries per layer cost the RP6 8-10
                // fps during spells.)
                rayQueryEXT rf;
                rayQueryInitializeEXT(rf, u_tlas, gl_RayFlagsNoOpaqueEXT, 0x2, Ow, 0.0, Rw, hit ? hit_t : u_rt_refl_len);
                while (rayQueryProceedEXT(rf)) {
                    if (rayQueryGetIntersectionTypeEXT(rf, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
                    float t = rayQueryGetIntersectionTEXT(rf, false);
                    if (dot(R * t, n) < u_fx_minh) continue;   // height of the hit above the surface point
                    uint fc = rt_fx_col[rayQueryGetIntersectionPrimitiveIndexEXT(rf, false)];
                    float a = float(fc >> 24) / 255.0;
                    fxc += vec3(float(fc & 255u), float((fc >> 8) & 255u), float((fc >> 16) & 255u)) / 255.0 * a;
                    fx_t = fxa == 0.0 ? t : min(fx_t, t);   // the nearest one sets the distance fade
                    fxa = 1.0;
                    if (++nfx >= 4) rayQueryTerminateEXT(rf);
                }
            }
            float fres = fresnel_r(dot(n, -V), m_rough, m_metal);
            float k = clamp(fres * 1.6 + 0.08, 0.0, 1.0) * (hidden ? u_rt_hidden : 1.0) * (1.0 - hit_t / u_rt_refl_len * 0.5) * surf_k * fd * u_ssr_amt * (sky ? u_rt_sky_amt : 1.0);
            o_refl = vec4(col * k, k);
            if (fxa > 0.0) {
                // Effects are glows (beams, bolts, the water orb's highlights): their reflection adds light over the floor and its reflection
                // instead of covering them, with a stronger base than the Fresnel-only opaque reflection (same as the screen-space effects
                // layer, PSPRECOMP_SSR_FX_GAIN): at the floor's steep angles a glow at plain Fresnel strength was invisible.
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * (1.0 - fx_t / u_rt_refl_len * 0.5) * surf_k * fd * u_ssr_amt;
                o_refl.rgb += min(fxc, vec3(1.5)) * kf;   // fxc is already colour x coverage, summed
                if (u_rt_dbg != 0) dbg = vec3(1.0, 0.0, 1.0);
            }
            if (u_rt_dbg == 1 || (u_rt_dbg == 2 && v_uv.x > 0.5)) o_refl = vec4(dbg * 0.6, 0.6);
            racc += o_refl;
            }
            o_refl = racc / float(u_rt_refl_rays);
        } else

        {
        float upness = dot(n, u_up);
        vec3 V = normalize(P);
        vec3 R = reflect(V, n);
        float maxd = u_ssr_len * lp;
        float stepl = maxd / float(u_n_ssr);
        float jit = ign(gl_FragCoord.xy);
        vec3 O = P + n * (0.004 * lp);
        vec2 hit = vec2(-1.0);
        float hit_t = 0.0, prev_t = 0.0;
        vec2 last_in = vec2(-1.0);
        bool fx_hit = false;
        float fx_t = 0.0;
        vec4 fx_c = vec4(0.0);
        vec2 fx_uv = vec2(0.5);
        for (int i = 0; i < u_n_ssr; ++i) {
            float t = (float(i) + jit) * stepl;
            vec3 S = O + R * t;
            vec4 cl = u_proj * vec4(S, 1.0);
            if (cl.w <= 1e-5) break;
            vec2 uv2 = cl.xy / cl.w * 0.5 + 0.5;
            if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0) break;
            last_in = uv2;
            float wd2 = wd_at(uv2);
            if (u_have_fx != 0 && !fx_hit) {
                // effects layer: orbs, beams, auras, glass. They write no scene depth, so the ray is tested against the nearest effect
                // fragment's own depth; an effect later covered by opaque geometry (its depth behind the scene's) is not there.
                vec4 fc = texture(u_fxc, uv2);
                if (fc.a > 0.03 || max(fc.r, max(fc.g, fc.b)) > 0.06) {
                    float efd = texture(u_fxd, snap_uv(uv2)).r;
                    if ((is_far(wd2) || fx_floats(uv2, wd2, efd))) {
                        float de = length(vpos(uv2, efd)), ds = length(S);
                        if (ds > de - 0.02 * lp && ds < de + stepl * 2.5 + u_fx_thick) { fx_hit = true; fx_t = t; fx_c = fc; fx_uv = uv2; }
                    }
                }
            }
            if (!is_far(wd2)) {
                float diff = length(S) - length(vpos(uv2, wd2));
                if (diff > 0.0 && diff < stepl * 2.5 + 0.01 * lp) {
                    float lo = prev_t, hi = t;
                    for (int k = 0; k < 4; ++k) {           // refine between the previous and this step
                        float mid = 0.5 * (lo + hi);
                        vec3 Sm = O + R * mid;
                        vec4 cm = u_proj * vec4(Sm, 1.0);
                        vec2 um = cm.xy / cm.w * 0.5 + 0.5;
                        float wm = wd_at(um);
                        if (!is_far(wm) && length(Sm) - length(vpos(um, wm)) > 0.0) hi = mid; else lo = mid;
                    }
                    vec3 Sh = O + R * hi;
                    vec4 ch = u_proj * vec4(Sh, 1.0);
                    hit = ch.xy / ch.w * 0.5 + 0.5;
                    hit_t = hi;
                    break;
                }
            }
            prev_t = t;
        }
        if (hit.x < 0.0 && last_in.x >= 0.0 && is_far(wd_at(last_in))) {   // the ray left the scene into the sky
            hit = last_in; hit_t = maxd;
            // The sky (and the beams / clouds painted on it) lies in the ray's direction, at infinity: take the picture where that
            // direction projects, not where the short march happened to end (that showed the sky straight above the floor point).
            vec4 cs = u_proj * vec4(R, 0.0);
            if (cs.w > 1e-5) {
                vec2 us = cs.xy / cs.w * 0.5 + 0.5;
                if (us.x > 0.0 && us.x < 1.0 && us.y > 0.0 && us.y < 1.0 && is_far(wd_at(us))) hit = us;
            }
        }
        if (hit.x >= 0.0 || fx_hit) {
            vec3 col = vec3(0.0);
            vec2 euv = fx_uv;
            float ht = hit_t;
            if (hit.x >= 0.0) { col = texture(u_color, hit).rgb; euv = hit; }
            vec2 e = min(euv, 1.0 - euv);
            float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
            float fres = fresnel_r(dot(n, -V), m_rough, m_metal);
            float refl_k = surf_k * fd * u_ssr_amt;
            float k = hit.x >= 0.0 ? clamp(fres * 1.6 + 0.08, 0.0, 1.0) * edge * (1.0 - ht / maxd * 0.6) * refl_k : 0.0;
            o_refl = vec4(col * k, k);
            if (fx_hit) {
                // Effects are glowing / see-through things that read as bright reflections even at a steep angle on a wet floor, so they
                // get a stronger base than the Fresnel-only opaque reflection (an orb or beam at 14% was invisible).
                float efe = min(min(fx_uv.x, 1.0 - fx_uv.x), min(fx_uv.y, 1.0 - fx_uv.y));
                float kf = clamp(fres * 1.6 + u_fx_gain, 0.0, 1.0) * smoothstep(0.0, 0.12, efe) * (1.0 - fx_t / maxd * 0.5) * refl_k;
                float cov = clamp(max(fx_c.a, max(fx_c.r, max(fx_c.g, fx_c.b))), 0.0, 1.0);
                float af = cov * kf;
                o_refl = vec4(fx_c.rgb * kf + o_refl.rgb * (1.0 - fx_c.a * kf), af + o_refl.a * (1.0 - af));
            }
            if (u_fx_dbg == 1 && fx_hit) o_refl = vec4(0.8, 0.0, 0.8, 0.8);
        }
        }
    }
    n = n_geo;
    if (m_metal > 0.5 && o_refl.a > 0.0) {   // metal: the reflection takes the surface's colour (gold reflects gold)
        vec3 alb = texture(u_color, v_uv).rgb;
        float l = max(dot(alb, vec3(0.299, 0.587, 0.114)), 0.05);
        o_refl.rgb *= clamp(alb / l, 0.0, 2.0) * 0.85;
    }
    if (u_mask_dbg != 0 && u_n_ssr > 0) o_refl = vec4(0.7 * m_rough * surf_k, 0.7 * surf_k, 0.7 * m_metal * surf_k, 0.7 * surf_k);

    if (u_n_lights > 0 && u_use_rt != 0) {
        vec3 Pw = (u_view_inv * vec4(P, 1.0)).xyz;
        vec3 nw = normalize((u_view_inv * vec4(n, 0.0)).xyz);
        vec3 acc = vec3(0.0), acc_fx = vec3(0.0);
        // a fighter is not shadowed by its own body from these lights either (as k_rt_code does for the sun): without it the hair cards
        // threw blocky shadows over the face under Kefka's Tower's stage lights (issue #94)
        bool self_l = u_fmask != 0 && u_char_mode != 2 && fighter_at(v_uv);   // Sharp: lit and shadowed like the stage
        for (int li = 0; li < u_n_lights; ++li) {
            vec3 d = u_lp[li].xyz - Pw;
            float dist = length(d), rad = u_lp[li].w;
            if (dist >= rad || dist < 1e-3) continue;
            vec3 L = d / dist;
            float ndl = max(dot(nw, L), 0.0);
            // On a fighter the normal comes from the depth of a low-poly body: each facet turned to the light differently, the light
            // drew the facets as blocky patches (no shadow involved). Wrap lighting there: the facing only shades it gently.
            if (self_l && u_char_wrap != 0) ndl = 0.35 + 0.65 * (0.5 + 0.5 * dot(nw, L));
            if (li >= u_fx0 && u_fx_split != 0 && v_uv.x > 0.5) continue;
            float x = 1.0 - dist / rad, att = li >= u_fx0 ? x : x * x;   // an attack's flash carries further than a lamp's pool
            if (li >= u_fx0) att *= smoothstep(0.0, 1.5, dist);   // fades out right at the effect: a caster's hand glow must not blow out her own body
            if (ndl * att < 0.002) continue;
            rayQueryEXT rl;
            float ltrans = 1.0;
            if (self_l && u_char_mode == 0) {   // Off: the light reaches a fighter unblocked
                if (li >= u_fx0) acc_fx += u_lc[li].rgb * (ndl * att); else acc += u_lc[li].rgb * (ndl * att);
                continue;
            }
            if (self_l) {   // every triangle a candidate; a fighter's own (low bit of its coverage) within u_rt_self_len: passed through (Off) / part of the light (Soft)
                rayQueryInitializeEXT(rl, u_tlas, gl_RayFlagsNoOpaqueEXT, 0x1, Pw + nw * 0.06, 0.0, L, max(dist - 0.1 - u_lc[li].w, 0.01));
                while (rayQueryProceedEXT(rl)) {
                    if (rayQueryGetIntersectionTypeEXT(rl, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
                    uint pc = rt_tri_col[rayQueryGetIntersectionInstanceCustomIndexEXT(rl, false) + rayQueryGetIntersectionPrimitiveIndexEXT(rl, false)];
                    float th = rayQueryGetIntersectionTEXT(rl, false);
                    if ((pc & 0x01000000u) != 0u && th < u_rt_self_len) {
                        if (u_char_mode == 1 && th > 0.12) ltrans *= 1.0 - u_char_amt * float(pc >> 24) / 255.0;
                        continue;
                    }
                    rayQueryConfirmIntersectionEXT(rl);
                    rayQueryTerminateEXT(rl);
                }
            } else {
                rayQueryInitializeEXT(rl, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x1, Pw + nw * 0.06, 0.0, L, max(dist - 0.1 - u_lc[li].w, 0.01));
                while (rayQueryProceedEXT(rl)) {}
            }
            if (rayQueryGetIntersectionTypeEXT(rl, true) != gl_RayQueryCommittedIntersectionNoneEXT) continue;
            att *= ltrans;
            if (li >= u_fx0) acc_fx += u_lc[li].rgb * (ndl * att); else acc += u_lc[li].rgb * (ndl * att);
        }
        vec3 alb = texture(u_color, v_uv).rgb;
        // an attack's flash lights dark surfaces too (the scene colour is the albedo here, and a dark floor took almost nothing): effect lights
        // see at least u_fx_alb
        o_refl.rgb += (alb * acc + max(alb, vec3(u_fx_alb)) * acc_fx) * fd * u_lscale;
    }
    o_refl.rgb += mk * u_lscale;
    if (u_fmask_dbg != 0 && u_fmask != 0 && fighter_at(v_uv)) o_refl = vec4(0.9, 0.0, 0.9, 0.85);   // debug (PSPRECOMP_FMASK_DEBUG): the pixels marked as fighter

}
