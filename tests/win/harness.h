// Shared plumbing for the Windows backend tests: a child process that owns
// the test windows, event waiting, and cleanup guards that keep the user's
// desktop untouched (foreground restored, reservations removed on abnormal
// exits).
#pragma once

#include "brocompositor/event_queue.h"
#include "brocompositor/win/shell_backend.h"

#include <windows.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bctest {

using namespace std::chrono_literals;
using brocompositor::Event;
using brocompositor::Rect;

// Call first in every Windows test: physical-pixel coordinates in this
// process, and work-area cleanup on Ctrl+C / unhandled exceptions.
void init_windows_test();

class TestApp {
public:
    TestApp() = default;
    ~TestApp();
    bool start();
    DWORD pid() const { return pi_.dwProcessId; }
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

// Accumulates events from a queue and waits for matching ones.
class EventLog {
public:
    explicit EventLog(brocompositor::EventQueue& q) : queue_(q) {}

    // Pumps until pred matches an event (after the last mark()) that no
    // previous wait consumed; consumes and returns it.
    template <class T>
    std::optional<T> wait(const std::function<bool(const T&)>& pred,
                          std::chrono::milliseconds timeout = 3000ms) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            for (size_t i = from_; i < all_.size(); ++i) {
                if (used_[i]) continue;
                if (auto* e = std::get_if<T>(&all_[i]); e && pred(*e)) {
                    used_[i] = true;
                    return *e;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            queue_.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        }
    }

    // Events of type T (after the last mark()) matching pred, without consuming.
    template <class T>
    std::vector<T> collect(const std::function<bool(const T&)>& pred) {
        pump();
        std::vector<T> out;
        for (size_t i = from_; i < all_.size(); ++i)
            if (auto* e = std::get_if<T>(&all_[i]); e && pred(*e)) out.push_back(*e);
        return out;
    }

    // Lets events keep arriving until `quiet` passes with none.
    void settle(std::chrono::milliseconds quiet = 300ms);
    // Later waits/collects only look at events after this point.
    void mark() {
        pump();
        from_ = all_.size();
    }
    void pump();
    const std::vector<Event>& all() const { return all_; }

private:
    brocompositor::EventQueue& queue_;
    std::vector<Event> all_;
    std::vector<bool> used_;
    size_t from_ = 0;
};

// Restores the foreground window on scope exit (tests that take focus).
class ForegroundGuard {
public:
    ForegroundGuard();
    ~ForegroundGuard();

private:
    HWND previous_;
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
