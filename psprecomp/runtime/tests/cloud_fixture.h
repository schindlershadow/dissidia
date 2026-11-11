// Shared fixtures for the cloud-sync host tests: synthetic PSP save folders (PARAM.SFO + one data file) signed the way the
// runtime does, and small file helpers.
#pragma once
#include "psp_cloudsync.h"
#include "psp_savedata_crypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using psp_cloud::Bytes;

static inline Bytes slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), {}); }
static inline void spit(const fs::path& p, const Bytes& b) { std::ofstream f(p, std::ios::binary); f.write((const char*)b.data(), (std::streamsize)b.size()); }

struct E { const char* key; uint16_t fmt; uint32_t max; Bytes v; };
static inline Bytes make_sfo(const std::vector<E>& es) {
    Bytes keys; std::vector<uint32_t> kofs;
    for (auto& e : es) { kofs.push_back((uint32_t)keys.size()); for (const char* c = e.key; *c; ++c) keys.push_back((uint8_t)*c); keys.push_back(0); }
    uint32_t kt = 20 + 16 * (uint32_t)es.size(), dt = (kt + (uint32_t)keys.size() + 3) & ~3u;
    Bytes out(dt + 0, 0);
    auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) out[o + i] = uint8_t(v >> (8 * i)); };
    auto w16 = [&](size_t o, uint16_t v) { out[o] = uint8_t(v); out[o + 1] = uint8_t(v >> 8); };
    std::memcpy(out.data(), "\0PSF", 4); w32(4, 0x101); w32(8, kt); w32(12, dt); w32(16, (uint32_t)es.size());
    uint32_t doff = 0;
    for (size_t i = 0; i < es.size(); ++i) {
        w16(20 + 16 * i, (uint16_t)kofs[i]); w16(22 + 16 * i, es[i].fmt); w32(24 + 16 * i, (uint32_t)es[i].v.size());
        w32(28 + 16 * i, es[i].max); w32(32 + 16 * i, doff); doff += es[i].max;
    }
    std::memcpy(out.data() + kt, keys.data(), keys.size());
    for (auto& e : es) { Bytes d = e.v; d.resize(e.max, 0); out.insert(out.end(), d.begin(), d.end()); }
    while (out.size() % 16) out.push_back(0);
    return out;
}
static inline Bytes str(const std::string& s) { Bytes b(s.begin(), s.end()); b.push_back(0); return b; }

static inline void make_save(const fs::path& root, const std::string& name, const std::string& title, const std::string& detail,
                      const std::string& file, uint8_t seed) {
    fs::path d = root / name;
    fs::create_directories(d);
    Bytes fl(3168, 0);
    std::memcpy(fl.data(), file.c_str(), file.size());
    Bytes params(128, 0); params[0] = 0x41;
    Bytes sfo = make_sfo({{"CATEGORY", 0x204, 4, str("MS")}, {"SAVEDATA_DETAIL", 0x204, 1024, str(detail)},
                          {"SAVEDATA_DIRECTORY", 0x204, 64, str(name)}, {"SAVEDATA_FILE_LIST", 4, 3168, fl},
                          {"SAVEDATA_PARAMS", 4, 128, params}, {"SAVEDATA_TITLE", 0x204, 128, str("Slot")},
                          {"TITLE", 0x204, 128, str(title)}});
    // sign it the way the runtime does (so a re-sign of an unchanged file must be a no-op)
    {
        auto g32 = [&](size_t o) { return uint32_t(sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | uint32_t(sfo[o + 3]) << 24); };
        auto g16 = [&](size_t o) { return uint16_t(sfo[o] | sfo[o + 1] << 8); };
        uint32_t kt = g32(8), dt = g32(12), n = g32(16);
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp((const char*)&sfo[kt + g16(20 + 16 * i)], "SAVEDATA_PARAMS") == 0)
                psp_sdcrypto::update_sfo_hash(sfo, dt + g32(20 + 16 * i + 12), psp_sdcrypto::crypt_mode_from_params_flag(0x41));
    }
    spit(d / "PARAM.SFO", sfo);
    Bytes data(300);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(seed * 7 + i);
    spit(d / file, data);
    spit(d / "ICON0.PNG", Bytes{1, 2, 3});
    spit(d / "stray.sync-conflict.BIN", Bytes{9});
}



// ---- a simulated install ----
struct Dev {
    std::string root, disc;
    std::string saves() const { return root + "/ms0/PSP/SAVEDATA"; }
    psp_cloud::Options opts() const {
        psp_cloud::Options o;
        o.saves_root = saves();
        o.backup_root = root + "/backup";
        o.state_path = root + "/sync/state.txt";
        o.disc_id = disc;
        return o;
    }
    fs::path dir(const std::string& suffix) const { return fs::path(saves()) / (disc + suffix); }
    void change(const std::string& suffix, const std::string& file, uint8_t seed) const {
        Bytes d(300);
        for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(seed * 11 + i);
        spit(dir(suffix) / file, d);
    }
};

