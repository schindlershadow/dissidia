// Minimal cross-platform child-process helper for the setup wizard: run a command with a given environment and working
// directory, read its combined stdout/stderr line by line from another thread, and kill the whole process tree on cancel.
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace setup {

using EnvList = std::vector<std::pair<std::string, std::string>>;

class Proc {
public:
    Proc() = default;
    Proc(const Proc&) = delete;
    Proc& operator=(const Proc&) = delete;
    ~Proc();

    // Starts the command (argv[0] is looked up on PATH when it has no directory part). False + error() on failure.
    bool start(const std::vector<std::string>& argv, const EnvList& env, const std::string& cwd);
    // Pops one output line (without the newline); false when none is pending right now.
    bool read_line(std::string& out);
    // True while the child runs or output is still queued.
    bool busy();
    // Exit code once finished (valid when !busy()); -1 if killed.
    int exit_code() const { return exit_code_.load(); }
    // Kills the child and everything it started (process group / job object).
    void kill();
    const std::string& error() const { return error_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::thread reader_;
    std::mutex mu_;
    std::deque<std::string> lines_;
    std::atomic<bool> done_{true};
    std::atomic<int> exit_code_{0};
    std::string error_;
    void reader_loop();
    void finish();
};

// Starts a program that outlives the caller (the game); no output capture. Returns false on failure.
bool spawn_detached(const std::vector<std::string>& argv, const EnvList& env, const std::string& cwd, std::string* error,
                    const std::string& log_file = "");   // log_file: the child's stdout+stderr go there (truncated)

}  // namespace setup
