// Private state of win::ShellBackend, shared by its translation units:
//   shell_backend.cpp   thread, listener window, public method plumbing
//   shell_tracking.cpp  WinEvent hooks -> tracked windows -> events
//   window_ops.cpp      place / hide / show / focus / close
//   appbar.cpp          edge reservations
#pragma once

#include "brocompositor/win/shell_backend.h"
#include "win/monitors.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace brocompositor::win {

inline constexpr UINT kMsgCall = WM_APP + 1;      // lParam: std::function<void()>*
inline constexpr UINT kMsgFlush = WM_APP + 2;     // coalesced change reporting
inline constexpr UINT kMsgAppBar = WM_APP + 3;    // appbar notifications
inline constexpr UINT kMsgDirty = WM_APP + 4;     // lParam: HWND to re-snapshot

enum class Hidden : uint32_t { None = 0, Park, Minimize, Hide };

struct Tracked {
    WindowId id = kNoWindow;
    HWND hwnd = nullptr;
    WindowSnapshot last;   // what the host was last told
    Hidden hidden = Hidden::None;
    Rect restore_frame;    // frame before a Park
};

struct AppBar {
    ReservationId id = kNoReservation;
    HWND hwnd = nullptr;
    MonitorId monitor = kNoMonitor;
    Edge edge = Edge::Top;
    int32_t thickness = 0;
    Rect granted;
};

struct ShellBackend::Impl {
    ShellConfig config;
    EventQueue queue;
    MonitorRegistry monitors;

    // ---- shell thread ----
    std::thread thread;
    DWORD thread_id = 0;
    HWND listener = nullptr;
    std::vector<HWINEVENTHOOK> hooks;
    std::set<HWND> dirty;       // shell thread only
    bool flush_posted = false;  // shell thread only

    // ---- shared, guarded by mutex ----
    mutable std::mutex mutex;
    std::unordered_map<HWND, Tracked> by_hwnd;
    std::unordered_map<WindowId, HWND> by_id;
    std::map<ReservationId, AppBar> appbars;
    WindowId next_window = 1;
    ReservationId next_reservation = 1;

    // Runs fn on the shell thread and waits for it.
    void call(const std::function<void()>& fn);
    bool on_shell_thread() const { return GetCurrentThreadId() == thread_id; }
    bool in_scope(HWND hwnd) const;

    // shell_tracking.cpp (shell thread)
    bool install_hooks();
    void remove_hooks();
    void report_initial_state();
    void on_win_event(DWORD event, HWND hwnd, LONG id_object, LONG id_child);
    void consider(HWND hwnd);
    void forget(HWND hwnd);
    void mark_dirty(HWND hwnd);
    void flush();
    void flush_one(HWND hwnd);
    void report_monitors_if_changed();
    std::vector<MonitorSnapshot> reported_monitors;
    // Full snapshot including id/owner/app_id (any thread, DPI scope required).
    WindowSnapshot make_snapshot(HWND hwnd);

    // window_ops.cpp (any thread)
    void request_report(HWND hwnd) { PostMessageW(listener, kMsgDirty, 0, reinterpret_cast<LPARAM>(hwnd)); }
    HWND hwnd_of(WindowId id) const;
    bool place(WindowId id, const Rect& frame);
    bool set_visible(WindowId id, bool visible);
    FocusResult focus(WindowId id);
    FocusResult focus_hwnd(HWND hwnd);
    bool close(WindowId id);
    void restore_all();
    size_t rescue_offscreen();

    // appbar.cpp (public entry points marshal to the shell thread)
    ReservationId reserve(MonitorId monitor, Edge edge, int32_t thickness, Rect* granted);
    bool release(ReservationId id);
    void release_all();
    void on_appbar_message(HWND hwnd, WPARAM notification);
    bool negotiate(AppBar& bar);
};

LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK appbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Process-wide list of live appbar windows for emergency_release_reservations().
void register_emergency_appbar(HWND hwnd);
void unregister_emergency_appbar(HWND hwnd);

}  // namespace brocompositor::win
