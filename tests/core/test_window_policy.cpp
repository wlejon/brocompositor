// The core's shell-tunable policy: window states, edge reservations and the
// pointer-interaction rules, driven purely by synthetic facts.
#include "brocompositor/window_manager.h"

#include "check.h"
#include "printers.h"

using namespace brocompositor;

namespace {

const Rect kM1{0, 0, 1920, 1080};
const Rect kM2{1920, 0, 1280, 1024};

MonitorSnapshot mon(MonitorId id, Rect bounds, bool primary) {
    MonitorSnapshot m;
    m.id = id;
    m.name = std::to_string(id);
    m.bounds = bounds;
    m.work_area = bounds;
    m.primary = primary;
    return m;
}

WindowSnapshot win(WindowId id, MonitorId m, Rect frame) {
    WindowSnapshot s;
    s.id = id;
    s.monitor = m;
    s.frame = frame;
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

std::optional<Rect> placed(const std::vector<Command>& cmds, WindowId id) {
    std::optional<Rect> r;
    for (const auto& p : only<PlaceWindow>(cmds))
        if (p.id == id) r = p.frame;
    return r;
}

std::optional<WindowState> state_of(const std::vector<Command>& cmds, WindowId id) {
    std::optional<WindowState> r;
    for (const auto& s : only<SetWindowState>(cmds))
        if (s.id == id) r = s.state;
    return r;
}

// A well-behaved backend: every command becomes the fact it asks for.
void echo(WindowManager& wm, const std::vector<Command>& cmds) {
    for (const auto& c : cmds) {
        if (auto* st = std::get_if<SetWindowState>(&c)) {
            auto v = wm.window(st->id);
            if (!v) continue;
            WindowSnapshot s = v->snapshot;
            s.minimized = st->state == WindowState::Minimized;
            if (st->state != WindowState::Minimized) {
                s.maximized = st->state == WindowState::Maximized;
                s.fullscreen = st->state == WindowState::Fullscreen;
            }
            echo(wm, wm.handle(WindowChanged{s, change::State}));
        } else if (auto* p = std::get_if<PlaceWindow>(&c)) {
            auto v = wm.window(p->id);
            if (!v) continue;
            WindowSnapshot s = v->snapshot;
            s.frame = p->frame;
            echo(wm, wm.handle(WindowChanged{s, change::Geometry}));
        } else if (auto* f = std::get_if<FocusWindow>(&c)) {
            wm.handle(FocusChanged{f->id});
        }
    }
}

void reservations_shrink_the_work_area() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    CHECK_EQ(wm.work_area(1), kM1);

    auto top = wm.reserve_edge(kNoMonitor, Edge::Top, 34);
    CHECK(top.id > kShellReservationBase);
    CHECK_EQ(wm.work_area(1), (Rect{0, 34, 1920, 1046}));
    CHECK_EQ(wm.reservation(top.id)->rect, (Rect{0, 0, 1920, 34}));
    CHECK_EQ(wm.monitors()[0].work_area, (Rect{0, 34, 1920, 1046}));

    auto bottom = wm.reserve_edge(1, Edge::Bottom, 80);
    CHECK_EQ(wm.work_area(1), (Rect{0, 34, 1920, 966}));
    CHECK_EQ(wm.reservation(bottom.id)->rect, (Rect{0, 1000, 1920, 80}));
    auto left = wm.reserve_edge(1, Edge::Left, 10);
    auto right = wm.reserve_edge(1, Edge::Right, 20);
    CHECK_EQ(wm.work_area(1), (Rect{10, 34, 1890, 966}));
    CHECK_EQ(wm.reservation(right.id)->rect, (Rect{1900, 34, 20, 966}));
    CHECK_EQ(wm.reservations().size(), size_t(4));
    wm.release_edge(left.id);
    wm.release_edge(right.id);

    // A platform reservation (the backend's work area) comes first.
    MonitorSnapshot m = mon(1, kM1, true);
    m.work_area = Rect{0, 0, 1920, 1040};
    wm.handle(MonitorsChanged{{m}});
    CHECK_EQ(wm.work_area(1), (Rect{0, 34, 1920, 926}));
    CHECK_EQ(wm.reservation(bottom.id)->rect, (Rect{0, 960, 1920, 80}));

    // kNoMonitor follows the primary monitor.
    MonitorSnapshot a = mon(1, kM1, false), b = mon(2, kM2, true);
    wm.handle(MonitorsChanged{{a, b}});
    CHECK_EQ(wm.reservation(top.id)->rect, (Rect{1920, 0, 1280, 34}));
    CHECK_EQ(wm.work_area(1), (Rect{0, 0, 1920, 1000}));

    CHECK(wm.release_edge(top.id).empty());
    CHECK(!wm.reservation(top.id));
    CHECK_EQ(wm.work_area(2), kM2);
    CHECK_EQ(wm.reserve_edge(1, Edge::Top, 0).id, kNoReservation);
    CHECK(wm.release_edge(top.id).empty());  // twice: unknown
}

void maximize_and_restore_floating() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    auto bar = wm.reserve_edge(kNoMonitor, Edge::Top, 34);
    const Rect orig{100, 100, 640, 480};
    wm.handle(WindowAdded{win(1, 1, orig)});

    auto c1 = wm.maximize(1);
    CHECK(std::holds_alternative<SetWindowState>(c1.front()));  // the state, then its geometry
    CHECK_EQ(state_of(c1, 1), std::optional<WindowState>(WindowState::Maximized));
    CHECK_EQ(placed(c1, 1), std::optional<Rect>(Rect{0, 34, 1920, 1046}));
    echo(wm, c1);
    CHECK(wm.window(1)->snapshot.maximized);
    CHECK(wm.maximize(1).empty());

    // The bar grows: the maximized window follows the work area.
    wm.release_edge(bar.id);
    auto c2 = wm.reserve_edge(kNoMonitor, Edge::Top, 50).commands;
    CHECK_EQ(placed(c2, 1), std::optional<Rect>(Rect{0, 50, 1920, 1030}));
    echo(wm, c2);

    auto c3 = wm.restore(1);
    CHECK_EQ(state_of(c3, 1), std::optional<WindowState>(WindowState::Normal));
    CHECK_EQ(placed(c3, 1), std::optional<Rect>(orig));
    echo(wm, c3);
    CHECK(!wm.window(1)->snapshot.maximized);
    CHECK(wm.restore(1).empty());

    // Fullscreen covers the monitor, reservations or not.
    auto c4 = wm.fullscreen(1);
    CHECK_EQ(state_of(c4, 1), std::optional<WindowState>(WindowState::Fullscreen));
    CHECK_EQ(placed(c4, 1), std::optional<Rect>(kM1));
    echo(wm, c4);
    auto c5 = wm.restore(1);
    CHECK_EQ(placed(c5, 1), std::optional<Rect>(orig));
    echo(wm, c5);

    // A window the client maximized itself is not refitted by the core.
    auto s = wm.window(1)->snapshot;
    s.maximized = true;
    s.frame = Rect{0, 0, 1920, 1080};
    echo(wm, wm.handle(WindowChanged{s, change::State | change::Geometry}));
    CHECK(placed(wm.reserve_edge(kNoMonitor, Edge::Bottom, 10).commands, 1) == std::nullopt);
    // ... but restore still un-maximizes it, back to the last floating frame.
    auto c6 = wm.restore(1);
    CHECK_EQ(state_of(c6, 1), std::optional<WindowState>(WindowState::Normal));
    CHECK_EQ(placed(c6, 1), std::optional<Rect>(orig));

    CHECK(wm.maximize(42).empty());
    CHECK(wm.minimize(42).empty());
    CHECK(wm.restore(42).empty());
}

void minimize_moves_focus_and_restore_brings_it_back() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 300, 300})});
    wm.handle(WindowAdded{win(2, 1, Rect{400, 0, 300, 300})});
    wm.handle(FocusChanged{1});
    wm.handle(FocusChanged{2});

    auto c1 = wm.minimize(2);
    CHECK_EQ(state_of(c1, 2), std::optional<WindowState>(WindowState::Minimized));
    auto f = only<FocusWindow>(c1);
    CHECK(f.size() == 1 && f[0].id == 1);
    echo(wm, c1);
    CHECK(wm.window(2)->snapshot.minimized);
    CHECK(wm.minimize(2).empty());

    // Minimizing the last visible window focuses nothing.
    auto c2 = wm.minimize(1);
    f = only<FocusWindow>(c2);
    CHECK(f.size() == 1 && f[0].id == kNoWindow);
    echo(wm, c2);

    auto c3 = wm.restore(2);
    CHECK_EQ(state_of(c3, 2), std::optional<WindowState>(WindowState::Normal));
    CHECK(!placed(c3, 2));
    f = only<FocusWindow>(c3);
    CHECK(f.size() == 1 && f[0].id == 2);
    echo(wm, c3);
    CHECK(!wm.window(2)->snapshot.minimized);
    CHECK_EQ(wm.focused(), WindowId(2));

    // A maximized window minimized and restored comes back maximized.
    echo(wm, wm.maximize(2));
    echo(wm, wm.minimize(2));
    auto c4 = wm.restore(2);
    CHECK_EQ(state_of(c4, 2), std::optional<WindowState>(WindowState::Maximized));
}

void tiled_maximize_retiles_the_rest() {
    WindowManagerConfig cfg;
    cfg.layout.gap_inner = 0;
    cfg.layout.gap_outer = 0;
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    WorkspaceId ws = wm.active_workspace(1);
    wm.handle(WindowAdded{win(1, 1, Rect{0, 0, 100, 100})});
    wm.handle(WindowAdded{win(2, 1, Rect{0, 0, 100, 100})});
    echo(wm, wm.set_layout(ws, LayoutMode::Columns));
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{0, 0, 960, 1080}));

    auto c1 = wm.maximize(1);
    CHECK_EQ(placed(c1, 1), std::optional<Rect>(kM1));
    CHECK_EQ(placed(c1, 2), std::optional<Rect>(kM1));  // alone in the layout now
    echo(wm, c1);
    CHECK(!wm.window(1)->tiled);

    // Restore re-tiles it rather than placing the remembered frame.
    auto c2 = wm.restore(1);
    CHECK(!placed(c2, 1));
    echo(wm, c2);
    CHECK(wm.window(1)->tiled);
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{0, 0, 960, 1080}));
    CHECK_EQ(wm.window(2)->snapshot.frame, (Rect{960, 0, 960, 1080}));

    // A reservation re-tiles the workspace into the smaller work area.
    auto c3 = wm.reserve_edge(1, Edge::Top, 40).commands;
    CHECK_EQ(placed(c3, 1), std::optional<Rect>(Rect{0, 40, 960, 1040}));
}

void pointer_interaction_policy() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.handle(WindowAdded{win(1, 1, Rect{100, 100, 400, 300})});
    using PA = PressAction;
    auto press = [&](int x, int y, uint32_t mods = 0, PressButton b = PressButton::Left) {
        return wm.classify_press(1, Point{x, y}, mods, b);
    };
    // Defaults: 38 px title band, 6 px resize border, Super/Alt drags.
    CHECK(press(300, 120) == (PressDecision{PA::Move, 0, false, true}));
    CHECK(press(300, 137) == (PressDecision{PA::Move, 0, false, true}));
    CHECK(press(300, 138).action == PA::None);
    CHECK(press(102, 250) == (PressDecision{PA::Resize, resize_edge::Left, true, false}));
    CHECK(press(497, 397) == (PressDecision{PA::Resize, resize_edge::Right | resize_edge::Bottom, true, false}));
    CHECK(press(300, 101).edges == resize_edge::Top);
    CHECK(press(300, 250, modifier::Super) == (PressDecision{PA::Move, 0, true, false}));
    CHECK(press(300, 250, modifier::Alt, PressButton::Right) ==
          (PressDecision{PA::Resize, resize_edge::Right | resize_edge::Bottom, true, false}));
    CHECK(press(300, 250, modifier::Ctrl).action == PA::None);
    CHECK(press(300, 120, 0, PressButton::Right).action == PA::None);
    CHECK(press(50, 50).action == PA::None);
    CHECK(wm.classify_press(9, Point{300, 120}, 0, PressButton::Left).action == PA::None);

    InteractionConfig ic;
    ic.titlebar_height = 0;
    ic.resize_border = 10;
    ic.drag_modifiers = modifier::Ctrl;
    wm.set_interaction(ic);
    CHECK(press(300, 120).action == PA::None);
    CHECK(press(108, 250).edges == resize_edge::Left);
    CHECK(press(300, 250, modifier::Super).action == PA::None);
    CHECK(press(300, 250, modifier::Ctrl).action == PA::Move);

    // A maximized window has no resize border; a fullscreen one no title band.
    wm.set_interaction(InteractionConfig{});
    echo(wm, wm.maximize(1));
    CHECK(press(1, 500).action == PA::None);
    CHECK(press(500, 10).action == PA::Move);
    echo(wm, wm.fullscreen(1));
    CHECK(press(500, 10).action == PA::None);
}

}  // namespace

int main() {
    reservations_shrink_the_work_area();
    maximize_and_restore_floating();
    minimize_moves_focus_and_restore_brings_it_back();
    tiled_maximize_retiles_the_rest();
    pointer_interaction_policy();
    return bctest::finish("test_window_policy");
}
