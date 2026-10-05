// Displays and virtual edge reservations: menu-bar/Dock-aware work areas in
// Quartz points, Retina dpi, reservations that stack, renegotiate when one
// is released, and never change what anything else sees.
#include "harness.h"

using namespace bctest;
using namespace brocompositor;

namespace {

const MonitorSnapshot* primary(const std::vector<MonitorSnapshot>& ms) {
    for (const auto& m : ms)
        if (m.primary) return &m;
    return nullptr;
}

void monitors() {
    std::printf("-- monitors\n");
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(getpid()), &err);
    REQUIRE(backend);
    EventLog log(backend->events());
    auto first = log.wait<MonitorsChanged>([](const MonitorsChanged&) { return true; });
    REQUIRE(first);
    auto ms = backend->monitors();
    REQUIRE(!ms.empty());
    CHECK(ms == first->monitors);
    size_t primaries = 0;
    for (const auto& m : ms) {
        primaries += m.primary ? 1 : 0;
        CHECK(m.id != kNoMonitor);
        CHECK(!m.name.empty());
        CHECK(!m.bounds.empty());
        CHECK(m.bounds.contains(m.work_area));
        CHECK(m.dpi >= 96 && m.dpi % 48 == 0);
        std::printf("   %u %s bounds %d,%d %dx%d work %d,%d %dx%d dpi %u%s\n", m.id, m.name.c_str(), m.bounds.x,
                    m.bounds.y, m.bounds.width, m.bounds.height, m.work_area.x, m.work_area.y, m.work_area.width,
                    m.work_area.height, m.dpi, m.primary ? " primary" : "");
    }
    CHECK_EQ(primaries, size_t(1));
    const MonitorSnapshot* p = primary(ms);
    REQUIRE(p);
    CHECK_EQ(p->bounds.x, 0);
    CHECK_EQ(p->bounds.y, 0);
}

void reservations() {
    std::printf("-- reservations\n");
    std::string err;
    auto backend = mac::ShellBackend::create(test_shell_config(getpid()), &err);
    REQUIRE(backend);
    auto bystander = mac::ShellBackend::create(test_shell_config(getpid()), &err);
    REQUIRE(bystander);
    EventLog log(backend->events());
    log.settle(200ms);
    auto initial = backend->monitors();
    const MonitorSnapshot* p0 = primary(initial);
    REQUIRE(p0);
    MonitorSnapshot p = *p0;
    Rect wa = p.work_area;

    auto work_area_now = [&](const Rect& want) {
        return log.wait<MonitorsChanged>([&](const MonitorsChanged& e) {
            const MonitorSnapshot* m = primary(e.monitors);
            return m && m->work_area == want;
        });
    };

    log.mark();
    auto t0 = std::chrono::steady_clock::now();
    ReservationId top = backend->reserve_edge(p.id, Edge::Top, 40);
    CHECK(std::chrono::steady_clock::now() - t0 < 50ms);
    REQUIRE(top != kNoReservation);
    Rect top_rect{wa.x, wa.y, wa.width, 40};
    CHECK(log.wait<ReservationChanged>(
        [&](const ReservationChanged& e) -> bool { return e.id == top && e.monitor == p.id && e.rect == top_rect; }));
    CHECK(work_area_now(Rect{wa.x, wa.y + 40, wa.width, wa.height - 40}));
    CHECK(backend->reservation_rect(top) == top_rect);

    ReservationId left = backend->reserve_edge(p.id, Edge::Left, 30);
    REQUIRE(left != kNoReservation);
    Rect left_rect{wa.x, wa.y + 40, 30, wa.height - 40};
    CHECK(log.wait<ReservationChanged>([&](const ReservationChanged& e) -> bool { return e.id == left && e.rect == left_rect; }));
    CHECK(work_area_now(Rect{wa.x + 30, wa.y + 40, wa.width - 30, wa.height - 40}));

    // Another backend (and so every other application) still sees the
    // system's own work area: reservations are this backend's view only.
    auto others = bystander->monitors();
    const MonitorSnapshot* other = primary(others);
    REQUIRE(other);
    CHECK(other->work_area == wa);

    // Releasing the top strip moves the left one up.
    CHECK(backend->release_edge(top));
    CHECK(!backend->reservation_rect(top));
    Rect left_moved{wa.x, wa.y, 30, wa.height};
    CHECK(log.wait<ReservationChanged>([&](const ReservationChanged& e) -> bool { return e.id == left && e.rect == left_moved; }));
    CHECK(work_area_now(Rect{wa.x + 30, wa.y, wa.width - 30, wa.height}));
    CHECK(backend->release_edge(left));
    CHECK(work_area_now(wa));

    CHECK(!backend->release_edge(left));
    CHECK_EQ(backend->reserve_edge(0xFFFFFFF0u, Edge::Top, 40), kNoReservation);
    CHECK_EQ(backend->reserve_edge(p.id, Edge::Top, 0), kNoReservation);
}

}  // namespace

int main() {
    mac_permissions();
    require_display();
    monitors();
    reservations();
    return finish("test_mac_displays");
}
