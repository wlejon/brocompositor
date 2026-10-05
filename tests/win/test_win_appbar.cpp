// Edge reservations change the real work area of the right monitor, are
// reported as ReservationChanged + MonitorsChanged, and are given back on
// release, on backend destruction and on the emergency path. The host's
// calls never wait on the shell (Explorer): the grant arrives as an event.
// The work area of every monitor is checked against its original value at
// the end.
#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

std::vector<Rect> g_original;

Rect expected_after(const Rect& work, Edge edge, const Rect& granted) {
    Rect r = work;
    switch (edge) {
        case Edge::Top: r.height -= granted.bottom() - r.y; r.y = granted.bottom(); break;
        case Edge::Bottom: r.height = granted.y - r.y; break;
        case Edge::Left: r.width -= granted.right() - r.x; r.x = granted.right(); break;
        case Edge::Right: r.width = granted.x - r.x; break;
    }
    return r;
}

std::optional<Rect> work_of(const std::vector<MonitorSnapshot>& ms, MonitorId id) {
    for (auto& m : ms)
        if (m.id == id) return m.work_area;
    return std::nullopt;
}

Rect os_work_area(const MonitorSnapshot& m) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(m.native)), &mi);
    return Rect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
}

std::optional<Rect> granted_rect(EventLog& log, ReservationId id) {
    auto e = log.wait<ReservationChanged>([&](const ReservationChanged& r) { return r.id == id; });
    if (!e) return std::nullopt;
    return e->rect;
}

void reserve_each_monitor() {
    // Manage nothing (this process has no windows); reservations only.
    auto shell = win::ShellBackend::create(test_shell_config(GetCurrentProcessId()), nullptr);
    REQUIRE(shell);
    EventLog log(shell->events());
    auto monitors = shell->monitors();
    REQUIRE(!monitors.empty());

    const Edge edges[] = {Edge::Top, Edge::Left, Edge::Right, Edge::Bottom};
    int i = 0;
    for (const auto& m : monitors) {
        Edge edge = edges[i++ % 4];
        Rect before = m.work_area;
        log.mark();
        auto t0 = std::chrono::steady_clock::now();
        ReservationId id = shell->reserve_edge(m.id, edge, 37);
        auto call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        REQUIRE(id != kNoReservation);
        CHECK(call_ms < 50ms);
        auto granted = granted_rect(log, id);
        REQUIRE(granted.has_value());
        std::printf("  %s: edge %d granted {%d,%d %dx%d}\n", m.name.c_str(), int(edge), granted->x, granted->y,
                    granted->width, granted->height);
        CHECK(m.bounds.contains(*granted));
        bool vertical = edge == Edge::Left || edge == Edge::Right;
        CHECK_EQ(vertical ? granted->width : granted->height, 37);
        Rect expect = expected_after(before, edge, *granted);
        // The real work area of that monitor shrank by the strip ...
        CHECK(wait_until([&] { return os_work_area(m) == expect; }));
        CHECK_EQ(os_work_area(m), expect);
        // ... and the backend reported it.
        auto ev = log.wait<MonitorsChanged>([&](const MonitorsChanged& e) { return work_of(e.monitors, m.id) == expect; });
        CHECK(ev.has_value());
        // Other monitors are untouched.
        for (const auto& o : monitors)
            if (o.id != m.id) CHECK_EQ(os_work_area(o), o.work_area);
        CHECK_EQ(shell->reservation_rect(id), granted);

        CHECK(shell->release_edge(id));
        CHECK(!shell->release_edge(id));
        CHECK(wait_until([&] { return os_work_area(m) == before; }));
        CHECK(log.wait<MonitorsChanged>([&](const MonitorsChanged& e) { return work_of(e.monitors, m.id) == before; })
                  .has_value());
    }
    CHECK(shell->reserve_edge(kNoMonitor, Edge::Top, 30) == kNoReservation);
    CHECK(shell->reserve_edge(monitors[0].id, Edge::Top, 0) == kNoReservation);

    // Released before the shell thread got to it: nothing is ever reserved.
    Rect before = os_work_area(monitors[0]);
    ReservationId quick = shell->reserve_edge(monitors[0].id, Edge::Top, 33);
    CHECK(quick != kNoReservation);
    CHECK(shell->release_edge(quick));
    log.settle();
    CHECK_EQ(os_work_area(monitors[0]), before);
}

void destructor_and_emergency_release() {
    Rect before;
    MonitorSnapshot primary;
    {
        auto shell = win::ShellBackend::create(test_shell_config(GetCurrentProcessId()), nullptr);
        REQUIRE(shell);
        for (auto& m : shell->monitors())
            if (m.primary) primary = m;
        before = primary.work_area;
        REQUIRE(shell->reserve_edge(primary.id, Edge::Top, 29) != kNoReservation);
        CHECK(wait_until([&] { return os_work_area(primary) != before; }));
    }  // destructor releases
    CHECK(wait_until([&] { return os_work_area(primary) == before; }));

    {
        auto shell = win::ShellBackend::create(test_shell_config(GetCurrentProcessId()), nullptr);
        REQUIRE(shell);
        REQUIRE(shell->reserve_edge(primary.id, Edge::Bottom, 31) != kNoReservation);
        CHECK(wait_until([&] { return os_work_area(primary) != before; }));
        // What a console-control / crash handler calls.
        win::emergency_release_reservations();
        CHECK(wait_until([&] { return os_work_area(primary) == before; }));
    }
    CHECK_EQ(os_work_area(primary), before);
}

}  // namespace

int main() {
    init_windows_test();
    g_original = monitor_work_areas();
    reserve_each_monitor();
    destructor_and_emergency_release();
    // The desktop is exactly as we found it.
    wait_until([] { return monitor_work_areas() == g_original; });
    CHECK_EQ(monitor_work_areas(), g_original);
    return finish("test_win_appbar");
}
