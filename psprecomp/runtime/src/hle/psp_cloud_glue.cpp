#include "psp_cloud_glue.h"

#include "psp_cloud_service.h"
#include "psp_savemgr.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace svc = psp_cloud_svc;
namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_on{false};
bool disabled() {
    const char* e = std::getenv("PSPRECOMP_CLOUD");
    return e && e[0] == '0';
}
}  // namespace

void psp_cloud_glue_boot(const std::string& disc0_dir, const std::string& ms0_dir) {
    if (disabled()) { std::fprintf(stderr, "[CLOUD] disabled (PSPRECOMP_CLOUD=0)\n"); return; }
    svc::Paths p;
    const fs::path ms0 = fs::u8path(ms0_dir);
    std::error_code ec;
    p.workdir = fs::weakly_canonical(ms0, ec).parent_path().u8string();
    p.saves_root = (ms0 / "PSP" / "SAVEDATA").u8string();
    p.backup_root = (ms0 / "PSP" / "SAVEDATA_BACKUP").u8string();
    p.disc_id = psp_savemgr::installed_disc_id(fs::u8path(disc0_dir).parent_path().u8string());
    if (!psp_savemgr::valid_disc_id(p.disc_id)) p.disc_id.clear();
    svc::init(p);
    g_on = true;
    if (svc::configured()) {
        const svc::Config c = svc::config();
        std::fprintf(stderr, "[CLOUD] %s sync %s (disc %s)\n", svc::provider_name(c.provider), c.auto_sync ? "on" : "manual only",
                     p.disc_id.empty() ? "?" : p.disc_id.c_str());
        svc::startup_sync();
    }
}

void psp_cloud_glue_before_savedata() {
    if (!g_on) return;
    if (!svc::wait_ready(20000)) std::fprintf(stderr, "[CLOUD] start-up sync still running after 20 s; continuing\n");
}

void psp_cloud_glue_after_save() {
    if (g_on) svc::after_save();
}

void psp_cloud_glue_manager_hooks(psp_sddialog::ManagerConfig& mc) {
    if (!g_on) return;
    mc.cloud_status = [] { return svc::status(); };
    mc.cloud_config = [] { return svc::config(); };
    mc.cloud_configured = [] { return svc::configured(); };
    mc.cloud_sync = [] { svc::sync_now(); };
    mc.cloud_signin = [] { svc::signin_start(); };
    mc.cloud_signin_cancel = [] { svc::signin_cancel(); };
    mc.cloud_signout = [] { svc::signout(); };
    mc.cloud_resolve = [](const std::string& s, psp_cloud::Choice c) { svc::resolve(s, c); };
}

void psp_cloud_glue_shutdown() {
    if (!g_on) return;
    svc::shutdown(4000);       // lets a running upload finish
    g_on = false;
}
