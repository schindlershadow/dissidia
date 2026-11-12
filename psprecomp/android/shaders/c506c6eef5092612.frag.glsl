#version 450
layout(std140, set = 0, binding = 0) uniform PspUniforms {
    vec2 u_size;
    float u_scale;
    int u_mode;
    int u_aa;
    float u_sharp;
};





layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 0) uniform sampler2D u_tex;






vec3 fetch(vec2 uv) { return texture(u_tex, uv).rgb; }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 bicubic(vec2 uv) {
    vec2 tc = uv * u_size;
    vec2 t1 = floor(tc - 0.5) + 0.5;
    vec2 f = tc - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 s12 = w1 + w2;
    vec2 t12 = (t1 + w2 / s12) / u_size;
    vec2 t0 = (t1 - 1.0) / u_size;
    vec2 t3 = (t1 + 2.0) / u_size;
    vec3 c = fetch(vec2(t0.x, t0.y)) * (w0.x * w0.y) + fetch(vec2(t12.x, t0.y)) * (s12.x * w0.y) + fetch(vec2(t3.x, t0.y)) * (w3.x * w0.y)
           + fetch(vec2(t0.x, t12.y)) * (w0.x * s12.y) + fetch(vec2(t12.x, t12.y)) * (s12.x * s12.y) + fetch(vec2(t3.x, t12.y)) * (w3.x * s12.y)
           + fetch(vec2(t0.x, t3.y)) * (w0.x * w3.y) + fetch(vec2(t12.x, t3.y)) * (s12.x * w3.y) + fetch(vec2(t3.x, t3.y)) * (w3.x * w3.y);
    return max(c, vec3(0.0));
}

vec3 center(vec2 uv) {
    if (u_mode == 2) return bicubic(uv);
    if (u_mode == 1 && u_scale > 1.0) {
        // Sharp bilinear: the blend between two texels is squeezed into about one output pixel.
        vec2 t = uv * u_size;
        vec2 fl = floor(t);
        vec2 cd = fract(t) - 0.5;                       // distance from the texel centre
        float rr = 0.5 - 0.5 / u_scale;                 // flat region around the centre
        vec2 f = (cd - clamp(cd, -rr, rr)) * u_scale + 0.5;
        return fetch((fl + f) / u_size);
    }
    return fetch(uv);
}

void main() {
    vec2 inv = 1.0 / u_size;
    vec3 m = center(v_uv);
    if (u_aa != 0) {
        // FXAA (the compact edge-directed blend): taps one texel away along the detected edge.
        vec3 nw = fetch(v_uv + vec2(-1.0, -1.0) * inv), ne = fetch(v_uv + vec2(1.0, -1.0) * inv);
        vec3 sw = fetch(v_uv + vec2(-1.0, 1.0) * inv),  se = fetch(v_uv + vec2(1.0, 1.0) * inv);
        float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
        float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
        float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
        vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
        float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
        float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
        dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * inv;
        vec3 a = 0.5 * (fetch(v_uv + dir * (1.0 / 3.0 - 0.5)) + fetch(v_uv + dir * (2.0 / 3.0 - 0.5)));
        vec3 b = a * 0.5 + 0.25 * (fetch(v_uv + dir * -0.5) + fetch(v_uv + dir * 0.5));
        float lb = luma(b);
        m = (lb < lmin || lb > lmax) ? a : b;
    }
    if (u_sharp > 0.0) {
        // Contrast-adaptive sharpening (after AMD CAS): sharpen less where the neighbourhood is already contrasty.
        vec3 n = fetch(v_uv + vec2(0.0, -1.0) * inv), s = fetch(v_uv + vec2(0.0, 1.0) * inv);
        vec3 w = fetch(v_uv + vec2(-1.0, 0.0) * inv), e = fetch(v_uv + vec2(1.0, 0.0) * inv);
        vec3 mn = min(m, min(min(n, s), min(w, e)));
        vec3 mx = max(m, max(max(n, s), max(w, e)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, u_sharp));
        m = clamp((m + wgt * (n + s + w + e)) / (1.0 + 4.0 * wgt), 0.0, 1.0);
    }
    o_color = vec4(m, 1.0);
}
