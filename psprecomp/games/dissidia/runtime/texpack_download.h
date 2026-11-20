// games/dissidia/runtime/texpack_download.h — Settings > Graphics > Download Dissidia Recomp HD (texpack_download.cpp).
#pragma once
#include "psp_settings.h"

// Fills the hook's download fields: the game's own pack (id recomp-hd), downloaded from GitHub and installed when it is missing.
void dissidia_texpack_download_register(PspTexPackHook& hook);
