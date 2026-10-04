// Windows backend, shell role: brocompositor runs as a window manager on top
// of DWM and manages top-level windows owned by other processes.
//
// A ShellBackend owns one thread (the "shell thread") that installs
// out-of-context WinEvent hooks, owns the hidden listener window that receives
// display/work-area broadcasts, and owns the appbar windows that reserve
// screen edges. Everything it learns is pushed as value snapshots into
// events(), which the host drains on its own thread. All other methods may be
// called from any thread; window operations run on the calling thread with a
// per-monitor-v2 DPI context, so every coordinate is in physical pixels
// regardless of the host process's DPI awareness.
//
// Lifetime and safety: the destructor puts back every window this backend
// hid, removes every edge reservation (so the work area returns to what it
// was), and unhooks. Nothing the backend does persists past the process,
// except a parked window if the process is killed while it is parked; see
// rescue_offscreen_windows().
#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/event_queue.h"
#include "brocompositor/events.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::win {

enum class HideMethod : uint32_t {
    // Move the window beyond the virtual screen. It stays a normal, restored
    // window (no minimize animation, no state the user could confuse with
    // their own minimize; the taskbar still lists it). DWM does not compose
    // it while parked, so a WindowCapture keeps its last frame and resumes
    // with current content when the window is shown again (verified by
    // test_win_capture). Maximized windows fall back to Minimize.
    Park = 0,
    Minimize = 1,  // SW_SHOWMINNOACTIVE; capture stops while minimized
    Hide = 2,      // SW_HIDE; also removes the taskbar button
};

struct ShellConfig {
    // Only windows of these processes are reported or touched (empty: all).
    // Tests use this to confine the backend to windows they created.
    std::vector<uint32_t> process_filter;
    HideMethod hide_method = HideMethod::Park;
    // Window that receives focus for FocusWindow{kNoWindow} (0: the desktop).
    uint64_t idle_focus_window = 0;
    // Report the windows that already exist at startup.
    bool report_existing = true;
};

enum class FocusResult : uint32_t {
    Focused = 0,         // the window is now the foreground window
    AlreadyFocused = 1,
    Denied = 2,          // Windows' foreground rules refused every strategy
    NoSuchWindow = 3,
};

class ShellBackend {
public:
    // Starts the shell thread; on return the initial MonitorsChanged, the
    // existing windows and the current focus are already queued.
    static std::unique_ptr<ShellBackend> create(const ShellConfig& config, std::string* error);
    ~ShellBackend();

    ShellBackend(const ShellBackend&) = delete;
    ShellBackend& operator=(const ShellBackend&) = delete;

    EventQueue& events();

    // Executes one core command; false when the window is unknown or the OS
    // refused. The resulting facts arrive through events() either way.
    bool execute(const Command& command);
    size_t execute(const std::vector<Command>& commands);  // returns failures

    bool place(WindowId id, const Rect& frame);
    bool set_visible(WindowId id, bool visible);
    FocusResult focus(WindowId id);
    bool close(WindowId id);

    // Fresh OS query (not the last reported snapshot).
    std::optional<WindowSnapshot> query(WindowId id) const;
    std::vector<MonitorSnapshot> monitors() const;
    uint64_t native_handle(WindowId id) const;
    WindowId find(uint64_t native_handle) const;

    // Edge reservation through the shell's appbar protocol: the work area of
    // `monitor` shrinks by `thickness` px on `edge` for as long as the
    // reservation lives. Returns kNoReservation on failure; `granted` receives
    // the rectangle the host should cover with its panel window.
    ReservationId reserve_edge(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted);
    bool release_edge(ReservationId id);
    std::optional<Rect> reservation_rect(ReservationId id) const;

    // Puts back every window this backend hid. Called by the destructor.
    void restore_all();

    // Recovery for windows left parked by a process that died: moves every
    // in-scope window that lies entirely outside the virtual screen back onto
    // the primary monitor. Returns how many were moved.
    size_t rescue_offscreen_windows();

    struct Impl;

private:
    explicit ShellBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Removes every edge reservation held by any live ShellBackend in this
// process. Safe from any thread, including a console control handler or an
// unhandled-exception filter, so the work area is restored on abnormal exits.
void emergency_release_reservations();

}  // namespace brocompositor::win
