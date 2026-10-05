// A child process for the Wayland tests (the scripted client, weston demo
// clients, foot, wl-clipboard, ...): stdout lines are collected on a reader
// thread, stdin stays open for commands, and the destructor kills and reaps.
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace bctest {

class Child {
public:
    // argv[0] is looked up in PATH. `env` entries ("K=V") override the
    // current environment. Returns nullptr when the program cannot start.
    static std::unique_ptr<Child> spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                                        bool capture_stderr = false);
    ~Child();

    pid_t pid() const { return pid_; }
    // Waits until a stdout line starts with `prefix`; returns that line.
    bool wait_line(const std::string& prefix, int timeout_ms, std::string* line = nullptr);
    size_t count(const std::string& prefix) const;
    // Waits until at least `n` lines start with `prefix`.
    bool wait_count(const std::string& prefix, size_t n, int timeout_ms);
    std::vector<std::string> lines() const;
    std::string output() const;  // all lines joined with '\n'
    void send(const std::string& text);
    void close_stdin();
    // Waits for exit; true when it exited, *status = exit code (or -signal).
    bool wait_exit(int timeout_ms, int* status = nullptr);
    bool running();
    void kill_now();
    // SIGTERM, then SIGKILL after `timeout_ms` (lets servers clean up sockets).
    void terminate(int timeout_ms = 2000);

private:
    Child() = default;
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::thread reader_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::string> lines_;
    bool exited_ = false;
    int status_ = 0;
};

// The full path of a program on PATH ("" when missing).
std::string which(const std::string& program);

}  // namespace bctest
