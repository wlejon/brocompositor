// Focus through Accessibility: focusing a window makes its application
// frontmost and the window first in the window server's order; focus
// tracking reports it; a later request supersedes an earlier one; focusing
// "no window" hands focus to Finder. Needs Accessibility and an unlocked
// session.
#include "harness.h"

using namespace bctest;
using namespace brocompositor;

namespace {

template <class T>
bool done(const Completion<T>& c, std::chrono::milliseconds t = 5000ms) {
    return c.wait_for(t) == std::future_status::ready;
}

void focus() {
    std::printf("-- focus\n");
    TestApp one, two;
    REQUIRE(one.start());
    REQUIRE(two.start());
    uint32_t a = one.create("a", Rect{200, 200, 300, 200}, "C03030");
    uint32_t b = two.create("b", Rect{560, 200, 300, 200}, "3030C0");
    REQUIRE(a && b);
    auto config = test_shell_config(one.pid());
    config.process_filter.push_back(uint32_t(two.pid()));
    std::string err;
    auto backend = mac::ShellBackend::create(config, &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    auto wa = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == a; });
    auto wb = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == b; });
    REQUIRE(wa && wb);

    auto fa = backend->focus(wa->window.id);
    REQUIRE(done(fa));
    FocusResult ra = fa.get();
    CHECK(ra == FocusResult::Focused || ra == FocusResult::AlreadyFocused);
    CHECK(log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == wa->window.id; }));

    auto fb = backend->focus(wb->window.id);
    REQUIRE(done(fb));
    CHECK(fb.get() == FocusResult::Focused);
    CHECK(log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == wb->window.id; }));
    auto again = backend->focus(wb->window.id);
    REQUIRE(done(again));
    CHECK(again.get() == FocusResult::AlreadyFocused);

    // Two requests in a row for windows of one application: the later wins.
    uint32_t a2 = one.create("a2", Rect{240, 260, 300, 200}, "C0C030");
    auto wa2 = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == a2; });
    REQUIRE(wa2);
    auto first = backend->focus(wa->window.id);
    auto second = backend->focus(wa2->window.id);
    REQUIRE(done(first) && done(second));
    CHECK(first.get() == FocusResult::Superseded);
    CHECK(second.get() == FocusResult::Focused);
    CHECK(log.wait<FocusChanged>([&](const FocusChanged& e) { return e.id == wa2->window.id; }));

    // No managed window: Finder takes focus, tracking reports kNoWindow.
    auto none = backend->focus(kNoWindow);
    REQUIRE(done(none));
    CHECK(none.get() == FocusResult::Focused);
    CHECK(log.wait<FocusChanged>([](const FocusChanged& e) { return e.id == kNoWindow; }));
    CHECK(backend->focus(wa->window.id + 1000).get() == FocusResult::NoSuchWindow);
}

}  // namespace

int main() {
    bctest::require_mutate("test_mac_focus", "takes the foreground from the user");
    mac_permissions();
    require_accessibility();
    require_unlocked();
    require_display();
    uint32_t previous = frontmost_pid();
    focus();
    // Give the front back to whoever had it (this process has Accessibility).
    if (previous) make_frontmost(previous);
    return finish("test_mac_focus");
}
