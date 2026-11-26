// PSP NPDRM EDAT (PGD) decryption: what sceNpDrmEdataSetupKey sets up for a file the game opened with the NPDRM flag.
//
// A downloadable-content file of a PSP game (Dissidia 012's ms0:/PSP/GAME/<id>/<8 hex digits>.edat) is a PSP "EDAT": the
// 8 bytes "\0PSPEDAT", a header, and at offset 0x90 a PGD container (AES, MACs and key derivation through KIRK / amctrl.prx)
// whose blocks hold the real file. The data is read through the PGD, so the game sees the plain file. A file without the
// magic is plain data and is read as it is (PPSSPP does the same: community DLC is often not encrypted).
//
// Ported from PPSSPP (GPL-2.0-or-later): ext/libkirk/amctrl.c (pgd_open, pgd_decrypt_block, sceDrmBB*) and the EDAT handling in
// Core/HLE/scePspNpDrm_user.cpp / sceIo.cpp. The licensee key (sceNpDrmSetLicenseeKey) is not part of the PGD key derivation
// of an EDAT, only of NPDRM modules, so it is not needed here. Host-testable: no dependency on the runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp_npdrm {

// True when `p` (>= 8 bytes) starts with the EDAT magic "\0PSPEDAT".
bool is_edat(const uint8_t* p, size_t n);

// Decrypts a whole EDAT file image into its plain data. 0 = ok. <0: not an EDAT (-1), a PGD header the keys do not open or a
// truncated file (-2). `plain` is cleared on failure.
int decrypt_edat(const uint8_t* file, size_t n, std::vector<uint8_t>& plain);

// Builds an EDAT image from plain data (drm type 1, key index 0: the fixed-key flavour; any other `drm_type` makes the per-console-key flavour), the inverse of decrypt_edat. For tests
// and tools that make an EDAT; `seed16` picks the keys, `block_size` is a power of two (>= 16). 0 = ok.
int encrypt_edat(const uint8_t* plain, size_t n, uint32_t block_size, const uint8_t seed16[16], std::vector<uint8_t>& file_out,
                 int drm_type = 1);

}  // namespace psp_npdrm
