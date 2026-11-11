// Converts the shipped uber-shader (and its PSP_FX variant) to Vulkan GLSL, compiles both stages to SPIR-V and checks the
// reported std140 block layout. No GPU needed.
#include "../src/vk/psp_vk_glsl.h"
#include "psp_ge_shader_src.h"
#include <cstdio>

using namespace pspvk;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void one(const char* tag, const std::string& defines) {
    ConvertedProgram p; std::string err;
    if (!convert_glsl_to_vulkan(defines, k_vertex_shader_src, k_fragment_shader_src, &p, &err)) { std::printf("FAIL %s convert: %s\n", tag, err.c_str()); fails++; return; }
    std::vector<uint32_t> v, f;
    if (!compile_to_spirv(Stage::Vertex, p.vertex, &v, &err)) { std::printf("FAIL %s vertex: %s\n", tag, err.c_str()); fails++; }
    if (!compile_to_spirv(Stage::Fragment, p.fragment, &f, &err)) { std::printf("FAIL %s fragment: %s\n", tag, err.c_str()); fails++; }
    std::printf("%s: vertex %zu words, fragment %zu words, ubo %u bytes, %zu uniforms\n", tag, v.size(), f.size(), p.ubo_size, p.uniforms.size());
    CHECK(!v.empty() && v[0] == 0x07230203u);
    CHECK(p.samplers.size() == 1 && p.samplers[0] == "u_texture");
    const UniformMember* w = p.find("u_world"); const UniformMember* lt = p.find("u_lt"); const UniformMember* b = p.find("u_bones");
    CHECK(w && w->offset % 16 == 0 && w->size == 64);
    CHECK(b && b->array == 8 && b->stride == 64 && b->size == 512);
    CHECK(lt && lt->array == 20 && lt->size == 320);
    const UniformMember* cr = p.find("u_color_test_ref");
    CHECK(cr && cr->offset % 16 == 0 && cr->size == 12);
    for (auto& u : p.uniforms) for (auto& o : p.uniforms) if (&u != &o) CHECK(u.offset + u.size <= o.offset || o.offset + o.size <= u.offset);   // no overlaps
    CHECK(p.ubo_size % 16 == 0);
}

int main() {
    one("uber", "");
    one("uber+FX", "#define PSP_FX 1\n");
    // A bool in the second stage only, mismatching types, and unsupported types are reported.
    ConvertedProgram p; std::string err;
    CHECK(!convert_glsl_to_vulkan("", "void main(){}", "uniform float a;\nuniform int a;\nvoid main(){}", &p, &err));
    CHECK(!convert_glsl_to_vulkan("", "void main(){}", "uniform dmat4 m;\nvoid main(){}", &p, &err));
    std::printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}
