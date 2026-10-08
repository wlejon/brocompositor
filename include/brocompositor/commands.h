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

// A window's display state.
enum class WindowState : uint32_t {
    Normal = 0,      // neither minimized, maximized nor fullscreen
    Minimized = 1,   // iconified: not drawn, not focusable until restored
    Maximized = 2,   // fills its monitor's work area
    Fullscreen = 3,  // covers its whole monitor without decoration
};

inline const char* to_string(WindowState s) {
    switch (s) {
        case WindowState::Normal: return "normal";
        case WindowState::Minimized: return "minimized";
        case WindowState::Maximized: return "maximized";
        case WindowState::Fullscreen: return "fullscreen";
    }
    return "normal";
}

// Put a window into a state. The core pairs it with the PlaceWindow that
// gives the state its geometry (the work area for Maximized, the monitor for
// Fullscreen, the remembered frame when leaving either), sent after this
// command. Minimized also hides the window; every other state un-minimizes
// it and shows it again if it was minimized. A backend that cannot put
// windows into states refuses the command: execute() returns false.
struct SetWindowState {
    WindowId id = kNoWindow;
    WindowState state = WindowState::Normal;
};

using Command = std::variant<PlaceWindow, SetWindowVisible, FocusWindow, CloseWindow, SetWindowState>;

}  // namespace brocompositor
