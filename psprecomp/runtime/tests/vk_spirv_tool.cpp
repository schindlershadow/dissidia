// Compiles the Vulkan GLSL files a device build saved in <cache>/miss (psp_vk_glsl.cpp, compile_cached) to SPIR-V:
//   vk_spirv_tool <cache dir>      reads <cache>/miss/*.vert.glsl|*.frag.glsl, writes <cache>/<name>.spv
#include "../src/vk/psp_vk_glsl.h"
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: vk_spirv_tool <cache dir>\n"); return 2; }
    const std::string dir = argv[1];
    DIR* d = opendir((dir + "/miss").c_str());
    if (!d) { std::fprintf(stderr, "no %s/miss\n", dir.c_str()); return 1; }
    int ok = 0, bad = 0;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        const bool vert = n.size() > 10 && n.substr(n.size() - 10) == ".vert.glsl";
        const bool frag = n.size() > 10 && n.substr(n.size() - 10) == ".frag.glsl";
        if (!vert && !frag) continue;
        std::ifstream in(dir + "/miss/" + n, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        std::vector<uint32_t> spv; std::string err;
        if (!pspvk::compile_to_spirv(vert ? pspvk::Stage::Vertex : pspvk::Stage::Fragment, ss.str(), &spv, &err)) { std::printf("FAIL %s: %s\n", n.c_str(), err.c_str()); bad++; continue; }
        const std::string out = dir + "/" + n.substr(0, n.size() - 5) + ".spv";
        if (FILE* f = std::fopen(out.c_str(), "wb")) { std::fwrite(spv.data(), 4, spv.size(), f); std::fclose(f); ok++; }
    }
    closedir(d);
    std::printf("%d compiled, %d failed\n", ok, bad);
    return bad ? 1 : 0;
}
