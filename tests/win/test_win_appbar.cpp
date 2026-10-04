// Edge reservations change the real work area of the right monitor, are
// reported as MonitorsChanged, and are given back on release, on backend
// destruction and on the emergency path. The work area of every monitor is
// checked against its original value at the end.
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

void reserve_each_monitor() {
    win::ShellConfig cfg;
    cfg.process_filter = {GetCurrentProcessId()};  // manage nothing; reservations only
    std::string err;
    auto shell = win::ShellBackend::create(cfg, &err);
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
        Rect granted;
        ReservationId id = shell->reserve_edge(m.id, edge, 37, &granted);
        REQUIRE(id != kNoReservation);
        std::printf("  %s: edge %d granted {%d,%d %dx%d}\n", m.name.c_str(), int(edge), granted.x, granted.y,
                    granted.width, granted.height);
        CHECK(m.bounds.contains(granted));
        bool vertical = edge == Edge::Left || edge == Edge::Right;
        CHECK_EQ(vertical ? granted.width : granted.height, 37);
        Rect expect = expected_after(before, edge, granted);
        // The real work area of that monitor shrank by the strip ...
        CHECK(wait_until([&] { return os_work_area(m) == expect; }));
        CHECK_EQ(os_work_area(m), expect);
        // ... and the backend reported it.
        auto ev = log.wait<MonitorsChanged>([&](const MonitorsChanged& e) { return work_of(e.monitors, m.id) == expect; });
        CHECK(ev.has_value());
        // Other monitors are untouched.
        for (const auto& o : monitors)
            if (o.id != m.id) CHECK_EQ(os_work_area(o), o.work_area);
        CHECK_EQ(shell->reservation_rect(id), std::optional<Rect>(granted));

        CHECK(shell->release_edge(id));
        CHECK(!shell->release_edge(id));
        CHECK(wait_until([&] { return os_work_area(m) == before; }));
        CHECK(log.wait<MonitorsChanged>([&](const MonitorsChanged& e) { return work_of(e.monitors, m.id) == before; })
                  .has_value());
    }
    CHECK(shell->reserve_edge(kNoMonitor, Edge::Top, 30, nullptr) == kNoReservation);
    CHECK(shell->reserve_edge(monitors[0].id, Edge::Top, 0, nullptr) == kNoReservation);
}

void destructor_and_emergency_release() {
    Rect before;
    MonitorSnapshot primary;
    {
        win::ShellConfig cfg;
        cfg.process_filter = {GetCurrentProcessId()};
        auto shell = win::ShellBackend::create(cfg, nullptr);
        REQUIRE(shell);
        for (auto& m : shell->monitors())
            if (m.primary) primary = m;
        before = primary.work_area;
        REQUIRE(shell->reserve_edge(primary.id, Edge::Top, 29, nullptr) != kNoReservation);
        CHECK(wait_until([&] { return os_work_area(primary) != before; }));
    }  // destructor releases
    CHECK(wait_until([&] { return os_work_area(primary) == before; }));

    {
        win::ShellConfig cfg;
        cfg.process_filter = {GetCurrentProcessId()};
        auto shell = win::ShellBackend::create(cfg, nullptr);
        REQUIRE(shell);
        REQUIRE(shell->reserve_edge(primary.id, Edge::Bottom, 31, nullptr) != kNoReservation);
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
