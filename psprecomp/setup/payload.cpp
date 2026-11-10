#include "payload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#include "third_party/miniz.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace setup {

static FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

fs::path self_exe() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return fs::path(buf);
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

fs::path data_dir() {
#ifdef _WIN32
    if (const char* d = std::getenv("LOCALAPPDATA")) return fs::path(d) / "DissidiaRecomp";
    if (const char* d = std::getenv("APPDATA")) return fs::path(d) / "DissidiaRecomp";
    return fs::path("DissidiaRecomp");
#else
    if (const char* d = std::getenv("XDG_DATA_HOME")) if (*d) return fs::path(d) / "DissidiaRecomp";
    if (const char* h = std::getenv("HOME")) return fs::path(h) / ".local/share/psprecomp";
    return fs::path("/tmp/psprecomp");
#endif
}

bool find_payload(const fs::path& exe, Payload& out) {
    FILE* f = open_file(exe, "rb");
    if (!f) return false;
    bool ok = false;
    unsigned char t[kTrailerSize];
    if (std::fseek(f, -(long)kTrailerSize, SEEK_END) == 0 && std::fread(t, 1, kTrailerSize, f) == kTrailerSize &&
        std::memcmp(t + 40, "PSPRPAY1", 8) == 0) {
        uint64_t size = 0;
        for (int i = 7; i >= 0; i--) size = (size << 8) | t[32 + i];
        long end = std::ftell(f);
        if (size > 0 && (uint64_t)end >= size + kTrailerSize) {
            out.size = size;
            out.offset = (uint64_t)end - kTrailerSize - size;
            out.version.assign((const char*)t, strnlen((const char*)t, 32));
            ok = true;
        }
    }
    std::fclose(f);
    return ok;
}

static bool safe_rel(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= n.size()) {
        size_t j = n.find_first_of("/\\", i);
        if (j == std::string::npos) j = n.size();
        if (n.compare(i, j - i, "..") == 0) return false;
        i = j + 1;
    }
    return true;
}

std::string extract_payload(const fs::path& exe, const Payload& p, const fs::path& dest,
                            const std::function<bool(double)>& progress) {
    std::error_code ec;
    if (fs::exists(dest / ".payload-ok", ec)) return "";
    fs::path part = dest;
    part += ".partial";
    fs::remove_all(part, ec);
    fs::create_directories(part, ec);
    if (ec) return "cannot create " + part.string() + ": " + ec.message();

    FILE* f = open_file(exe, "rb");
    if (!f) return "cannot open " + exe.string();
    if (std::fseek(f, (long)p.offset, SEEK_SET) != 0) { std::fclose(f); return "cannot seek in " + exe.string(); }
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_cfile(&zip, f, p.size, 0)) {
        std::string e = std::string("the bundled archive is damaged: ") + mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        std::fclose(f);
        return e;
    }
    std::string err;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    uint64_t total = 0, done = 0;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st)) total += st.m_uncomp_size;
    }
    if (!total) total = 1;
    for (mz_uint i = 0; i < n && err.empty(); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { err = "damaged archive entry"; break; }
        std::string name = st.m_filename;
        if (!safe_rel(name)) { err = "unsafe path in the archive: " + name; break; }
        fs::path target = part / fs::u8path(name);
        if (st.m_is_directory) { fs::create_directories(target, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        unsigned mode = ((st.m_version_made_by >> 8) == 3) ? (st.m_external_attr >> 16) : 0;
        if ((mode & 0170000) == 0120000) {   // symlink: the content is the link target
            size_t sz = 0;
            void* buf = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
            if (!buf) { err = "cannot read " + name; break; }
            std::string link((const char*)buf, sz);
            mz_free(buf);
#ifdef _WIN32
            // no symlinks on Windows (the Windows release has none): ignore
#else
            fs::create_symlink(link, target, ec);
#endif
        } else {
            FILE* out = open_file(target, "wb");
            if (!out) { err = "cannot write " + target.string(); break; }
            bool ok = mz_zip_reader_extract_to_cfile(&zip, i, out, 0) != 0;
            ok = (std::fclose(out) == 0) && ok;
            if (!ok) { err = "cannot unpack " + name + " (disk full?)"; break; }
#ifndef _WIN32
            if (mode & 0111) fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                                             fs::perm_options::add, ec);
#endif
        }
        done += st.m_uncomp_size;
        if (progress && !progress((double)done / (double)total)) err = "cancelled";
    }
    mz_zip_reader_end(&zip);
    std::fclose(f);
    if (!err.empty()) { fs::remove_all(part, ec); return err; }
    if (FILE* m = open_file(part / ".payload-ok", "wb")) { std::fputs(p.version.c_str(), m); std::fclose(m); }
    fs::remove_all(dest, ec);
    fs::rename(part, dest, ec);
    if (ec) return "cannot move " + part.string() + " to " + dest.string() + ": " + ec.message();
    return "";
}

}  // namespace setup
