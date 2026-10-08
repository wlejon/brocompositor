#include "host_compositor_internal.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <mutex>
#include <vector>

namespace brocompositor::api {

namespace {

std::mutex g_comp_mu;

struct CompositorListener {
    uint64_t id = 0;
    std::string event_type;
    std::shared_ptr<ev::Persistent> callback;
};

uint64_t g_next_listener_id = 1;
std::vector<CompositorListener> g_listeners;

} // namespace

void dispatchListenerEvent(const std::string& type, Value eventPayload) {
    std::vector<std::shared_ptr<ev::Persistent>> targets;
    {
        std::lock_guard lock(g_comp_mu);
        for (const auto& l : g_listeners) {
            if (l.event_type == type || l.event_type == "*" ||
                (type == "windowCreated" && l.event_type == "windowAdded") ||
                (type == "windowAdded" && l.event_type == "windowCreated") ||
                (type == "windowClosed" && l.event_type == "windowRemoved") ||
                (type == "windowRemoved" && l.event_type == "windowClosed") ||
                (type == "focusChanged" && l.event_type == "focus") ||
                (type == "workspaceChanged" && l.event_type == "workspace") ||
                (type == "layoutChanged" && l.event_type == "layout")) {
                targets.push_back(l.callback);
            }
        }
    }

    ev::Persistent payloadP(eventPayload);
    for (const auto& cb : targets) {
        if (cb && ev::isFunction(cb->get())) {
            Value arg = payloadP.get();
            ev::call(cb->get(), ev::undefined(), std::span<const Value>(&arg, 1));
        }
    }
}

void drainCompositorEvents() {
    auto q = activeEventQueue();
    auto wm = activeWindowManager();
    if (!q || !wm) return;

    auto events = q->drain();
    const bool fed = hostFeedsEvents();
    for (const auto& evItem : events) {
        if (!fed) dispatchCommands(wm->handle(evItem));

        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, WindowAdded>) {
                ObjectBuilder obj;
                obj.set("type", "windowCreated");
                obj.set("id", static_cast<double>(e.window.id));
                obj.set("windowId", static_cast<double>(e.window.id));
                auto view = wm->window(e.window.id);
                ev::Persistent winP(view ? windowViewToJs(*view, e.window.id == wm->focused())
                                         : windowSnapshotToJs(e.window, e.window.id == wm->focused()));
                obj.set("window", winP.get());
                dispatchListenerEvent("windowCreated", obj.build());
                dispatchListenerEvent("windowAdded", obj.build());
            } else if constexpr (std::is_same_v<T, WindowRemoved>) {
                ObjectBuilder obj;
                obj.set("type", "windowClosed");
                obj.set("id", static_cast<double>(e.id));
                obj.set("windowId", static_cast<double>(e.id));
                dispatchListenerEvent("windowClosed", obj.build());
                dispatchListenerEvent("windowRemoved", obj.build());
            } else if constexpr (std::is_same_v<T, WindowChanged>) {
                ObjectBuilder obj;
                obj.set("type", "windowChanged");
                obj.set("id", static_cast<double>(e.window.id));
                obj.set("windowId", static_cast<double>(e.window.id));
                obj.set("changes", static_cast<double>(e.changes));
                auto view = wm->window(e.window.id);
                ev::Persistent winP(view ? windowViewToJs(*view, e.window.id == wm->focused())
                                         : windowSnapshotToJs(e.window, e.window.id == wm->focused()));
                obj.set("window", winP.get());
                dispatchListenerEvent("windowChanged", obj.build());
            } else if constexpr (std::is_same_v<T, FocusChanged>) {
                ObjectBuilder obj;
                obj.set("type", "focusChanged");
                obj.set("id", static_cast<double>(e.id));
                obj.set("windowId", static_cast<double>(e.id));
                dispatchListenerEvent("focusChanged", obj.build());
            } else if constexpr (std::is_same_v<T, MonitorsChanged>) {
                ObjectBuilder obj;
                obj.set("type", "monitorsChanged");
                // The manager's view: work areas exclude shell reservations.
                const auto& mons = wm->monitors();
                ev::Persistent arr(ev::makeArray(static_cast<uint32_t>(mons.size())));
                for (uint32_t i = 0; i < mons.size(); ++i) {
                    ev::Persistent m(monitorSnapshotToJs(mons[i]));
                    ev::setElement(arr.get(), i, m.get());
                }
                obj.set("monitors", arr.get());
                dispatchListenerEvent("monitorsChanged", obj.build());
                dispatchShellReservations();
            } else if constexpr (std::is_same_v<T, MoveSizeStarted>) {
                ObjectBuilder obj;
                obj.set("type", "moveSizeStarted");
                obj.set("id", static_cast<double>(e.id));
                dispatchListenerEvent("moveSizeStarted", obj.build());
            } else if constexpr (std::is_same_v<T, MoveSizeEnded>) {
                ObjectBuilder obj;
                obj.set("type", "moveSizeEnded");
                obj.set("id", static_cast<double>(e.id));
                dispatchListenerEvent("moveSizeEnded", obj.build());
            } else if constexpr (std::is_same_v<T, ReservationChanged>) {
                ObjectBuilder obj;
                obj.set("type", "reservationChanged");
                obj.set("id", static_cast<double>(e.id));
                obj.set("monitor", static_cast<double>(e.monitor));
                ev::Persistent rP(rectToJs(e.rect));
                obj.set("rect", rP.get());
                obj.set("shell", false);
                dispatchListenerEvent("reservationChanged", obj.build());
            }
        }, evItem);
    }
}

void clearCompositorListeners() {
    std::lock_guard lock(g_comp_mu);
    g_listeners.clear();
}

void installEventsOnto(Value compObj) {
    ObjectBuilder comp(compObj);

    // bro.compositor.on(event, callback) -> ListenerToken
    auto onFn = [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (!reader.isString(0) || !reader.isFunction(1)) {
            return ev::fromDouble(0.0);
        }

        std::string event = reader.getString(0);
        uint64_t id = 0;
        {
            std::lock_guard lock(g_comp_mu);
            id = g_next_listener_id++;
            CompositorListener l;
            l.id = id;
            l.event_type = event;
            l.callback = std::make_shared<ev::Persistent>(reader.get(1));
            g_listeners.push_back(std::move(l));
        }

        ObjectBuilder handle;
        handle.set("id", static_cast<double>(id));
        handle.set("event", event);
        handle.def("remove", 0, [id](Value, std::span<const Value>) -> Value {
            std::lock_guard lock(g_comp_mu);
            std::erase_if(g_listeners, [id](const auto& l) { return l.id == id; });
            return ev::fromBool(true);
        });
        return handle.build();
    };

    comp.def("on", 2, onFn);
    comp.def("addEventListener", 2, onFn);
    comp.def("addListener", 2, onFn);

    // bro.compositor.off(eventOrHandle, callback?) -> boolean
    auto offFn = [](Value, std::span<const Value> args) -> Value {
        ArgReader reader(args);
        if (reader.count() == 0) return ev::fromBool(false);

        std::lock_guard lock(g_comp_mu);
        if (reader.isObject(0)) {
            ev::Persistent objP(reader.get(0));
            double id = ArgReader::getPropDouble(objP.get(), "id", 0.0);
            if (id > 0) {
                uint64_t targetId = static_cast<uint64_t>(id);
                size_t removed = std::erase_if(g_listeners, [targetId](const auto& l) {
                    return l.id == targetId;
                });
                return ev::fromBool(removed > 0);
            }
        }

        if (reader.isString(0)) {
            std::string event = reader.getString(0);
            if (reader.isFunction(1)) {
                Value targetCb = reader.get(1);
                size_t removed = std::erase_if(g_listeners, [&](const auto& l) {
                    return l.event_type == event && l.callback && (l.callback->get() == targetCb);
                });
                return ev::fromBool(removed > 0);
            }
            size_t removed = std::erase_if(g_listeners, [&](const auto& l) {
                return l.event_type == event;
            });
            return ev::fromBool(removed > 0);
        }

        return ev::fromBool(false);
    };

    comp.def("off", 1, offFn);
    comp.def("removeEventListener", 1, offFn);
    comp.def("removeListener", 1, offFn);
}

} // namespace brocompositor::api
