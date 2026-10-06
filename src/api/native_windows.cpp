#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

namespace brocompositor::api {

void installWindowsOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // bro.compositor.getWindows() -> Array<WindowInfo>
    comp.def("getWindows", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm) return ev::makeArray(0);

        auto winIds = wm->windows();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(winIds.size())));
        WindowId focusedId = wm->focused();
        for (uint32_t i = 0; i < winIds.size(); ++i) {
            auto wOpt = wm->window(winIds[i]);
            if (wOpt) {
                ev::Persistent item(windowViewToJs(*wOpt, wOpt->snapshot.id == focusedId));
                ev::setElement(arr.get(), i, item.get());
            }
        }
        return arr.get();
    });

    // bro.compositor.getWindow(id) -> WindowInfo | null
    comp.def("getWindow", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::null();
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        WindowId id = reader.getUint64(0);
        auto wOpt = wm->window(id);
        if (!wOpt) return ev::null();
        return windowViewToJs(*wOpt, wOpt->snapshot.id == wm->focused());
    });

    // bro.compositor.focusWindow(id) -> boolean
    comp.def("focusWindow", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        auto wOpt = wm->window(id);
        if (!wOpt) return ev::fromBool(false);

        auto cmds = wm->focus(id);
        dispatchCommands(cmds);

        ObjectBuilder evObj;
        evObj.set("type", "focusChanged");
        evObj.set("id", static_cast<double>(id));
        evObj.set("windowId", static_cast<double>(id));
        dispatchListenerEvent("focusChanged", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.moveWindow(id, rectOrX, y?, width?, height?) -> boolean
    comp.def("moveWindow", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        auto wOpt = wm->window(id);
        if (!wOpt) return ev::fromBool(false);

        Rect r = wOpt->snapshot.frame;
        if (reader.isObject(1)) {
            ev::Persistent objP(reader.get(1));
            if (ArgReader::hasProp(objP.get(), "x")) r.x = ArgReader::getPropInt(objP.get(), "x");
            if (ArgReader::hasProp(objP.get(), "y")) r.y = ArgReader::getPropInt(objP.get(), "y");
            if (ArgReader::hasProp(objP.get(), "width")) r.width = ArgReader::getPropInt(objP.get(), "width");
            if (ArgReader::hasProp(objP.get(), "height")) r.height = ArgReader::getPropInt(objP.get(), "height");
        } else if (reader.has(1)) {
            r.x = reader.getInt(1);
            if (reader.has(2)) r.y = reader.getInt(2);
            if (reader.has(3)) r.width = reader.getInt(3);
            if (reader.has(4)) r.height = reader.getInt(4);
        }

        std::vector<Command> cmds;
        cmds.push_back(PlaceWindow{id, r});
        dispatchCommands(cmds);

        WindowSnapshot s = wOpt->snapshot;
        s.frame = r;
        wm->handle(WindowChanged{s, change::Geometry});

        ObjectBuilder evObj;
        evObj.set("type", "windowChanged");
        evObj.set("id", static_cast<double>(id));
        evObj.set("windowId", static_cast<double>(id));
        ev::Persistent winP(windowSnapshotToJs(s, id == wm->focused(), wOpt->workspace,
                                              wOpt->floating, wOpt->tiled, wOpt->shown));
        evObj.set("window", winP.get());
        dispatchListenerEvent("windowChanged", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.resizeWindow(id, width, height) -> boolean
    comp.def("resizeWindow", 3, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0) || !reader.has(1) || !reader.has(2)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        auto wOpt = wm->window(id);
        if (!wOpt) return ev::fromBool(false);

        Rect r = wOpt->snapshot.frame;
        r.width = reader.getInt(1);
        r.height = reader.getInt(2);

        std::vector<Command> cmds;
        cmds.push_back(PlaceWindow{id, r});
        dispatchCommands(cmds);

        WindowSnapshot s = wOpt->snapshot;
        s.frame = r;
        wm->handle(WindowChanged{s, change::Geometry});

        return ev::fromBool(true);
    });

    // bro.compositor.closeWindow(id) -> boolean
    comp.def("closeWindow", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        auto wOpt = wm->window(id);
        if (!wOpt) return ev::fromBool(false);

        std::vector<Command> cmds;
        cmds.push_back(CloseWindow{id});
        dispatchCommands(cmds);

        wm->handle(WindowRemoved{id});

        ObjectBuilder evObj;
        evObj.set("type", "windowClosed");
        evObj.set("id", static_cast<double>(id));
        evObj.set("windowId", static_cast<double>(id));
        dispatchListenerEvent("windowClosed", evObj.build());
        dispatchListenerEvent("windowRemoved", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.setFloating(id, floating) -> boolean
    comp.def("setFloating", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId id = reader.getUint64(0);
        bool floating = reader.getBool(1, true);
        auto cmds = wm->set_floating(id, floating);
        dispatchCommands(cmds);
        return ev::fromBool(true);
    });

    // bro.compositor.swapWindows(idA, idB) -> boolean
    comp.def("swapWindows", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0) || !reader.has(1)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId a = reader.getUint64(0);
        WindowId b = reader.getUint64(1);
        auto cmds = wm->swap(a, b);
        dispatchCommands(cmds);
        return ev::fromBool(true);
    });

    // bro.compositor.focusDirection(direction) -> boolean
    comp.def("focusDirection", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.isString(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        std::string dirStr = reader.getString(0);
        Direction dir = Direction::Right;
        if (dirStr == "left") dir = Direction::Left;
        else if (dirStr == "up") dir = Direction::Up;
        else if (dirStr == "down") dir = Direction::Down;
        else if (dirStr == "right") dir = Direction::Right;
        auto cmds = wm->focus_direction(dir);
        dispatchCommands(cmds);
        return ev::fromBool(true);
    });
}

} // namespace brocompositor::api
