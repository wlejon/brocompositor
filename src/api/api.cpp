#include "api.h"
#include "host_compositor_internal.h"
#include "object_builder.h"

#include <mutex>

namespace brocompositor::api {

namespace {

std::mutex g_state_mu;

std::shared_ptr<brocompositor::WindowManager> g_custom_wm;
std::shared_ptr<brocompositor::WindowManager> g_default_wm;

std::shared_ptr<brocompositor::EventQueue> g_custom_queue;
std::shared_ptr<brocompositor::EventQueue> g_default_queue;

CommandSink g_custom_command_sink;

void defaultExecuteCommands(const std::vector<brocompositor::Command>& cmds,
                            brocompositor::WindowManager& wm) {
    for (const auto& c : cmds) {
        if (auto* p = std::get_if<brocompositor::PlaceWindow>(&c)) {
            auto v = wm.window(p->id);
            if (v) {
                brocompositor::WindowSnapshot s = v->snapshot;
                s.frame = p->frame;
                wm.handle(brocompositor::WindowChanged{s, brocompositor::change::Geometry});
            }
        } else if (auto* f = std::get_if<brocompositor::FocusWindow>(&c)) {
            wm.handle(brocompositor::FocusChanged{f->id});
        } else if (auto* cw = std::get_if<brocompositor::CloseWindow>(&c)) {
            wm.handle(brocompositor::WindowRemoved{cw->id});
        }
    }
}


} // namespace

std::shared_ptr<brocompositor::WindowManager> activeWindowManager() {
    std::lock_guard lock(g_state_mu);
    if (g_custom_wm) return g_custom_wm;
    if (!g_default_wm) {
        g_default_wm = std::make_shared<brocompositor::WindowManager>();
    }
    return g_default_wm;
}

void setWindowManager(std::shared_ptr<brocompositor::WindowManager> wm) {
    std::lock_guard lock(g_state_mu);
    g_custom_wm = std::move(wm);
}

std::shared_ptr<brocompositor::WindowManager> getWindowManager() {
    return activeWindowManager();
}

std::shared_ptr<brocompositor::EventQueue> activeEventQueue() {
    std::lock_guard lock(g_state_mu);
    if (g_custom_queue) return g_custom_queue;
    if (!g_default_queue) {
        g_default_queue = std::make_shared<brocompositor::EventQueue>();
    }
    return g_default_queue;
}

void setEventQueue(std::shared_ptr<brocompositor::EventQueue> queue) {
    std::lock_guard lock(g_state_mu);
    g_custom_queue = std::move(queue);
}

std::shared_ptr<brocompositor::EventQueue> getEventQueue() {
    return activeEventQueue();
}

void setCommandSink(CommandSink sink) {
    std::lock_guard lock(g_state_mu);
    g_custom_command_sink = std::move(sink);
}

void dispatchCommands(const std::vector<brocompositor::Command>& cmds) {
    if (cmds.empty()) return;

    CommandSink sink;
    std::shared_ptr<brocompositor::WindowManager> wm;
    {
        std::lock_guard lock(g_state_mu);
        sink = g_custom_command_sink;
        wm = g_custom_wm ? g_custom_wm : g_default_wm;
    }

    if (sink) {
        sink(cmds);
    }
    if (wm) {
        defaultExecuteCommands(cmds, *wm);
    }
}

Value makeError(const std::string& msg) {
    ev::Persistent text(ev::fromUtf8(msg));
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) return r.value;
    }
    return text.get();
}

Value rectToJs(const brocompositor::Rect& r) {
    ObjectBuilder b;
    b.set("x", r.x);
    b.set("y", r.y);
    b.set("width", r.width);
    b.set("height", r.height);
    return b.build();
}

Value windowSnapshotToJs(const brocompositor::WindowSnapshot& snap, bool focused,
                         brocompositor::WorkspaceId ws, bool floating, bool tiled, bool shown) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(snap.id));
    b.set("owner", static_cast<double>(snap.owner));
    b.set("native", static_cast<double>(snap.native));
    b.set("processId", static_cast<double>(snap.process_id));
    b.set("process_id", static_cast<double>(snap.process_id));
    b.set("title", snap.title);
    b.set("appId", snap.app_id);
    b.set("app_id", snap.app_id);
    b.set("className", snap.class_name);
    b.set("class_name", snap.class_name);
    b.set("frame", rectToJs(snap.frame));
    b.set("x", snap.frame.x);
    b.set("y", snap.frame.y);
    b.set("width", snap.frame.width);
    b.set("height", snap.frame.height);
    b.set("monitor", static_cast<double>(snap.monitor));
    b.set("workspaceId", static_cast<double>(ws));
    b.set("workspace", static_cast<double>(ws));
    b.set("dpi", static_cast<double>(snap.dpi));
    b.set("minimized", snap.minimized);
    b.set("maximized", snap.maximized);
    b.set("fullscreen", snap.fullscreen);
    b.set("resizable", snap.resizable);
    b.set("floating", floating);
    b.set("tiled", tiled);
    b.set("shown", shown);
    b.set("visible", shown);
    b.set("focused", focused);
    return b.build();
}

Value windowViewToJs(const brocompositor::WindowView& view, bool focused) {
    return windowSnapshotToJs(view.snapshot, focused, view.workspace,
                              view.floating, view.tiled, view.shown);
}

Value workspaceViewToJs(const brocompositor::WorkspaceView& ws) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(ws.id));
    b.set("name", ws.name);
    b.set("monitor", static_cast<double>(ws.monitor));
    b.set("layout", to_string(ws.layout));
    b.set("layoutMode", to_string(ws.layout));
    b.set("active", ws.active);

    ev::Persistent winArr(ev::makeArray(static_cast<uint32_t>(ws.windows.size())));
    for (uint32_t i = 0; i < ws.windows.size(); ++i) {
        ev::setElement(winArr.get(), i, ev::fromDouble(static_cast<double>(ws.windows[i])));
    }
    b.set("windows", winArr.get());

    ev::Persistent focusArr(ev::makeArray(static_cast<uint32_t>(ws.focus_order.size())));
    for (uint32_t i = 0; i < ws.focus_order.size(); ++i) {
        ev::setElement(focusArr.get(), i, ev::fromDouble(static_cast<double>(ws.focus_order[i])));
    }
    b.set("focusOrder", focusArr.get());
    b.set("focus_order", focusArr.get());

    return b.build();
}

Value monitorSnapshotToJs(const brocompositor::MonitorSnapshot& mon) {
    ObjectBuilder b;
    b.set("id", static_cast<double>(mon.id));
    b.set("name", mon.name);
    b.set("bounds", rectToJs(mon.bounds));
    b.set("workArea", rectToJs(mon.work_area));
    b.set("work_area", rectToJs(mon.work_area));
    b.set("dpi", static_cast<double>(mon.dpi));
    b.set("scale", static_cast<double>(mon.dpi) / 96.0);
    b.set("primary", mon.primary);
    b.set("native", static_cast<double>(mon.native));
    return b.build();
}

Value ensureBroCompositor() {
    ev::Persistent globalThisVal;
    auto gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        globalThisVal.set(gt.value);
    }

    ev::Persistent broP;
    auto bro = ev::globalValue("bro");
    if (bro.found && ev::isObject(bro.value)) broP.set(bro.value);
    if (!ev::isObject(broP.get()) && ev::isObject(globalThisVal.get())) {
        Value candidate = ev::getProperty(globalThisVal.get(), "bro");
        if (ev::isObject(candidate)) broP.set(candidate);
    }
    if (!ev::isObject(broP.get())) {
        broP.set(ev::createObject());
        ev::registerGlobal("bro", broP.get());
        if (ev::isObject(globalThisVal.get())) {
            globalThisVal.set(ev::setProperty(globalThisVal.get(), "bro", broP.get()));
        }
    }

    ev::Persistent compP(ev::getProperty(broP.get(), "compositor"));
    if (!ev::isObject(compP.get())) {
        compP.set(ev::createObject());
        broP.set(ev::setProperty(broP.get(), "compositor", compP.get()));
    }
    return compP.get();
}

void installCompositor() {
    ev::Persistent compObj(ensureBroCompositor());
    ObjectBuilder comp(compObj.get());

    comp.def("available", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(true);
    });
    comp.def("isAvailable", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(true);
    });

    installWindowsOnto(compObj.get());
    installWorkspacesOnto(compObj.get());
    installEventsOnto(compObj.get());
}

void tickCompositorAsync() {
    drainCompositorEvents();
    if (ev::microtasksPending()) {
        ev::drainMicrotasks();
    }
}

void shutdownCompositorAsync() {
    clearCompositorListeners();
}

} // namespace brocompositor::api
