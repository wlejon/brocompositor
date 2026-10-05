// Accessibility (AX) helpers. Every function here is a synchronous call into
// the target application, served by that application's main thread: call
// them only on that application's worker (mac/app_worker.h), never on a
// thread anything else waits for.
//
// Messaging timeouts are per element: an element copied out of another
// (a window out of the application, a button out of a window) does not
// inherit its timeout but starts at the system default (measured ~1.5 s on
// macOS 26, documented as 6 s). Every element this backend keeps or calls
// gets its timeout set explicitly (set_timeout).
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
    bool closable = false;  // has a close button
    Rect frame;
    // The application no longer lists the window (AXWindows) but its element
    // still answers: the window is on a Space that is not active (another
    // desktop, or the desktop a fullscreen Space covers). A window that was
    // closed or ordered out answers kAXErrorInvalidUIElement instead.
    bool other_space = false;
};

void set_timeout(AXUIElementRef element, std::chrono::milliseconds timeout);
// The application element with a messaging timeout on every call through it.
Element application(uint32_t pid, std::chrono::milliseconds timeout);

// The application's windows on the active Space (and its minimized ones),
// each with `timeout` set.
std::vector<Element> windows(AXUIElementRef app, std::chrono::milliseconds timeout, AXError* error);
// The element still refers to a live UI element (one cheap attribute read).
AXError probe(AXUIElementRef element);
// The CGWindowID of the application's focused (key) window, 0 for none.
uint32_t focused_window(AXUIElementRef app, AXError* error);
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
AXError press_close_button(AXUIElementRef window, std::chrono::milliseconds timeout);

// The application did not answer within the messaging timeout (hung or
// busy), or is gone.
inline bool unresponsive(AXError e) { return e == kAXErrorCannotComplete; }
inline bool gone(AXError e) { return e == kAXErrorInvalidUIElement; }

}  // namespace brocompositor::mac::ax
