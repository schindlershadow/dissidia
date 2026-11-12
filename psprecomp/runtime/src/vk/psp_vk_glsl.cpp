#include "psp_vk_glsl.h"
#ifndef PSPVK_NO_GLSLANG
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

namespace pspvk {

const UniformMember* ConvertedProgram::find(const std::string& name) const {
    for (auto& u : uniforms) if (u.name == name) return &u;
    return nullptr;
}

namespace {

struct TypeInfo { uint32_t size, align; };
bool type_info(const std::string& t, TypeInfo* ti) {
    static const std::map<std::string, TypeInfo> m = {
        {"float", {4, 4}}, {"int", {4, 4}}, {"uint", {4, 4}}, {"bool", {4, 4}},
        {"vec2", {8, 8}}, {"ivec2", {8, 8}}, {"uvec2", {8, 8}},
        {"vec3", {12, 16}}, {"ivec3", {12, 16}}, {"uvec3", {12, 16}},
        {"vec4", {16, 16}}, {"ivec4", {16, 16}}, {"uvec4", {16, 16}},
        {"mat2", {32, 16}}, {"mat3", {48, 16}}, {"mat4", {64, 16}},   // std140: matrix columns are padded to vec4
    };
    auto it = m.find(t);
    if (it == m.end()) return false;
    *ti = it->second;
    return true;
}

struct Shared {
    std::vector<UniformMember> uniforms;
    std::vector<std::string> samplers;
    std::map<std::string, int> varying_loc, attr_loc;
    std::map<std::string, int> frag_out;
    bool rt = false;
};

const std::regex& re_uniform() {
    static const std::regex r(R"(^\s*uniform\s+(\w+)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*;)");
    return r;
}
const std::regex& re_io() {
    static const std::regex r(R"(^\s*(in|out)\s+((?:flat\s+|smooth\s+|noperspective\s+)?\w+)\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*;)");
    return r;
}

bool convert_stage(Stage st, const std::string& src, Shared& sh, std::string* body, std::string* err) {
    std::istringstream in(src);
    std::string line, out;
    while (std::getline(in, line)) {
        std::smatch m;
        static const std::regex re_vid(R"(\bgl_VertexID\b)"), re_iid(R"(\bgl_InstanceID\b)");
        if (line.find("gl_VertexID") != std::string::npos) line = std::regex_replace(line, re_vid, "gl_VertexIndex");
        if (line.find("gl_InstanceID") != std::string::npos) line = std::regex_replace(line, re_iid, "gl_InstanceIndex");
        if (std::regex_search(line, m, re_uniform())) {
            const std::string type = m[1], name = m[2];
            const int arr = m[3].matched ? std::stoi(m[3]) : 0;
            if (type == "accelerationStructureEXT") {
                sh.rt = true;
                out += "layout(set = 2, binding = 0) uniform accelerationStructureEXT " + name + ";\n";
                out += "layout(set = 2, binding = 1, std430) readonly buffer RtTriColBlock { uint rt_tri_col[]; };\n";   // per-triangle RGBA8 (psp_rt.h)
                out += "layout(set = 2, binding = 2, std430) readonly buffer RtFxColBlock { uint rt_fx_col[]; };\n";   // the effects layer's
                continue;
            }
            if (type.rfind("sampler", 0) == 0) {
                auto it = std::find(sh.samplers.begin(), sh.samplers.end(), name);
                size_t b = it - sh.samplers.begin();
                if (it == sh.samplers.end()) sh.samplers.push_back(name);
                out += "layout(set = 1, binding = " + std::to_string(b) + ") uniform " + type + " " + name + ";\n";
                continue;
            }
            TypeInfo ti;
            if (!type_info(type, &ti)) { if (err) *err = "unsupported uniform type '" + type + "' for " + name; return false; }
            bool known = false;
            for (auto& u : sh.uniforms) if (u.name == name) {
                known = true;
                if (u.type != type || u.array != arr) { if (err) *err = "uniform '" + name + "' declared with different types in the two stages"; return false; }
            }
            if (!known) { UniformMember u; u.name = name; u.type = type; u.array = arr; sh.uniforms.push_back(u); }
            out += "\n";   // keep line numbers; the declaration moves into the block
            continue;
        }
        if (std::regex_search(line, m, re_io())) {
            const bool is_in = m[1] == "in";
            const std::string name = m[3];
            int loc;
            if (st == Stage::Vertex && is_in) loc = sh.attr_loc.emplace(name, (int)sh.attr_loc.size()).first->second;
            else if (st == Stage::Fragment && !is_in) loc = sh.frag_out.emplace(name, (int)sh.frag_out.size()).first->second;
            else loc = sh.varying_loc.emplace(name, (int)sh.varying_loc.size()).first->second;
            out += "layout(location = " + std::to_string(loc) + ") " + line + "\n";
            continue;
        }
        out += line + "\n";
    }
    *body = out;
    return true;
}

void layout_block(std::vector<UniformMember>& us, uint32_t* total) {
    uint32_t off = 0;
    for (auto& u : us) {
        TypeInfo ti{};
        type_info(u.type, &ti);
        if (u.array) {
            const uint32_t stride = (ti.size + 15) & ~15u;
            u.stride = stride; u.size = stride * (uint32_t)u.array;
            off = (off + 15) & ~15u;
        } else {
            u.stride = 0; u.size = ti.size;
            off = (off + ti.align - 1) & ~(ti.align - 1);
        }
        u.offset = off;
        off += u.size;
    }
    *total = (off + 15) & ~15u;
}

std::string block_text(const std::vector<UniformMember>& us) {
    std::string b = "layout(std140, set = 0, binding = 0) uniform PspUniforms {\n";
    std::string defs;
    for (auto& u : us) {
        if (u.type == "bool") {
            b += "    int " + u.name + "__i;\n";
            defs += "#define " + u.name + " (" + u.name + "__i != 0)\n";
        } else {
            b += "    " + u.type + " " + u.name + (u.array ? "[" + std::to_string(u.array) + "]" : "") + ";\n";
        }
    }
    return b + "};\n" + defs;
}

}  // namespace

std::string strip_glsl_version(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        std::string line = s.substr(i, e == std::string::npos ? std::string::npos : e - i);
        size_t k = line.find_first_not_of(" \t");
        if (!(k != std::string::npos && (line.compare(k, 8, "#version") == 0 || line.compare(k, 10, "precision ") == 0))) out += line;
        out += "\n";
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

bool convert_glsl_to_vulkan(const std::string& defines, const std::string& vs, const std::string& fs,
                            ConvertedProgram* out, std::string* err) {
    Shared sh;
    std::string vb, fb;
    if (!convert_stage(Stage::Vertex, vs, sh, &vb, err) || !convert_stage(Stage::Fragment, fs, sh, &fb, err)) return false;
    // Pass 1 saw every declaration, so the block can be finalised; bool uniforms keep their GL-visible name via #define.
    // A bool uniform named like a #define target would recurse, which GLSL forbids; the sources never do that.
    uint32_t total = 0;
    layout_block(sh.uniforms, &total);
    const std::string blk = block_text(sh.uniforms);
    const std::string head = std::string(sh.rt ? "#version 460\n#extension GL_EXT_ray_query : require\n" : "#version 450\n") + defines + blk;

    // The vertex main(): GL clip z in [-w, w] -> Vulkan [0, w] (y is handled by the viewport's negative height).
    size_t p = vb.find("void main()");
    if (p == std::string::npos) { if (err) *err = "vertex shader has no main()"; return false; }
    vb.replace(p, 11, "void ge_gl_main()");
    vb += "\nvoid main() {\n    ge_gl_main();\n    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";

    out->vertex_inputs = 0;
    {
        static const std::regex re_in(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s)");
        for (auto it = std::sregex_iterator(vb.begin(), vb.end(), re_in); it != std::sregex_iterator(); ++it)
            out->vertex_inputs |= 1u << std::stoi((*it)[1]);
    }
    out->vertex = head + vb;
    out->fragment = head + fb;
    out->uniforms = sh.uniforms;
    out->ubo_size = total;
    out->samplers = sh.samplers;
    out->uses_rt = sh.rt;
    return true;
}

#ifdef PSPVK_NO_GLSLANG
bool compile_to_spirv(Stage, const std::string&, std::vector<uint32_t>*, std::string* err) {
    if (err) *err = "this build has no GLSL compiler (PSPVK_NO_GLSLANG)";
    return false;
}
#else
bool compile_to_spirv(Stage stage, const std::string& glsl, std::vector<uint32_t>* spirv, std::string* err) {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
    const EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : stage == Stage::Fragment ? EShLangFragment : EShLangCompute;
    const bool v12 = stage == Stage::Compute || glsl.find("GL_EXT_ray_query") != std::string::npos;
    glslang::TShader shader(lang);
    const char* s = glsl.c_str();
    shader.setStrings(&s, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, v12 ? glslang::EShTargetVulkan_1_2 : glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, v12 ? glslang::EShTargetSpv_1_4 : glslang::EShTargetSpv_1_3);
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
        if (err) *err = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram prog;
    prog.addShader(&shader);
    if (!prog.link(msgs)) { if (err) *err = std::string(prog.getInfoLog()) + prog.getInfoDebugLog(); return false; }
    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = true;   // the driver optimises; avoids needing SPIRV-Tools-opt at runtime
    glslang::GlslangToSpv(*prog.getIntermediate(lang), *spirv, &logger, &opts);
    return !spirv->empty();
}
#endif

static std::string cache_dir() {
    if (const char* d = std::getenv("PSPRECOMP_VK_CACHE_DIR")) if (*d) return d;
    const char* x = std::getenv("XDG_CACHE_HOME");
    const char* h = std::getenv("HOME");
    if (x && *x) return std::string(x) + "/dissidia-vk";
    if (h && *h) return std::string(h) + "/.cache/dissidia-vk";
    return "";
}

bool compile_cached(Stage stage, const std::string& glsl, std::vector<uint32_t>* spirv, std::string* err) {
    const std::string dir = cache_dir();
    char name[64] = {};
    if (!dir.empty()) {
        uint64_t h = 0xcbf29ce484222325ull ^ 0x5053ull;   // salt: bump when the converter's output semantics change
        for (unsigned char c : glsl) { h ^= c; h *= 0x100000001b3ull; }
        h ^= stage == Stage::Vertex ? 1 : stage == Stage::Fragment ? 2 : 3; h *= 0x100000001b3ull;
        std::snprintf(name, sizeof name, "/%016llx.%s.spv", (unsigned long long)h, stage == Stage::Vertex ? "vert" : stage == Stage::Fragment ? "frag" : "comp");
        if (FILE* f = std::fopen((dir + name).c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0 && n % 4 == 0) {
                spirv->resize((size_t)n / 4);
                if (std::fread(spirv->data(), 1, (size_t)n, f) == (size_t)n) { std::fclose(f); return true; }
            }
            std::fclose(f);
        }
    }
    if (!compile_to_spirv(stage, glsl, spirv, err)) {
        if (err && !dir.empty() && err->find("PSPVK_NO_GLSLANG") != std::string::npos)
        {
            *err = "no precompiled SPIR-V for " + std::string(name + 1) + " in " + dir + " (run the game once on a desktop build with Vulkan and copy the folder)";
            // Keep the exact text so tools/vk_spirv_tool can compile it on a machine that has glslang.
            mkdir((dir + "/miss").c_str(), 0755);
            if (FILE* f = std::fopen((dir + "/miss" + std::string(name, std::strlen(name) - 4) + ".glsl").c_str(), "wb")) { std::fwrite(glsl.data(), 1, glsl.size(), f); std::fclose(f); }
        }
        return false;
    }
    if (!dir.empty()) {
        mkdir(dir.c_str(), 0755);
        if (FILE* f = std::fopen((dir + name).c_str(), "wb")) { std::fwrite(spirv->data(), 4, spirv->size(), f); std::fclose(f); }
    }
    return true;
}

}  // namespace pspvk
