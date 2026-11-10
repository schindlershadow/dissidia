// Self-extracting single-file release: psprecomp_setup(.exe) with a zip of the whole release folder appended
// (release/make_single.py writes it). Layout of the file:
//   [ the wizard program ][ zip payload ][ trailer: version[32] (NUL padded), payload size u64 LE, magic "PSPRPAY1" ]
// On first start the payload is unpacked into a per-user folder and the wizard runs from there (see main()).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace setup {

struct Payload {
    std::string version;
    uint64_t offset = 0;   // of the zip inside the file
    uint64_t size = 0;
};

constexpr size_t kTrailerSize = 48;

std::filesystem::path self_exe();
// Per-user folder the payload is unpacked into: %LOCALAPPDATA%\psprecomp (Windows), $XDG_DATA_HOME or ~/.local/share/psprecomp.
std::filesystem::path data_dir();
bool find_payload(const std::filesystem::path& exe, Payload& out);
// Unpacks into `dest` (through dest.partial, renamed when complete; a finished unpack is recognised by dest/.payload-ok and
// skipped). progress(fraction) may return false to cancel. Returns "" or an error message.
std::string extract_payload(const std::filesystem::path& exe, const Payload& p, const std::filesystem::path& dest,
                            const std::function<bool(double)>& progress);

}  // namespace setup
