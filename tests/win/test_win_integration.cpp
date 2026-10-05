// The portable core driving the real Windows backend: tiling, workspaces
// (parked windows), focus pulling a hidden workspace back, close re-tiling,
// and a reservation shrinking the tiled area. Only the child process's
// windows are in scope, so the user's windows are never touched.
#include "brocompositor/window_manager.h"

#include "check.h"
#include "printers.h"
#include "win/harness.h"

#include <cstdio>

using namespace brocompositor;
using namespace bctest;

namespace {

uint64_t native(HWND h) { return uint64_t(reinterpret_cast<uintptr_t>(h)); }

struct Rig {
    std::unique_ptr<win::ShellBackend> shell;
    WindowManager wm;
    explicit Rig(WindowManagerConfig cfg) : wm(std::move(cfg)) {}

    // Executes commands through the typed (asynchronous) methods and waits
    // for them, so a test step sees their effect.
    void execute_and_wait(const std::vector<Command>& cmds) {
        std::vector<Completion<bool>> pending;
        for (const Command& c : cmds) {
            if (auto* p = std::get_if<PlaceWindow>(&c)) pending.push_back(shell->place(p->id, p->frame));
            else if (auto* v = std::get_if<SetWindowVisible>(&c)) pending.push_back(shell->set_visible(v->id, v->visible));
            else if (auto* f = std::get_if<FocusWindow>(&c)) shell->focus(f->id).wait_for(2s);
            else if (auto* cw = std::get_if<brocompositor::CloseWindow>(&c)) pending.push_back(shell->close(cw->id));
        }
        for (auto& p : pending) p.wait_for(5s);
    }
    // Host loop iteration: facts in, commands out, executed.
    void pump(std::chrono::milliseconds quiet = 250ms) {
        auto& q = shell->events();
        do {
            for (auto& e : q.drain()) execute_and_wait(wm.handle(e));
        } while (q.wait_for(quiet));
    }
    void run(const std::vector<Command>& cmds) {
        execute_and_wait(cmds);
        pump();
    }
    WindowId id_of(HWND h) { return shell->find(native(h)); }
};

bool on_any_monitor(HWND h) {
    Rect f = frame_of(h);
    RECT r{f.x, f.y, f.right(), f.bottom()};
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

void run() {
    TestApp app;
    REQUIRE(app.start());
    Rect wa = primary_work_area();
    HWND a = app.create("a", Rect{wa.x + 50, wa.y + 50, 500, 400}, "aa0000");
    HWND b = app.create("b", Rect{wa.x + 90, wa.y + 90, 500, 400}, "00aa00");
    HWND c = app.create("c", Rect{wa.x + 130, wa.y + 130, 500, 400}, "0000aa");
    REQUIRE(a && b && c);

    WindowManagerConfig cfg;
    cfg.layout.gap_inner = 4;
    cfg.layout.gap_outer = 6;
    Rig rig(cfg);
    rig.shell = win::ShellBackend::create(test_shell_config(app.pid()), nullptr);
    REQUIRE(rig.shell);
    rig.pump();

    WindowId ia = rig.id_of(a), ib = rig.id_of(b), ic = rig.id_of(c);
    REQUIRE(ia && ib && ic);
    // Default floating: nothing moved.
    CHECK_EQ(frame_of(a), rig.wm.window(ia)->snapshot.frame);
    MonitorId mon = rig.wm.window(ia)->snapshot.monitor;
    WorkspaceId ws1 = rig.wm.active_workspace(mon);
    REQUIRE(ws1 != kNoWorkspace);
    Rect work;
    for (auto& m : rig.wm.monitors())
        if (m.id == mon) work = m.work_area;

    // Tile: the real frames match the layout exactly.
    rig.run(rig.wm.set_layout(ws1, LayoutMode::Columns));
    auto layout = compute_layout(LayoutMode::Columns, work, rig.wm.workspace(ws1)->windows, cfg.layout);
    for (auto [id, h] : {std::pair{ia, a}, std::pair{ib, b}, std::pair{ic, c}}) {
        CHECK_EQ(std::optional<Rect>(frame_of(h)), layout.find(id));
        CHECK_EQ(rig.wm.window(id)->snapshot.frame, frame_of(h));  // facts reconciled
    }

    // Second workspace: our windows are parked, a new window tiles alone.
    WorkspaceId ws2 = rig.wm.add_workspace(mon, "two");
    rig.run(rig.wm.activate_workspace(ws2));
    CHECK(!on_any_monitor(a) && !on_any_monitor(b) && !on_any_monitor(c));
    CHECK(IsWindowVisible(a) && !IsIconic(a));
    HWND d = app.create("d", Rect{wa.x + 200, wa.y + 200, 400, 300}, "aaaa00");
    REQUIRE(d);
    rig.pump();
    WindowId id_d = rig.id_of(d);
    REQUIRE(id_d);
    CHECK_EQ(rig.wm.window(id_d)->workspace, ws2);
    CHECK_EQ(rig.wm.window(ia)->workspace, ws1);  // parking was not mistaken for a user move
    rig.run(rig.wm.set_layout(ws2, LayoutMode::BSP));
    Rect inner = work.inset(Margins{6, 6, 6, 6});
    CHECK_EQ(frame_of(d), inner);

    // Back to ws1 by the host.
    rig.run(rig.wm.activate_workspace(ws1));
    CHECK(on_any_monitor(a) && on_any_monitor(b) && on_any_monitor(c));
    CHECK(!on_any_monitor(d));
    CHECK_EQ(std::optional<Rect>(frame_of(b)), layout.find(ib));

    // Focus arriving on a parked window (what Alt-Tab does) pulls ws2 up.
    if (interactive_desktop()) {
        UserInputMonitor user;
        uint64_t input_before = user.count();
        auto r = rig.shell->focus(id_d).get();
        if (r != win::FocusResult::Focused && user.count() != input_before) {
            std::printf("  (focus-follows-workspace step skipped: the user was using the desktop)\n");
        } else {
            CHECK(r == win::FocusResult::Focused);
            rig.pump();
            CHECK(rig.wm.workspace(ws2)->active);
            CHECK(on_any_monitor(d));
            CHECK(!on_any_monitor(a));
        }
        rig.run(rig.wm.activate_workspace(ws1));
    } else {
        std::printf("  (focus-follows-workspace step skipped: input desktop is not Default)\n");
    }

    // Closing a tiled window re-tiles the rest.
    CHECK(app.ok("destroy b"));
    rig.pump();
    auto two = compute_layout(LayoutMode::Columns, work, {ia, ic}, cfg.layout);
    CHECK_EQ(std::optional<Rect>(frame_of(a)), two.find(ia));
    CHECK_EQ(std::optional<Rect>(frame_of(c)), two.find(ic));

    // A reservation shrinks the work area; the tiled windows follow.
    ReservationId res = rig.shell->reserve_edge(mon, Edge::Top, 44);
    REQUIRE(res != kNoReservation);
    rig.pump(400ms);
    auto granted = rig.shell->reservation_rect(res);
    REQUIRE(granted.has_value());
    CHECK_EQ(frame_of(a).y, granted->bottom() + 6);
    CHECK(rig.shell->release_edge(res));
    rig.pump(400ms);
    CHECK_EQ(std::optional<Rect>(frame_of(a)), two.find(ia));

    // Tear down while d is parked: the backend puts it back on screen.
    CHECK(!on_any_monitor(d));
    rig.shell.reset();
    CHECK(on_any_monitor(d));
}

}  // namespace

int main() {
    init_windows_test();
    ForegroundGuard guard;
    run();
    return finish("test_win_integration");
}
