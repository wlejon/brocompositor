#pragma once

#include "brocompositor/commands.h"
#include "brocompositor/event_queue.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
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

/// The page that used `bro.compositor` is going away while the compositor
/// stays (a host reloading its shell): drops its listeners and releases the
/// edge reservations it made, so the next page starts from the platform's
/// work area rather than stacking its own reservations on the old page's.
void resetCompositorScript();

/// Sets the WindowManager used by the API (if nullptr, a default WindowManager is used).
void setWindowManager(std::shared_ptr<brocompositor::WindowManager> wm);

/// Gets the WindowManager currently used by the API.
std::shared_ptr<brocompositor::WindowManager> getWindowManager();

/// Sets the EventQueue used by the API (if nullptr, a default EventQueue is used).
void setEventQueue(std::shared_ptr<brocompositor::EventQueue> queue);

/// Gets the EventQueue currently used by the API.
std::shared_ptr<brocompositor::EventQueue> getEventQueue();

/// Sets an optional command sink for executing commands emitted by WindowManager.
/// The sink returns how many commands the backend refused (a backend's
/// execute() result); calls whose commands were refused report failure to JS.
using CommandSink = std::function<size_t(const std::vector<brocompositor::Command>&)>;
void setCommandSink(CommandSink sink);

/// The host already hands every event it pushes into the API's EventQueue to
/// its WindowManager (and executes the commands that answer it), so the API
/// only reports the events to script and must not feed them a second time.
/// Off by default: the API feeds the window manager itself.
void setHostFeedsEvents(bool on);

/// Where the pointer is, in layout coordinates, for a beginMove / beginResize
/// called without {x, y} (a shell's title-bar handler). Returns false when
/// the host cannot say.
using PointerSource = std::function<bool(int32_t& x, int32_t& y)>;
void setPointerSource(PointerSource source);

/// Mints an xdg-activation token for a process the shell launches
/// (bro.compositor.activationToken): "" when the host cannot.
using ActivationTokenSource = std::function<std::string(const std::string& appId)>;
void setActivationTokenSource(ActivationTokenSource source);

/// A window's own icon (the backend's: xdg-toplevel-icon on Wayland), the
/// image closest to `size` px, for bro.compositor.getWindowIcon; nullopt
/// when it set none.
using WindowIconSource = std::function<std::optional<brocompositor::WindowIcon>(WindowId id, int32_t size)>;
void setWindowIconSource(WindowIconSource source);

} // namespace brocompositor::api

using brocompositor::api::installCompositor;
using brocompositor::api::tickCompositorAsync;
using brocompositor::api::shutdownCompositorAsync;
using brocompositor::api::setWindowManager;
using brocompositor::api::getWindowManager;
using brocompositor::api::setEventQueue;
using brocompositor::api::getEventQueue;
using brocompositor::api::setCommandSink;
using brocompositor::api::setHostFeedsEvents;
using brocompositor::api::setPointerSource;
using brocompositor::api::setActivationTokenSource;
using brocompositor::api::setWindowIconSource;
