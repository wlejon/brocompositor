// Private state of win::ShellBackend, shared by its translation units:
//   shell_backend.cpp   thread, listener window, public method plumbing
//   shell_tracking.cpp  WinEvent hooks -> tracked windows -> events
//   window_ops.cpp      place / hide / show / focus / close (on op workers)
//   recovery.cpp        journal upkeep, start-up recovery, off-screen rescue
//   appbar.cpp          edge reservations
//
// Threads: the shell thread (hooks, listener, appbars), one op worker per
// target process (shell::SerialWorkers), and the host's threads. Impl is
// owned through a shared_ptr that every queued job also holds, so a worker
// stuck in a hung application can outlive the ShellBackend.
#pragma once

#include "brocompositor/win/shell_backend.h"
#include "shell/journal.h"
#include "shell/serial_workers.h"
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

inline constexpr UINT kMsgCall = WM_APP + 1;      // lParam: std::function<void()>* (owned by the receiver)
inline constexpr UINT kMsgFlush = WM_APP + 2;     // coalesced change reporting
inline constexpr UINT kMsgAppBar = WM_APP + 3;    // appbar notifications
inline constexpr UINT kMsgDirty = WM_APP + 4;     // lParam: HWND to re-snapshot
inline constexpr UINT kMsgDisplays = WM_APP + 5;  // brodisplays queued a topology change

enum class Hidden : uint32_t { None = 0, Park, Minimize, Hide };

struct Tracked {
    WindowId id = kNoWindow;
    HWND hwnd = nullptr;
    WindowSnapshot last;   // what the host was last told
    Hidden hidden = Hidden::None;
    Rect restore_frame;    // frame before the backend hid it
    Rect parked_frame;     // where a Park put it
};

struct AppBar {
    ReservationId id = kNoReservation;
    HWND hwnd = nullptr;
    MonitorId monitor = kNoMonitor;
    Edge edge = Edge::Top;
    int32_t thickness = 0;
    Rect granted;
    bool negotiated = false;
};

struct ShellBackend::Impl : std::enable_shared_from_this<ShellBackend::Impl> {
    ShellConfig config;
    EventQueue queue;
    MonitorRegistry monitors;
    std::shared_ptr<shell::SerialWorkers> workers;
    std::unique_ptr<shell::Journal> journal;
    std::shared_future<RecoveryReport> recovery;

    // ---- shell thread ----
    std::thread thread;
    DWORD thread_id = 0;
    HWND listener = nullptr;
    std::vector<HWINEVENTHOOK> hooks;
    std::set<HWND> dirty;       // shell thread only
    bool flush_posted = false;  // shell thread only

    // Serializes journal rewrites (snapshot + write), so an older snapshot
    // never overwrites a newer one. Taken before `mutex`, never inside it.
    std::mutex journal_mutex;

    // ---- shared, guarded by mutex ----
    mutable std::mutex mutex;
    std::unordered_map<HWND, Tracked> by_hwnd;
    std::unordered_map<WindowId, HWND> by_id;
    std::map<ReservationId, AppBar> appbars;
    WindowId next_window = 1;
    ReservationId next_reservation = 1;
    std::atomic<uint64_t> focus_generation{0};

    // Runs fn on the shell thread without waiting (false once the shell
    // thread is gone).
    bool post(std::function<void()> fn);
    // Runs fn on the shell thread and waits for it (shell-internal use only:
    // never from a host-facing method).
    void call(const std::function<void()>& fn);
    bool on_shell_thread() const { return GetCurrentThreadId() == thread_id; }
    bool in_scope(HWND hwnd) const;

    // Queues fn on the op worker of hwnd's process.
    template <class T>
    Completion<T> run_for(HWND hwnd, T fallback, std::function<T(Impl&)> fn);

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

    // window_ops.cpp (op workers)
    void request_report(HWND hwnd) {
        std::lock_guard<std::mutex> lock(mutex);  // the listener is gone once the shell thread ended
        if (listener) PostMessageW(listener, kMsgDirty, 0, reinterpret_cast<LPARAM>(hwnd));
    }
    // brodisplays' watcher thread: a topology change is queued.
    void request_displays() {
        std::lock_guard<std::mutex> lock(mutex);
        if (listener) PostMessageW(listener, kMsgDisplays, 0, 0);
    }
    HWND hwnd_of(WindowId id) const;
    bool do_place(HWND h, const Rect& frame);
    bool do_set_visible(HWND h, bool visible);
    FocusResult do_focus(HWND h, uint64_t generation);
    bool do_close(HWND h);

    // recovery.cpp
    void journal_sync();  // rewrites the journal from by_hwnd / appbars (any thread)
    RecoveryReport recover_stale();  // op-worker context
    size_t rescue_offscreen();

    // appbar.cpp (shell thread)
    void negotiate_new(ReservationId id);
    void remove_appbar_window(HWND hwnd);
    void release_all();
    void on_appbar_message(HWND hwnd, WPARAM notification);
    bool negotiate(AppBar& bar);
};

template <class T>
Completion<T> ShellBackend::Impl::run_for(HWND hwnd, T fallback, std::function<T(Impl&)> fn) {
    auto promise = std::make_shared<std::promise<T>>();
    Completion<T> result = promise->get_future().share();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    bool queued = workers->post(pid, [self = shared_from_this(), promise, fn = std::move(fn), fallback] {
        try {
            promise->set_value(fn(*self));
        } catch (...) {
            promise->set_value(fallback);
        }
    });
    if (!queued) promise->set_value(fallback);
    return result;
}

LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK appbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Process-wide list of live appbar windows for emergency_release_reservations().
void register_emergency_appbar(HWND hwnd);
void unregister_emergency_appbar(HWND hwnd);
void appbar_remove(HWND hwnd);

// Process identity for the journal: creation time as a FILETIME tick count
// (0 when the process cannot be opened).
uint64_t process_start_time(DWORD pid);

}  // namespace brocompositor::win
