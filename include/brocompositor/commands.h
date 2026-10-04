// What the policy core asks a backend to do. Commands are values; the host
// hands them to whichever backend it runs (Windows shell backend now, the
// Wayland server later). A backend that cannot honour a command reports
// failure from execute() and the next facts it emits describe what actually
// happened, so the core never has to trust that a command succeeded.
#pragma once

#include "brocompositor/events.h"

#include <variant>

namespace brocompositor {

// Place the window's visible frame (same space as WindowSnapshot::frame).
// Backends restore a maximized/minimized window before placing it.
struct PlaceWindow {
    WindowId id = kNoWindow;
    Rect frame;
};

// Show or hide a window for workspace switching. How a backend hides is its
// own business (Windows parks it off-screen by default so capture stays live;
// a Wayland server simply stops drawing it).
struct SetWindowVisible {
    WindowId id = kNoWindow;
    bool visible = true;
};

// Make the window the active, keyboard-focused window. id == kNoWindow means
// "focus no managed window" (issued when the focused window was hidden and the
// new workspace is empty); the backend moves focus to a neutral target.
struct FocusWindow {
    WindowId id = kNoWindow;
};

// Politely ask the window to close (WM_CLOSE / xdg_toplevel.close).
struct CloseWindow {
    WindowId id = kNoWindow;
};

using Command = std::variant<PlaceWindow, SetWindowVisible, FocusWindow, CloseWindow>;

}  // namespace brocompositor
