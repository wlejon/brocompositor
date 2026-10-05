// Crash recovery: the journal mirrors every window this backend hid and every
// appbar it registered; a backend starting later undoes what a dead instance
// left (shell/journal.h). Also the journal-less off-screen rescue.
#include "win/shell_impl.h"
#include "win/util.h"

#include <shellapi.h>

namespace brocompositor::win {

namespace {

// Whether a process with this pid (and, when start != 0, this start time)
// exists. A process we cannot open for a reason other than "no such process"
// counts as alive: never recover the journal of an owner we cannot rule out.
bool process_alive(uint32_t pid, uint64_t start) {
    if (pid == 0) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() != ERROR_INVALID_PARAMETER;
    bool running = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    FILETIME created{}, exited{}, kernel{}, user{};
    uint64_t t = 0;
    if (GetProcessTimes(p, &created, &exited, &kernel, &user))
        t = uint64_t(created.dwHighDateTime) << 32 | created.dwLowDateTime;
    CloseHandle(p);
    if (!running) return false;
    return start == 0 || t == start;
}

bool on_some_monitor(const Rect& r) {
    RECT rc = to_RECT(r);
    return MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) != nullptr;
}

Rect primary_work_area() {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return to_rect(mi.rcWork);
}

// `frame` if it is still on a monitor, else the same size inside the
// primary work area.
Rect onto_a_monitor(const Rect& frame) {
    if (on_some_monitor(frame)) return frame;
    Rect work = primary_work_area();
    return Rect{work.x + 64, work.y + 64, std::min(frame.width, work.width - 128),
                std::min(frame.height, work.height - 128)};
}

bool move_frame(HWND h, const Rect& frame) {
    Rect o = frame.outset(invisible_borders(h));
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (IsHungAppWindow(h)) flags |= SWP_ASYNCWINDOWPOS;
    return SetWindowPos(h, nullptr, o.x, o.y, o.width, o.height, flags) != FALSE;
}

// Undoes one journaled hide if the window is still the one we hid and still
// where we left it. True when it was put back.
bool restore_entry(const shell::ParkedEntry& e) {
    HWND h = to_hwnd(e.window);
    if (!IsWindow(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != e.pid || process_start_time(pid) != e.pid_start) return false;  // a different window now
    switch (Hidden(e.method)) {
        case Hidden::Park:
            // Only if nothing moved it back onto a monitor meanwhile.
            if (frame_bounds(h).intersects(virtual_screen())) return false;
            return move_frame(h, onto_a_monitor(e.restore));
        case Hidden::Minimize:
            if (!IsIconic(h)) return false;
            ShowWindowAsync(h, SW_SHOWNOACTIVATE);
            return true;
        case Hidden::Hide:
            if (IsWindowVisible(h)) return false;
            ShowWindowAsync(h, SW_SHOWNA);
            return true;
        case Hidden::None: break;
    }
    return false;
}

}  // namespace

void ShellBackend::Impl::journal_sync() {
    if (!journal || !journal->enabled()) return;
    std::lock_guard<std::mutex> jlock(journal_mutex);
    shell::JournalState s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [h, t] : by_hwnd) {
            if (t.hidden == Hidden::None) continue;
            shell::ParkedEntry e;
            e.window = from_hwnd(h);
            e.pid = t.last.process_id;
            e.restore = t.restore_frame;
            e.parked = t.parked_frame;
            e.method = uint32_t(t.hidden);
            s.parked.push_back(e);
        }
        for (auto& [id, bar] : appbars)
            if (bar.hwnd) s.reservations.push_back(from_hwnd(bar.hwnd));
    }
    for (auto& e : s.parked) e.pid_start = process_start_time(e.pid);
    journal->write(s);
}

RecoveryReport ShellBackend::Impl::recover_stale() {
    RecoveryReport report;
    DWORD self = GetCurrentProcessId();
    auto stale = shell::Journal::claim_stale(std::filesystem::path(journal->file()).parent_path(),
                                             process_alive, self);
    for (const shell::StaleJournal& j : stale) {
        ++report.journals;
        // Leaked reservations: the dead owner's appbar windows are gone, but
        // the shell may still hold their registrations (and the work-area
        // strip). ABM_REMOVE by handle gives the space back.
        for (uint64_t r : j.state.reservations) {
            HWND h = to_hwnd(r);
            if (IsWindow(h)) {
                DWORD owner = 0;
                GetWindowThreadProcessId(h, &owner);
                if (owner != j.pid && process_alive(owner, 0)) continue;  // the handle belongs to someone else now
            }
            appbar_remove(h);
            ++report.reservations_removed;
        }
        // Hidden windows: each restored on its own process's worker, so one
        // hung application does not hold up the others.
        std::vector<std::shared_future<bool>> pending;
        for (const shell::ParkedEntry& e : j.state.parked) {
            if (e.pid == 0) continue;  // key 0 is this job's own worker
            auto promise = std::make_shared<std::promise<bool>>();
            pending.push_back(promise->get_future().share());
            if (!workers->post(e.pid, [e, promise] {
                    DpiScope dpi;
                    promise->set_value(restore_entry(e));
                }))
                promise->set_value(false);
        }
        for (auto& p : pending) {
            if (p.wait_for(std::chrono::seconds(10)) == std::future_status::ready && p.get())
                ++report.windows_restored;
            else
                ++report.windows_skipped;
        }
        shell::Journal::discard(j);
    }
    if (report.reservations_removed) post([this] { report_monitors_if_changed(); });
    return report;
}

size_t ShellBackend::Impl::rescue_offscreen() {
    DpiScope dpi;
    Rect vs = virtual_screen();
    std::vector<HWND> all;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&all));
    size_t moved = 0;
    for (HWND h : all) {
        if (!in_scope(h) || !IsWindowVisible(h) || IsIconic(h) || h == listener) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        Rect f = frame_bounds(h);
        if (f.empty() || f.intersects(vs)) continue;
        Rect work = primary_work_area();
        Rect target{work.x + 64, work.y + 64, std::min(f.width, work.width - 128),
                    std::min(f.height, work.height - 128)};
        bool was_hidden = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = by_hwnd.find(h);
            if (it != by_hwnd.end()) {
                was_hidden = it->second.hidden != Hidden::None;
                it->second.hidden = Hidden::None;
            }
        }
        if (was_hidden) journal_sync();
        if (move_frame(h, target)) ++moved;
        request_report(h);
    }
    return moved;
}

}  // namespace brocompositor::win
