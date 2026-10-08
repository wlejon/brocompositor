// bro.compositor window-management policy: window states, the shell's edge
// reservations and the pointer-interaction settings (WindowManager policy,
// window_manager.h).
#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <algorithm>
#include <cctype>

namespace brocompositor::api {

namespace {

// The reservations script made through reserveEdge and has not released:
// they belong to the page, so they go when it does (releaseScriptReservations).
// Touched only on the JS thread.
std::vector<ReservationId> g_script_reservations;

const char* edgeName(Edge e) {
    switch (e) {
        case Edge::Left: return "left";
        case Edge::Top: return "top";
        case Edge::Right: return "right";
        case Edge::Bottom: return "bottom";
    }
    return "top";
}

bool parseEdge(const std::string& s, Edge& out) {
    if (s == "top") out = Edge::Top;
    else if (s == "bottom") out = Edge::Bottom;
    else if (s == "left") out = Edge::Left;
    else if (s == "right") out = Edge::Right;
    else return false;
    return true;
}

uint32_t modifierBit(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "shift") return modifier::Shift;
    if (name == "ctrl" || name == "control") return modifier::Ctrl;
    if (name == "alt" || name == "option") return modifier::Alt;
    if (name == "super" || name == "meta" || name == "win" || name == "cmd" || name == "command")
        return modifier::Super;
    return 0;
}

// "super+alt", "super|alt", "alt, super", ["super", "alt"], "none".
uint32_t parseModifiers(Value v) {
    uint32_t bits = 0;
    if (ev::isString(v)) {
        std::string s = ev::toUtf8(v), cur;
        for (char c : s + "+") {
            if (c == '+' || c == '|' || c == ',' || std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) bits |= modifierBit(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    } else if (ev::isObject(v)) {
        ev::Persistent arr(v);
        double n = ArgReader::getPropDouble(arr.get(), "length", 0.0);
        for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
            ev::Persistent item(ev::getElement(arr.get(), i));
            if (ev::isString(item.get())) bits |= modifierBit(ev::toUtf8(item.get()));
        }
    }
    return bits;
}

Value modifiersToJs(uint32_t bits) {
    const std::pair<uint32_t, const char*> names[] = {
        {modifier::Ctrl, "ctrl"}, {modifier::Alt, "alt"}, {modifier::Shift, "shift"}, {modifier::Super, "super"}};
    uint32_t n = 0;
    for (auto& [bit, name] : names) n += (bits & bit) ? 1 : 0;
    ev::Persistent arr(ev::makeArray(n));
    uint32_t i = 0;
    for (auto& [bit, name] : names)
        if (bits & bit) ev::setElement(arr.get(), i++, ev::fromUtf8(name));
    return arr.get();
}

Value interactionToJs(const InteractionConfig& c) {
    ObjectBuilder b;
    b.set("titlebarHeight", c.titlebar_height);
    b.set("resizeBorder", c.resize_border);
    ev::Persistent mods(modifiersToJs(c.drag_modifiers));
    b.set("dragModifiers", mods.get());
    ObjectBuilder min;
    min.set("width", c.min_size.width);
    min.set("height", c.min_size.height);
    b.set("minSize", min.build());
    b.set("dragThreshold", c.drag_threshold);
    return b.build();
}

void emitReservation(const EdgeReservation& r) {
    ev::Persistent payload(reservationToJs(r));
    dispatchListenerEvent("reservationChanged", payload.get());
}

// Runs a window-state action and reports whether the window exists and the
// backend accepted every command it produced.
Value stateAction(std::span<const Value> args,
                  std::vector<Command> (WindowManager::*action)(WindowId)) {
    ArgReader reader(args);
    if (!reader.has(0)) return ev::fromBool(false);
    auto wm = activeWindowManager();
    if (!wm) return ev::fromBool(false);
    WindowId id = reader.getUint64(0);
    if (!wm->window(id)) return ev::fromBool(false);
    auto cmds = ((*wm).*action)(id);
    if (dispatchCommands(cmds) != 0) return ev::fromBool(false);

    if (auto v = wm->window(id)) {
        ObjectBuilder evObj;
        evObj.set("type", "windowChanged");
        evObj.set("id", static_cast<double>(id));
        evObj.set("windowId", static_cast<double>(id));
        evObj.set("changes", static_cast<double>(change::State | change::Geometry));
        ev::Persistent winP(windowViewToJs(*v, id == wm->focused()));
        evObj.set("window", winP.get());
        dispatchListenerEvent("windowChanged", evObj.build());
    }
    return ev::fromBool(true);
}

}  // namespace

Value reservationToJs(const EdgeReservation& r) {
    ObjectBuilder b;
    b.set("type", "reservationChanged");
    b.set("id", static_cast<double>(r.id));
    b.set("monitor", static_cast<double>(r.monitor));
    b.set("edge", edgeName(r.edge));
    b.set("thickness", r.thickness);
    ev::Persistent rect(rectToJs(r.rect));
    b.set("rect", rect.get());
    b.set("shell", true);
    return b.build();
}

void dispatchShellReservations() {
    auto wm = activeWindowManager();
    if (!wm) return;
    for (const auto& r : wm->reservations()) emitReservation(r);
}

void releaseScriptReservations() {
    std::vector<ReservationId> ids;
    ids.swap(g_script_reservations);
    auto wm = activeWindowManager();
    if (!wm) return;
    std::vector<Command> commands;
    for (ReservationId id : ids) {
        auto more = wm->release_edge(id);
        commands.insert(commands.end(), more.begin(), more.end());
    }
    dispatchCommands(commands);
}

void installPolicyOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // ---- window states -> boolean ----
    comp.def("minimizeWindow", 1, [](Value, std::span<const Value> args) -> Value {
        return stateAction(args, &WindowManager::minimize);
    });
    comp.def("maximizeWindow", 1, [](Value, std::span<const Value> args) -> Value {
        return stateAction(args, &WindowManager::maximize);
    });
    comp.def("fullscreenWindow", 1, [](Value, std::span<const Value> args) -> Value {
        return stateAction(args, &WindowManager::fullscreen);
    });
    comp.def("restoreWindow", 1, [](Value, std::span<const Value> args) -> Value {
        return stateAction(args, &WindowManager::restore);
    });

    // bro.compositor.reserveEdge(edge, thickness, monitor?) -> id (0: refused)
    comp.def("reserveEdge", 3, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        Edge edge = Edge::Top;
        if (!reader.isString(0) || !parseEdge(reader.getString(0), edge)) return ev::fromDouble(0.0);
        int32_t thickness = reader.getInt(1, 0);
        MonitorId mon = reader.has(2) ? reader.getUint(2) : kNoMonitor;
        auto wm = activeWindowManager();
        if (!wm || thickness <= 0) return ev::fromDouble(0.0);
        auto res = wm->reserve_edge(mon, edge, thickness);
        if (res.id != kNoReservation) g_script_reservations.push_back(res.id);
        dispatchCommands(res.commands);
        if (auto r = wm->reservation(res.id)) emitReservation(*r);
        return ev::fromDouble(static_cast<double>(res.id));
    });

    // bro.compositor.releaseEdge(id) -> boolean
    comp.def("releaseEdge", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm || !reader.has(0)) return ev::fromBool(false);
        ReservationId id = reader.getUint64(0);
        auto r = wm->reservation(id);
        if (!r) return ev::fromBool(false);
        std::erase(g_script_reservations, id);
        dispatchCommands(wm->release_edge(id));
        r->rect = Rect{};
        emitReservation(*r);
        return ev::fromBool(true);
    });

    // bro.compositor.getReservations() -> Array<Reservation>
    comp.def("getReservations", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm) return ev::makeArray(0);
        auto list = wm->reservations();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(list.size())));
        for (uint32_t i = 0; i < list.size(); ++i) {
            ev::Persistent item(reservationToJs(list[i]));
            ev::setElement(arr.get(), i, item.get());
        }
        return arr.get();
    });

    // bro.compositor.getWorkArea(monitor?) -> Rect | null
    comp.def("getWorkArea", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        MonitorId mon = reader.has(0) ? reader.getUint(0) : kNoMonitor;
        if (mon == kNoMonitor) {
            for (const auto& m : wm->monitors()) {
                if (mon == kNoMonitor) mon = m.id;
                if (m.primary) {
                    mon = m.id;
                    break;
                }
            }
        }
        for (const auto& m : wm->monitors())
            if (m.id == mon) return rectToJs(m.work_area);
        return ev::null();
    });

    // bro.compositor.getInteraction() / setInteraction(partial) -> Interaction
    comp.def("getInteraction", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        return interactionToJs(wm->interaction());
    });
    comp.def("setInteraction", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        InteractionConfig c = wm->interaction();
        if (reader.isObject(0)) {
            ev::Persistent o(reader.get(0));
            if (ArgReader::hasProp(o.get(), "titlebarHeight"))
                c.titlebar_height = std::max(0, ArgReader::getPropInt(o.get(), "titlebarHeight"));
            if (ArgReader::hasProp(o.get(), "resizeBorder"))
                c.resize_border = std::max(0, ArgReader::getPropInt(o.get(), "resizeBorder"));
            if (ArgReader::hasProp(o.get(), "dragModifiers")) {
                ev::Persistent m(ArgReader::getProp(o.get(), "dragModifiers"));
                c.drag_modifiers = parseModifiers(m.get());
            }
            if (ArgReader::hasProp(o.get(), "minSize")) {
                ev::Persistent m(ArgReader::getProp(o.get(), "minSize"));
                if (ev::isObject(m.get())) {
                    c.min_size.width = std::max(1, ArgReader::getPropInt(m.get(), "width", c.min_size.width));
                    c.min_size.height = std::max(1, ArgReader::getPropInt(m.get(), "height", c.min_size.height));
                }
            }
            if (ArgReader::hasProp(o.get(), "dragThreshold"))
                c.drag_threshold = std::max(0, ArgReader::getPropInt(o.get(), "dragThreshold"));
        }
        wm->set_interaction(c);
        return interactionToJs(c);
    });
}

}  // namespace brocompositor::api
