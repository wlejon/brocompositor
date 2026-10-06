#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/event_queue.h"

#include <functional>
#include <memory>
#include <vector>

namespace brocompositor {
class WindowManager;
}

namespace brocompositor::api {

/// Mounts `bro.compositor` in the current Bronze realm.
void installCompositor();

/// Drains native EventQueue and dispatches JS callbacks on the JS thread.
void tickCompositorAsync();

/// Clears active handlers and listeners.
void shutdownCompositorAsync();

/// Sets the WindowManager used by the API (if nullptr, a default WindowManager is used).
void setWindowManager(std::shared_ptr<brocompositor::WindowManager> wm);

/// Gets the WindowManager currently used by the API.
std::shared_ptr<brocompositor::WindowManager> getWindowManager();

/// Sets the EventQueue used by the API (if nullptr, a default EventQueue is used).
void setEventQueue(std::shared_ptr<brocompositor::EventQueue> queue);

/// Gets the EventQueue currently used by the API.
std::shared_ptr<brocompositor::EventQueue> getEventQueue();

/// Sets an optional command sink for executing commands emitted by WindowManager.
using CommandSink = std::function<void(const std::vector<brocompositor::Command>&)>;
void setCommandSink(CommandSink sink);

} // namespace brocompositor::api

using brocompositor::api::installCompositor;
using brocompositor::api::tickCompositorAsync;
using brocompositor::api::shutdownCompositorAsync;
using brocompositor::api::setWindowManager;
using brocompositor::api::getWindowManager;
using brocompositor::api::setEventQueue;
using brocompositor::api::getEventQueue;
using brocompositor::api::setCommandSink;
