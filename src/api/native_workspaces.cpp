#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

namespace brocompositor::api {

void installWorkspacesOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // bro.compositor.getWorkspaces() -> Array<WorkspaceInfo>
    comp.def("getWorkspaces", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm) return ev::makeArray(0);

        auto wsList = wm->workspaces();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(wsList.size())));
        for (uint32_t i = 0; i < wsList.size(); ++i) {
            ev::Persistent item(workspaceViewToJs(wsList[i]));
            ev::setElement(arr.get(), i, item.get());
        }
        return arr.get();
    });

    // bro.compositor.getWorkspace(id) -> WorkspaceInfo | null
    comp.def("getWorkspace", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::null();
        auto wm = activeWindowManager();
        if (!wm) return ev::null();
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        auto wsOpt = wm->workspace(id);
        if (!wsOpt) return ev::null();
        return workspaceViewToJs(*wsOpt);
    });

    // bro.compositor.switchWorkspace(id) -> boolean
    comp.def("switchWorkspace", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        auto wsOpt = wm->workspace(id);
        if (!wsOpt) return ev::fromBool(false);

        auto cmds = wm->activate_workspace(id);
        dispatchCommands(cmds);

        ObjectBuilder evObj;
        evObj.set("type", "workspaceChanged");
        evObj.set("workspaceId", static_cast<double>(id));
        ev::Persistent wsP(workspaceViewToJs(*wsOpt));
        evObj.set("workspace", wsP.get());
        dispatchListenerEvent("workspaceChanged", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.createWorkspace(monitorId?, name?) -> number
    comp.def("createWorkspace", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromDouble(0.0);
        MonitorId monId = 1;
        if (reader.has(0) && reader.isNumber(0)) {
            monId = static_cast<MonitorId>(reader.getUint(0, 1));
        } else if (!wm->monitors().empty()) {
            monId = wm->monitors()[0].id;
        }
        std::string name = reader.isString(1) ? reader.getString(1) : "Workspace";
        WorkspaceId id = wm->add_workspace(monId, name);

        ObjectBuilder evObj;
        evObj.set("type", "workspaceChanged");
        evObj.set("workspaceId", static_cast<double>(id));
        dispatchListenerEvent("workspaceChanged", evObj.build());

        return ev::fromDouble(static_cast<double>(id));
    });

    // bro.compositor.removeWorkspace(id) -> boolean
    comp.def("removeWorkspace", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        auto cmds = wm->remove_workspace(id);
        dispatchCommands(cmds);

        ObjectBuilder evObj;
        evObj.set("type", "workspaceChanged");
        evObj.set("workspaceId", static_cast<double>(id));
        dispatchListenerEvent("workspaceChanged", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.moveWindowToWorkspace(windowId, workspaceId, follow?) -> boolean
    comp.def("moveWindowToWorkspace", 3, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0) || !reader.has(1)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WindowId wid = reader.getUint64(0);
        WorkspaceId wsid = static_cast<WorkspaceId>(reader.getUint(1));
        bool follow = reader.getBool(2, false);
        auto cmds = wm->move_window_to_workspace(wid, wsid, follow);
        dispatchCommands(cmds);
        return ev::fromBool(true);
    });

    // bro.compositor.setLayoutMode(workspaceId, "tiling" | "floating" | "columns" | "bsp" | "grid" | "master-stack") -> boolean
    comp.def("setLayoutMode", 2, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0) || !reader.isString(1)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        std::string modeStr = reader.getString(1);

        LayoutMode mode = LayoutMode::Floating;
        if (modeStr == "tiling" || modeStr == "columns") {
            mode = LayoutMode::Columns;
        } else if (modeStr == "bsp") {
            mode = LayoutMode::BSP;
        } else if (modeStr == "master-stack" || modeStr == "masterStack") {
            mode = LayoutMode::MasterStack;
        } else if (modeStr == "grid") {
            mode = LayoutMode::Grid;
        } else if (modeStr == "floating") {
            mode = LayoutMode::Floating;
        }

        auto cmds = wm->set_layout(id, mode);
        dispatchCommands(cmds);

        ObjectBuilder evObj;
        evObj.set("type", "layoutChanged");
        evObj.set("workspaceId", static_cast<double>(id));
        evObj.set("layout", to_string(mode));
        dispatchListenerEvent("layoutChanged", evObj.build());

        return ev::fromBool(true);
    });

    // bro.compositor.getLayoutMode(workspaceId) -> string
    comp.def("getLayoutMode", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromUtf8("floating");
        auto wm = activeWindowManager();
        if (!wm) return ev::fromUtf8("floating");
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        auto wsOpt = wm->workspace(id);
        if (!wsOpt) return ev::fromUtf8("floating");
        return ev::fromUtf8(to_string(wsOpt->layout));
    });

    // bro.compositor.relayout(workspaceId) -> boolean
    comp.def("relayout", 1, [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.has(0)) return ev::fromBool(false);
        auto wm = activeWindowManager();
        if (!wm) return ev::fromBool(false);
        WorkspaceId id = static_cast<WorkspaceId>(reader.getUint(0));
        auto cmds = wm->relayout(id);
        dispatchCommands(cmds);
        return ev::fromBool(true);
    });

    // bro.compositor.getMonitors() -> Array<MonitorInfo>
    comp.def("getMonitors", 0, [](Value, std::span<const Value>) -> Value {
        auto wm = activeWindowManager();
        if (!wm) return ev::makeArray(0);
        const auto& mons = wm->monitors();
        ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(mons.size())));
        for (uint32_t i = 0; i < mons.size(); ++i) {
            ev::Persistent item(monitorSnapshotToJs(mons[i]));
            ev::setElement(arr.get(), i, item.get());
        }
        return arr.get();
    });
}

} // namespace brocompositor::api
