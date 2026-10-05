// Window discovery and lifecycle from the window server's list (no
// permission needed), enriched by Accessibility / Screen Recording where
// granted: added with the right identity, title and geometry, moves and
// title changes, minimize, close (also right after a minimize cycle, when
// AX re-creates the window's element), a window its application orders out,
// focus tracking, fullscreen Spaces, report_existing, and scope.
#include "harness.h"

using namespace bctest;
using namespace brocompositor;

namespace {

bool titles_visible() { return mac_permissions().accessibility || mac_permissions().screen_recording; }

void lifecycle() {
    std::printf("-- lifecycle\n");
    TestApp app;
    REQUIRE(app.start());
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    REQUIRE(log.wait<MonitorsChanged>([](const MonitorsChanged& e) { return !e.monitors.empty(); }));

    // The application had no window when the backend started: its first
    // window is reported with its title (the backend waits for the
    // application's first AX scan) and its geometry.
    Rect frame{160, 180, 420, 300};
    uint32_t cgid = app.create("first", frame, "C03030");
    REQUIRE(cgid != 0);
    auto added = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == cgid; });
    REQUIRE(added);
    const WindowSnapshot& s = added->window;
    WindowId id = s.id;
    CHECK(id != kNoWindow);
    CHECK_EQ(s.process_id, uint32_t(app.pid()));
    CHECK(near(s.frame, frame));
    CHECK_EQ(s.app_id, std::string("bc_mac_test_app"));
    CHECK(s.monitor != kNoMonitor);
    CHECK(s.dpi >= 96 && s.dpi % 48 == 0);
    CHECK(!s.minimized);
    if (titles_visible()) CHECK_EQ(s.title, std::string("first"));
    else CHECK_EQ(s.title, std::string());  // honest: not visible without a permission
    if (mac_permissions().accessibility) CHECK_EQ(s.class_name, std::string("AXStandardWindow"));
    CHECK_EQ(backend->find(cgid), id);
    CHECK_EQ(backend->native_handle(id), uint64_t(cgid));
    auto q = backend->query(id);
    REQUIRE(q);
    CHECK(near(q->frame, frame));

    // The application moves its window.
    log.mark();
    Rect moved{240, 220, 500, 320};
    REQUIRE(app.ok("move first 240 220 500 320"));
    CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) {
        return e.window.id == id && (e.changes & change::Geometry) && near(e.window.frame, moved);
    }));

    if (titles_visible()) {
        log.mark();
        REQUIRE(app.ok("title first renamed window"));
        CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) {
            return e.window.id == id && (e.changes & change::Title) && e.window.title == "renamed window";
        }));
    }

    // Focus tracking: the frontmost application's front-most window.
    if (!mac_permissions().screen_locked) {
        log.mark();
        uint32_t previous = frontmost_pid();
        REQUIRE(app.ok("activate first"));
        CHECK(log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == id; }, 5000ms));
        // Hand the front back to whoever had it: focus leaves the scope.
        if (previous && previous != uint32_t(app.pid())) {
            CHECK(app.ok("yield " + std::to_string(previous)));
            CHECK(log.wait<FocusChanged>([](const FocusChanged& e) { return e.id == kNoWindow; }, 5000ms));
        }
    } else {
        std::printf("   focus tracking: SKIP (screen locked)\n");
    }

    // Minimized by its application: tracked as minimized with Accessibility,
    // gone from the window server's on-screen set (removed) without.
    log.mark();
    REQUIRE(app.ok("minimize first"));
    WindowId current = id;
    if (mac_permissions().accessibility) {
        CHECK(log.wait<WindowChanged>(
            [&](const WindowChanged& e) { return e.window.id == id && (e.changes & change::State) && e.window.minimized; },
            5000ms));
        log.mark();
        REQUIRE(app.ok("unminimize first"));
        CHECK(log.wait<WindowChanged>(
            [&](const WindowChanged& e) { return e.window.id == id && (e.changes & change::State) && !e.window.minimized; },
            5000ms));
        CHECK(log.collect<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }).empty());
    } else {
        CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }, 5000ms));
        REQUIRE(app.ok("unminimize first"));
        auto again = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == cgid; }, 5000ms);
        CHECK(again);
        if (again) {
            CHECK(again->window.id != id);  // ids are never reused
            current = again->window.id;
        }
    }

    // Closed right after the minimize cycle (AX hands out a new element for
    // a deminiaturized window: the close must still be noticed).
    log.mark();
    REQUIRE(app.ok("close first"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == current; }, 5000ms));
    CHECK(!backend->query(current));
    CHECK_EQ(backend->find(cgid), kNoWindow);

    // Ordered out but kept by its application (a closed panel): the window
    // server still lists it, off screen; it is gone for the policy core.
    uint32_t panel = app.create("panel", Rect{300, 260, 300, 200}, "30C0C0");
    REQUIRE(panel != 0);
    auto padded = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == panel; });
    REQUIRE(padded);
    log.mark();
    REQUIRE(app.ok("orderout panel"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == padded->window.id; }, 5000ms));
    CHECK_EQ(backend->find(panel), kNoWindow);

    // Scope: nothing outside the process filter was ever reported.
    for (auto& e : log.collect<WindowAdded>([](const WindowAdded&) { return true; }))
        CHECK_EQ(e.window.process_id, uint32_t(app.pid()));
}

// A fullscreen window lives in its own Space. Entering it switches the
// active Space: the window reports fullscreen (AX), and a window left on the
// desktop Space is off screen there, so it is reported removed (as a window
// on another virtual desktop is on Windows) and comes back, as a new
// window, when the desktop is active again.
void fullscreen_space() {
    if (!mac_permissions().accessibility || mac_permissions().screen_locked) {
        std::printf("-- fullscreen Space: SKIP (needs Accessibility and an unlocked screen)\n");
        return;
    }
    std::printf("-- fullscreen Space\n");
    TestApp app;
    REQUIRE(app.start());
    uint32_t fs_id = app.create("fs", Rect{200, 200, 400, 300}, "C0C030");
    uint32_t left_id = app.create("left", Rect{700, 220, 300, 200}, "3030C0");
    REQUIRE(fs_id && left_id);
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    auto fs = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == fs_id; });
    auto left = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == left_id; });
    REQUIRE(fs && left);

    uint32_t previous = frontmost_pid();
    log.mark();
    REQUIRE(app.ok("activate fs"));
    std::string r = app.cmd("fullscreen fs", 8000);
    if (r != "ok") {
        CHECK_EQ(r, std::string("ok"));
        return;
    }
    CHECK(log.wait<WindowChanged>(
        [&](const WindowChanged& e) { return e.window.id == fs->window.id && e.window.fullscreen; }, 5000ms));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == left->window.id; }, 5000ms));
    // A fullscreen window lives in its own Space: hiding it is refused.
    auto hide = backend->set_visible(fs->window.id, false);
    CHECK(hide.wait_for(5s) == std::future_status::ready && !hide.get());

    log.mark();
    CHECK_EQ(app.cmd("unfullscreen fs", 8000), std::string("ok"));
    CHECK(log.wait<WindowChanged>(
        [&](const WindowChanged& e) { return e.window.id == fs->window.id && !e.window.fullscreen; }, 5000ms));
    auto back = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == left_id; }, 5000ms);
    CHECK(back);
    if (back) CHECK(back->window.id != left->window.id);
    if (previous && previous != uint32_t(app.pid())) app.ok("yield " + std::to_string(previous));
}

void report_existing_off() {
    std::printf("-- report_existing = false\n");
    TestApp app;
    REQUIRE(app.start());
    uint32_t old_id = app.create("old", Rect{180, 200, 300, 200}, "30C030");
    REQUIRE(old_id != 0);
    auto config = test_shell_config(app.pid());
    config.report_existing = false;
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    uint32_t new_id = app.create("new", Rect{220, 240, 300, 200}, "3030C0");
    REQUIRE(new_id != 0);
    CHECK(log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == new_id; }));
    log.settle(500ms);
    CHECK(log.collect<WindowAdded>([&](const WindowAdded& e) { return e.window.native == old_id; }).empty());
}

}  // namespace

int main() {
    mac_permissions();
    require_display();
    lifecycle();
    fullscreen_space();
    report_existing_off();
    return finish("test_mac_lifecycle");
}
