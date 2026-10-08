// bro.compositor for hosts that composite the desktop themselves: the
// stacking order, the decoration band the shell's window frames take,
// interactive move / resize started from script (a frame's title bar or
// edge), snapping, and the events that report them (WindowManager,
// window_manager.h).
#include "api.h"
#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <cctype>
#include <mutex>

namespace brocompositor::api {

namespace {

std::mutex g_pointer_mu;
PointerSource g_pointer_source;

// What the last tick reported, so events fire on change only.
uint64_t g_reported_stack_serial = ~uint64_t(0);
std::optional<DragInfo> g_reported_drag;

Value idsToJs(const std::vector<WindowId>& ids) {
    ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(ids.size())));
    for (uint32_t i = 0; i < ids.size(); ++i) ev::setElement(arr.get(), i, ev::fromDouble(double(ids[i])));
    return arr.get();
}

Value marginsToJs(const Margins& m) {
    ObjectBuilder b;
    b.set("top", m.top);
    b.set("left", m.left);
    b.set("right", m.right);
    b.set("bottom", m.bottom);
    return b.build();
}

Margins marginsFromJs(Value v, Margins m) {
    if (ev::isNumber(v)) {
        int32_t n = std::max(0, static_cast<int32_t>(ev::toDouble(v)));
        return Margins{n, n, n, n};
    }
    if (!ev::isObject(v)) return m;
    ev::Persistent o(v);
    auto field = [&](const char* name, int32_t& out) {
        if (ArgReader::hasProp(o.get(), name)) out = std::max(0, ArgReader::getPropInt(o.get(), name));
    };
    field("top", m.top);
    field("left", m.left);
    field("right", m.right);
    field("bottom", m.bottom);
    return m;
}

Value decorationToJs(const DecorationConfig& d) {
    ObjectBuilder b;
    ev::Persistent in(marginsToJs(d.insets));
    ev::Persistent mx(marginsToJs(d.maximized_insets));
    b.set("insets", in.get());
    b.set("maximizedInsets", mx.get());
    return b.build();
}

Value snappingToJs(const SnapConfig& c) {
    ObjectBuilder b;
    b.set("enabled", c.enabled);
    b.set("edgeThreshold", c.edge_threshold);
    b.set("cornerSize", c.corner_size);
    b.set("restoreOnDrag", c.restore_on_drag);
    return b.build();
}

// "top left", "bottom-right", ["top", "left"], or resize_edge bits.
uint32_t parseEdges(Value v) {
    if (ev::isNumber(v)) return static_cast<uint32_t>(ev::toDouble(v));
    std::string s;
    if (ev::isString(v)) {
        s = ev::toUtf8(v);
    } else if (ev::isObject(v)) {
        ev::Persistent arr(v);
        double n = ArgReader::getPropDouble(arr.get(), "length", 0.0);
        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
            ev::Persistent item(ev::getElement(arr.get(), i));
            if (ev::isString(item.get())) s += ev::toUtf8(item.get()) + " ";
        }
    }
    uint32_t bits = 0;
    std::string cur;
    for (char c : s + " ") {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            continue;
        }
        if (cur == "top" || cur == "n") bits |= resize_edge::Top;
        else if (cur == "bottom" || cur == "s") bits |= resize_edge::Bottom;
        else if (cur == "left" || cur == "w") bits |= resize_edge::Left;
        else if (cur == "right" || cur == "e") bits |= resize_edge::Right;
        cur.clear();
    }
    return bits;
}

Value edgesToJs(uint32_t bits) {
    std::string s;
    if (bits & resize_edge::Top) s += "top";
    if (bits & resize_edge::Bottom) s += s.empty() ? "bottom" : " bottom";
    if (bits & resize_edge::Left) s += s.empty() ? "left" : " left";
    if (bits & resize_edge::Right) s += s.empty() ? "right" : " right";
    return ev::fromUtf8(s);
}

Value dragToJs(const DragInfo& d) {
    ObjectBuilder b;
    b.set("windowId", static_cast<double>(d.window));
    b.set("action", d.action == PressAction::Resize ? "resize" : "move");
    b.set("edges", edgesToJs(d.edges));
    b.set("active", d.active);
    b.set("snap", to_string(d.snap));
    if (d.snap != SnapZone::None) {
        ev::Persistent r(rectToJs(d.snap_rect));
        b.set("snapRect", r.get());
    } else {
        b.set("snapRect", ev::null());
    }
    return b.build();
}

// The pointer a drag starts from: {x, y} in the options, else the host's.
// Reads through the rooted handle each time: a property read can collect,
// and the object may move.
bool pointerFor(const ev::Persistent& opts, Point& out) {
    if (ev::isObject(opts.get()) && ArgReader::hasProp(opts.get(), "x") && ArgReader::hasProp(opts.get(), "y")) {
        const int32_t x = ArgReader::getPropInt(opts.get(), "x");
        const int32_t y = ArgReader::getPropInt(opts.get(), "y");
        out = Point{x, y};
        return true;
    }
    PointerSource src;
    {
        std::lock_guard lock(g_pointer_mu);
        src = g_pointer_source;
    }
    if (!src) return false;
    return src(out.x, out.y);
}

}  // namespace

void setPointerSource(PointerSource source) {
    std::lock_guard lock(g_pointer_mu);
    g_pointer_source = std::move(source);
}

// Fired from tickCompositorAsync: the stacking order and the drag's armed
// snap, when they changed since the last tick.
void dispatchInteractionChanges() {
    auto wm = activeWindowManager();
    if (!wm) return;
    if (wm->stacking_serial() != g_reported_stack_serial) {
        g_reported_stack_serial = wm->stacking_serial();
        ObjectBuilder obj;
        obj.set("type", "stackingChanged");
        ev::Persistent ids(idsToJs(wm->stacking()));
        obj.set("stacking", ids.get());
        dispatchListenerEvent("stackingChanged", obj.build());
    }
    std::optional<DragInfo> d = wm->drag();
    auto zone = [](const std::optional<DragInfo>& x) { return x && x->active ? x->snap : SnapZone::None; };
    const bool changed = zone(d) != zone(g_reported_drag) ||
                         (zone(d) != SnapZone::None && d->snap_rect != g_reported_drag->snap_rect) ||
                         (d && g_reported_drag && d->window != g_reported_drag->window && zone(d) != SnapZone::None);
    if (changed) {
        ObjectBuilder obj;
        obj.set("type", "snapPreview");
        const SnapZone z = zone(d);
        obj.set("windowId", static_cast<double>(d ? d->window : kNoWindow));
        obj.set("zone", to_string(z));
        if (z != SnapZone::None) {
            ev::Persistent r(rectToJs(d->snap_rect));
            obj.set("rect", r.get());
        } else {
            obj.set("rect", ev::null());
        }
        dispatchListenerEvent("snapPreview", obj.build());
    }
    g_reported_drag = d;
}

void installInteractionOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // ---- stacking ----
    // bro.compositor.getStacking() -> Array<windowId>, bottom to top
    comp.def("getStacking", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        return wm ? idsToJs(wm->stacking()) : ev::makeArray(0);
    });
    // bro.compositor.raiseWindow(id) -> boolean (false: unknown or already on top)
    comp.def("raiseWindow", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0)) return ev::fromBool(false);
        return ev::fromBool(wm->raise(reader.getUint64(0)));
    });

    // ---- decorations ----
    comp.def("getDecorations", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        return wm ? decorationToJs(wm->decoration()) : ev::null();
    });
    // bro.compositor.setDecorations({insets, maximizedInsets}) -> Decorations
    comp.def("setDecorations", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        DecorationConfig d = wm->decoration();
        if (reader.isObject(0)) {
            ev::Persistent o(reader.get(0));
            if (ArgReader::hasProp(o.get(), "insets")) {
                ev::Persistent v(ArgReader::getProp(o.get(), "insets"));
                d.insets = marginsFromJs(v.get(), d.insets);
            }
            if (ArgReader::hasProp(o.get(), "maximizedInsets")) {
                ev::Persistent v(ArgReader::getProp(o.get(), "maximizedInsets"));
                d.maximized_insets = marginsFromJs(v.get(), d.maximized_insets);
            }
        }
        dispatchCommands(wm->set_decoration(d));
        return decorationToJs(wm->decoration());
    });

    // ---- interactive move / resize ----
    // bro.compositor.beginMove(id, {x, y, immediate}?) -> boolean
    comp.def("beginMove", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0)) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        ev::Persistent opts(reader.has(1) ? reader.get(1) : ev::undefined());
        Point p;
        if (!pointerFor(opts, p)) return ev::fromBool(false);
        bool immediate = ev::isObject(opts.get()) && ArgReader::getPropBool(opts.get(), "immediate", false);
        dispatchCommands(wm->begin_move(id, p, immediate));
        auto d = wm->drag();
        return ev::fromBool(d && d->window == id);
    });
    // bro.compositor.beginResize(id, edges, {x, y, immediate}?) -> boolean
    comp.def("beginResize", 3, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0) || !reader.has(1)) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        uint32_t edges = parseEdges(reader.get(1));
        ev::Persistent opts(reader.has(2) ? reader.get(2) : ev::undefined());
        Point p;
        if (!pointerFor(opts, p)) return ev::fromBool(false);
        bool immediate = !ev::isObject(opts.get()) || ArgReader::getPropBool(opts.get(), "immediate", true);
        dispatchCommands(wm->begin_resize(id, p, edges, immediate));
        auto d = wm->drag();
        return ev::fromBool(d && d->window == id);
    });
    // bro.compositor.dragTo(x, y) / endDrag() / cancelDrag(): a host that
    // routes the pointer itself (bro under DRM) drives these; script needs
    // them only without one.
    comp.def("dragTo", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !wm->drag()) return ev::fromBool(false);
        dispatchCommands(wm->drag_to(Point{reader.getInt(0), reader.getInt(1)}));
        return ev::fromBool(true);
    });
    comp.def("endDrag", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm || !wm->drag()) return ev::fromBool(false);
        dispatchCommands(wm->end_drag());
        return ev::fromBool(true);
    });
    comp.def("cancelDrag", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm || !wm->drag()) return ev::fromBool(false);
        dispatchCommands(wm->cancel_drag());
        return ev::fromBool(true);
    });
    comp.def("getDrag", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        auto d = wm ? wm->drag() : std::nullopt;
        return d ? dragToJs(*d) : ev::null();
    });

    // ---- snapping ----
    comp.def("getSnapping", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        return wm ? snappingToJs(wm->snapping()) : ev::null();
    });
    comp.def("setSnapping", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        SnapConfig c = wm->snapping();
        if (reader.isObject(0)) {
            ev::Persistent o(reader.get(0));
            if (ArgReader::hasProp(o.get(), "enabled")) c.enabled = ArgReader::getPropBool(o.get(), "enabled");
            if (ArgReader::hasProp(o.get(), "edgeThreshold"))
                c.edge_threshold = std::max(1, ArgReader::getPropInt(o.get(), "edgeThreshold"));
            if (ArgReader::hasProp(o.get(), "cornerSize"))
                c.corner_size = std::max(0, ArgReader::getPropInt(o.get(), "cornerSize"));
            if (ArgReader::hasProp(o.get(), "restoreOnDrag"))
                c.restore_on_drag = ArgReader::getPropBool(o.get(), "restoreOnDrag");
        }
        wm->set_snapping(c);
        return snappingToJs(c);
    });
    // bro.compositor.snapWindow(id, "left" | "right" | "maximize" | "top-left" | ... | "none") -> boolean
    comp.def("snapWindow", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0) || !reader.isString(1)) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        auto zone = snap_zone_from_string(reader.getString(1));
        if (!zone || !wm->window(id)) return ev::fromBool(false);
        auto cmds = wm->snap(id, *zone);
        if (cmds.empty()) return ev::fromBool(false);
        return ev::fromBool(dispatchCommands(cmds) == 0);
    });
    // bro.compositor.snapWindowToward(id, "left" | "right" | "up" | "down") -> boolean
    comp.def("snapWindowToward", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0) || !reader.isString(1)) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        std::string d = reader.getString(1);
        Direction dir;
        if (d == "left") dir = Direction::Left;
        else if (d == "right") dir = Direction::Right;
        else if (d == "up") dir = Direction::Up;
        else if (d == "down") dir = Direction::Down;
        else return ev::fromBool(false);
        auto cmds = wm->snap_toward(id, dir);
        if (cmds.empty()) return ev::fromBool(false);
        return ev::fromBool(dispatchCommands(cmds) == 0);
    });
}

}  // namespace brocompositor::api
