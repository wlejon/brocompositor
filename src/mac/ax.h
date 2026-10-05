// Accessibility (AX) helpers. Every function here is a synchronous call into
// the target application, served by that application's main thread: call
// them only on that application's worker (mac/app_worker.h), never on a
// thread anything else waits for.
#pragma once

#include "brocompositor/geometry.h"
#include "mac/cf.h"

#include <ApplicationServices/ApplicationServices.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace brocompositor::mac::ax {

using Element = CFRef<AXUIElementRef>;

struct WindowInfo {
    uint32_t cgid = 0;  // CGWindowID (0: not a window-server window)
    std::string title;
    std::string subrole;  // AXStandardWindow, AXDialog, AXFloatingWindow, ...
    bool minimized = false;
    bool fullscreen = false;
    bool resizable = true;
    Rect frame;
};

// The application element with a messaging timeout on every call through it.
Element application(uint32_t pid, std::chrono::milliseconds timeout);

std::vector<Element> windows(AXUIElementRef app, AXError* error);
Element focused_window(AXUIElementRef app, AXError* error);
// The window server id of an AX window (private _AXUIElementGetWindow, looked
// up at run time; 0 when unavailable).
uint32_t window_id(AXUIElementRef window);
std::optional<WindowInfo> info(AXUIElementRef window, AXError* error);
std::optional<Rect> frame(AXUIElementRef window, AXError* error);

AXError set_position(AXUIElementRef window, Point p);
AXError set_size(AXUIElementRef window, Size s);
AXError set_bool(AXUIElementRef element, CFStringRef attribute, bool value);
std::optional<bool> get_bool(AXUIElementRef element, CFStringRef attribute, AXError* error);
AXError raise(AXUIElementRef window);
AXError press_close_button(AXUIElementRef window);

// The application did not answer within the messaging timeout (hung or
// busy), or is gone.
inline bool unresponsive(AXError e) { return e == kAXErrorCannotComplete; }
inline bool gone(AXError e) { return e == kAXErrorInvalidUIElement; }

}  // namespace brocompositor::mac::ax
