// bc_win_shell_child: a ShellBackend host that test_win_recovery kills hard.
//
//   bc_win_shell_child <journal_dir> <pid> <hwnd>
//
// Manages only <pid>'s windows, parks <hwnd>, reserves 41 px at the top of
// the primary monitor, prints "ready <appbar-granted-rect>" and then waits
// to be terminated (TerminateProcess: no handler, no destructor runs).
#include "brocompositor/win/shell_backend.h"
#include "event_log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace brocompositor;

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    win::ShellConfig cfg;
    cfg.journal_dir = argv[1];
    cfg.process_filter = {uint32_t(std::strtoul(argv[2], nullptr, 10))};
    uint64_t hwnd = std::strtoull(argv[3], nullptr, 10);
    auto shell = win::ShellBackend::create(cfg, nullptr);
    if (!shell) return 3;
    bctest::EventLog log(shell->events());
    auto added = log.wait<WindowAdded>([&](const WindowAdded& e) { return e.window.native == hwnd; });
    if (!added) return 4;
    if (!shell->set_visible(added->window.id, false).get()) return 5;
    MonitorId primary = kNoMonitor;
    for (auto& m : shell->monitors())
        if (m.primary) primary = m.id;
    ReservationId r = shell->reserve_edge(primary, Edge::Top, 41);
    auto granted = log.wait<ReservationChanged>([&](const ReservationChanged& e) { return e.id == r; });
    if (!granted || granted->rect.empty()) return 6;
    std::printf("ready %d %d %d %d\n", granted->rect.x, granted->rect.y, granted->rect.width, granted->rect.height);
    std::fflush(stdout);
    for (;;) Sleep(1000);
}
