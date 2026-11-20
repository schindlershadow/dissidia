// games/dissidia/runtime/texpack_download.cpp — Settings > Graphics > Download Dissidia Recomp HD.
//
// The game's own HD texture pack, the one the installers' "HD textures" downloads (tools/fetch_textures.py --pack ai), installed from the
// game when it is missing: Niosa's D012FFRT (a PPSSPP pack, github.com/Niosa/D012FFRT) plus our own upscales of the other textures
// (github.com/schindlershadow/dissidia-hd-textures, <id>.png files), into <mods>/texpacks/recomp-hd (runtime/include/psp_texpacks.h).
// Both zips are streamed to disk (psp_download_to_file: about 1.3 GB, never in memory), unpacked with psp_texpacks_install_parts and deleted.
// Registered on the texture pack hook by hooks_io.cpp.
#include "texpack_download.h"

#include "psp_download.h"
#include "psp_texpacks.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace {

namespace fs = std::filesystem;

struct Download {
    const char* url;
    const char* file;
    uint64_t expect;   // about how big it is (codeload often sends no Content-Length): the progress estimate
};
const Download kDownloads[] = {
    {"https://codeload.github.com/Niosa/D012FFRT/zip/main", ".dl-d012ffrt.zip", 220ull << 20},
    {"https://codeload.github.com/schindlershadow/dissidia-hd-textures/zip/main", ".dl-hdtextures.zip", 1150ull << 20},
};
constexpr int kDownloadPct = 80;   // the downloads are 0-80 %, unpacking 80-100 %

bool fetch(const Download& d, const fs::path& to, uint64_t before, uint64_t all, std::atomic<int>* pct, std::string* err) {
    const uint64_t cap = d.expect - (d.expect >> 6);   // an estimate never reaches its end early
    return psp_download_to_file(d.url, to.u8string(), [&](uint64_t got) { pct->store(int((before + std::min(got, cap)) * kDownloadPct / all)); }, err);
}

bool download_recomp_hd(const std::string& mods_dir, std::atomic<int>* pct, std::string* err) {
    if (!psp_download_available()) { *err = "this build can not download over https"; return false; }
    const fs::path packs = fs::u8path(mods_dir) / "texpacks";
    std::error_code ec;
    fs::create_directories(packs, ec);
    uint64_t all = 0, before = 0;
    for (const Download& d : kDownloads) all += d.expect;
    std::vector<PspTexPackPart> parts;
    auto cleanup = [&] { for (const Download& d : kDownloads) fs::remove(packs / d.file, ec); };
    for (const Download& d : kDownloads) {
        if (!fetch(d, packs / d.file, before, all, pct, err)) { cleanup(); return false; }
        before += d.expect;
    }
    PspTexPackPart pack;   // Niosa's pack as it is, without its sources
    pack.zip = (packs / kDownloads[0].file).u8string();
    pack.skip_suffixes = {".psd", ".lnk", ".svg"};
    PspTexPackPart ids;    // our upscales: textures/<id>.png, flat (the runtime looks those up first)
    ids.zip = (packs / kDownloads[1].file).u8string();
    ids.ids_flat = true;
    const char* credits =
        "HD textures: D012FFRT by Niosa (https://github.com/Niosa/D012FFRT), the community retexture project (Credits.txt in this folder),\n"
        "plus AI upscales of the remaining textures by schindlershadow (https://github.com/schindlershadow/dissidia-hd-textures),\n"
        "made with Real-ESRGAN by Xintao Wang et al. (BSD-3-Clause, https://github.com/xinntao/Real-ESRGAN).\n"
        "The artwork belongs to Square Enix; this is an unofficial fan project.\n";
    const bool ok = psp_texpacks_install_parts(mods_dir, "recomp-hd", "Dissidia Recomp HD", "ai", credits, {pack, ids}, pct, kDownloadPct, 100, err);
    cleanup();
    return ok;
}

}  // namespace

void dissidia_texpack_download_register(PspTexPackHook& hook) {
    hook.download_id = "recomp-hd";
    hook.download_name = "Dissidia Recomp HD";
    hook.download_help = "Our HD texture pack, about 1.3 GB (WiFi is best; about 3 GB of free space while it unpacks): the D012FFRT community pack "
                         "by Niosa plus our own AI upscales of the other textures.";
    hook.download = download_recomp_hd;
}
