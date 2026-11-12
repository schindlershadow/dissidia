// Custom Vulkan drivers for Android (Adreno): the same idea as Eden / Dolphin / PPSSPP. A driver is a "adrenotools package": a zip with
// meta.json + one vulkan.*.so (Mesa Turnip builds, newer Qualcomm builds). The game loads it through libadrenotools instead of the
// phone's own Vulkan driver, which is how a phone gets Vulkan extensions its vendor driver lacks (VK_KHR_ray_query on Adreno 740+).
//
// This file holds everything around that: the folder of installed drivers, the live list of drivers that can be downloaded (GitHub
// releases of a public package feed), the download + install, and the hook psp_vk_loader.cpp calls to open Vulkan through a driver.
// The list / zip / meta parts are plain C++ and run (and are tested) on any host; only open_vulkan() is Android-only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace psp_gpu {

struct Installed {
    std::string id;        // folder name under root_dir()
    std::string name;      // meta.json "name"
    std::string version;   // meta.json "driverVersion"
    std::string library;   // meta.json "libraryName": the .so inside the folder
    int min_api = 0;       // meta.json "minApi"
};

struct Remote {
    std::string id;        // package id: the asset's file name without ".zip" (also the folder it installs to)
    std::string name;      // shown in the menu: the release title + the asset
    std::string url;       // browser_download_url
    std::string published; // ISO date of the release
    uint64_t size = 0;
};

struct Status {
    enum State { Idle, Busy, Error, Done } state = Idle;
    std::string text;      // one line for the menu
    unsigned revision = 0; // changes whenever anything above does (the menu redraws on it)
};

// Folder holding the installed drivers. Android: <app internal storage>/gpu_drivers (dlopen of a driver from the sd card is refused);
// PSPRECOMP_GPU_DRIVER_DIR overrides (psp_android_init sets it). Not created until something is installed.
std::string root_dir();

// Whether this device can use a custom driver at all: Android, arm64, an Adreno GPU (/dev/kgsl-3d0) and libadrenotools built in.
bool supported(std::string* why = nullptr);

std::vector<Installed> installed();
bool remove(const std::string& id, std::string* err = nullptr);

// The driver chosen in the menu (PSPRECOMP_GPU_DRIVER, applied from the saved settings at start-up); "" = the phone's own driver.
std::string selected();
// A driver that crashed (or hung) while Vulkan was being opened is not tried again until the user picks one: the crash marker
// is written before the driver is opened and cleared once the device exists. These manage it.
bool crashed(const std::string& id);
void clear_crash(const std::string& id);
void note_started();              // Vulkan came up (or fell back cleanly): clear the "opening" marker

// ---- the live list ----
// refresh() downloads the feed on a worker thread; catalog() is what it found (newest first). The feed is the GitHub releases API of
// the repositories in PSPRECOMP_GPU_DRIVER_FEEDS (comma separated "owner/repo"; default K11MCH1/AdrenoToolsDrivers).
void refresh();
std::vector<Remote> catalog();
bool catalog_loaded();
void install(const Remote& r);    // download + unpack on a worker thread; progress in status()
Status status();

// ---- pure helpers (exposed for tests) ----
bool parse_releases(const std::string& json, std::vector<Remote>* out, std::string* err = nullptr);   // GitHub /releases JSON
// Tidy a parsed list for the menu: drops the memory-mode variants (Gmem / Sysmem) and packages for a newer GPU generation than
// `gpu_gen` (7 = Adreno 7xx; 0 = unknown: keep all), puts Turnip builds first, then other community builds, Qualcomm's own last; newest
// first within each group. Names become readable ("Turnip v26.0.0 R8").
void curate(std::vector<Remote>* list, int gpu_gen);
int gpu_generation();             // 6 / 7 / 8 from /sys/class/kgsl/kgsl-3d0/gpu_model ("Adreno740"), 0 when unknown
bool parse_meta(const std::string& json, Installed* out);
// Unpack an adrenotools package into `dir` (created): meta.json and the library it names, nothing else (no path from the zip is trusted).
bool unpack_package(const std::vector<uint8_t>& zip, const std::string& dir, Installed* out, std::string* err);
std::string sanitize_id(const std::string& s);

#if defined(__ANDROID__)
// Opens Vulkan through the selected driver; returns its vkGetInstanceProcAddr (as void*), or nullptr (the caller falls back to the phone's
// driver; *err says why when something went wrong, empty when no driver is selected).
void* open_vulkan(std::string* err);
bool custom_driver_active();
#endif

}  // namespace psp_gpu
