// Window operations through Accessibility, on windows of a test application:
// place, park / show (journaled), minimize hiding, close, restore on
// teardown; and a hung application stalls nothing but its own operations.
// Needs Accessibility (skips otherwise, naming the binary to grant it to).
#include "harness.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace bctest;
using namespace brocompositor;

namespace {

template <class T>
bool done(const Completion<T>& c, std::chrono::milliseconds t = 5000ms) {
    return c.wait_for(t) == std::future_status::ready;
}

Rect app_frame(TestApp& app, const std::string& name) {
    std::istringstream in(app.cmd("frame " + name));
    std::string ok;
    double x = 0, y = 0, w = 0, h = 0;
    in >> ok >> x >> y >> w >> h;
    return Rect{int32_t(x), int32_t(y), int32_t(w), int32_t(h)};
}

size_t journal_parked_entries() {
    size_t n = 0;
    for (auto& f : std::filesystem::directory_iterator(test_journal_dir())) {
        std::ifstream in(f.path());
        std::string line;
        while (std::getline(in, line)) n += line.rfind("park ", 0) == 0 ? 1 : 0;
    }
    return n;
}

int64_t on_displays(const Rect& r, const std::vector<MonitorSnapshot>& ms) {
    int64_t a = 0;
    for (const auto& m : ms) a += m.bounds.intersected(r).area();
    return a;
}

WindowId add(EventLog& log, uint32_t cgid) {
    auto e = log.wait<WindowAdded>([&](const WindowAdded& a) { return a.window.native == cgid; });
    return e ? e->window.id : kNoWindow;
}

void operations() {
    std::printf("-- place / park / show / close\n");
    TestApp app;
    REQUIRE(app.start());
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    uint32_t cgid = app.create("ops", Rect{200, 200, 400, 300}, "C08030");
    WindowId id = add(log, cgid);
    REQUIRE(id != kNoWindow);
    auto monitors = backend->monitors();

    Rect target{260, 240, 520, 360};
    auto placed = backend->place(id, target);
    REQUIRE(done(placed));
    CHECK(placed.get());
    CHECK(near(app_frame(app, "ops"), target));
    CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) { return e.window.id == id && near(e.window.frame, target); }));

    // Park: (almost) nothing left on any display, journaled while hidden,
    // no geometry facts reported for the backend's own move.
    log.mark();
    auto hidden = backend->set_visible(id, false);
    REQUIRE(done(hidden));
    CHECK(hidden.get());
    auto q = backend->query(id);
    REQUIRE(q);
    std::printf("   parked at %d,%d (%lld pt^2 visible)\n", q->frame.x, q->frame.y,
                (long long)on_displays(q->frame, monitors));
    CHECK(on_displays(q->frame, monitors) <= 64 * 64 || q->minimized);
    CHECK_EQ(journal_parked_entries(), size_t(1));
    log.settle(400ms);
    CHECK(log.collect<WindowChanged>([&](const WindowChanged& e) {
              return e.window.id == id && (e.changes & change::Geometry);
          }).empty());
    CHECK(log.collect<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }).empty());

    auto shown = backend->set_visible(id, true);
    REQUIRE(done(shown));
    CHECK(shown.get());
    CHECK(near(app_frame(app, "ops"), target));
    CHECK_EQ(journal_parked_entries(), size_t(0));

    // Minimize as the hide method.
    {
        auto config = test_shell_config(app.pid());
        config.hide_method = mac::HideMethod::Minimize;
        auto b2 = mac::ShellBackend::create(config, &err);
        REQUIRE(b2);
        EventLog log2(b2->events());
        WindowId id2 = add(log2, cgid);
        REQUIRE(id2 != kNoWindow);
        auto h = b2->set_visible(id2, false);
        REQUIRE(done(h));
        CHECK(h.get());
        CHECK(eventually([&] { auto s = b2->query(id2); return s && s->minimized; }) ||
              log2.wait<WindowChanged>([&](const WindowChanged& e) { return e.window.id == id2 && e.window.minimized; }));
        // Teardown puts it back.
    }
    CHECK(eventually([&] { return near(app_frame(app, "ops"), target); }, 5000ms));

    auto closed = backend->close(id);
    REQUIRE(done(closed));
    CHECK(closed.get());
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }));
}

void teardown_restores() {
    std::printf("-- teardown restores parked windows\n");
    TestApp app;
    REQUIRE(app.start());
    Rect frame{220, 200, 380, 260};
    uint32_t cgid = app.create("park", frame, "8030C0");
    REQUIRE(cgid);
    {
        std::string err;
        auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
        REQUIRE(backend);
        EventLog log(backend->events());
        WindowId id = add(log, cgid);
        REQUIRE(id);
        auto h = backend->set_visible(id, false);
        REQUIRE(done(h));
        CHECK(h.get());
        CHECK(!near(app_frame(app, "park"), frame));
    }
    CHECK(near(app_frame(app, "park"), frame));
    CHECK_EQ(journal_parked_entries(), size_t(0));
}

// A hung application: every host call returns at once, the healthy
// application's operations go ahead, the hung one's fail after its
// messaging timeout, and teardown is bounded and leaves the hung window
// journaled.
void hung_application() {
    std::printf("-- hung application\n");
    TestApp healthy, hung;
    REQUIRE(healthy.start());
    REQUIRE(hung.start());
    uint32_t hw = healthy.create("healthy", Rect{200, 200, 300, 200}, "30A030");
    uint32_t uw = hung.create("hung", Rect{560, 200, 300, 200}, "A03030");
    REQUIRE(hw && uw);
    auto config = test_shell_config(healthy.pid());
    config.process_filter.push_back(uint32_t(hung.pid()));
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    WindowId hid = add(log, hw), uid = add(log, uw);
    REQUIRE(hid && uid);

    // Park the hung one first (it answers), then hang it.
    auto parked = backend->set_visible(uid, false);
    REQUIRE(done(parked));
    CHECK(parked.get());
    REQUIRE(hung.ok("hang 6000"));

    auto t0 = std::chrono::steady_clock::now();
    auto place_hung = backend->place(uid, Rect{600, 260, 300, 200});
    auto focus_hung = backend->focus(uid);
    auto place_healthy = backend->place(hid, Rect{240, 240, 320, 220});
    auto calls = std::chrono::steady_clock::now() - t0;
    std::printf("   host calls took %lld ms\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(calls).count());
    CHECK(calls < 100ms);
    REQUIRE(done(place_healthy, 3000ms));
    CHECK(place_healthy.get());
    CHECK(near(app_frame(healthy, "healthy"), Rect{240, 240, 320, 220}));
    // The hung application's operations fail after the messaging timeout.
    CHECK(done(place_hung, 4000ms));
    CHECK(done(focus_hung, 4000ms));

    auto t1 = std::chrono::steady_clock::now();
    backend.reset();
    auto teardown = std::chrono::steady_clock::now() - t1;
    std::printf("   teardown took %lld ms\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(teardown).count());
    CHECK(teardown < 2800ms);
    // The hung window could not be put back: it stays journaled for the
    // next instance.
    CHECK(eventually([] { return journal_parked_entries() == 1; }, 3000ms));
    hung.kill_now();
}

}  // namespace

int main() {
    mac_permissions();
    require_accessibility();
    require_display();
    operations();
    teardown_restores();
    hung_application();
    return finish("test_mac_ops");
}
