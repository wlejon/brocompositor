// Crash recovery: the journal mirrors every window this backend parked or
// minimized; a backend starting later undoes what a dead instance left
// (shell/journal.h). Also the journal-less off-screen rescue. Both need
// Accessibility: without it every journaled window counts as skipped and
// the journal is kept for a later instance that has the permission.
#include "mac/shell_impl.h"

#include <cstdlib>
#include <thread>

namespace brocompositor::mac {

namespace {

std::vector<Rect> current_displays() {
    std::vector<Rect> out;
    for (const auto& s : sys::screens()) out.push_back(s.frame);
    return out;
}

Rect main_visible_frame() {
    for (const auto& s : sys::screens())
        if (s.primary) return s.visible.empty() ? s.frame : s.visible;
    return Rect{0, 0, 1280, 800};
}

// `frame` if a usable part of it is on a display, else the same size inside
// the main display's visible frame.
Rect onto_a_display(const Rect& frame, const std::vector<Rect>& displays) {
    if (visible_area(frame, displays) > kParkedVisibleArea) return frame;
    Rect work = main_visible_frame();
    return Rect{work.x + 64, work.y + 64, std::min(frame.width, work.width - 128),
                std::min(frame.height, work.height - 128)};
}

// Undoes one journaled hide if the window is still the one we hid and still
// hidden. True when it was put back.
bool restore_entry(AppWorker::Context& c, const shell::ParkedEntry& e) {
    if (sys::process_start_time(e.pid) != e.pid_start) return false;  // a different process now
    AXUIElementRef w = c.find(uint32_t(e.window));
    if (!w) return false;
    AXError err = kAXErrorSuccess;
    auto info = ax::info(w, &err);
    c.worker->note(err);
    if (!info) return false;
    std::vector<Rect> displays = current_displays();
    switch (Hidden(e.method)) {
        case Hidden::Park:
            // Only if nothing moved it back onto a display meanwhile.
            if (visible_area(info->frame, displays) > kParkedVisibleArea) return false;
            return set_frame(w, onto_a_display(e.restore, displays)) == kAXErrorSuccess;
        case Hidden::Minimize:
            if (!info->minimized) return false;
            return ax::set_bool(w, kAXMinimizedAttribute, false) == kAXErrorSuccess;
        case Hidden::None: break;
    }
    return false;
}

}  // namespace

std::string default_journal_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/Library/Application Support/brocompositor/journal";
}

void ShellBackend::Impl::journal_sync() {
    if (!journal || !journal->enabled()) return;
    std::lock_guard<std::mutex> jl(journal_mutex);
    shell::JournalState state;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [cgid, t] : by_cgid) {
            if (t.hidden == Hidden::None) continue;
            shell::ParkedEntry e;
            e.window = cgid;
            e.pid = t.pid;
            e.pid_start = t.pid_start;
            e.restore = t.restore_frame;
            e.parked = t.parked_frame;
            e.method = uint32_t(t.hidden);
            state.parked.push_back(e);
        }
    }
    journal->write(state);
}

RecoveryReport ShellBackend::Impl::recover_stale() {
    RecoveryReport report;
    std::filesystem::path dir = journal->file().parent_path();
    auto stale = shell::Journal::claim_stale(dir, sys::process_alive, self_pid);
    for (auto& j : stale) {
        ++report.journals;
        bool keep = false;
        for (const shell::ParkedEntry& e : j.state.parked) {
            if (!sys::process_alive(e.pid, e.pid_start)) {
                ++report.windows_skipped;  // its application is gone, and the window with it
                continue;
            }
            if (!permissions.accessibility) {
                ++report.windows_skipped;
                keep = true;
                continue;
            }
            auto done = run_on<bool>(e.pid, false, [e](Impl&, AppWorker::Context& c) { return restore_entry(c, e); });
            if (done.wait_for(std::chrono::seconds(10)) == std::future_status::ready && done.get())
                ++report.windows_restored;
            else
                ++report.windows_skipped;
        }
        // Without Accessibility nothing could be undone: hand the journal
        // back under its dead owner's name, where a later instance (with the
        // permission) claims it again.
        if (keep) {
            std::error_code ec;
            std::filesystem::rename(
                j.file, dir / (std::to_string(j.pid) + "-" + std::to_string(j.pid_start) + ".journal"), ec);
        } else {
            shell::Journal::discard(j);
        }
    }
    return report;
}

size_t ShellBackend::Impl::rescue_offscreen() {
    if (!permissions.accessibility) return 0;
    std::vector<Rect> displays = current_displays();
    std::vector<std::pair<uint32_t, uint32_t>> lost;  // pid, cgid
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [cgid, t] : by_cgid)
            if (t.hidden == Hidden::None && !t.last.minimized &&
                visible_area(t.last.frame, displays) <= kParkedVisibleArea)
                lost.emplace_back(t.pid, cgid);
    }
    std::vector<Completion<bool>> results;
    for (auto [pid, cgid] : lost)
        results.push_back(run_on<bool>(pid, false, [cgid](Impl&, AppWorker::Context& c) {
            AXUIElementRef w = c.find(cgid);
            if (!w) return false;
            auto f = ax::frame(w, nullptr);
            if (!f) return false;
            std::vector<Rect> now = current_displays();
            Rect target = onto_a_display(*f, now);
            return target != *f && set_frame(w, target) == kAXErrorSuccess;
        }));
    size_t moved = 0;
    for (auto& r : results)
        if (r.wait_for(std::chrono::seconds(10)) == std::future_status::ready && r.get()) ++moved;
    return moved;
}

}  // namespace brocompositor::mac
