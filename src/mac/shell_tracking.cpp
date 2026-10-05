// Window-server list + AX facts -> tracked windows -> value events.
//
// The tracking thread re-reads the window server's list every poll interval
// and whenever something announced a change (an AppWorker's AX
// notification, an NSWorkspace notification). It never calls into another
// application: AX facts come from what each AppWorker last published.
#include "mac/shell_impl.h"

#include <algorithm>
#include <cmath>

namespace brocompositor::mac {

namespace {

uint32_t diff(const WindowSnapshot& a, const WindowSnapshot& b) {
    uint32_t c = 0;
    if (a.frame != b.frame) c |= change::Geometry;
    if (a.title != b.title) c |= change::Title;
    if (a.minimized != b.minimized || a.maximized != b.maximized || a.fullscreen != b.fullscreen) c |= change::State;
    if (a.monitor != b.monitor || a.dpi != b.dpi) c |= change::Monitor;
    return c;
}

bool manageable_subrole(const std::string& s) {
    return s.empty() || s == "AXStandardWindow" || s == "AXDialog" || s == "AXUnknown";
}

}  // namespace

void ShellBackend::Impl::wake() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        wake_flag = true;
    }
    wake_cv.notify_one();
}

void ShellBackend::Impl::run(std::promise<void>* ready) {
    // Initial state: give the application workers a moment for their first
    // AX scan, so the first report already knows minimized windows/titles.
    if (!config.report_existing)
        for (const auto& w : sys::window_list(false)) ignored.insert(w.id);
    refresh();
    if (permissions.accessibility) {
        auto deadline = std::chrono::steady_clock::now() + std::min(config.ax_timeout, std::chrono::milliseconds(500));
        for (;;) {
            bool all = true;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& [pid, w] : workers) all = all && (w->scanned() || w->unresponsive());
            }
            if (all || std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        refresh();
    }
    ready->set_value();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(wake_mutex);
            wake_cv.wait_for(lock, config.poll_interval, [&] { return wake_flag || stop_flag; });
            if (stop_flag) return;
            bool woken = wake_flag;
            wake_flag = false;
            if (woken) {
                // Coalesce a burst of notifications into one pass.
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        refresh();
    }
}

std::vector<MonitorSnapshot> ShellBackend::Impl::build_monitors(const std::vector<sys::Screen>& list,
                                                                std::vector<Event>* renegotiated) {
    std::vector<MonitorSnapshot> out;
    for (const sys::Screen& s : list) {
        MonitorSnapshot m;
        m.id = s.display_id;
        m.name = s.name;
        m.bounds = s.frame;
        m.work_area = s.visible.empty() ? s.frame : s.visible;
        m.dpi = uint32_t(std::lround(96.0 * s.scale));
        m.primary = s.primary;
        m.native = s.display_id;
        // Reservations stack inwards in the order they were made.
        for (auto& [id, r] : reservations) {
            if (r.monitor != m.id || !r.granted) continue;
            Rect& w = m.work_area;
            int32_t t = std::min(r.thickness, (r.edge == Edge::Left || r.edge == Edge::Right) ? w.width : w.height);
            Rect strip;
            switch (r.edge) {
                case Edge::Left:
                    strip = Rect{w.x, w.y, t, w.height};
                    w = Rect{w.x + t, w.y, w.width - t, w.height};
                    break;
                case Edge::Top:
                    strip = Rect{w.x, w.y, w.width, t};
                    w = Rect{w.x, w.y + t, w.width, w.height - t};
                    break;
                case Edge::Right:
                    strip = Rect{w.right() - t, w.y, t, w.height};
                    w = Rect{w.x, w.y, w.width - t, w.height};
                    break;
                case Edge::Bottom:
                    strip = Rect{w.x, w.bottom() - t, w.width, t};
                    w = Rect{w.x, w.y, w.width, w.height - t};
                    break;
            }
            if (strip != r.rect) {
                r.rect = strip;
                if (renegotiated) renegotiated->push_back(ReservationChanged{r.id, r.monitor, strip});
            }
        }
        out.push_back(m);
    }
    return out;
}

void ShellBackend::Impl::refresh() {
    std::vector<sys::Screen> now_screens = sys::screens();
    // Every display asleep: the window server's geometry is not meaningful
    // (it reports a scaled placeholder space). Wait for a display to wake.
    if (now_screens.empty()) return;
    std::vector<sys::CgWindow> list = sys::window_list(false);
    uint32_t front = sys::frontmost_pid();

    // Application facts and AX snapshots, gathered outside the lock.
    std::map<uint32_t, std::map<uint32_t, ax::WindowInfo>> ax_infos;
    std::map<uint32_t, bool> ax_known;
    std::set<uint32_t> pids, checked;
    for (const auto& w : list) {
        if (w.layer != 0 || w.pid == self_pid || !in_scope(w.pid)) continue;
        // An application still launching may not be Regular yet: re-ask
        // until it is (once per pass).
        auto known = apps.find(w.pid);
        if ((known == apps.end() || !known->second || !known->second->regular) && checked.insert(w.pid).second)
            apps[w.pid] = sys::app(w.pid);
        const auto& a = apps[w.pid];
        if (!a || !a->regular) continue;
        if (pids.insert(w.pid).second && permissions.accessibility) {
            if (auto worker = worker_for(w.pid); worker && worker->scanned()) {
                ax_infos[w.pid] = worker->windows();
                ax_known[w.pid] = true;
            }
        }
    }

    std::vector<Event> events;
    std::vector<std::shared_ptr<AppWorker>> retired;
    bool journal_dirty = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        screens = now_screens;
        std::vector<MonitorSnapshot> mons = build_monitors(now_screens, &events);
        if (mons != reported_monitors) {
            reported_monitors = mons;
            events.push_back(MonitorsChanged{mons});
        }
        auto monitor_of = [&](const Rect& r) -> const MonitorSnapshot* {
            const MonitorSnapshot* best = nullptr;
            int64_t area = -1;
            for (const auto& m : reported_monitors) {
                int64_t a = m.bounds.intersected(r).area();
                if (a > area) {
                    area = a;
                    best = &m;
                }
            }
            return best;
        };

        std::set<uint32_t> seen;
        for (const auto& w : list) {
            if (w.layer != 0 || !pids.count(w.pid) || ignored.count(w.id)) continue;
            auto tracked = by_cgid.find(w.id);
            bool is_tracked = tracked != by_cgid.end();
            const ax::WindowInfo* ai = nullptr;
            if (ax_known.count(w.pid)) {
                auto& infos = ax_infos[w.pid];
                auto it = infos.find(w.id);
                if (it != infos.end()) ai = &it->second;
                else if (!is_tracked && w.onscreen) {
                    // Not an AX window (yet): ask the application once more,
                    // decide on the next pass.
                    if (rescan_asked.insert(w.id).second)
                        if (auto wk = workers.find(w.pid); wk != workers.end()) wk->second->request_rescan();
                    continue;
                }
            }
            if (ai && !manageable_subrole(ai->subrole)) continue;
            bool hidden_by_us = is_tracked && tracked->second.hidden != Hidden::None;
            bool minimized = ai && ai->minimized;
            bool visible = w.onscreen && w.alpha > 0.0 && w.frame.width > 1 && w.frame.height > 1;
            if (!visible && !(is_tracked && (hidden_by_us || minimized))) continue;
            seen.insert(w.id);

            WindowSnapshot s;
            s.native = w.id;
            s.process_id = w.pid;
            s.title = !w.title.empty() ? w.title : ai ? ai->title : std::string();
            const auto& app = apps[w.pid];
            s.app_id = app ? (!app->bundle_id.empty() ? app->bundle_id : app->executable) : w.owner;
            s.class_name = ai ? ai->subrole : std::string();
            s.frame = w.frame;
            if (const MonitorSnapshot* m = monitor_of(w.frame)) {
                s.monitor = m->id;
                s.dpi = m->dpi;
                s.maximized = w.frame == m->work_area;
                s.fullscreen = ai ? ai->fullscreen : w.frame == m->bounds;
            }
            s.minimized = minimized;
            s.resizable = ai ? ai->resizable : true;

            if (!is_tracked) {
                Tracked t;
                t.id = next_window++;
                t.cgid = w.id;
                t.pid = w.pid;
                t.pid_start = sys::process_start_time(w.pid);
                s.id = t.id;
                t.last = s;
                by_id.emplace(t.id, w.id);
                by_cgid.emplace(w.id, t);
                rescan_asked.erase(w.id);
                events.push_back(WindowAdded{s});
                continue;
            }
            Tracked& t = tracked->second;
            s.id = t.id;
            WindowSnapshot report = s;
            uint32_t changes = diff(t.last, s);
            // While the backend keeps a window hidden, its geometry and state
            // are the backend's doing, not facts for the policy core.
            if (hidden_by_us) {
                report = t.last;
                report.title = s.title;
                changes &= change::Title;
            }
            if (!changes) continue;
            t.last = report;
            events.push_back(WindowChanged{report, changes});
        }

        for (auto it = by_cgid.begin(); it != by_cgid.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            journal_dirty |= it->second.hidden != Hidden::None;
            events.push_back(WindowRemoved{it->second.id});
            by_id.erase(it->second.id);
            it = by_cgid.erase(it);
        }

        // Focus: the frontmost application's front-most normal window.
        WindowId focus = kNoWindow;
        for (const auto& w : list) {
            if (w.layer != 0 || !w.onscreen || w.pid != front) continue;
            auto it = by_cgid.find(w.id);
            if (it != by_cgid.end()) focus = it->second.id;
            break;
        }
        if (focus != focused) {
            focused = focus;
            events.push_back(FocusChanged{focus});
        }

        // Workers of applications that quit (or left scope).
        std::set<uint32_t> alive;
        for (const auto& w : list) alive.insert(w.pid);
        for (auto it = workers.begin(); it != workers.end();) {
            if (alive.count(it->first)) {
                ++it;
                continue;
            }
            retired.push_back(it->second);
            apps.erase(it->first);
            it = workers.erase(it);
        }
    }
    for (auto& w : retired) w->stop();
    if (journal_dirty) journal_sync();
    for (auto& e : events) queue.push(std::move(e));
}

}  // namespace brocompositor::mac
