// Shared plumbing for the Windows backend tests: a child process that owns
// the test windows, event waiting, and cleanup guards that keep the user's
// desktop untouched (foreground restored, reservations removed on abnormal
// exits, a private crash-recovery journal directory removed at exit).
#pragma once

#include "brocompositor/event_queue.h"
#include "brocompositor/win/shell_backend.h"
#include "event_log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bctest {

using namespace std::chrono_literals;
using brocompositor::Event;
using brocompositor::Rect;

// Call first in every Windows test: physical-pixel coordinates in this
// process, and work-area cleanup on Ctrl+C / unhandled exceptions.
void init_windows_test();

// A journal directory private to this test process (created on first use,
// removed at exit), so tests never touch a real host's journals.
std::string test_journal_dir();
// A ShellConfig confined to `pid`'s windows with the private journal.
brocompositor::win::ShellConfig test_shell_config(DWORD pid);

class TestApp {
public:
    TestApp() = default;
    ~TestApp();
    bool start();
    DWORD pid() const { return pi_.dwProcessId; }
    HANDLE process() const { return pi_.hProcess; }
    std::string cmd(const std::string& line);  // returns the reply line
    // Creates a window (outer rect) and returns its HWND, or nullptr.
    HWND create(const std::string& name, const Rect& outer, const std::string& rrggbb,
                const std::string& flags = std::string());
    bool ok(const std::string& line) { return cmd(line).rfind("ok", 0) == 0; }

private:
    PROCESS_INFORMATION pi_{};
    HANDLE in_write_ = nullptr;
    HANDLE out_read_ = nullptr;
};

// Restores the foreground window on scope exit (tests that take focus).
class ForegroundGuard {
public:
    ForegroundGuard();
    ~ForegroundGuard();

private:
    HWND previous_;
};

// Counts real (non-injected) keyboard and mouse input from the user, through
// low-level hooks on a thread of its own. Tests that depend on the
// foreground use it to tell "the backend failed" from "the user was using
// the machine at the same moment".
class UserInputMonitor {
public:
    UserInputMonitor();
    ~UserInputMonitor();
    uint64_t count() const;  // real input events seen so far
    bool active() const { return thread_id_ != 0; }

private:
    std::thread thread_;
    DWORD thread_id_ = 0;
};

Rect frame_of(HWND hwnd);   // DWMWA_EXTENDED_FRAME_BOUNDS
Rect outer_of(HWND hwnd);   // GetWindowRect
Rect primary_work_area();
Rect virtual_screen_rect();
std::vector<Rect> monitor_work_areas();
// True when the user's Default desktop is the input desktop. While a screen
// saver or the secure desktop (lock screen, UAC) has the input, nothing can
// take the foreground and focus tests must skip.
bool interactive_desktop();
bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 3000ms);
bool force_foreground(HWND hwnd);

}  // namespace bctest
