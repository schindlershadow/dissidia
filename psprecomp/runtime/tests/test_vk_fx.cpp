// The shipped FX shaders (games/dissidia/fx_shaders) must still build on the Vulkan backend: assemble the program exactly
// as psp_fx_shader.cpp does (uber vertex shader with PSP_FX + the registry's fragment source), convert it with
// pspvk::convert_glsl_to_vulkan and compile both stages with glslang.
//   test_vk_fx <fx_shaders dir>
#include "psp_fx_shader.h"
#include "psp_ge_shader_src.h"
#include "../src/vk/psp_vk_glsl.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static bool read_file(const std::string& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); *out = ss.str();
    return true;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "games/dissidia/fx_shaders";
    // One draw per shipped fragment file; the hashes are placeholders.
    const char* manifest = R"JSON({"version":1,"draws":[
        {"name":"lit","hash":"00000000000000aa","frag":"lit/character.frag"},
        {"name":"lit_screen","hash":"00000000000000ab","frag":"lit/character.frag","screen":true}]})JSON";
    psp_fx::Manifest m; std::string err;
    if (!psp_fx::parse_manifest(manifest, m, &err)) { std::printf("FAIL manifest: %s\n", err.c_str()); return 1; }
    int failures = 0, built = 0;
    psp_fx::Registry reg;
    const int n = reg.build(m,
        [&](const std::string& rel, std::string* d) { return read_file(dir + "/" + rel, d); },
        [&](const psp_fx::ProgramSource& ps, std::string* log) -> unsigned {
            pspvk::ConvertedProgram p;
            if (!pspvk::convert_glsl_to_vulkan("", std::string("#define PSP_FX 1\n") + k_vertex_shader_src, pspvk::strip_glsl_version(ps.frag), &p, &err)) { *log = "convert: " + err; return 0; }
            std::vector<uint32_t> v, f;
            if (!pspvk::compile_to_spirv(pspvk::Stage::Vertex, p.vertex, &v, &err)) { *log = "vertex: " + err; return 0; }
            if (!pspvk::compile_to_spirv(pspvk::Stage::Fragment, p.fragment, &f, &err)) { *log = "fragment: " + err; return 0; }
            std::printf("built: ubo %u bytes, %zu uniforms, %zu samplers, vertex inputs 0x%x\n", p.ubo_size, p.uniforms.size(), p.samplers.size(), p.vertex_inputs);
            return (unsigned)++built;
        },
        [&](const std::string& l) { std::printf("log: %s\n", l.c_str()); failures++; });
    std::printf("%d usable entries, %d programs built\n", n, built);
    if (n < 1 || built < 1) failures++;
    std::printf(failures ? "FAILED\n" : "PASS\n");
    return failures ? 1 : 0;
}
