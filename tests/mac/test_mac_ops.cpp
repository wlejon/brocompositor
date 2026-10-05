// Window operations through Accessibility, on windows of a test application:
// place, park / show (journaled), minimize hiding, close, restore on
// teardown, a parked window across a Space switch and closed by its
// application; and a hung application stalls nothing but its own
// operations. Needs Accessibility (skips otherwise, naming the binary to
// grant it to).
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

// The application's own view: minimized (miniaturized) or not.
bool app_minimized(TestApp& app, const std::string& name) {
    std::istringstream in(app.cmd("state " + name));
    std::string ok;
    int minimized = 0;
    in >> ok >> minimized;
    return ok == "ok" && minimized == 1;
}

// The hide method of every journaled entry (1 park, 2 minimize).
std::vector<uint32_t> journal_methods() {
    std::vector<uint32_t> out;
    for (auto& f : std::filesystem::directory_iterator(test_journal_dir())) {
        if (f.path().extension() != ".journal") continue;
        std::ifstream in(f.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("park ", 0) != 0) continue;
            std::istringstream l(line);
            std::string word;
            std::vector<std::string> fields;
            while (l >> word) fields.push_back(word);
            out.push_back(uint32_t(std::stoul(fields.back())));
        }
    }
    return out;
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
    log.mark();
    auto placed = backend->place(id, target);
    REQUIRE(done(placed));
    CHECK(placed.get());
    CHECK(near(app_frame(app, "ops"), target));
    auto q = backend->query(id);  // a completed operation is what query() reports
    REQUIRE(q);
    CHECK(near(q->frame, target));
    CHECK(log.wait<WindowChanged>([&](const WindowChanged& e) { return e.window.id == id && near(e.window.frame, target); }));

    // Park: the window server keeps a sliver of every window on a display
    // (see README); the backend reports where the window really went, and
    // minimizes instead when the sliver is too large.
    log.mark();
    auto hidden = backend->set_visible(id, false);
    REQUIRE(done(hidden));
    CHECK(hidden.get());
    q = backend->query(id);
    REQUIRE(q);
    Rect truth = app_frame(app, "ops");
    bool minimized = app_minimized(app, "ops");
    std::printf("   parked at %d,%d %dx%d (%lld pt^2 visible)%s\n", truth.x, truth.y, truth.width, truth.height,
                (long long)on_displays(truth, monitors), minimized ? ", minimized instead" : "");
    auto methods = journal_methods();
    CHECK_EQ(methods.size(), size_t(1));
    if (!minimized) {
        CHECK(near(q->frame, truth, 0));  // query() agrees with the application at once
        CHECK(on_displays(truth, monitors) <= 64 * 64);
        CHECK(on_displays(truth, monitors) > 0);  // macOS keeps a sliver: honest about it
        if (!methods.empty()) CHECK_EQ(methods[0], 1u);
    } else if (!methods.empty()) {
        CHECK_EQ(methods[0], 2u);
    }
    log.settle(400ms);
    CHECK(log.collect<WindowChanged>([&](const WindowChanged& e) {
              return e.window.id == id && (e.changes & (change::Geometry | change::State));
          }).empty());
    CHECK(log.collect<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id; }).empty());

    auto shown = backend->set_visible(id, true);
    REQUIRE(done(shown));
    CHECK(shown.get());
    CHECK(near(app_frame(app, "ops"), target));
    CHECK(!app_minimized(app, "ops"));
    CHECK(journal_methods().empty());

    // Minimize as the hide method (a second backend over the same window).
    {
        auto config = test_shell_config(app.pid());
        config.hide_method = mac::HideMethod::Minimize;
        auto b2 = mac::ShellBackend::create(config, &err);
        REQUIRE(b2);
        EventLog log2(b2->events());
        WindowId id2 = add(log2, cgid);
        REQUIRE(id2 != kNoWindow);
        log2.mark();
        auto h = b2->set_visible(id2, false);
        REQUIRE(done(h));
        CHECK(h.get());
        CHECK(app_minimized(app, "ops"));
        auto bq = b2->query(id2);
        REQUIRE(bq);  // still tracked: hidden by this backend
        log2.settle(400ms);
        // The backend's own hiding is not a fact for the policy core.
        CHECK(log2.collect<WindowChanged>([&](const WindowChanged& e) {
                  return e.window.id == id2 && (e.changes & change::State);
              }).empty());
        CHECK(log2.collect<WindowRemoved>([&](const WindowRemoved& e) { return e.id == id2; }).empty());
        // Teardown puts it back.
    }
    CHECK(eventually([&] { return !app_minimized(app, "ops") && near(app_frame(app, "ops"), target); }, 5000ms));

    log.mark();
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
        CHECK(!near(app_frame(app, "park"), frame) || app_minimized(app, "park"));
    }
    CHECK(near(app_frame(app, "park"), frame));
    CHECK(!app_minimized(app, "park"));
    CHECK(journal_methods().empty());
}

// A window the backend parked stays tracked (and journaled) while another
// Space is active, here a fullscreen window's, although the window server
// and AXWindows then no longer list it as on screen; it is shown again
// afterwards. A parked window its application closes is reported removed
// and leaves the journal.
void parked_window_facts() {
    if (mac_permissions().screen_locked) {
        std::printf("-- parked window across a Space switch: SKIP (screen locked)\n");
        return;
    }
    std::printf("-- parked window across a Space switch, then closed\n");
    TestApp app;
    REQUIRE(app.start());
    Rect frame{240, 220, 360, 240};
    uint32_t parked_id = app.create("parked", frame, "C03080");
    uint32_t fs_id = app.create("fs", Rect{700, 200, 300, 200}, "80C030");
    REQUIRE(parked_id && fs_id);
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    WindowId pid_ = add(log, parked_id);
    REQUIRE(pid_);
    auto h = backend->set_visible(pid_, false);
    REQUIRE(done(h));
    CHECK(h.get());
    CHECK_EQ(journal_methods().size(), size_t(1));

    uint32_t previous = frontmost_pid();
    log.mark();
    REQUIRE(app.ok("activate fs"));
    std::string r = app.cmd("fullscreen fs", 8000);
    CHECK_EQ(r, std::string("ok"));
    if (r == "ok") {
        log.settle(1500ms);  // long enough for the off-screen decision (an AX scan or two)
        CHECK(log.collect<WindowRemoved>([&](const WindowRemoved& e) { return e.id == pid_; }).empty());
        CHECK(backend->find(parked_id) == pid_);
        CHECK_EQ(journal_methods().size(), size_t(1));
        CHECK_EQ(app.cmd("unfullscreen fs", 8000), std::string("ok"));
    }
    auto shown = backend->set_visible(pid_, true);
    REQUIRE(done(shown));
    CHECK(shown.get());
    CHECK(near(app_frame(app, "parked"), frame));
    CHECK(journal_methods().empty());
    if (previous && previous != uint32_t(app.pid())) app.ok("yield " + std::to_string(previous));

    // Parked, then closed by its application.
    auto h2 = backend->set_visible(pid_, false);
    REQUIRE(done(h2));
    CHECK(h2.get());
    CHECK_EQ(journal_methods().size(), size_t(1));
    log.mark();
    REQUIRE(app.ok("close parked"));
    CHECK(log.wait<WindowRemoved>([&](const WindowRemoved& e) { return e.id == pid_; }, 5000ms));
    CHECK(eventually([] { return journal_methods().empty(); }));
}

// A hung application: every host call returns at once, the healthy
// application's operations go ahead, the hung one's fail after its
// messaging timeout without forgetting that its window is parked, and
// teardown is bounded and leaves the hung window journaled.
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
    // The hung application's operations fail after the messaging timeout,
    // and the failed placement does not un-hide (un-journal) the window.
    CHECK(done(place_hung, 4000ms));
    CHECK(!place_hung.get());
    CHECK(done(focus_hung, 4000ms));
    CHECK(focus_hung.get() != FocusResult::Focused);
    CHECK_EQ(journal_methods().size(), size_t(1));

    auto t1 = std::chrono::steady_clock::now();
    backend.reset();
    auto teardown = std::chrono::steady_clock::now() - t1;
    std::printf("   teardown took %lld ms\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(teardown).count());
    CHECK(teardown < 2800ms);
    // The hung window could not be put back: it stays journaled for the
    // next instance.
    CHECK(eventually([] { return journal_methods().size() == 1; }, 3000ms));
    hung.kill_now();
}

}  // namespace

int main() {
    mac_permissions();
    require_accessibility();
    require_display();
    operations();
    teardown_restores();
    parked_window_facts();
    hung_application();
    return finish("test_mac_ops");
}
