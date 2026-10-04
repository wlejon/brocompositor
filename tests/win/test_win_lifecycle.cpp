// Window discovery and lifecycle against real windows owned by a child process.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

std::function<bool(const WindowAdded&)> added(HWND h) {
    return [h](const WindowAdded& e) { return e.window.native == native(h); };
}

void run() {
    TestApp app;
    REQUIRE(app.start());
    TestApp outsider;  // a second process outside the backend's scope
    REQUIRE(outsider.start());

    Rect wa = primary_work_area();
    Rect r0{wa.x + 60, wa.y + 60, 500, 400};
    HWND pre = app.create("pre", r0, "336699");
    REQUIRE(pre);

    win::ShellConfig cfg;
    cfg.process_filter = {app.pid()};
    std::string err;
    auto shell = win::ShellBackend::create(cfg, &err);
    if (!shell) std::fprintf(stderr, "create: %s\n", err.c_str());
    REQUIRE(shell);
    EventLog log(shell->events());

    // Initial state: monitors, the pre-existing window, focus.
    auto mons = log.wait<MonitorsChanged>([](const MonitorsChanged&) { return true; });
    REQUIRE(mons.has_value());
    CHECK(!mons->monitors.empty());
    int primaries = 0;
    for (auto& m : mons->monitors) {
        primaries += m.primary;
        CHECK(m.work_area.width > 0 && m.bounds.contains(m.work_area));
        CHECK(m.dpi >= 96);
    }
    CHECK_EQ(primaries, 1);
    auto a0 = log.wait<WindowAdded>(added(pre));
    REQUIRE(a0.has_value());
    WindowId pre_id = a0->window.id;
    CHECK_EQ(a0->window.title, std::string("pre"));
    CHECK_EQ(a0->window.app_id, std::string("bc_test_app.exe"));
    CHECK_EQ(a0->window.class_name, std::string("bc_test_app"));
    CHECK_EQ(a0->window.process_id, uint32_t(app.pid()));
    CHECK_EQ(a0->window.frame, frame_of(pre));
    // The visible frame excludes the invisible resize borders.
    CHECK(outer_of(pre).contains(a0->window.frame));
    CHECK(a0->window.monitor != kNoMonitor);
    CHECK(log.wait<FocusChanged>([](const FocusChanged&) { return true; }).has_value());

    // A window created hidden is discovered when it is shown (not at creation).
    log.mark();
    HWND late = app.create("late", Rect{wa.x + 100, wa.y + 100, 400, 300}, "993366", "hidden");
    REQUIRE(late);
    CHECK(!log.wait<WindowAdded>(added(late), 400ms).has_value());
    CHECK(app.ok("show late"));
    auto a1 = log.wait<WindowAdded>(added(late));
    REQUIRE(a1.has_value());
    WindowId late_id = a1->window.id;
    CHECK(late_id != pre_id);

    // Tool windows are never managed, and destroying one reports nothing.
    log.mark();
    HWND tool = app.create("tool", Rect{wa.x + 120, wa.y + 120, 200, 200}, "777777", "tool");
    REQUIRE(tool);
    CHECK(app.ok("destroy tool"));
    log.settle();
    CHECK(log.collect<WindowAdded>([](const WindowAdded&) { return true; }).empty());
    CHECK(log.collect<WindowRemoved>([](const WindowRemoved&) { return true; }).empty());

    // Windows of other processes are invisible to a filtered backend.
    HWND foreign = outsider.create("foreign", Rect{wa.x + 140, wa.y + 140, 300, 200}, "aaaaaa");
    REQUIRE(foreign);
    log.settle();
    CHECK(log.collect<WindowAdded>([](const WindowAdded&) { return true; }).empty());

    // Title, geometry and state changes arrive as WindowChanged with the
    // right change bits and a fresh snapshot.
    CHECK(app.ok("title late hello brocompositor"));
    auto c1 = log.wait<WindowChanged>([&](const WindowChanged& e) {
        return e.window.id == late_id && (e.changes & change::Title);
    });
    CHECK(c1 && c1->window.title == "hello brocompositor");

    CHECK(app.ok("move late " + std::to_string(wa.x + 300) + " " + std::to_string(wa.y + 200) + " 640 480"));
    auto c2 = log.wait<WindowChanged>([&](const WindowChanged& e) {
        return e.window.id == late_id && (e.changes & change::Geometry) && e.window.frame == frame_of(late);
    });
    CHECK(c2.has_value());
    CHECK_EQ(c2 ? c2->window.frame.width : 0, frame_of(late).width);

    CHECK(app.ok("minimize late"));
    auto c3 = log.wait<WindowChanged>(
        [&](const WindowChanged& e) { return e.window.id == late_id && e.window.minimized; });
    CHECK(c3 && (c3->changes & change::State));
    CHECK(app.ok("restore late"));
    CHECK(log.wait<WindowChanged>(
                 [&](const WindowChanged& e) { return e.window.id == late_id && !e.window.minimized; })
              .has_value());
    CHECK(app.ok("maximize late"));
    CHECK(log.wait<WindowChanged>(
                 [&](const WindowChanged& e) { return e.window.id == late_id && e.window.maximized; })
              .has_value());
    CHECK(app.ok("restore late"));

    // Owned dialogs name their owner.
    HWND dlg = app.create("dlg", Rect{wa.x + 200, wa.y + 200, 300, 200}, "cccc00", "owner=pre");
    REQUIRE(dlg);
    auto a2 = log.wait<WindowAdded>(added(dlg));
    REQUIRE(a2.has_value());
    CHECK_EQ(a2->window.owner, pre_id);

    // Removal: destroy and app-initiated hide both remove; a re-shown window
    // comes back under a new id (ids are never reused).
    CHECK(app.ok("destroy dlg"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == a2->window.id; }).has_value());
    CHECK(app.ok("hide late"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == late_id; }).has_value());
    CHECK(app.ok("show late"));
    auto a3 = log.wait<WindowAdded>(added(late));
    CHECK(a3 && a3->window.id != late_id);
    CHECK(app.ok("destroy late"));
    CHECK(app.ok("destroy pre"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return a3 && e.id == a3->window.id; }).has_value());
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == pre_id; }).has_value());

    // Every add has a matching remove, and nothing outside the filter leaked.
    log.settle();
    size_t adds = 0, removes = 0;
    for (const auto& e : log.all()) {
        if (auto* a = std::get_if<WindowAdded>(&e)) {
            ++adds;
            CHECK_EQ(a->window.process_id, uint32_t(app.pid()));
        }
        if (std::holds_alternative<WindowRemoved>(e)) ++removes;
    }
    CHECK_EQ(adds, removes);
    CHECK(!shell->query(pre_id).has_value());
}

}  // namespace

int main() {
    init_windows_test();
    run();
    return finish("test_win_lifecycle");
}
