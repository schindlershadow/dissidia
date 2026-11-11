// The two calls main.cpp makes into cloud sync (the rest of it lives inside the runtime core; see src/hle/psp_cloud_glue.h).
// This header is public (installed with the SDK) because main.cpp is compiled outside the core.
#pragma once

#include <string>

// disc0_dir: the mapped disc0 folder, ms0_dir: the emulated memory stick folder. Loads cloud.ini and starts the first sync in the
// background when cloud sync is on. PSPRECOMP_CLOUD=0 switches the whole feature off.
void psp_cloud_glue_boot(const std::string& disc0_dir, const std::string& ms0_dir);
// Lets a running upload finish (a few seconds at most).
void psp_cloud_glue_shutdown();
