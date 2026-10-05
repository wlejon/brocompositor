// Private state of mac::ShellBackend, shared by its translation units:
//   shell_backend.cpp   lifetime, public method plumbing, reservations
//   shell_tracking.cpp  window-server list + AX facts -> tracked windows -> events
//   window_ops.cpp      place / hide / show / focus / close (on app workers)
//   recovery.cpp        journal upkeep, start-up recovery, off-screen rescue
//
// Threads: the tracking thread, one AppWorker per application (AX), and the
// host's threads. Impl is owned through a shared_ptr that every queued job
// also holds, so a worker stuck in a hung application can outlive the
// ShellBackend.
#pragma once

#include "brocompositor/mac/shell_backend.h"
#include "mac/app_worker.h"
#include "mac/system.h"
#include "shell/journal.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace brocompositor::mac {

enum class Hidden : uint32_t { None = 0, Park = 1, Minimize = 2 };

struct Tracked {
    WindowId id = kNoWindow;
    uint32_t cgid = 0;
    uint32_t pid = 0;
    uint64_t pid_start = 0;
    WindowSnapshot last;  // what the host was last told
    Hidden hidden = Hidden::None;
    Rect restore_frame;   // frame before the backend hid it
    Rect parked_frame;    // where a Park left it (read back)
};

struct Reservation {
    ReservationId id = kNoReservation;
    MonitorId monitor = kNoMonitor;
    Edge edge = Edge::Top;
    int32_t thickness = 0;
    Rect rect;
    bool granted = false;
};

struct ShellBackend::Impl : std::enable_shared_from_this<ShellBackend::Impl> {
    ShellConfig config;
    Permissions permissions;
    EventQueue queue;
    std::unique_ptr<shell::Journal> journal;
    std::shared_future<RecoveryReport> recovery;
    std::unique_ptr<sys::WorkspaceWatch> watch;
    uint32_t self_pid = 0;

    // ---- tracking thread ----
    std::thread thread;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake_flag = false;
    bool stop_flag = false;
    std::map<uint32_t, std::optional<sys::App>> apps;  // pid -> application facts (tracking thread)
    std::set<uint32_t> ignored;                        // pre-existing windows when !report_existing
    std::set<uint32_t> rescan_asked;                   // ids AX did not know yet (tracking thread)
    void wake();
    void run(std::promise<void>* ready);

    // Serializes journal rewrites (snapshot + write). Taken before `mutex`.
    std::mutex journal_mutex;

    // ---- shared, guarded by mutex ----
    mutable std::mutex mutex;
    std::unordered_map<uint32_t, Tracked> by_cgid;
    std::unordered_map<WindowId, uint32_t> by_id;
    std::map<uint32_t, std::shared_ptr<AppWorker>> workers;
    std::map<ReservationId, Reservation> reservations;
    std::vector<sys::Screen> screens;               // last awake display set
    std::vector<MonitorSnapshot> reported_monitors;
    WindowId focused = kNoWindow;
    WindowId next_window = 1;
    ReservationId next_reservation = 1;
    std::atomic<uint64_t> focus_generation{0};
    std::atomic<bool> stopping{false};

    bool in_scope(uint32_t pid) const;
    uint32_t cgid_of(WindowId id) const;
    uint32_t pid_of(uint32_t cgid) const;

    // The application's worker, started on demand (nullptr without
    // Accessibility or once stopping).
    std::shared_ptr<AppWorker> worker_for(uint32_t pid);
    // Queues fn on the worker of `pid`; `fallback` when it cannot run.
    template <class T>
    Completion<T> run_on(uint32_t pid, T fallback, std::function<T(Impl&, AppWorker::Context&)> fn);

    // shell_tracking.cpp (tracking thread)
    void refresh();
    // Monitors with reservations applied; updates reservation rectangles and
    // appends ReservationChanged for any that moved. Caller holds mutex.
    std::vector<MonitorSnapshot> build_monitors(const std::vector<sys::Screen>& s, std::vector<Event>* renegotiated);

    // window_ops.cpp (app workers)
    bool do_place(AppWorker::Context& c, uint32_t cgid, const Rect& frame);
    bool do_set_visible(AppWorker::Context& c, uint32_t cgid, bool visible);
    FocusResult do_focus(AppWorker::Context& c, uint32_t cgid, uint64_t generation);
    bool do_close(AppWorker::Context& c, uint32_t cgid);
    std::vector<Rect> display_frames() const;

    // recovery.cpp
    void journal_sync();
    RecoveryReport recover_stale();
    size_t rescue_offscreen();
};

template <class T>
Completion<T> ShellBackend::Impl::run_on(uint32_t pid, T fallback, std::function<T(Impl&, AppWorker::Context&)> fn) {
    auto promise = std::make_shared<std::promise<T>>();
    Completion<T> result = promise->get_future().share();
    std::shared_ptr<AppWorker> w = worker_for(pid);
    bool queued = w && w->post([self = shared_from_this(), promise, fn = std::move(fn), fallback](
                                   AppWorker::Context* c) {
        if (!c) {
            promise->set_value(fallback);
            return;
        }
        try {
            promise->set_value(fn(*self, *c));
        } catch (...) {
            promise->set_value(fallback);
        }
    });
    if (!queued) promise->set_value(fallback);
    return result;
}

std::string default_journal_dir();

// window_ops.cpp helpers shared with recovery.cpp.
// A parked window may keep at most this much of itself (points^2) on the
// displays: macOS keeps a sliver of every window reachable.
inline constexpr int64_t kParkedVisibleArea = 64 * 64;
AXError set_frame(AXUIElementRef window, const Rect& frame);
int64_t visible_area(const Rect& r, const std::vector<Rect>& displays);

}  // namespace brocompositor::mac
