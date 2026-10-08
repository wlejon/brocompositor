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
bool g_host_feeds_events = false;

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
        } else if (auto* st = std::get_if<brocompositor::SetWindowState>(&c)) {
            auto v = wm.window(st->id);
            if (v) {
                brocompositor::WindowSnapshot s = v->snapshot;
                using WS = brocompositor::WindowState;
                s.minimized = st->state == WS::Minimized;
                if (st->state != WS::Minimized) {
                    s.maximized = st->state == WS::Maximized;
                    s.fullscreen = st->state == WS::Fullscreen;
                }
                wm.handle(brocompositor::WindowChanged{s, brocompositor::change::State});
            }
        }
    }
}


} // namespace

std::shared_ptr<brocompositor::WindowManager> activeWindowManager() {
    std::lock_guard lock(g_state_mu);
    if (g_custom_wm) return g_custom_wm;
    if (!g_default_wm) {
        g_default_wm = std::make_shared<brocompositor::WindowManager>();
        brocompositor::MonitorSnapshot mon;
        mon.id = 1;
        mon.name = "default";
        mon.bounds = brocompositor::Rect{0, 0, 1920, 1080};
        mon.work_area = brocompositor::Rect{0, 0, 1920, 1080};
        mon.primary = true;
        mon.dpi = 96;
        g_default_wm->handle(brocompositor::MonitorsChanged{{mon}});
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

void setHostFeedsEvents(bool on) {
    std::lock_guard lock(g_state_mu);
    g_host_feeds_events = on;
}

bool hostFeedsEvents() {
    std::lock_guard lock(g_state_mu);
    return g_host_feeds_events;
}

size_t dispatchCommands(const std::vector<brocompositor::Command>& cmds) {
    if (cmds.empty()) return 0;

    CommandSink sink;
    std::shared_ptr<brocompositor::WindowManager> wm;
    {
        std::lock_guard lock(g_state_mu);
        sink = g_custom_command_sink;
        wm = g_custom_wm ? g_custom_wm : g_default_wm;
    }

    // With a backend, the window manager learns what happened from the
    // backend's own events, as each change lands. Echoing the commands as
    // facts as well would tell it the outcome before the client has acted:
    // the backend's next report (the window not maximized yet, a frame not
    // resized yet) would then read as the client undoing it, and the core
    // would drop what it keeps for the restore. The echo is the stand-in
    // for a backend, so it runs only without one.
    if (sink) return sink(cmds);
    if (wm) defaultExecuteCommands(cmds, *wm);
    return 0;
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
    b.set("decorated", snap.decorated);
    b.set("floating", floating);
    b.set("tiled", tiled);
    b.set("shown", shown);
    b.set("visible", shown);
    b.set("focused", focused);
    return b.build();
}

Value windowViewToJs(const brocompositor::WindowView& view, bool focused) {
    ev::Persistent obj(windowSnapshotToJs(view.snapshot, focused, view.workspace,
                                          view.floating, view.tiled, view.shown));
    ObjectBuilder b(obj.get());
    b.set("snap", to_string(view.snap));
    ev::Persistent outer(rectToJs(view.outer));
    b.set("outerFrame", outer.get());
    ObjectBuilder deco;
    deco.set("top", view.decoration.top);
    deco.set("left", view.decoration.left);
    deco.set("right", view.decoration.right);
    deco.set("bottom", view.decoration.bottom);
    b.set("decoration", deco.build());
    b.set("framed", view.framed);
    b.set("borderless", view.framed && view.decoration == brocompositor::Margins{});
    return b.obj.get();
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

    comp.set("available", true);
    comp.def("isAvailable", 0, [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(true);
    });

    installWindowsOnto(compObj.get());
    installWorkspacesOnto(compObj.get());
    installEventsOnto(compObj.get());
    installPolicyOnto(compObj.get());
    installInteractionOnto(compObj.get());
}

void tickCompositorAsync() {
    drainCompositorEvents();
    dispatchInteractionChanges();
    if (ev::microtasksPending()) {
        ev::drainMicrotasks();
    }
}

void shutdownCompositorAsync() {
    clearCompositorListeners();
}

void resetCompositorScript() {
    clearCompositorListeners();
    releaseScriptReservations();
}

} // namespace brocompositor::api
