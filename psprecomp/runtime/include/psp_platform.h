// Small portability layer: the POSIX calls the runtime uses that Windows (mingw, via the toolchain bundle) lacks or spells differently.
// Header-only; on Linux/macOS everything here is a thin inline over the POSIX call, so behaviour there is unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/statvfs.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace psp_plat {

// ---- sparse guest memory: reserve/commit zeroed pages, only touched pages cost memory ----
inline void* alloc_zeroed(size_t n) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // nullptr on failure
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
inline void free_pages(void* p, size_t n) {
#ifdef _WIN32
    (void)n; VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

// ---- files and directories ----
inline int mkdir_(const char* path, int mode = 0755) {
#ifdef _WIN32
    (void)mode; return _mkdir(path);
#else
    return ::mkdir(path, (mode_t)mode);
#endif
}
inline ssize_t pread_(int fd, void* buf, size_t n, int64_t off) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov{}; ov.Offset = (DWORD)(off & 0xffffffffu); ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) { return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1; }
    return (ssize_t)got;
#else
    return ::pread(fd, buf, n, (off_t)off);
#endif
}
// modification time of a stat result in nanoseconds
inline long long mtime_ns(const struct stat& st) {
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (long long)st.st_mtime * 1000000000LL;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}
// free bytes on the volume holding `path`; false when unknown
inline bool free_bytes(const char* path, uint64_t* out) {
#ifdef _WIN32
    ULARGE_INTEGER avail{}, total{}, freeb{};
    if (!GetDiskFreeSpaceExA(path, &avail, &total, &freeb)) return false;
    *out = avail.QuadPart; return true;
#else
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) return false;
    *out = (uint64_t)sv.f_bavail * sv.f_frsize; return true;
#endif
}

// ---- environment and time ----
inline void setenv_(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    ::setenv(k, v, 1);
#endif
}
inline void unsetenv_(const char* k) {
#ifdef _WIN32
    _putenv_s(k, "");
#else
    ::unsetenv(k);
#endif
}
inline bool localtime_(time_t t, struct tm* out) {
#ifdef _WIN32
    return localtime_s(out, &t) == 0;
#else
    return localtime_r(&t, out) != nullptr;
#endif
}
inline long gettid_() {
#ifdef _WIN32
    return (long)GetCurrentThreadId();
#else
    return -1;   // psp_prof.h keeps its own Linux/macOS versions
#endif
}

// ---- sockets (Winsock on Windows) ----
#ifdef _WIN32
using sock_t = SOCKET;
inline void net_init() { static bool done = false; if (!done) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); done = true; } }
inline int close_socket(int fd) { return closesocket((SOCKET)fd); }
inline int poll_(struct pollfd* p, unsigned n, int timeout_ms) { return WSAPoll(p, (ULONG)n, timeout_ms); }
inline void set_nonblocking(int fd) { u_long on = 1; ioctlsocket((SOCKET)fd, FIONBIO, &on); }   // recvfrom has no MSG_DONTWAIT here
constexpr int kMsgDontWait = 0;                 // Winsock has no MSG_DONTWAIT; callers poll() first
constexpr int kShutBoth = SD_BOTH;
#define PSP_SOCKOPT_CAST(p) (reinterpret_cast<const char*>(p))
#else
using sock_t = int;
inline void net_init() {}
inline int close_socket(int fd) { return ::close(fd); }
inline int poll_(struct pollfd* p, unsigned n, int timeout_ms) { return ::poll(p, (nfds_t)n, timeout_ms); }
inline void set_nonblocking(int) {}   // POSIX callers pass MSG_DONTWAIT
constexpr int kMsgDontWait = MSG_DONTWAIT;
constexpr int kShutBoth = SHUT_RDWR;
#define PSP_SOCKOPT_CAST(p) (p)
#endif

}  // namespace psp_plat
