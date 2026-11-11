// The few calls the runtime makes into cloud sync (psp_cloud_service): boot it with the game's folders, wait for the start-up
// sync before the first save dialog, push after a save, hand the Manage Saves screen its cloud hooks, shut down.
// PSPRECOMP_CLOUD=0 switches all of it off (nothing is read, started or contacted).
#pragma once

#include "psp_savemgr_ui.h"

#include <string>

// disc0_dir: the mapped disc0 folder (<workdir>/disc0), ms0_dir: the emulated memory stick folder. Loads cloud.ini and, when
// cloud sync is configured and automatic, starts the first sync in the background.
void psp_cloud_glue_boot(const std::string& disc0_dir, const std::string& ms0_dir);
// Blocks (up to 20 s) while the start-up sync is still running, so a download never replaces a save the game is reading.
void psp_cloud_glue_before_savedata();
void psp_cloud_glue_after_save();
void psp_cloud_glue_manager_hooks(psp_sddialog::ManagerConfig& mc);
void psp_cloud_glue_shutdown();
