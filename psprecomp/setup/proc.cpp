#include "proc.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace setup {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring quote_cmdline(const std::vector<std::string>& argv) {
    std::wstring cl;
    for (size_t i = 0; i < argv.size(); i++) {
        std::wstring a = widen(argv[i]);
        if (i) cl += L' ';
        bool need = a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos;
        if (!need) { cl += a; continue; }
        cl += L'"';
        size_t bs = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { bs++; continue; }
            if (c == L'"') { cl.append(bs * 2 + 1, L'\\'); cl += L'"'; bs = 0; continue; }
            cl.append(bs, L'\\'); bs = 0; cl += c;
        }
        cl.append(bs * 2, L'\\');
        cl += L'"';
    }
    return cl;
}
static void apply_env(const EnvList& env) {
    for (auto& kv : env) SetEnvironmentVariableW(widen(kv.first).c_str(), widen(kv.second).c_str());
}
struct Proc::Impl {
    HANDLE process = nullptr, job = nullptr, read_end = nullptr;
};
#else
struct Proc::Impl {
    pid_t pid = -1;
    int read_fd = -1;
};
#endif

Proc::~Proc() {
    if (impl_) {
        kill();
        if (reader_.joinable()) reader_.join();
#ifdef _WIN32
        if (impl_->process) CloseHandle(impl_->process);
        if (impl_->job) CloseHandle(impl_->job);
        if (impl_->read_end) CloseHandle(impl_->read_end);
#else
        if (impl_->read_fd >= 0) close(impl_->read_fd);
#endif
        delete impl_;
    }
}

bool Proc::start(const std::vector<std::string>& argv, const EnvList& env, const std::string& cwd) {
    if (argv.empty()) { error_ = "empty command"; return false; }
    if (impl_) { kill(); if (reader_.joinable()) reader_.join(); delete impl_; impl_ = nullptr; }
    impl_ = new Impl();
    lines_.clear();
    exit_code_ = 0;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { error_ = "CreatePipe failed"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    apply_env(env);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cl = quote_cmdline(argv);
    std::wstring wcwd = widen(cwd);
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); error_ = "cannot run " + argv[0] + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (impl_->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(impl_->job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->read_end = rd;
#else
    int fds[2];
    if (pipe(fds) != 0) { error_ = std::string("pipe: ") + std::strerror(errno); return false; }
    pid_t pid = fork();
    if (pid < 0) { error_ = std::string("fork: ") + std::strerror(errno); close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        setsid();   // own process group: kill(-pid) takes ninja, zig and python down together
        close(fds[0]);
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[1]);
        int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        for (auto& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        std::vector<char*> av;
        for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        std::fprintf(stderr, "cannot run %s: %s\n", av[0], std::strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    impl_->pid = pid;
    impl_->read_fd = fds[0];
#endif
    done_ = false;
    reader_ = std::thread(&Proc::reader_loop, this);
    return true;
}

void Proc::reader_loop() {
    std::string cur;
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(impl_->read_end, buf, sizeof buf, &n, nullptr) || n == 0) break;
#else
        ssize_t n = read(impl_->read_fd, buf, sizeof buf);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
#endif
        for (ssize_t i = 0; i < (ssize_t)n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!cur.empty()) { std::lock_guard<std::mutex> l(mu_); lines_.push_back(cur); }
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    if (!cur.empty()) { std::lock_guard<std::mutex> l(mu_); lines_.push_back(cur); }
    finish();
}

void Proc::finish() {
#ifdef _WIN32
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(impl_->process, &code);
    exit_code_ = (int)code;
#else
    int st = 0;
    while (waitpid(impl_->pid, &st, 0) < 0 && errno == EINTR) {}
    exit_code_ = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
    done_ = true;
}

bool Proc::read_line(std::string& out) {
    std::lock_guard<std::mutex> l(mu_);
    if (lines_.empty()) return false;
    out = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

bool Proc::busy() {
    if (!done_) return true;
    std::lock_guard<std::mutex> l(mu_);
    return !lines_.empty();
}

void Proc::kill() {
    if (!impl_ || done_) return;
#ifdef _WIN32
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    else if (impl_->process) TerminateProcess(impl_->process, 1);
#else
    if (impl_->pid > 0) ::kill(-impl_->pid, SIGTERM);
#endif
}

bool spawn_detached(const std::vector<std::string>& argv, const EnvList& env, const std::string& cwd, std::string* error) {
    if (argv.empty()) return false;
#ifdef _WIN32
    apply_env(env);
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cl = quote_cmdline(argv);
    std::wstring wcwd = widen(cwd);
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        if (error) *error = "cannot start " + argv[0] + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) { if (error) *error = std::string("fork: ") + std::strerror(errno); return false; }
    if (pid == 0) {
        setsid();
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        for (auto& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        std::vector<char*> av;
        for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }
    return true;
#endif
}

}  // namespace setup
