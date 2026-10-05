// Windows backend, shell role: brocompositor runs as a window manager on top
// of DWM and manages top-level windows owned by other processes.
//
// A ShellBackend owns one thread (the "shell thread") that installs
// out-of-context WinEvent hooks, owns the hidden listener window that receives
// display/work-area broadcasts, and owns the appbar windows that reserve
// screen edges. Everything it learns is pushed as value snapshots into
// events(), which the host drains on its own thread.
//
// Nothing the host calls blocks on another process. Window operations
// (place, hide/show, focus, close) are queued to a worker thread per target
// process and return a Completion; SetWindowPos / ShowWindow on a hung
// application stall only that application's worker (and switch to their
// async forms once Windows reports the window hung). Edge reservations are
// negotiated with the shell (Explorer) on the shell thread; the granted
// rectangle arrives as ReservationChanged. Queries read the backend's own
// bookkeeping or non-blocking OS state. Every coordinate is in physical
// pixels (per-monitor-v2) regardless of the host's DPI awareness.
//
// Lifetime and recovery: the destructor puts back every window this backend
// hid and removes every edge reservation, waiting at most
// ShellConfig::shutdown_timeout for applications that do not respond. State
// that would outlive a crash or a hard kill (parked/minimized windows, appbar
// reservations) is mirrored in a journal file (ShellConfig::journal_dir); the
// next backend started with the same journal directory undoes whatever a
// dead instance left behind (recovery()). See README "Crash recovery" for
// what a hard kill can and cannot leave behind.
#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/event_queue.h"
#include "brocompositor/events.h"
#include "brocompositor/shell.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::win {

using brocompositor::FocusResult;

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
    // Crash-recovery journal directory. Empty: the default,
    // %LOCALAPPDATA%\brocompositor\journal. "-" disables journaling and
    // recovery (nothing then survives a hard kill to be undone).
    std::string journal_dir;
    // Undo what dead instances journaled in journal_dir when starting.
    bool recover = true;
    // How long the destructor waits for applications to take their windows
    // back before leaving them to the journal (a hung application).
    std::chrono::milliseconds shutdown_timeout{2000};
};

class ShellBackend {
public:
    // Starts the shell thread; on return the initial MonitorsChanged, the
    // existing windows and the current focus are already queued, and
    // recovery of dead instances' journals is under way (recovery()).
    static std::unique_ptr<ShellBackend> create(const ShellConfig& config, std::string* error);
    ~ShellBackend();

    ShellBackend(const ShellBackend&) = delete;
    ShellBackend& operator=(const ShellBackend&) = delete;

    EventQueue& events();

    // Queues one core command; false only when the window is unknown. The
    // outcome arrives through events() as facts either way.
    bool execute(const Command& command);
    size_t execute(const std::vector<Command>& commands);  // returns how many were refused

    // Asynchronous window operations; an unknown window completes at once
    // with false / NoSuchWindow. Operations on windows of one process run in
    // the order they were issued. A later focus() supersedes an earlier one
    // that has not finished.
    Completion<bool> place(WindowId id, const Rect& frame);
    Completion<bool> set_visible(WindowId id, bool visible);
    Completion<FocusResult> focus(WindowId id);
    Completion<bool> close(WindowId id);

    // Fresh OS query (not the last reported snapshot); never blocks on the
    // window's process.
    std::optional<WindowSnapshot> query(WindowId id) const;
    std::vector<MonitorSnapshot> monitors() const;
    uint64_t native_handle(WindowId id) const;
    WindowId find(uint64_t native_handle) const;

    // Edge reservation through the shell's appbar protocol: the work area of
    // `monitor` shrinks by `thickness` px on `edge` for as long as the
    // reservation lives. Returns kNoReservation for an unknown monitor or a
    // non-positive thickness; otherwise the negotiation runs on the shell
    // thread and ends in ReservationChanged{id, monitor, rect} (an empty rect
    // when the shell refused). Renegotiations (taskbar moved) arrive the same
    // way.
    ReservationId reserve_edge(MonitorId monitor, Edge edge, int32_t thickness);
    bool release_edge(ReservationId id);
    // The granted rectangle, once negotiated.
    std::optional<Rect> reservation_rect(ReservationId id) const;

    // Puts back every window this backend hid.
    Completion<size_t> restore_all();

    // Outcome of the start-up recovery of dead instances' journals.
    Completion<RecoveryReport> recovery() const;

    // Heuristic recovery for windows left off-screen by something that kept
    // no journal: moves every in-scope window that lies entirely outside the
    // virtual screen back onto the primary monitor. Returns how many moved.
    Completion<size_t> rescue_offscreen_windows();

    struct Impl;

private:
    explicit ShellBackend(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

// Removes every edge reservation held by any live ShellBackend in this
// process. Safe from any thread, including a console control handler or an
// unhandled-exception filter, so the work area is restored on abnormal exits.
void emergency_release_reservations();

}  // namespace brocompositor::win
