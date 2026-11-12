// GL-style GLSL (loose uniforms, version-less body) -> Vulkan GLSL 450 -> SPIR-V.
// The uber-shader and the user FX shaders (FX_SHADERS.md) are written against OpenGL: loose `uniform` declarations,
// location-less varyings. Vulkan has neither, so this converts the text instead of asking shader authors to change:
//   * every non-sampler `uniform` becomes a member of ONE std140 block (set 0, binding 0) shared by both stages;
//     `bool` uniforms are stored as int and the name is #defined back to `(name != 0)`;
//   * `uniform sampler2D` gets (set 1, binding n); in/out without a layout get a location (by name across stages);
//   * the vertex main() gets a wrapper that maps GL clip depth [-w,w] to Vulkan's [0,w].
// The block layout (offsets) is reported so the draw path can write values by uniform name.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pspvk {

struct UniformMember {
    std::string name;       // as in the GLSL source (a bool keeps its name; it is stored as an int)
    std::string type;       // GLSL type
    int array = 0;          // 0 = not an array
    uint32_t offset = 0;    // std140 byte offset in the block
    uint32_t size = 0;      // bytes occupied (array: stride * count)
    uint32_t stride = 0;    // array element stride (0 when not an array)
};

struct ConvertedProgram {
    std::string vertex, fragment;                 // GLSL 450, ready for glslang
    std::vector<UniformMember> uniforms;
    uint32_t ubo_size = 0;                        // rounded to 16
    std::vector<std::string> samplers;            // binding n = index, descriptor set 1
    bool uses_rt = false;                         // declares `uniform accelerationStructureEXT x;` -> set 2, binding 0 (ray query)
    uint32_t vertex_inputs = 0;                   // bit n: the vertex shader declares `layout(location = n) in`
    const UniformMember* find(const std::string& name) const;
};

/// vs/fs: GLSL bodies without "#version" (anything before the first declaration, e.g. "#define PSP_FX 1", goes in `defines`).
bool convert_glsl_to_vulkan(const std::string& defines, const std::string& vs, const std::string& fs,
                            ConvertedProgram* out, std::string* err);

/// Drops "#version" and "precision" lines (the GL front matter) from a GL shader source.
std::string strip_glsl_version(const std::string& src);

// Compute is for the ray-query passes (psp_vk_rt.h): compiled for Vulkan 1.2 / SPIR-V 1.4, which GL_EXT_ray_query needs.
enum class Stage { Vertex, Fragment, Compute };
/// glslang (not available when built with PSPVK_NO_GLSLANG, e.g. Android: those builds only load cached SPIR-V).
/// Thread-safe after the first call.
bool compile_to_spirv(Stage stage, const std::string& glsl450, std::vector<uint32_t>* spirv, std::string* err);

/// SPIR-V through the on-disk cache: the key is a hash of the exact Vulkan GLSL text, so the files are portable between
/// platforms (a desktop run fills the cache; an Android build without glslang loads it). Directory: PSPRECOMP_VK_CACHE_DIR,
/// else ~/.cache/dissidia-vk (XDG_CACHE_HOME). On a miss it compiles (when glslang is built in) and stores the result.
bool compile_cached(Stage stage, const std::string& glsl450, std::vector<uint32_t>* spirv, std::string* err);

}  // namespace pspvk
