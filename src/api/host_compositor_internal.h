#pragma once

#include "embed/embed.h"
#include "brocompositor/window_manager.h"
#include "brocompositor/event_queue.h"
#include "brocompositor/commands.h"
#include "brocompositor/layout.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace brocompositor::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Service accessors and command dispatching
std::shared_ptr<brocompositor::WindowManager> activeWindowManager();
std::shared_ptr<brocompositor::EventQueue> activeEventQueue();
void dispatchCommands(const std::vector<brocompositor::Command>& cmds);

// Error helper
Value makeError(const std::string& msg);

// Conversions
Value rectToJs(const brocompositor::Rect& r);
Value windowSnapshotToJs(const brocompositor::WindowSnapshot& snap, bool focused = false,
                         brocompositor::WorkspaceId ws = brocompositor::kNoWorkspace,
                         bool floating = false, bool tiled = false, bool shown = true);
Value windowViewToJs(const brocompositor::WindowView& view, bool focused = false);
Value workspaceViewToJs(const brocompositor::WorkspaceView& ws);
Value monitorSnapshotToJs(const brocompositor::MonitorSnapshot& mon);

// Subsystem installers
void installWindowsOnto(Value compObj);
void installWorkspacesOnto(Value compObj);
void installEventsOnto(Value compObj);

// Event handling
void drainCompositorEvents();
void clearCompositorListeners();
void dispatchListenerEvent(const std::string& type, Value eventPayload);

} // namespace brocompositor::api
