// Ray-query shadow prototype test (src/vk/psp_vk_rt.h): builds a triangle soup (floor + random boxes), casts a ray per
// random point toward a light on the GPU (VK_KHR_ray_query), and compares with a CPU ray caster. SKIPs (exit 0) when no
// Vulkan device exposes ray query. Run on llvmpipe (software) or a real RT device: PSPRECOMP_VK_DEVICE=<index|name>.
#include "../src/vk/psp_vk_rt.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace pspvk;
struct V3 { double x, y, z; };
static V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Moller-Trumbore, both faces; t in (tmin, tmax)
static bool hit(V3 o, V3 d, const float* t, double tmin, double tmax) {
    V3 a{t[0], t[1], t[2]}, b{t[3], t[4], t[5]}, c{t[6], t[7], t[8]};
    V3 e1 = sub(b, a), e2 = sub(c, a), p = cross(d, e2);
    double det = dot(e1, p);
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    V3 s = sub(o, a);
    double u = dot(s, p) * inv; if (u < 0 || u > 1) return false;
    V3 q = cross(s, e1);
    double v = dot(d, q) * inv; if (v < 0 || u + v > 1) return false;
    double tt = dot(e2, q) * inv;
    return tt > tmin && tt < tmax;
}

static void quad(std::vector<float>& v, V3 a, V3 b, V3 c, V3 d) {
    const V3 p[6] = {a, b, c, a, c, d};
    for (auto& q : p) { v.push_back((float)q.x); v.push_back((float)q.y); v.push_back((float)q.z); }
}
static void box(std::vector<float>& v, V3 mn, V3 mx) {
    V3 c[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z}, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    quad(v, c[0], c[1], c[2], c[3]); quad(v, c[5], c[4], c[7], c[6]); quad(v, c[4], c[0], c[3], c[7]);
    quad(v, c[1], c[5], c[6], c[2]); quad(v, c[3], c[2], c[6], c[7]); quad(v, c[4], c[5], c[1], c[0]);
}

int main() {
    ContextOptions o; o.want_ray_query = true;
    if (const char* d = std::getenv("PSPRECOMP_VK_DEVICE")) { if (*d >= '0' && *d <= '9') o.device_index = std::atoi(d); else o.device_name = d; }
    Context c; std::string err;
    if (!c.init(o, &err)) { std::printf("SKIP: no Vulkan device: %s\n", err.c_str()); return 0; }
    if (!c.has_ray_query()) { std::printf("SKIP: %s has no ray query\n", c.device_name().c_str()); return 0; }
    std::printf("device %s\n", c.device_name().c_str());
    RtShadow rt;
    if (!rt.init(c, &err)) { std::printf("FAIL init: %s\n", err.c_str()); return 1; }

    std::mt19937 rng(7);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::vector<float> tris;
    quad(tris, {-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20});
    for (int i = 0; i < 40; i++) {
        double x = U(rng) * 30 - 15, z = U(rng) * 30 - 15, w = 0.5 + U(rng) * 2, h = 0.5 + U(rng) * 4;
        box(tris, {x, 0.0, z}, {x + w, h, z + w});
    }
    const uint32_t ntri = (uint32_t)(tris.size() / 9);
    if (!rt.build_scene(tris.data(), ntri, &err)) { std::printf("FAIL build: %s\n", err.c_str()); return 1; }
    std::printf("scene: %u triangles, build %.2f ms\n", ntri, rt.last_build_ms());

    const uint32_t N = 20000;
    std::vector<float> pts(N * 3);
    for (uint32_t i = 0; i < N; i++) { pts[i * 3] = (float)(U(rng) * 36 - 18); pts[i * 3 + 1] = (float)(0.02 + U(rng) * 5); pts[i * 3 + 2] = (float)(U(rng) * 36 - 18); }
    const float L[3] = {0.35f / 1.0f, 0.8f, 0.25f};
    const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const float dir[3] = {L[0] / ln, L[1] / ln, L[2] / ln};
    const float maxd = 60.f, bias = 0.001f;
    std::vector<uint8_t> gpu(N);
    if (!rt.trace_points(pts.data(), N, dir, maxd, bias, gpu.data(), &err)) { std::printf("FAIL trace: %s\n", err.c_str()); return 1; }
    uint32_t bad = 0, lit = 0, shadow = 0;
    for (uint32_t i = 0; i < N; i++) {
        V3 org{pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]}, d{dir[0], dir[1], dir[2]};
        bool ref = false;
        for (uint32_t t = 0; t < ntri && !ref; t++) ref = hit(org, d, &tris[t * 9], bias, maxd);
        (ref ? shadow : lit)++;
        if (ref != (gpu[i] != 0)) bad++;
    }
    std::printf("%u points: %u shadowed, %u lit by the CPU reference; %u mismatches; trace %.2f ms\n", N, shadow, lit, bad, rt.last_trace_ms());
    // Grazing rays can differ by one ulp between the two casters: allow a handful.
    if (bad > N / 500 || shadow == 0 || lit == 0) { std::printf("FAIL\n"); return 1; }
    std::printf("PASS\n");
    return 0;
}
