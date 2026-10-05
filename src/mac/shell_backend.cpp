// mac::ShellBackend lifetime and public methods. Tracking lives in
// shell_tracking.cpp, AX operations in window_ops.cpp, the journal in
// recovery.cpp.
#include "mac/shell_impl.h"

#include <unistd.h>

#include <thread>

namespace brocompositor::mac {

// ---------------------------------------------------------------- Impl helpers

bool ShellBackend::Impl::in_scope(uint32_t pid) const {
    if (config.process_filter.empty()) return true;
    for (uint32_t p : config.process_filter)
        if (p == pid) return true;
    return false;
}

uint32_t ShellBackend::Impl::cgid_of(WindowId id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = by_id.find(id);
    return it == by_id.end() ? 0 : it->second;
}

uint32_t ShellBackend::Impl::pid_of(uint32_t cgid) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = by_cgid.find(cgid);
    return it == by_cgid.end() ? 0 : it->second.pid;
}

std::shared_ptr<AppWorker> ShellBackend::Impl::worker_for(uint32_t pid) {
    if (!permissions.accessibility || pid == 0 || pid == self_pid) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping) return nullptr;
    auto it = workers.find(pid);
    if (it != workers.end()) return it->second;
    std::weak_ptr<Impl> weak = weak_from_this();
    auto w = AppWorker::start(pid, config.ax_timeout, [weak](uint32_t) {
        if (auto self = weak.lock()) self->wake();
    });
    workers.emplace(pid, w);
    return w;
}

// ---------------------------------------------------------------- public

ShellBackend::ShellBackend(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<ShellBackend> ShellBackend::create(const ShellConfig& config, std::string* error) {
    if (CFRef<CFDictionaryRef> session(CGSessionCopyCurrentDictionary()); !session) {
        if (error) *error = "no window server session (not logged in to the GUI)";
        return nullptr;
    }
    auto impl = std::make_shared<Impl>();
    impl->config = config;
    impl->permissions = query_permissions();
    impl->self_pid = uint32_t(getpid());
    std::string dir = config.journal_dir.empty() ? default_journal_dir() : config.journal_dir;
    if (dir == "-") dir.clear();
    impl->journal = std::make_unique<shell::Journal>(dir, impl->self_pid, sys::process_start_time(impl->self_pid));

    Impl* d = impl.get();
    if (!impl->displays.start([d] { d->wake(); }, error)) return nullptr;
    impl->watch = sys::WorkspaceWatch::start([d] { d->wake(); });
    std::promise<void> ready;
    impl->thread = std::thread([d, &ready] { d->run(&ready); });
    ready.get_future().wait();

    // Recovery of dead instances' journals runs on its own thread: restoring
    // a window waits on its (possibly hung) application.
    auto promise = std::make_shared<std::promise<RecoveryReport>>();
    impl->recovery = promise->get_future().share();
    if (config.recover && impl->journal->enabled())
        std::thread([self = impl, promise] { promise->set_value(self->recover_stale()); }).detach();
    else
        promise->set_value(RecoveryReport{});
    return std::unique_ptr<ShellBackend>(new ShellBackend(std::move(impl)));
}

ShellBackend::~ShellBackend() {
    // Put hidden windows back (bounded: a hung application keeps its window
    // in the journal for the next instance), then stop.
    auto restored = restore_all();
    restored.wait_until(std::chrono::steady_clock::now() + impl_->config.shutdown_timeout);
    impl_->watch.reset();
    {
        std::lock_guard<std::mutex> lock(impl_->wake_mutex);
        impl_->stop_flag = true;
    }
    impl_->wake_cv.notify_all();
    impl_->thread.join();
    impl_->displays.stop();
    std::map<uint32_t, std::shared_ptr<AppWorker>> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        workers.swap(impl_->workers);
    }
    for (auto& [pid, w] : workers) w->stop();
    // Whatever is still hidden (a stuck worker) stays journaled; everything
    // else was undone, so a clean shutdown leaves no journal behind.
    impl_->journal_sync();
}

EventQueue& ShellBackend::events() { return impl_->queue; }
const Permissions& ShellBackend::permissions() const { return impl_->permissions; }

bool ShellBackend::execute(const Command& command) {
    WindowId id = std::visit([](const auto& c) { return c.id; }, command);
    bool focus_none = std::holds_alternative<FocusWindow>(command) && id == kNoWindow;
    if (!focus_none && !impl_->cgid_of(id)) return false;
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, PlaceWindow>) place(c.id, c.frame);
            else if constexpr (std::is_same_v<T, SetWindowVisible>) set_visible(c.id, c.visible);
            else if constexpr (std::is_same_v<T, FocusWindow>) focus(c.id);
            else if constexpr (std::is_same_v<T, CloseWindow>) close(c.id);
        },
        command);
    return true;
}

size_t ShellBackend::execute(const std::vector<Command>& commands) {
    size_t refused = 0;
    for (const auto& c : commands) refused += execute(c) ? 0 : 1;
    return refused;
}

Completion<bool> ShellBackend::place(WindowId id, const Rect& frame) {
    uint32_t cgid = impl_->cgid_of(id);
    if (!cgid || frame.empty()) return completed(false);
    return impl_->run_on<bool>(impl_->pid_of(cgid), false, [cgid, frame](Impl& d, AppWorker::Context& c) {
        return d.do_place(c, cgid, frame);
    });
}

Completion<bool> ShellBackend::set_visible(WindowId id, bool visible) {
    uint32_t cgid = impl_->cgid_of(id);
    if (!cgid) return completed(false);
    return impl_->run_on<bool>(impl_->pid_of(cgid), false, [cgid, visible](Impl& d, AppWorker::Context& c) {
        return d.do_set_visible(c, cgid, visible);
    });
}

Completion<FocusResult> ShellBackend::focus(WindowId id) {
    uint32_t cgid = 0, pid = 0;
    if (id == kNoWindow) {
        pid = sys::finder_pid();
    } else {
        cgid = impl_->cgid_of(id);
        if (!cgid) return completed(FocusResult::NoSuchWindow);
        pid = impl_->pid_of(cgid);
    }
    if (!pid) return completed(FocusResult::NoSuchWindow);
    if (!impl_->permissions.accessibility) return completed(FocusResult::Unavailable);
    uint64_t gen = ++impl_->focus_generation;
    return impl_->run_on<FocusResult>(pid, FocusResult::Denied, [cgid, gen](Impl& d, AppWorker::Context& c) {
        return d.do_focus(c, cgid, gen);
    });
}

Completion<bool> ShellBackend::close(WindowId id) {
    uint32_t cgid = impl_->cgid_of(id);
    if (!cgid) return completed(false);
    return impl_->run_on<bool>(impl_->pid_of(cgid), false,
                               [cgid](Impl& d, AppWorker::Context& c) { return d.do_close(c, cgid); });
}

Completion<size_t> ShellBackend::restore_all() {
    // One job per tracked window, queued behind whatever is already queued
    // for its application, so a hide still in flight is undone too. The job
    // looks at the window's state when it runs.
    std::vector<std::pair<uint32_t, uint32_t>> tracked;  // pid, cgid
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& [cgid, t] : impl_->by_cgid) tracked.emplace_back(t.pid, cgid);
    }
    std::vector<Completion<bool>> results;
    for (auto [pid, cgid] : tracked)
        results.push_back(impl_->run_on<bool>(pid, false, [cgid](Impl& d, AppWorker::Context& c) {
            bool hidden = false;
            {
                std::lock_guard<std::mutex> lock(d.mutex);
                auto it = d.by_cgid.find(cgid);
                hidden = it != d.by_cgid.end() && it->second.hidden != Hidden::None;
            }
            return hidden && d.do_set_visible(c, cgid, true);
        }));
    if (results.empty()) return completed(size_t(0));
    auto promise = std::make_shared<std::promise<size_t>>();
    auto f = promise->get_future().share();
    std::thread([results = std::move(results), promise] {
        size_t ok = 0;
        for (auto& r : results) ok += r.get() ? 1 : 0;
        promise->set_value(ok);
    }).detach();
    return f;
}

Completion<RecoveryReport> ShellBackend::recovery() const { return impl_->recovery; }

Completion<size_t> ShellBackend::rescue_offscreen_windows() {
    auto promise = std::make_shared<std::promise<size_t>>();
    auto f = promise->get_future().share();
    std::thread([self = impl_, promise] { promise->set_value(self->rescue_offscreen()); }).detach();
    return f;
}

std::optional<WindowSnapshot> ShellBackend::query(WindowId id) const {
    uint32_t cgid = impl_->cgid_of(id);
    if (!cgid) return std::nullopt;
    auto fresh = sys::describe_window(cgid);
    if (!fresh) return std::nullopt;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->by_cgid.find(cgid);
    if (it == impl_->by_cgid.end()) return std::nullopt;
    WindowSnapshot s = it->second.last;
    if (!fresh->title.empty()) s.title = fresh->title;
    s.frame = fresh->frame;  // the truth, even while the backend keeps it parked
    const MonitorSnapshot* best = nullptr;
    int64_t area = -1;
    for (const auto& m : impl_->reported_monitors)
        if (int64_t a = m.bounds.intersected(s.frame).area(); a > area) {
            area = a;
            best = &m;
        }
    if (best) {
        s.monitor = best->id;
        s.dpi = best->dpi;
    }
    return s;
}

std::vector<MonitorSnapshot> ShellBackend::monitors() const {
    std::vector<sys::Screen> screens = impl_->displays.screens();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (screens.empty()) return impl_->reported_monitors;
    // Read-only: reservation rectangles are updated by the tracking pass.
    auto saved = impl_->reservations;
    auto out = impl_->build_monitors(screens, nullptr);
    impl_->reservations = std::move(saved);
    return out;
}

uint64_t ShellBackend::native_handle(WindowId id) const { return impl_->cgid_of(id); }

WindowId ShellBackend::find(uint64_t native) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->by_cgid.find(uint32_t(native));
    return it == impl_->by_cgid.end() ? kNoWindow : it->second.id;
}

ReservationId ShellBackend::reserve_edge(MonitorId monitor, Edge edge, int32_t thickness) {
    if (thickness <= 0) return kNoReservation;
    std::vector<Event> events;
    ReservationId id = kNoReservation;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        bool known = false;
        for (const auto& m : impl_->reported_monitors) known = known || m.id == monitor;
        if (!known) return kNoReservation;
        id = impl_->next_reservation++;
        Reservation r;
        r.id = id;
        r.monitor = monitor;
        r.edge = edge;
        r.thickness = thickness;
        r.granted = true;  // virtual: nothing to negotiate with
        impl_->reservations.emplace(id, r);
        impl_->build_monitors(impl_->screens, &events);
    }
    for (auto& e : events) impl_->queue.push(std::move(e));
    impl_->wake();  // the tracking pass reports the smaller work area
    return id;
}

bool ShellBackend::release_edge(ReservationId id) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->reservations.erase(id)) return false;
    }
    impl_->wake();
    return true;
}

std::optional<Rect> ShellBackend::reservation_rect(ReservationId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->reservations.find(id);
    if (it == impl_->reservations.end() || !it->second.granted) return std::nullopt;
    return it->second.rect;
}

}  // namespace brocompositor::mac
