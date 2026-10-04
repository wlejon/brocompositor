// WinEvent hooks -> tracked windows -> value events.
//
// Discovery keys off EVENT_OBJECT_SHOW / UNCLOAKED (a window is not visible
// yet at EVENT_OBJECT_CREATE, so manageability cannot be decided there), with
// NAMECHANGE / LOCATIONCHANGE / FOREGROUND as late re-checks for windows that
// only become manageable after they were shown. Removal is reported only for
// windows that were added. Geometry/title/state changes are coalesced through
// a posted flush and reported only when a fresh snapshot actually differs.
#include "win/shell_impl.h"
#include "win/util.h"
#include "win/window_query.h"

namespace brocompositor::win {

namespace {

thread_local ShellBackend::Impl* t_impl = nullptr;

void CALLBACK hook_proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD,
                        DWORD) {
    if (t_impl) t_impl->on_win_event(event, hwnd, id_object, id_child);
}

struct Range {
    DWORD lo, hi;
    bool all_processes;
};

constexpr Range kRanges[] = {
    {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, true},  // focus leaving scope matters too
    {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, false},
    {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, false},
    {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, false},  // DESTROY, SHOW, HIDE
    {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, false},
    {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, false},
};

}  // namespace

bool ShellBackend::Impl::install_hooks() {
    t_impl = this;
    DWORD only_pid = config.process_filter.size() == 1 ? config.process_filter[0] : 0;
    for (const Range& r : kRanges) {
        HWINEVENTHOOK h = SetWinEventHook(r.lo, r.hi, nullptr, hook_proc, r.all_processes ? 0 : only_pid,
                                          0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
        if (!h) {
            remove_hooks();
            return false;
        }
        hooks.push_back(h);
    }
    return true;
}

void ShellBackend::Impl::remove_hooks() {
    for (HWINEVENTHOOK h : hooks) UnhookWinEvent(h);
    hooks.clear();
    t_impl = nullptr;
}

void ShellBackend::Impl::report_initial_state() {
    reported_monitors = monitors.enumerate();
    queue.push(MonitorsChanged{reported_monitors});
    if (config.report_existing) {
        std::vector<HWND> all;
        EnumWindows(
            [](HWND h, LPARAM p) -> BOOL {
                reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&all));
        // Bottom of the z-order first, so the layout order follows stacking age.
        for (auto it = all.rbegin(); it != all.rend(); ++it) consider(*it);
    }
    on_win_event(EVENT_SYSTEM_FOREGROUND, GetForegroundWindow(), OBJID_WINDOW, CHILDID_SELF);
}

WindowSnapshot ShellBackend::Impl::make_snapshot(HWND hwnd) {
    WindowSnapshot s = snapshot_window(hwnd, monitors);
    HWND owner = GetWindow(hwnd, GW_OWNER);
    bool need_app_id = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(hwnd);
        if (it != by_hwnd.end()) {
            s.id = it->second.id;
            s.app_id = it->second.last.app_id;
            need_app_id = s.app_id.empty();
        }
        auto o = owner ? by_hwnd.find(owner) : by_hwnd.end();
        if (o != by_hwnd.end()) s.owner = o->second.id;
    }
    if (need_app_id) s.app_id = process_image_name(s.process_id);
    return s;
}

void ShellBackend::Impl::consider(HWND hwnd) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (by_hwnd.count(hwnd)) return;
    }
    if (hwnd == listener || !in_scope(hwnd) || !is_manageable(hwnd)) return;
    // Owners first, so a dialog can name its owner.
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner && owner != hwnd) consider(owner);

    WindowSnapshot s = make_snapshot(hwnd);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (by_hwnd.count(hwnd)) return;
        Tracked t;
        t.id = next_window++;
        t.hwnd = hwnd;
        s.id = t.id;
        t.last = s;
        by_id.emplace(t.id, hwnd);
        by_hwnd.emplace(hwnd, std::move(t));
    }
    queue.push(WindowAdded{s});
}

void ShellBackend::Impl::forget(HWND hwnd) {
    WindowId id = kNoWindow;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(hwnd);
        if (it == by_hwnd.end()) return;
        id = it->second.id;
        by_id.erase(id);
        by_hwnd.erase(it);
    }
    dirty.erase(hwnd);
    queue.push(WindowRemoved{id});
}

void ShellBackend::Impl::mark_dirty(HWND hwnd) {
    dirty.insert(hwnd);
    if (!flush_posted) {
        flush_posted = PostMessageW(listener, kMsgFlush, 0, 0) != FALSE;
    }
}

void ShellBackend::Impl::flush() {
    flush_posted = false;
    std::set<HWND> pending;
    pending.swap(dirty);
    for (HWND h : pending) flush_one(h);
}

void ShellBackend::Impl::flush_one(HWND hwnd) {
    dirty.erase(hwnd);
    if (!IsWindow(hwnd)) {
        forget(hwnd);
        return;
    }
    Hidden hidden;
    WindowSnapshot last;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(hwnd);
        if (it == by_hwnd.end()) return;
        hidden = it->second.hidden;
        last = it->second.last;
    }
    WindowSnapshot now = make_snapshot(hwnd);
    WindowSnapshot report = now;
    uint32_t changes = diff(last, now);
    // While the backend itself keeps a window hidden, its geometry and state
    // are the backend's doing, not facts for the policy core.
    if (hidden != Hidden::None) {
        report = last;
        report.title = now.title;
        changes &= change::Title;
    }
    if (!changes) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(hwnd);
        if (it == by_hwnd.end()) return;
        it->second.last = report;
    }
    queue.push(WindowChanged{report, changes});
}

void ShellBackend::Impl::report_monitors_if_changed() {
    auto now = monitors.enumerate();
    if (now == reported_monitors) return;
    reported_monitors = now;
    queue.push(MonitorsChanged{now});
    // Monitor ids/DPI of windows may have changed with the topology.
    std::vector<HWND> all;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [h, t] : by_hwnd) all.push_back(h);
    }
    for (HWND h : all) mark_dirty(h);
}

void ShellBackend::Impl::on_win_event(DWORD event, HWND hwnd, LONG id_object, LONG id_child) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        flush();  // facts about the window precede the focus change
        if (hwnd && in_scope(hwnd)) consider(hwnd);
        WindowId id = kNoWindow;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_hwnd.find(hwnd);
            if (it != by_hwnd.end()) id = it->second.id;
        }
        queue.push(FocusChanged{id});
        return;
    }
    if (!hwnd || id_object != OBJID_WINDOW || id_child != CHILDID_SELF || hwnd == listener) return;
    if (!config.process_filter.empty() && config.process_filter.size() > 1 && !in_scope(hwnd)) return;

    bool tracked;
    Hidden hidden = Hidden::None;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_hwnd.find(hwnd);
        tracked = it != by_hwnd.end();
        if (tracked) hidden = it->second.hidden;
    }

    switch (event) {
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            if (tracked) mark_dirty(hwnd);
            else consider(hwnd);
            break;
        // Out-of-context events arrive late: re-check the current state so a
        // window hidden and re-shown in quick succession is not dropped.
        case EVENT_OBJECT_HIDE:
            if (tracked && hidden != Hidden::Hide && !IsWindowVisible(hwnd)) forget(hwnd);
            break;
        case EVENT_OBJECT_CLOAKED:
            if (tracked && hidden == Hidden::None && is_cloaked(hwnd)) forget(hwnd);
            break;
        case EVENT_OBJECT_DESTROY:
            if (tracked && !IsWindow(hwnd)) forget(hwnd);
            break;
        case EVENT_OBJECT_NAMECHANGE:
        case EVENT_OBJECT_LOCATIONCHANGE:
            if (tracked) mark_dirty(hwnd);
            else if (IsWindowVisible(hwnd)) consider(hwnd);
            break;
        case EVENT_SYSTEM_MINIMIZESTART:
        case EVENT_SYSTEM_MINIMIZEEND:
            if (tracked) mark_dirty(hwnd);
            break;
        case EVENT_SYSTEM_MOVESIZESTART:
        case EVENT_SYSTEM_MOVESIZEEND:
            if (!tracked) break;
            flush_one(hwnd);
            {
                WindowId id = kNoWindow;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    auto it = by_hwnd.find(hwnd);
                    if (it != by_hwnd.end()) id = it->second.id;
                }
                if (id == kNoWindow) break;
                if (event == EVENT_SYSTEM_MOVESIZESTART) queue.push(MoveSizeStarted{id});
                else queue.push(MoveSizeEnded{id});
            }
            break;
        default: break;
    }
}

}  // namespace brocompositor::win
