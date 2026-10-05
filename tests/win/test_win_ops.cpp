// Window operations on foreign windows: exact frame placement (per monitor,
// across DPI), restore-before-place, the three hide methods, restore on
// backend destruction, off-screen rescue, close, and isolation from a hung
// application (operations never block the caller, and a hung application
// delays only its own windows' operations).
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

std::unique_ptr<win::ShellBackend> make_shell(std::vector<uint32_t> pids, win::HideMethod method) {
    win::ShellConfig cfg = test_shell_config(0);
    cfg.process_filter = std::move(pids);
    cfg.hide_method = method;
    std::string err;
    auto shell = win::ShellBackend::create(cfg, &err);
    if (!shell) std::fprintf(stderr, "ShellBackend::create: %s\n", err.c_str());
    return shell;
}

WindowId id_of(EventLog& log, HWND h) {
    auto a = log.wait<WindowAdded>([h](const WindowAdded& e) { return e.window.native == native(h); });
    return a ? a->window.id : kNoWindow;
}

bool visible_on_any_monitor(HWND h) {
    Rect f = frame_of(h);
    RECT r{f.x, f.y, f.right(), f.bottom()};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

template <class T>
T done(const Completion<T>& c, std::chrono::milliseconds timeout = 5000ms) {
    if (c.wait_for(timeout) != std::future_status::ready) {
        std::fprintf(stderr, "operation did not complete in %lld ms\n", (long long)timeout.count());
        return T{};
    }
    return c.get();
}

void placement(TestApp& app, HWND h) {
    auto shell = make_shell({app.pid()}, win::HideMethod::Park);
    REQUIRE(shell);
    EventLog log(shell->events());
    WindowId id = id_of(log, h);
    REQUIRE(id != kNoWindow);

    // Exact visible-frame placement on every monitor (crossing DPI when the
    // monitors differ), with the snapshot reporting the right monitor/DPI.
    for (const auto& m : shell->monitors()) {
        Rect target{m.work_area.x + 40, m.work_area.y + 30, std::min(700, m.work_area.width - 80),
                    std::min(500, m.work_area.height - 60)};
        CHECK(done(shell->place(id, target)));
        CHECK_EQ(frame_of(h), target);
        auto q = shell->query(id);
        REQUIRE(q.has_value());
        CHECK_EQ(q->monitor, m.id);
        CHECK_EQ(q->dpi, m.dpi);
        CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) {
                     return e.window.id == id && e.window.frame == target && e.window.monitor == m.id;
                 }).has_value());
        std::printf("  monitor %s dpi %u: frame %d,%d %dx%d, outer %d,%d %dx%d\n", m.name.c_str(), m.dpi,
                    target.x, target.y, target.width, target.height, outer_of(h).x, outer_of(h).y,
                    outer_of(h).width, outer_of(h).height);
    }

    Rect wa = primary_work_area();
    Rect target{wa.x + 50, wa.y + 50, 640, 480};
    // A maximized or minimized window is restored before it is placed.
    CHECK(app.ok("maximize w"));
    CHECK(wait_until([&] { return IsZoomed(h) != FALSE; }));
    CHECK(done(shell->place(id, target)));
    CHECK(!IsZoomed(h));
    CHECK_EQ(frame_of(h), target);
    CHECK(app.ok("minimize w"));
    CHECK(wait_until([&] { return IsIconic(h) != FALSE; }));
    Rect t2{wa.x + 80, wa.y + 60, 600, 420};
    CHECK(done(shell->place(id, t2)));
    CHECK(!IsIconic(h));
    CHECK_EQ(frame_of(h), t2);
    // Unknown ids fail cleanly and at once.
    auto bad = shell->place(987654, t2);
    CHECK(bad.wait_for(0ms) == std::future_status::ready && !bad.get());
    CHECK(!done(shell->set_visible(987654, false)));
    CHECK(!shell->execute(PlaceWindow{987654, t2}));
}

void hide_methods(TestApp& app, HWND h) {
    Rect wa = primary_work_area();
    Rect home{wa.x + 120, wa.y + 90, 640, 400};
    for (auto method : {win::HideMethod::Park, win::HideMethod::Minimize, win::HideMethod::Hide}) {
        auto shell = make_shell({app.pid()}, method);
        REQUIRE(shell);
        EventLog log(shell->events());
        WindowId id = id_of(log, h);
        REQUIRE(id != kNoWindow);
        CHECK(done(shell->place(id, home)));
        log.settle();
        log.mark();

        CHECK(done(shell->set_visible(id, false)));
        CHECK(wait_until([&] { return !visible_on_any_monitor(h) || IsIconic(h) || !IsWindowVisible(h); }));
        switch (method) {
            case win::HideMethod::Park:
                // Still a normal visible window (capture keeps running), just
                // outside every monitor.
                CHECK(IsWindowVisible(h) && !IsIconic(h));
                CHECK(!frame_of(h).intersects(virtual_screen_rect()));
                break;
            case win::HideMethod::Minimize: CHECK(IsIconic(h)); break;
            case win::HideMethod::Hide: CHECK(!IsWindowVisible(h)); break;
        }
        // The backend's own hiding is not reported as a fact: no removal, no
        // geometry/state change.
        log.settle();
        CHECK(log.collect<WindowRemoved>([](const WindowRemoved&) { return true; }).empty());
        CHECK(log.collect<WindowChanged>([](const WindowChanged&) { return true; }).empty());
        CHECK(done(shell->set_visible(id, false)));  // idempotent

        CHECK(done(shell->set_visible(id, true)));
        CHECK(wait_until([&] { return frame_of(h) == home && IsWindowVisible(h) && !IsIconic(h); }));
        CHECK_EQ(frame_of(h), home);
        log.settle();
        CHECK(log.collect<WindowRemoved>([](const WindowRemoved&) { return true; }).empty());
        std::printf("  hide method %d round trip ok\n", int(method));
    }

    // Destroying the backend puts hidden windows back.
    {
        auto shell = make_shell({app.pid()}, win::HideMethod::Park);
        REQUIRE(shell);
        EventLog log(shell->events());
        WindowId id = id_of(log, h);
        CHECK(done(shell->set_visible(id, false)));
        CHECK(!visible_on_any_monitor(h));
    }
    CHECK_EQ(frame_of(h), home);
}

void rescue_and_close(TestApp& app, HWND h) {
    // Simulate a window left parked by a crashed shell that kept no journal.
    Rect vs = virtual_screen_rect();
    CHECK(app.ok("move w " + std::to_string(vs.right() + 500) + " " + std::to_string(vs.y) + " 600 400"));
    CHECK(wait_until([&] { return !visible_on_any_monitor(h); }));
    auto shell = make_shell({app.pid()}, win::HideMethod::Park);
    REQUIRE(shell);
    EventLog log(shell->events());
    WindowId id = id_of(log, h);
    CHECK_EQ(done(shell->rescue_offscreen_windows()), size_t(1));
    CHECK(primary_work_area().contains(frame_of(h)));
    CHECK(done(shell->close(id)));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }).has_value());
    CHECK(wait_until([&] { return !IsWindow(h); }));
}

// A hung application: its main thread stops pumping, so any SetWindowPos on
// its window blocks until it recovers (Windows only reports a window hung
// after ~5 s, so the async fallbacks do not apply yet). The backend's caller
// must not notice, and another application's windows keep moving.
void hung_application() {
    TestApp hung, healthy;
    REQUIRE(hung.start() && healthy.start());
    Rect wa = primary_work_area();
    HWND hh = hung.create("h", Rect{wa.x + 60, wa.y + 60, 500, 360}, "aa5500");
    HWND ok = healthy.create("k", Rect{wa.x + 620, wa.y + 60, 500, 360}, "0055aa");
    REQUIRE(hh && ok);
    auto shell = make_shell({hung.pid(), healthy.pid()}, win::HideMethod::Park);
    REQUIRE(shell);
    EventLog log(shell->events());
    WindowId ih = id_of(log, hh), ik = id_of(log, ok);
    REQUIRE(ih && ik);

    CHECK(hung.ok("hang 2500"));  // replies, then stops pumping for 2.5 s
    Sleep(100);
    Rect rh{wa.x + 90, wa.y + 90, 480, 340}, rk{wa.x + 650, wa.y + 90, 480, 340};
    auto t0 = std::chrono::steady_clock::now();
    auto fh = shell->place(ih, rh);
    auto ff = shell->focus(ih);
    auto fk = shell->place(ik, rk);
    auto vis = shell->set_visible(ih, false);
    auto host_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  host calls against a hung app returned in %lld ms\n", (long long)host_ms);
    CHECK(host_ms < 100);

    // The healthy application's window moves while the hung one's is stuck.
    CHECK(done(fk, 1500ms));
    CHECK_EQ(frame_of(ok), rk);
    CHECK(fh.wait_for(0ms) != std::future_status::ready);
    // Its operations run, in order, once it recovers.
    CHECK(done(fh, 6000ms));
    CHECK(done(vis, 6000ms));
    CHECK(!visible_on_any_monitor(hh));
    CHECK(ff.wait_for(6000ms) == std::future_status::ready);
    CHECK(done(shell->set_visible(ih, true)));
    CHECK_EQ(frame_of(hh), rh);

    // A backend torn down while an application is hung returns within its
    // shutdown_timeout, leaving the hidden window to the journal.
    CHECK(done(shell->set_visible(ih, false)));
    CHECK(hung.ok("hang 3000"));
    Sleep(100);
    shell->set_visible(ik, false);  // healthy: restored by the destructor
    t0 = std::chrono::steady_clock::now();
    shell.reset();
    auto teardown_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  teardown with a hung app: %lld ms\n", (long long)teardown_ms);
    CHECK(teardown_ms < 2800);
    CHECK(visible_on_any_monitor(ok));
    // The window the hung app could not take back is in the journal.
    size_t parked_entries = 0;
    std::error_code ec;
    for (auto& f : std::filesystem::directory_iterator(test_journal_dir(), ec)) {
        std::ifstream in(f.path());
        std::string l;
        while (std::getline(in, l)) parked_entries += l.rfind("park ", 0) == 0 ? 1 : 0;
    }
    CHECK_EQ(parked_entries, size_t(1));
    // Once the app wakes, the operation that was stuck in it completes (the
    // process still runs): the window comes back either through that or
    // through a new backend's journal recovery.
    Sleep(3200);
    auto next = make_shell({hung.pid(), healthy.pid()}, win::HideMethod::Park);
    REQUIRE(next);
    auto report = done(next->recovery(), 15000ms);
    std::printf("  recovery: %zu journals, %zu windows restored, %zu skipped\n", report.journals,
                report.windows_restored, report.windows_skipped);
    CHECK(wait_until([&] { return visible_on_any_monitor(hh); }, 5000ms));
}

}  // namespace

int main() {
    init_windows_test();
    TestApp app;
    if (!app.start()) return 1;
    Rect wa = primary_work_area();
    HWND h = app.create("w", Rect{wa.x + 100, wa.y + 100, 800, 600}, "2266aa");
    if (!h) return 1;
    placement(app, h);
    hide_methods(app, h);
    rescue_and_close(app, h);
    hung_application();
    return finish("test_win_ops");
}
