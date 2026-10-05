// macOS backend, shell role: brocompositor runs as a window manager on top
// of WindowServer and manages windows owned by other applications, the way
// the Windows backend does over DWM.
//
// What macOS permits without SIP changes or private Spaces APIs, and what
// this backend therefore does:
//
//   * Discovery comes from the window server's list (CGWindowList), which
//     needs no permission: every normal-layer, on-screen window of a regular
//     application, front to back, with its owner pid and frame. It is
//     polled on the backend's own thread (ShellConfig::poll_interval) and
//     re-read at once when (with Accessibility) an application reports a
//     window created, moved, resized, retitled, minimized or destroyed, or
//     (in a process running its main run loop) when an application
//     launches, quits or activates. With Accessibility a window is reported
//     only once its application's AX facts are in (titles, subrole,
//     minimized, fullscreen; a new application's first windows wait up to
//     2 x ax_timeout for them), and only standard windows, dialogs and
//     titled or closable windows of unknown subrole (AppKit's own helper
//     windows, such as the one animating a fullscreen transition, are not).
//   * A window the window server stops showing is, with Accessibility:
//     minimized (kept, minimized = true); fullscreen in its own Space while
//     another Space is active (kept); on another Space or of a hidden
//     application (removed after 1 s, unless this backend hid it, like a
//     cloaked window on Windows; it comes back as a new window); closed or
//     ordered out (removed). Without Accessibility it is removed at once.
//   * Window operations need the Accessibility permission (AX). Each
//     application gets its own worker thread with its own run loop and AX
//     observer; every AX call into an application runs there with a
//     messaging timeout (ShellConfig::ax_timeout), so a hung application
//     stalls its own worker and nothing else. Nothing the host calls blocks
//     on another process.
//   * Workspaces are emulated: a hidden window is parked off a bottom
//     corner of a display. macOS does not allow a window entirely off the
//     displays (measured on macOS 26: a position leaving any part of the
//     window on a display is kept horizontally, one beyond them is pulled
//     back to leave 40 pt; vertically the title bar is kept inside the
//     display's visible frame), so a parked window keeps a 1 pt column on
//     screen from its title bar down to the display's bottom edge (1 x 91 pt
//     on a 1496 x 967 point display with a bottom Dock). The backend tries
//     each display's bottom corners, reads back where the window really
//     went, and falls back to minimizing when more than 64 x 64 pt stayed
//     visible. A parked window stays parked (and journaled) across Space
//     switches.
//   * Edge reservations are virtual: macOS has no appbar protocol, so a
//     reservation shrinks the work area this backend reports (the host's
//     layout honours it) without changing what other applications see as
//     the screen's visible frame. The menu bar and Dock are always excluded
//     from the reported work area. Displays, their bounds and scale come
//     from CoreGraphics on every pass (NSScreen keeps a stale configuration
//     in a process that does not run NSApplication's event loop); only the
//     menu-bar / Dock insets come from NSScreen.visibleFrame.
//   * Focus is the frontmost application's key window (AXFocusedWindow;
//     without Accessibility its front-most window in the window server's
//     order). Focusing a window makes its application frontmost (AX, or
//     NSRunningApplication where AX reports success and changes nothing, as
//     for Finder showing only the desktop) and raises it.
//   * There are no move/size-started events (macOS reports no interactive
//     move loop to other processes).
//
// Coordinates are Quartz global display points (see geometry.h); a window's
// dpi is its display's 96 x pixels per point of its current mode.
//
// Lifetime and recovery mirror the Windows backend: the destructor puts back
// every window this backend parked or minimized (waiting at most
// ShellConfig::shutdown_timeout for applications that do not respond), and
// parked windows are journaled so that the next backend undoes what a
// crashed one left behind (needs Accessibility then too).
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

namespace brocompositor::mac {

using brocompositor::FocusResult;

// What this process may do, and to whom the user grants what is missing.
// Querying never prompts the user and never opens System Settings.
struct Permissions {
    // System Settings > Privacy & Security > Accessibility. Needed for every
    // window operation (place, hide/show, focus, close), for minimized-state
    // and title tracking, and for recovery.
    bool accessibility = false;
    // System Settings > Privacy & Security > Screen & System Audio
    // Recording. Needed for window capture and for other applications'
    // window titles in the window server's list.
    bool screen_recording = false;
    // The login session's screen is locked (or the display asleep with the
    // lock engaged): focus cannot change and capture delivers no frames.
    bool screen_locked = false;
    // The binary macOS checks those grants against: the subject tccd
    // attributes this process to (the app bundle or terminal that launched
    // it; for a program started over ssh /usr/libexec/sshd-keygen-wrapper,
    // launchd's ssh Program, although the connection's process runs
    // /usr/libexec/sshd-session). Grant the permissions to this path.
    std::string responsible_path;
    uint32_t responsible_pid = 0;  // the responsible process
};
Permissions query_permissions();

enum class HideMethod : uint32_t {
    // Park beyond the bottom-right corner of the display union (see above);
    // falls back to Minimize when the application keeps the window on
    // screen. Capture keeps running while parked.
    Park = 0,
    Minimize = 1,  // AXMinimized; the Dock animates it, capture stops
};

struct ShellConfig {
    // Only windows of these processes are reported or touched (empty: all).
    // Tests use this to confine the backend to windows they created.
    std::vector<uint32_t> process_filter;
    HideMethod hide_method = HideMethod::Park;
    // Report the windows that already exist at startup.
    bool report_existing = true;
    // How often the window server's list is re-read when nothing announced
    // a change.
    std::chrono::milliseconds poll_interval{250};
    // AX messaging timeout per call into an application.
    std::chrono::milliseconds ax_timeout{1000};
    // Crash-recovery journal directory. Empty: the default,
    // ~/Library/Application Support/brocompositor/journal. "-" disables
    // journaling and recovery.
    std::string journal_dir;
    // Undo what dead instances journaled in journal_dir when starting.
    bool recover = true;
    // How long the destructor waits for applications to take their windows
    // back before leaving them to the journal (a hung application).
    std::chrono::milliseconds shutdown_timeout{2000};
};

class ShellBackend {
public:
    // Starts the backend thread; on return the initial MonitorsChanged, the
    // existing windows and the current focus are already queued. Works
    // without permissions (discovery, monitors and focus tracking); see
    // permissions() for what is unavailable.
    static std::unique_ptr<ShellBackend> create(const ShellConfig& config, std::string* error);
    ~ShellBackend();

    ShellBackend(const ShellBackend&) = delete;
    ShellBackend& operator=(const ShellBackend&) = delete;

    EventQueue& events();
    // Permissions as of create() (they change only when the user acts; a
    // process started after a grant sees it, one already running may not).
    const Permissions& permissions() const;

    // Queues one core command; false only when the window is unknown.
    bool execute(const Command& command);
    size_t execute(const std::vector<Command>& commands);  // returns how many were refused

    // Asynchronous window operations, as on Windows: an unknown window
    // completes at once with false / NoSuchWindow, and without
    // Accessibility with false / Unavailable. Operations on windows of one
    // application run in the order they were issued; a later focus()
    // supersedes an earlier one that has not finished.
    Completion<bool> place(WindowId id, const Rect& frame);
    Completion<bool> set_visible(WindowId id, bool visible);
    Completion<FocusResult> focus(WindowId id);
    Completion<bool> close(WindowId id);

    // From the window server's list (fresh), never blocking on the window's
    // application. The list trails a change by a few frames; an operation
    // completes only once its result shows here (or after 300 ms).
    std::optional<WindowSnapshot> query(WindowId id) const;
    std::vector<MonitorSnapshot> monitors() const;
    uint64_t native_handle(WindowId id) const;  // CGWindowID
    WindowId find(uint64_t native_handle) const;

    // Virtual edge reservation: the reported work area of `monitor` shrinks
    // by `thickness` points on `edge`. Granted asynchronously like the
    // Windows appbar (ReservationChanged{id, monitor, rect}, followed by
    // MonitorsChanged); kNoReservation for an unknown monitor or a
    // non-positive thickness.
    ReservationId reserve_edge(MonitorId monitor, Edge edge, int32_t thickness);
    bool release_edge(ReservationId id);
    std::optional<Rect> reservation_rect(ReservationId id) const;

    // Puts back every window this backend hid.
    Completion<size_t> restore_all();
    // Outcome of the start-up recovery of dead instances' journals.
    Completion<RecoveryReport> recovery() const;
    // Moves every in-scope window that lies (almost) entirely outside the
    // displays back onto the main display. Needs Accessibility.
    Completion<size_t> rescue_offscreen_windows();

    struct Impl;

private:
    explicit ShellBackend(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace brocompositor::mac
