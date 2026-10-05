// Capability reporting is honest: permissions are queried without prompting,
// the backend reports the same, and every operation that needs a missing
// permission fails at once with a clear result instead of hanging or
// pretending.
#include "harness.h"

#include <filesystem>

using namespace bctest;
using namespace brocompositor;

namespace {

void reporting() {
    std::printf("-- permission reporting\n");
    const auto& p = mac_permissions();
    auto again = mac::query_permissions();
    CHECK_EQ(again.accessibility, p.accessibility);
    CHECK_EQ(again.screen_recording, p.screen_recording);
    CHECK_EQ(again.responsible_path, p.responsible_path);
    CHECK(!p.responsible_path.empty());
    CHECK(std::filesystem::exists(p.responsible_path));
    CHECK(p.responsible_pid != 0);

    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(getpid()), &err);
    REQUIRE(backend);
    CHECK_EQ(backend->permissions().accessibility, p.accessibility);
    CHECK_EQ(backend->permissions().screen_recording, p.screen_recording);
    if (!p.accessibility)
        std::printf("   without Accessibility: discovery, monitors and focus tracking only; window operations "
                    "report Unavailable\n");
    if (!p.screen_recording)
        std::printf("   without Screen Recording: no capture, and other applications' titles only via "
                    "Accessibility\n");
}

// Without Accessibility every operation completes at once, false /
// Unavailable, and nothing about the window changes.
void operations_without_accessibility() {
    if (mac_permissions().accessibility) {
        std::printf("-- operations without Accessibility: not applicable (granted)\n");
        return;
    }
    if (!display_awake()) {
        std::printf("-- operations without Accessibility: SKIP (no display awake)\n");
        return;
    }
    std::printf("-- operations without Accessibility\n");
    TestApp app;
    REQUIRE(app.start());
    Rect frame{200, 200, 360, 240};
    uint32_t cgid = app.create("plain", frame, "3070C0");
    REQUIRE(cgid != 0);
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(app.pid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    auto added = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == cgid; });
    REQUIRE(added);
    WindowId id = added->window.id;

    auto t0 = std::chrono::steady_clock::now();
    auto place = backend->place(id, Rect{300, 300, 200, 200});
    auto hide = backend->set_visible(id, false);
    auto focus = backend->focus(id);
    auto close = backend->close(id);
    auto rescue = backend->rescue_offscreen_windows();
    CHECK(place.wait_for(0ms) == std::future_status::ready && !place.get());
    CHECK(hide.wait_for(0ms) == std::future_status::ready && !hide.get());
    CHECK(focus.wait_for(0ms) == std::future_status::ready && focus.get() == FocusResult::Unavailable);
    CHECK(close.wait_for(0ms) == std::future_status::ready && !close.get());
    CHECK(rescue.wait_for(2s) == std::future_status::ready && rescue.get() == 0);
    CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
    CHECK(backend->execute(PlaceWindow{id, Rect{0, 0, 10, 10}}));  // known window: accepted, then fails
    CHECK(!backend->execute(PlaceWindow{id + 1000, Rect{0, 0, 10, 10}}));
    log.settle(400ms);
    auto q = backend->query(id);
    REQUIRE(q);
    CHECK(near(q->frame, frame));
    CHECK_EQ(app.cmd("frame plain"), std::string("ok 200 200 360 240"));
}

}  // namespace

int main() {
    reporting();
    operations_without_accessibility();
    return finish("test_mac_permissions");
}
