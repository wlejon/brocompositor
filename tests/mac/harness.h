// Shared plumbing for the macOS backend tests: a child application that owns
// the test windows, the permission/session checks that decide what a test
// can prove on this machine, and a private journal directory.
#pragma once

#include "brocompositor/mac/shell_backend.h"
#include "check.h"
#include "event_log.h"

#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <string>

namespace bctest {

using namespace std::chrono_literals;
using brocompositor::Rect;

// Prints what this machine allows (once per process) and returns it.
const brocompositor::mac::Permissions& mac_permissions();
// At least one display is awake (the window server reports real geometry).
bool display_awake();
// Exit code 77 (ctest "skipped") after printing the reason.
[[noreturn]] void skip(const std::string& why);
// Skips unless the requirement holds, naming the permission and the binary
// it must be granted to.
void require_accessibility();
void require_screen_recording();
void require_unlocked();
void require_display();

// The frontmost application's pid (to hand focus back after a test took it).
uint32_t frontmost_pid();
// Makes the application frontmost through Accessibility (needs the permission).
void make_frontmost(uint32_t pid);

// A journal directory private to this test process (removed at exit).
std::string test_journal_dir();
brocompositor::mac::ShellConfig test_shell_config(pid_t pid);

class TestApp {
public:
    TestApp() = default;
    ~TestApp();
    bool start();
    pid_t pid() const { return pid_; }
    std::string cmd(const std::string& line, int timeout_ms = 5000);  // the reply line ("" on timeout)
    bool ok(const std::string& line) { return cmd(line).rfind("ok", 0) == 0; }
    // Creates a window (Quartz points) and returns its CGWindowID (0 on failure).
    uint32_t create(const std::string& name, const Rect& frame, const std::string& rrggbb);
    // Kills the app without waiting for it (a hung app).
    void kill_now();

private:
    std::string read_line(int timeout_ms);
    pid_t pid_ = 0;
    int in_ = -1;   // write end of the child's stdin
    int out_ = -1;  // read end of the child's stdout
    std::string buffer_;
};

// Polls `pred` until it holds or the timeout passes.
bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 3000ms);

// Rect equality within `tolerance` points per edge (the window server rounds).
bool near(const Rect& a, const Rect& b, int tolerance = 2);

}  // namespace bctest
