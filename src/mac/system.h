// The macOS system facts the backend reads, behind plain C++: displays
// (NSScreen + CoreGraphics), running applications (NSRunningApplication),
// workspace notifications (NSWorkspace), the window server's window list
// (CGWindowList) and process identity. AppKit stays inside appkit.mm.
//
// Every rectangle is in Quartz global display points (top-left origin of the
// primary display, y down). Safe to call from any thread.
#pragma once

#include "brocompositor/geometry.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::mac::sys {

struct Screen {
    uint32_t display_id = 0;  // CGDirectDisplayID
    std::string name;         // localized display name
    Rect frame;               // bounds in Quartz global points (CGDisplayBounds)
    Rect visible;             // minus the menu bar and the Dock (NSScreen.visibleFrame's insets)
    double scale = 1.0;       // pixels per point of the current display mode
    bool primary = false;     // holds the menu bar origin (Quartz 0,0)
};
// What NSScreen knows that the display topology (brodisplays, see
// mac/displays.h) does not: the localized name and the visible frame's
// insets (menu bar, Dock) in points, per CGDirectDisplayID.
struct ScreenInsets {
    std::string name;
    double left = 0, top = 0, right = 0, bottom = 0;
};
std::map<uint32_t, ScreenInsets> screen_insets();

struct App {
    uint32_t pid = 0;
    std::string name;        // localizedName
    std::string bundle_id;
    std::string executable;  // executable file name
    bool regular = false;    // activation policy Regular (has a Dock icon / menu bar)
};
std::optional<App> app(uint32_t pid);
uint32_t frontmost_pid();
// The pid of Finder (the neutral focus target), 0 when not running.
uint32_t finder_pid();
// Asks the system to make the application frontmost (NSRunningApplication
// activate). Measured on macOS 26: honoured from a background process, and
// it activates Finder when Finder shows only the desktop, where AXFrontmost
// reports success and changes nothing.
bool activate(uint32_t pid);

// Calls `changed` (on an AppKit notification queue) when an application
// launches, terminates, activates, hides or unhides, or the display
// configuration changes. Notifications need the process's main run loop to
// be running (verified: without it none arrive, not even on an operation
// queue); the backend's polling covers hosts where it is not.
class WorkspaceWatch {
public:
    static std::unique_ptr<WorkspaceWatch> start(std::function<void()> changed);
    ~WorkspaceWatch();
    struct Impl;

private:
    explicit WorkspaceWatch(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// One entry of the window server's list.
struct CgWindow {
    uint32_t id = 0;  // CGWindowID
    uint32_t pid = 0;
    int32_t layer = 0;
    Rect frame;
    double alpha = 1.0;
    bool onscreen = false;
    std::string title;  // empty without Screen Recording (other processes' windows)
    std::string owner;  // owning application name
};
// Front to back. `onscreen_only` limits to windows ordered in on a display
// (excludes minimized windows and those of hidden applications).
std::vector<CgWindow> window_list(bool onscreen_only);
std::optional<CgWindow> describe_window(uint32_t id);

// Process identity for the journal (start time in microseconds since the
// epoch, 0 when unknown) and liveness.
uint64_t process_start_time(uint32_t pid);
bool process_alive(uint32_t pid, uint64_t start);

// The login session's screen is locked.
bool screen_locked();

// Diagnostic trace to stderr, on when the environment has
// BROCOMPOSITOR_TRACE=1 (what the backend saw and decided; for debugging
// against a real window server).
bool tracing();
void trace(const char* format, ...) __attribute__((format(printf, 1, 2)));

}  // namespace brocompositor::mac::sys
