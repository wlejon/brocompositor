// Window operations on foreign windows: exact frame placement (per monitor,
// across DPI), restore-before-place, the three hide methods, restore on
// backend destruction, off-screen rescue, close.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

std::unique_ptr<win::ShellBackend> make_shell(DWORD pid, win::HideMethod method) {
    win::ShellConfig cfg;
    cfg.process_filter = {pid};
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

void placement(TestApp& app, HWND h) {
    auto shell = make_shell(app.pid(), win::HideMethod::Park);
    REQUIRE(shell);
    EventLog log(shell->events());
    WindowId id = id_of(log, h);
    REQUIRE(id != kNoWindow);

    // Exact visible-frame placement on every monitor (crossing DPI when the
    // monitors differ), with the snapshot reporting the right monitor/DPI.
    for (const auto& m : shell->monitors()) {
        Rect target{m.work_area.x + 40, m.work_area.y + 30, std::min(700, m.work_area.width - 80),
                    std::min(500, m.work_area.height - 60)};
        CHECK(shell->place(id, target));
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
    CHECK(shell->place(id, target));
    CHECK(!IsZoomed(h));
    CHECK_EQ(frame_of(h), target);
    CHECK(app.ok("minimize w"));
    CHECK(wait_until([&] { return IsIconic(h) != FALSE; }));
    Rect t2{wa.x + 80, wa.y + 60, 600, 420};
    CHECK(shell->place(id, t2));
    CHECK(!IsIconic(h));
    CHECK_EQ(frame_of(h), t2);
    // Unknown ids fail cleanly.
    CHECK(!shell->place(987654, t2));
    CHECK(!shell->set_visible(987654, false));
}

void hide_methods(TestApp& app, HWND h) {
    Rect wa = primary_work_area();
    Rect home{wa.x + 120, wa.y + 90, 640, 400};
    for (auto method : {win::HideMethod::Park, win::HideMethod::Minimize, win::HideMethod::Hide}) {
        auto shell = make_shell(app.pid(), method);
        REQUIRE(shell);
        EventLog log(shell->events());
        WindowId id = id_of(log, h);
        REQUIRE(id != kNoWindow);
        CHECK(shell->place(id, home));
        log.settle();
        log.mark();

        CHECK(shell->set_visible(id, false));
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
        CHECK(shell->set_visible(id, false));  // idempotent

        CHECK(shell->set_visible(id, true));
        CHECK(wait_until([&] { return frame_of(h) == home && IsWindowVisible(h) && !IsIconic(h); }));
        CHECK_EQ(frame_of(h), home);
        log.settle();
        CHECK(log.collect<WindowRemoved>([](const WindowRemoved&) { return true; }).empty());
        std::printf("  hide method %d round trip ok\n", int(method));
    }

    // Destroying the backend puts hidden windows back.
    {
        auto shell = make_shell(app.pid(), win::HideMethod::Park);
        REQUIRE(shell);
        EventLog log(shell->events());
        WindowId id = id_of(log, h);
        CHECK(shell->set_visible(id, false));
        CHECK(!visible_on_any_monitor(h));
    }
    CHECK_EQ(frame_of(h), home);
}

void rescue_and_close(TestApp& app, HWND h) {
    // Simulate a window left parked by a crashed shell.
    Rect vs = virtual_screen_rect();
    CHECK(app.ok("move w " + std::to_string(vs.right() + 500) + " " + std::to_string(vs.y) + " 600 400"));
    CHECK(wait_until([&] { return !visible_on_any_monitor(h); }));
    auto shell = make_shell(app.pid(), win::HideMethod::Park);
    REQUIRE(shell);
    EventLog log(shell->events());
    WindowId id = id_of(log, h);
    CHECK_EQ(shell->rescue_offscreen_windows(), size_t(1));
    CHECK(primary_work_area().contains(frame_of(h)));
    CHECK(shell->close(id));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }).has_value());
    CHECK(wait_until([&] { return !IsWindow(h); }));
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
    return finish("test_win_ops");
}
