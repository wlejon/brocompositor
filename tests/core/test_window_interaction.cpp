// The core as a compositing host uses it: stacking order, decoration insets,
// interactive move / resize with edge snapping, and keyboard snapping, all
// driven by synthetic facts.
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

WindowSnapshot win(WindowId id, Rect frame, WindowId owner = kNoWindow) {
    WindowSnapshot s;
    s.id = id;
    s.monitor = 1;
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
            // Like a real backend, the window's monitor follows its frame.
            for (const auto& m : wm.monitors())
                if (m.bounds.contains(s.frame.center())) s.monitor = m.id;
            echo(wm, wm.handle(WindowChanged{s, change::Geometry}));
        } else if (auto* f = std::get_if<FocusWindow>(&c)) {
            echo(wm, wm.handle(FocusChanged{f->id}));
        }
    }
}

using Stack = std::vector<WindowId>;

void stacking_follows_focus() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.handle(WindowAdded{win(1, Rect{0, 0, 400, 300})});
    wm.handle(WindowAdded{win(2, Rect{100, 100, 400, 300})});
    wm.handle(WindowAdded{win(3, Rect{200, 200, 400, 300})});
    CHECK(wm.stacking() == (Stack{1, 2, 3}));  // new windows map on top

    // Focus raises, whether the backend reports it or the host asks for it.
    const uint64_t s0 = wm.stacking_serial();
    wm.handle(FocusChanged{1});
    CHECK(wm.stacking() == (Stack{2, 3, 1}));
    CHECK(wm.stacking_serial() != s0);
    auto c = wm.focus(2);
    CHECK(wm.stacking() == (Stack{3, 1, 2}));
    CHECK_EQ(only<FocusWindow>(c).size(), size_t(1));
    CHECK(!wm.raise(2));  // already on top
    CHECK(wm.raise(3));
    CHECK(wm.stacking() == (Stack{1, 2, 3}));

    // A dialog stays above the window that owns it.
    wm.handle(WindowAdded{win(4, Rect{50, 50, 200, 100}, 1)});
    CHECK(wm.stacking() == (Stack{1, 2, 3, 4}));
    wm.handle(FocusChanged{1});
    CHECK(wm.stacking() == (Stack{2, 3, 1, 4}));

    // Closing the focused window focuses the most recent one, which rises.
    echo(wm, wm.focus(3));
    CHECK(wm.stacking() == (Stack{2, 1, 4, 3}));
    echo(wm, wm.handle(WindowRemoved{3}));
    CHECK(wm.stacking() == (Stack{2, 1, 4}));
    CHECK_EQ(wm.focused(), WindowId(1));

    // Raising on focus is policy.
    WindowManagerConfig cfg;
    cfg.raise_on_focus = false;
    cfg.focus_on_map = true;
    WindowManager quiet(cfg);
    quiet.handle(MonitorsChanged{{mon(1, kM1, true)}});
    auto added = quiet.handle(WindowAdded{win(1, Rect{0, 0, 400, 300})});
    CHECK_EQ(only<FocusWindow>(added).size(), size_t(1));  // focus_on_map
    quiet.handle(WindowAdded{win(2, Rect{0, 0, 400, 300})});
    quiet.handle(FocusChanged{1});
    CHECK(quiet.stacking() == (Stack{1, 2}));
}

void decorations_fit_the_frame() {
    WindowManagerConfig cfg;
    cfg.decoration.insets = Margins{6, 36, 6, 6};
    cfg.decoration.maximized_insets = Margins{0, 32, 0, 0};
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.reserve_edge(kNoMonitor, Edge::Top, 40);

    // A decorated window placed against the top of the work area is moved
    // down so its title bar is below the panel.
    WindowSnapshot d = win(1, Rect{100, 40, 640, 480});
    d.decorated = true;
    auto c = wm.handle(WindowAdded{d});
    CHECK_EQ(placed(c, 1), std::optional<Rect>(Rect{100, 76, 640, 480}));
    echo(wm, c);
    CHECK_EQ(wm.decoration_insets(1), (Margins{6, 36, 6, 6}));
    CHECK_EQ(wm.window(1)->outer, (Rect{94, 40, 652, 522}));

    // Maximize fits the frame (title bar only) into the work area.
    auto m = wm.maximize(1);
    CHECK_EQ(placed(m, 1), std::optional<Rect>(Rect{0, 72, 1920, 1008}));
    echo(wm, m);
    CHECK_EQ(wm.decoration_insets(1), (Margins{0, 32, 0, 0}));
    echo(wm, wm.restore(1));
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{100, 76, 640, 480}));

    // An undecorated window is unaffected; a fullscreen one has no frame.
    wm.handle(WindowAdded{win(2, Rect{300, 300, 400, 300})});
    CHECK_EQ(wm.decoration_insets(2), Margins{});
    CHECK_EQ(placed(wm.maximize(2), 2), std::optional<Rect>(Rect{0, 40, 1920, 1040}));
    echo(wm, wm.fullscreen(1));
    CHECK_EQ(wm.decoration_insets(1), Margins{});
    echo(wm, wm.restore(1));

    // Presses inside a framed client are the client's.
    CHECK(wm.classify_press(1, Point{300, 80}, 0, PressButton::Left).action == PressAction::None);

    // New insets re-fit what the core keeps fitted.
    echo(wm, wm.maximize(1));
    DecorationConfig wide = cfg.decoration;
    wide.maximized_insets.top = 40;
    auto r = wm.set_decoration(wide);
    CHECK_EQ(placed(r, 1), std::optional<Rect>(Rect{0, 80, 1920, 1000}));
    CHECK(wm.set_decoration(wide).empty());

    // Tiling slots hold the whole frame.
    WindowManager tiler(cfg);
    tiler.handle(MonitorsChanged{{mon(1, kM1, true)}});
    WindowSnapshot t = win(1, Rect{0, 0, 100, 100});
    t.decorated = true;
    tiler.handle(WindowAdded{t});
    auto lay = tiler.set_layout(tiler.active_workspace(1), LayoutMode::Columns);
    CHECK(wm.snap_rect(1, SnapZone::None).empty());
    auto tp = placed(lay, 1);
    CHECK(tp.has_value());
    // The slot (gaps aside) holds the frame: the client sits inside the insets.
    const int32_t gap = tp->x - 6;  // the layout's outer gap, the same on every side
    CHECK_EQ(1920 - (tp->x + tp->width) - 6, gap);
    CHECK_EQ(tp->y - 36, gap);
    CHECK_EQ(1080 - (tp->y + tp->height) - 6, gap);
}

// Zero maximized insets: maximized windows are borderless. The client fills
// the work area edge to edge, the host still frames it (so it can float
// controls over it), and a press anywhere on it is the client's: no title
// band appears out of the interaction policy.
void borderless_maximized() {
    WindowManagerConfig cfg;
    cfg.decoration.insets = Margins{2, 28, 2, 2};
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.reserve_edge(kNoMonitor, Edge::Bottom, 40);
    WindowSnapshot d = win(1, Rect{100, 100, 640, 480});
    d.decorated = true;
    echo(wm, wm.handle(WindowAdded{d}));
    CHECK(wm.framed(1));
    CHECK(wm.window(1)->framed);
    CHECK_EQ(wm.decoration_insets(1), (Margins{2, 28, 2, 2}));

    auto m = wm.maximize(1);
    CHECK_EQ(placed(m, 1), std::optional<Rect>(Rect{0, 0, 1920, 1040}));
    echo(wm, m);
    CHECK_EQ(wm.decoration_insets(1), Margins{});
    CHECK(wm.framed(1));
    CHECK_EQ(wm.window(1)->outer, (Rect{0, 0, 1920, 1040}));
    CHECK(wm.classify_press(1, Point{1900, 5}, 0, PressButton::Left).action == PressAction::None);
    CHECK(wm.classify_press(1, Point{3, 500}, 0, PressButton::Left).action == PressAction::None);
    // The drag modifiers still move it.
    CHECK(wm.classify_press(1, Point{900, 500}, modifier::Super, PressButton::Left).action == PressAction::Move);

    // Fullscreen is not framed; an undecorated window never is.
    echo(wm, wm.fullscreen(1));
    CHECK(!wm.framed(1));
    echo(wm, wm.restore(1));
    wm.handle(WindowAdded{win(2, Rect{300, 300, 400, 300})});
    CHECK(!wm.framed(2));
    // No frames declared at all: nothing is framed.
    WindowManager bare;
    bare.handle(MonitorsChanged{{mon(1, kM1, true)}});
    bare.handle(WindowAdded{d});
    CHECK(!bare.framed(1));
}

void drag_moves_and_resizes() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm.handle(WindowAdded{win(1, Rect{100, 100, 400, 300})});
    wm.handle(WindowAdded{win(2, Rect{600, 100, 400, 300})});

    // A title-bar press waits for the threshold, and raises at once.
    auto b = wm.begin_move(1, Point{200, 110});
    CHECK(wm.stacking() == (Stack{2, 1}));
    CHECK_EQ(only<FocusWindow>(b).size(), size_t(1));
    echo(wm, b);
    CHECK(wm.drag() && !wm.drag()->active);
    CHECK(wm.drag_to(Point{202, 112}).empty());
    CHECK(!wm.drag()->active);
    auto mv = wm.drag_to(Point{250, 160});
    CHECK(wm.drag()->active);
    CHECK_EQ(placed(mv, 1), std::optional<Rect>(Rect{150, 150, 400, 300}));
    echo(wm, mv);
    // The title bar stays on screen.
    auto clamp = wm.drag_to(Point{250, -500});
    CHECK_EQ(placed(clamp, 1), std::optional<Rect>(Rect{150, 0, 400, 300}));
    echo(wm, clamp);
    echo(wm, wm.end_drag());
    CHECK(!wm.drag());
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{150, 0, 400, 300}));

    // Resize from the left edge stops at the minimum size.
    echo(wm, wm.begin_resize(1, Point{150, 100}, resize_edge::Left | resize_edge::Bottom));
    auto rs = wm.drag_to(Point{100, 150});
    CHECK_EQ(placed(rs, 1), std::optional<Rect>(Rect{100, 0, 450, 350}));
    echo(wm, rs);
    auto tiny = wm.drag_to(Point{900, -200});
    CHECK_EQ(placed(tiny, 1), std::optional<Rect>(Rect{350, 0, 200, 150}));
    echo(wm, tiny);
    wm.cancel_drag();
    CHECK(!wm.drag());

    // Nothing to resize while maximized; no drag of an unknown window.
    echo(wm, wm.maximize(2));
    CHECK(wm.begin_resize(2, Point{0, 0}, resize_edge::Right).empty());
    CHECK(!wm.drag());
    CHECK(wm.begin_move(99, Point{0, 0}).empty());
}

void drag_to_edges_snaps() {
    WindowManagerConfig cfg;
    cfg.decoration.insets = Margins{4, 30, 4, 4};
    cfg.decoration.maximized_insets = Margins{0, 30, 0, 0};
    WindowManager wm(cfg);
    wm.handle(MonitorsChanged{{mon(1, kM1, true), mon(2, kM2, false)}});
    wm.reserve_edge(1, Edge::Top, 40);
    WindowSnapshot s = win(1, Rect{300, 300, 600, 400});
    s.decorated = true;
    echo(wm, wm.handle(WindowAdded{s}));
    const Rect orig = wm.window(1)->snapshot.frame;

    // Dragging to the left edge arms the left half; the preview is the frame
    // rect, the window lands with its decoration inside it.
    echo(wm, wm.begin_move(1, Point{500, 285}, true));
    echo(wm, wm.drag_to(Point{3, 500}));
    CHECK(wm.drag()->snap == SnapZone::Left);
    CHECK_EQ(wm.drag()->snap_rect, (Rect{0, 40, 960, 1040}));
    echo(wm, wm.drag_to(Point{400, 500}));  // and away again: disarmed
    CHECK(wm.drag()->snap == SnapZone::None);
    echo(wm, wm.drag_to(Point{2, 500}));
    auto end = wm.end_drag();
    CHECK_EQ(placed(end, 1), std::optional<Rect>(Rect{4, 70, 952, 1006}));
    echo(wm, end);
    CHECK(wm.window(1)->snap == SnapZone::Left);

    // Dragging the snapped window restores its own size under the pointer.
    echo(wm, wm.begin_move(1, Point{480, 55}));
    auto out = wm.drag_to(Point{600, 300});
    auto p = placed(out, 1);
    CHECK(p.has_value());
    CHECK_EQ(p->width, orig.width);
    CHECK_EQ(p->height, orig.height);
    CHECK(p->x - 4 <= 600 && p->x + p->width + 4 > 600);  // still under the pointer
    CHECK_EQ(p->y - 30, 300 - (55 - 40));                 // same depth into the title bar
    echo(wm, out);
    CHECK(wm.window(1)->snap == SnapZone::None);

    // The top edge maximizes; the edge between the two monitors arms nothing.
    echo(wm, wm.drag_to(Point{800, 1}));
    CHECK(wm.drag()->snap == SnapZone::Maximize);
    echo(wm, wm.drag_to(Point{1918, 500}));
    CHECK(wm.drag()->snap == SnapZone::None);
    echo(wm, wm.drag_to(Point{800, 2}));
    auto mx = wm.end_drag();
    CHECK_EQ(state_of(mx, 1), std::optional<WindowState>(WindowState::Maximized));
    echo(wm, mx);
    CHECK(wm.window(1)->snapshot.maximized);

    // A maximized window dragged by its title bar comes back to its size.
    echo(wm, wm.begin_move(1, Point{960, 50}));
    auto un = wm.drag_to(Point{960, 200});
    CHECK_EQ(state_of(un, 1), std::optional<WindowState>(WindowState::Normal));
    CHECK_EQ(placed(un, 1)->width, orig.width);
    echo(wm, un);
    CHECK(!wm.window(1)->snapshot.maximized);
    // Dropped on the right edge of the second monitor: snapped there.
    echo(wm, wm.drag_to(Point{3198, 500}));
    CHECK(wm.drag()->snap == SnapZone::Right);
    CHECK_EQ(wm.drag()->snap_rect, (Rect{2560, 0, 640, 1024}));
    echo(wm, wm.end_drag());
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{2564, 30, 632, 990}));
    CHECK_EQ(wm.window(1)->workspace, wm.active_workspace(2));

    // Corners arm quarters when configured; snapping can be turned off.
    SnapConfig sc;
    sc.corner_size = 100;
    wm.set_snapping(sc);
    echo(wm, wm.begin_move(1, Point{2800, 20}, true));
    echo(wm, wm.drag_to(Point{3199, 1000}));
    CHECK(wm.drag()->snap == SnapZone::BottomRight);
    sc.enabled = false;
    wm.set_snapping(sc);
    echo(wm, wm.drag_to(Point{3199, 990}));
    CHECK(wm.drag()->snap == SnapZone::None);
    wm.cancel_drag();
}

void keyboard_snapping() {
    WindowManager wm;
    wm.handle(MonitorsChanged{{mon(1, kM1, true)}});
    const Rect orig{300, 300, 600, 400};
    wm.handle(WindowAdded{win(1, orig)});

    echo(wm, wm.snap_toward(1, Direction::Left));
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{0, 0, 960, 1080}));
    CHECK(wm.snap_toward(1, Direction::Left).empty());  // already there
    echo(wm, wm.snap_toward(1, Direction::Right));      // from the other half: restore
    CHECK_EQ(wm.window(1)->snapshot.frame, orig);
    echo(wm, wm.snap_toward(1, Direction::Right));
    CHECK_EQ(wm.window(1)->snapshot.frame, (Rect{960, 0, 960, 1080}));

    // A snapped window follows its work area.
    auto r = wm.reserve_edge(kNoMonitor, Edge::Top, 30);
    CHECK_EQ(placed(r.commands, 1), std::optional<Rect>(Rect{960, 30, 960, 1050}));
    echo(wm, r.commands);

    echo(wm, wm.snap_toward(1, Direction::Up));
    CHECK(wm.window(1)->snapshot.maximized);
    echo(wm, wm.snap_toward(1, Direction::Down));  // restore: back to the original frame
    CHECK(!wm.window(1)->snapshot.maximized);
    CHECK_EQ(wm.window(1)->snapshot.frame, orig);
    auto mn = wm.snap_toward(1, Direction::Down);  // normal: minimize
    CHECK_EQ(state_of(mn, 1), std::optional<WindowState>(WindowState::Minimized));

    // Snapping from maximized leaves the state but keeps the restore frame.
    WindowManager wm2;
    wm2.handle(MonitorsChanged{{mon(1, kM1, true)}});
    wm2.handle(WindowAdded{win(1, orig)});
    echo(wm2, wm2.maximize(1));
    auto sl = wm2.snap(1, SnapZone::Left);
    CHECK_EQ(state_of(sl, 1), std::optional<WindowState>(WindowState::Normal));
    echo(wm2, sl);
    CHECK_EQ(wm2.window(1)->snapshot.frame, (Rect{0, 0, 960, 1080}));
    echo(wm2, wm2.snap(1, SnapZone::None));
    CHECK_EQ(wm2.window(1)->snapshot.frame, orig);

    CHECK(snap_zone_from_string("top-right") == std::optional<SnapZone>(SnapZone::TopRight));
    CHECK(!snap_zone_from_string("sideways"));
    CHECK(std::string(to_string(SnapZone::Left)) == "left");
}

}  // namespace

int main() {
    stacking_follows_focus();
    decorations_fit_the_frame();
    borderless_maximized();
    drag_moves_and_resizes();
    drag_to_edges_snaps();
    keyboard_snapping();
    return bctest::finish("test_window_interaction");
}
