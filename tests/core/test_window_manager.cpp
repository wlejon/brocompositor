// The policy core driven purely by synthetic facts.
#include "brocompositor/window_manager.h"

#include "check.h"
#include "printers.h"

#include <algorithm>

using namespace brocompositor;

namespace {

MonitorSnapshot mon(MonitorId id, Rect bounds, bool primary, int32_t top_bar = 0) {
    MonitorSnapshot m;
    m.id = id;
    m.name = std::to_string(id);
    m.bounds = bounds;
    m.work_area = Rect{bounds.x, bounds.y + top_bar, bounds.width, bounds.height - top_bar};
    m.primary = primary;
    return m;
}

WindowSnapshot win(WindowId id, MonitorId m, Rect frame, WindowId owner = kNoWindow) {
    WindowSnapshot s;
    s.id = id;
    s.monitor = m;
    s.frame = frame;
    s.owner = owner;
    s.title = std::to_string(id);
    return s;
}

template <class T>
std::vector<T> only(const std::vector<Command>& cmds) {
    std::vector<T> out;
    for (const auto& c : cmds)
        if (auto* p = std::get_if<T>(&c)) out.push_back(*p);
    return out;
}

bool has_visible(const std::vector<Command>& cmds, WindowId id, bool visible) {
    for (const auto& v : only<SetWindowVisible>(cmds))
        if (v.id == id && v.visible == visible) return true;
    return false;
}

std::optional<Rect> placed(const std::vector<Command>& cmds, WindowId id) {
    std::optional<Rect> r;
    for (const auto& p : only<PlaceWindow>(cmds))
        if (p.id == id) r = p.frame;
    return r;
}

// Echo the core's placements back as facts, as a well-behaved backend would.
void echo(WindowManager& wm, const std::vector<Command>& cmds) {
    for (const auto& p : only<PlaceWindow>(cmds)) {
        auto v = wm.window(p.id);
        if (!v) continue;
        WindowSnapshot s = v->snapshot;
        s.frame = p.frame;
        wm.handle(WindowChanged{s, change::Geometry});
    }
}

const Rect kM1{0, 0, 1920, 1080};
const Rect kM2{1920, 0, 1280, 1024};

LayoutConfig no_gaps() {
    LayoutConfig c;
    c.gap_inner = 0;
    c.gap_outer = 0;
    return c;
}

void floating_default_never_moves_windows() {
    WindowManager wm;
    auto c0 = wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    CHECK(c0.empty());
    auto c1 = wm.handle(WindowAdded{win(1, 1, Rect{10, 10, 500, 400})});
    auto c2 = wm.handle(WindowAdded{win(2, 1, Rect{600, 10, 500, 400})});
    CHECK(c1.empty());
    CHECK(c2.empty());
    CHECK_EQ(wm.windows().size(), size_t(2));
    CHECK_EQ(wm.workspaces().size(), size_t(1));
    CHECK(!wm.window(1)->tiled);
}

void tiling_and_reconcile() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true, 40)}});
    wm.handle(WindowAdded{win(1, 1, Rect{10, 10, 500, 400})});
    wm.handle(WindowAdded{win(2, 1, Rect{600, 10, 500, 400})});
    WorkspaceId ws = wm.active_workspace(1);
    auto c = wm.set_layout(ws, LayoutMode::Columns);
    CHECK_EQ(placed(c, 1), (std::optional<Rect>(Rect{0, 40, 960, 1040})));
    CHECK_EQ(placed(c, 2), (std::optional<Rect>(Rect{960, 40, 960, 1040})));
    echo(wm, c);
    CHECK(wm.relayout(ws).size() == 2);  // explicit relayout re-issues
    // A third window splits the area; the existing ones are re-placed.
    auto c3 = wm.handle(WindowAdded{win(3, 1, Rect{0, 0, 300, 300})});
    CHECK_EQ(only<PlaceWindow>(c3).size(), size_t(3));
    echo(wm, c3);
    // A work-area change (a bar got taller) re-places everything.
    auto c4 = wm.handle(MonitorsChanged{{mon(1, kM1, true, 80)}});
    CHECK_EQ(placed(c4, 1)->y, 80);
    echo(wm, c4);
    // Stable state: facts that match produce nothing.
    auto v = wm.window(2)->snapshot;
    CHECK(wm.handle(WindowChanged{v, change::Title}).empty());
    // Back to floating restores the original geometry.
    auto c5 = wm.set_layout(ws, LayoutMode::Floating);
    CHECK_EQ(placed(c5, 1), (std::optional<Rect>(Rect{10, 10, 500, 400})));
}

void workspaces_hide_and_show() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    cfg.default_layout = LayoutMode::Columns;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    WorkspaceId ws1 = wm.active_workspace(1);
    echo(wm, wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 100, 100})}));
    echo(wm, wm.handle(WindowAdded{win(2, 1, Rect{0, 0, 100, 100})}));
    wm.handle(FocusChanged{2});
    WorkspaceId ws2 = wm.add_workspace(1, "two");
    CHECK(!wm.workspace(ws2)->active);

    auto c = wm.activate_workspace(ws2);
    CHECK(has_visible(c, 1, false));
    CHECK(has_visible(c, 2, false));
    CHECK(!wm.window(1)->shown);
    CHECK(wm.workspace(ws2)->active);
    // The focused window went into hiding and ws2 is empty: focus goes nowhere.
    auto f0 = only<FocusWindow>(c);
    CHECK(f0.size() == 1 && f0[0].id == kNoWindow);

    // A new window lands on the visible workspace and takes the whole area.
    auto c2 = wm.handle(WindowAdded{win(3, 1, Rect{0, 0, 100, 100})});
    CHECK_EQ(placed(c2, 3), (std::optional<Rect>(kM1)));
    CHECK_EQ(wm.window(3)->workspace, ws2);

    // Back to ws1: shown again, re-placed, and focus restored to the MRU (2).
    auto c3 = wm.activate_workspace(ws1);
    CHECK(has_visible(c3, 1, true));
    CHECK(has_visible(c3, 3, false));
    CHECK(placed(c3, 1).has_value());
    auto f = only<FocusWindow>(c3);
    CHECK(f.size() == 1 && f[0].id == 2);

    // Focus arriving on a hidden window (Alt-Tab, taskbar) brings its workspace
    // up without issuing a redundant focus command.
    auto c4 = wm.handle(FocusChanged{3});
    CHECK(wm.workspace(ws2)->active);
    CHECK(has_visible(c4, 3, true));
    CHECK(has_visible(c4, 1, false));
    CHECK(only<FocusWindow>(c4).empty());
    CHECK_EQ(wm.focused(), WindowId(3));

    // Moving a window to a hidden workspace hides it; follow=true goes along.
    auto c5 = wm.move_window_to_workspace(3, ws1, false);
    CHECK(has_visible(c5, 3, false));
    CHECK_EQ(wm.window(3)->workspace, ws1);
    auto c6 = wm.move_window_to_workspace(3, ws2, true);
    CHECK(wm.workspace(ws2)->active);
    CHECK(!only<FocusWindow>(c6).empty());

    // Removing a workspace hands its windows to a sibling.
    wm.activate_workspace(ws1);
    auto c7 = wm.remove_workspace(ws2);
    (void)c7;
    CHECK(!wm.workspace(ws2).has_value());
    CHECK_EQ(wm.window(3)->workspace, ws1);
    CHECK(wm.window(3)->shown);
    // The last workspace of a monitor cannot be removed.
    wm.remove_workspace(ws1);
    CHECK(wm.workspace(ws1).has_value());
}

void close_refocuses_and_relayouts() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    cfg.default_layout = LayoutMode::Columns;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    for (WindowId i = 1; i <= 3; ++i) echo(wm, wm.handle(WindowAdded{win(i, 1, Rect{0, 0, 10, 10})}));
    wm.handle(FocusChanged{1});
    wm.handle(FocusChanged{3});
    auto c = wm.handle(WindowRemoved{3});
    auto f = only<FocusWindow>(c);
    CHECK(f.size() == 1 && f[0].id == 1);
    CHECK_EQ(placed(c, 1), (std::optional<Rect>(Rect{0, 0, 960, 1080})));
    CHECK(!wm.window(3));
    // Removing an unknown id is a no-op.
    CHECK(wm.handle(WindowRemoved{999}).empty());
}

void transients_and_minimized() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    cfg.default_layout = LayoutMode::Columns;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    WorkspaceId ws1 = wm.active_workspace(1);
    echo(wm, wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 10, 10})}));
    echo(wm, wm.handle(WindowAdded{win(2, 1, Rect{0, 0, 10, 10})}));
    WorkspaceId ws2 = wm.add_workspace(1, "");
    wm.activate_workspace(ws2);
    // A dialog owned by a window on the hidden workspace joins it, hidden, floating.
    auto c = wm.handle(WindowAdded{win(5, 1, Rect{100, 100, 300, 200}, 1)});
    CHECK_EQ(wm.window(5)->workspace, ws1);
    CHECK(wm.window(5)->floating);
    CHECK(has_visible(c, 5, false));
    CHECK(!placed(c, 5));

    wm.activate_workspace(ws1);
    // User minimizes window 2: the other window takes the space.
    auto s = wm.window(2)->snapshot;
    s.minimized = true;
    auto c2 = wm.handle(WindowChanged{s, change::State});
    CHECK_EQ(placed(c2, 1), (std::optional<Rect>(kM1)));
    // Switching away does not hide a minimized window; switching back does
    // not re-show it either.
    auto c3 = wm.activate_workspace(ws2);
    CHECK(!has_visible(c3, 2, false));
    CHECK(has_visible(c3, 1, false));
    auto c4 = wm.activate_workspace(ws1);
    CHECK(!has_visible(c4, 2, true));
    // Restoring it brings it back into the layout.
    s.minimized = false;
    auto c5 = wm.handle(WindowChanged{s, change::State});
    CHECK_EQ(only<PlaceWindow>(c5).size(), size_t(2));
}

void drag_swaps_and_snaps_back() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    cfg.default_layout = LayoutMode::Columns;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    echo(wm, wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 10, 10})}));
    echo(wm, wm.handle(WindowAdded{win(2, 1, Rect{0, 0, 10, 10})}));
    WorkspaceId ws = wm.active_workspace(1);
    // Drag 1 over 2's slot.
    wm.handle(MoveSizeStarted{1});
    auto s = wm.window(1)->snapshot;
    s.frame = Rect{1300, 300, 400, 400};
    CHECK(wm.handle(WindowChanged{s, change::Geometry}).empty());  // no fight mid-drag
    auto c = wm.handle(MoveSizeEnded{1});
    CHECK_EQ(wm.workspace(ws)->windows, (std::vector<WindowId>{2, 1}));
    CHECK_EQ(placed(c, 1), (std::optional<Rect>(Rect{960, 0, 960, 1080})));
    CHECK_EQ(placed(c, 2), (std::optional<Rect>(Rect{0, 0, 960, 1080})));
    echo(wm, c);
    // A small nudge inside its own slot snaps back.
    wm.handle(MoveSizeStarted{1});
    s = wm.window(1)->snapshot;
    s.frame.x += 30;
    wm.handle(WindowChanged{s, change::Geometry});
    auto c2 = wm.handle(MoveSizeEnded{1});
    CHECK_EQ(placed(c2, 1), (std::optional<Rect>(Rect{960, 0, 960, 1080})));
}

void multi_monitor() {
    WindowManagerConfig cfg;
    cfg.layout = no_gaps();
    cfg.default_layout = LayoutMode::Columns;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true), mon(2, kM2, false)}});
    WorkspaceId ws1 = wm.active_workspace(1), ws2 = wm.active_workspace(2);
    CHECK(ws1 != kNoWorkspace && ws2 != kNoWorkspace && ws1 != ws2);
    echo(wm, wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 10, 10})}));
    auto c = wm.handle(WindowAdded{win(2, 2, Rect{2000, 0, 10, 10})});
    CHECK_EQ(placed(c, 2), (std::optional<Rect>(kM2)));
    echo(wm, c);
    CHECK_EQ(wm.window(2)->workspace, ws2);

    // Directional focus crosses monitors.
    wm.handle(FocusChanged{1});
    auto f = only<FocusWindow>(wm.focus_direction(Direction::Right));
    CHECK(f.size() == 1 && f[0].id == 2);
    CHECK(only<FocusWindow>(wm.focus_direction(Direction::Left)).empty());

    // Window 1 jumps to monitor 2 (keyboard snap): it joins monitor 2's workspace.
    auto s = wm.window(1)->snapshot;
    s.monitor = 2;
    s.frame = Rect{2100, 100, 500, 500};
    auto c2 = wm.handle(WindowChanged{s, change::Geometry | change::Monitor});
    CHECK_EQ(wm.window(1)->workspace, ws2);
    CHECK_EQ(wm.workspace(ws2)->windows, (std::vector<WindowId>{2, 1}));
    CHECK_EQ(placed(c2, 1), (std::optional<Rect>(Rect{2560, 0, 640, 1024})));
    CHECK_EQ(placed(c2, 2), (std::optional<Rect>(Rect{1920, 0, 640, 1024})));
    echo(wm, c2);

    // Monitor 2 is unplugged: its workspace moves to monitor 1, hidden.
    auto c3 = wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    CHECK_EQ(wm.workspace(ws2)->monitor, MonitorId(1));
    CHECK(!wm.workspace(ws2)->active);
    CHECK(has_visible(c3, 1, false));
    CHECK(has_visible(c3, 2, false));
    // And it can be brought up there.
    auto c4 = wm.activate_workspace(ws2);
    CHECK_EQ(placed(c4, 1), (std::optional<Rect>(Rect{960, 0, 960, 1080})));
    CHECK(has_visible(c4, 1, true));
}

void manage_predicate_and_unknown_ids() {
    WindowManagerConfig cfg;
    cfg.manage = [](const WindowSnapshot& s) { return s.title != "7"; };
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.handle(WindowAdded{win(7, 1, Rect{0, 0, 10, 10})});
    CHECK(!wm.window(7));
    CHECK(wm.handle(WindowChanged{win(7, 1, Rect{}), change::Geometry}).empty());
    CHECK(wm.handle(FocusChanged{7}).empty());
    CHECK_EQ(wm.focused(), kNoWindow);
    // Windows that arrive before any monitor wait for one.
    WindowManager wm2;
    wm2.handle(WindowAdded{win(1, 5, Rect{0, 0, 10, 10})});
    CHECK_EQ(wm2.window(1)->workspace, kNoWorkspace);
    wm2.handle(MonitorsChanged{{mon(5, kM1, true)}});
    CHECK(wm2.window(1)->workspace != kNoWorkspace);
}

}  // namespace

int main() {
    floating_default_never_moves_windows();
    tiling_and_reconcile();
    workspaces_hide_and_show();
    close_refocuses_and_relayouts();
    transients_and_minimized();
    drag_swaps_and_snaps_back();
    multi_monitor();
    manage_predicate_and_unknown_ids();
    return bctest::finish("test_window_manager");
}
