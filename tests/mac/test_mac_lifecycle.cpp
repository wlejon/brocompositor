// Window discovery and lifecycle from the window server's list (no
// permission needed), enriched by Accessibility / Screen Recording where
// granted: added with the right identity and geometry, moves and title
// changes, minimize, close, focus tracking, report_existing, and scope.
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
    CHECK_EQ(backend->find(cgid), id);
    CHECK_EQ(backend->native_handle(id), uint64_t(cgid));
    auto q = backend->query(id);
    REQUIRE(q);
    CHECK(near(q->frame, frame));

    // The application moves its window.
    Rect moved{240, 220, 500, 320};
    REQUIRE(app.ok("move first 240 220 500 320"));
    auto changed = log.wait<WindowChanged>([&](const WindowChanged& e) {
        return e.window.id == id && (e.changes & change::Geometry) && near(e.window.frame, moved);
    });
    CHECK(changed);

    if (titles_visible()) {
        REQUIRE(app.ok("title first renamed window"));
        CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) {
            return e.window.id == id && (e.changes & change::Title) && e.window.title == "renamed window";
        }));
    }

    // Focus tracking: the frontmost application's front-most window.
    if (!mac_permissions().screen_locked) {
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
    REQUIRE(app.ok("minimize first"));
    WindowId current = id;
    if (mac_permissions().accessibility) {
        CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) { return e.window.id == id && e.window.minimized; },
                                      5000ms));
        REQUIRE(app.ok("unminimize first"));
        CHECK(log.wait<WindowChanged>(
            [&](const WindowChanged& e) { return e.window.id == id && !e.window.minimized; }, 5000ms));
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

    REQUIRE(app.ok("close first"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == current; }));
    CHECK(!backend->query(current));
    CHECK_EQ(backend->find(cgid), kNoWindow);

    // Scope: nothing outside the process filter was ever reported.
    for (auto& e : log.collect<WindowAdded>([](const WindowAdded&) { return true; }))
        CHECK_EQ(e.window.process_id, uint32_t(app.pid()));
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
    report_existing_off();
    return finish("test_mac_lifecycle");
}
