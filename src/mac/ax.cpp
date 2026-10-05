#include "mac/ax.h"

#include <dlfcn.h>

#include <cmath>

namespace brocompositor::mac::ax {

namespace {

const CFStringRef kFullScreen = CFSTR("AXFullScreen");

template <class T>
CFRef<T> copy_attr(AXUIElementRef e, CFStringRef attr, AXError* error) {
    CFRef<T> out;
    AXError err = AXUIElementCopyAttributeValue(e, attr, out.out_type());
    if (error) *error = err;
    return err == kAXErrorSuccess ? out : CFRef<T>();
}

}  // namespace

Element application(uint32_t pid, std::chrono::milliseconds timeout) {
    Element app(AXUIElementCreateApplication(pid_t(pid)));
    if (app) AXUIElementSetMessagingTimeout(app.get(), float(timeout.count()) / 1000.0f);
    return app;
}

std::vector<Element> windows(AXUIElementRef app, AXError* error) {
    std::vector<Element> out;
    auto list = copy_attr<CFArrayRef>(app, kAXWindowsAttribute, error);
    if (!list || CFGetTypeID(list.get()) != CFArrayGetTypeID()) return out;
    for (CFIndex i = 0, n = CFArrayGetCount(list.get()); i < n; ++i)
        out.push_back(Element::retain(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(list.get(), i))));
    return out;
}

Element focused_window(AXUIElementRef app, AXError* error) {
    return copy_attr<AXUIElementRef>(app, kAXFocusedWindowAttribute, error);
}

uint32_t window_id(AXUIElementRef window) {
    using Fn = AXError (*)(AXUIElementRef, CGWindowID*);
    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "_AXUIElementGetWindow"));
    CGWindowID id = 0;
    if (!fn || fn(window, &id) != kAXErrorSuccess) return 0;
    return id;
}

std::optional<Rect> frame(AXUIElementRef window, AXError* error) {
    auto pos = copy_attr<AXValueRef>(window, kAXPositionAttribute, error);
    if (!pos) return std::nullopt;
    auto size = copy_attr<AXValueRef>(window, kAXSizeAttribute, error);
    if (!size) return std::nullopt;
    CGPoint p{};
    CGSize s{};
    if (!AXValueGetValue(pos.get(), kAXValueTypeCGPoint, &p) || !AXValueGetValue(size.get(), kAXValueTypeCGSize, &s))
        return std::nullopt;
    int32_t x = int32_t(std::lround(p.x)), y = int32_t(std::lround(p.y));
    return Rect{x, y, int32_t(std::lround(s.width)), int32_t(std::lround(s.height))};
}

std::optional<bool> get_bool(AXUIElementRef element, CFStringRef attribute, AXError* error) {
    auto v = copy_attr<CFBooleanRef>(element, attribute, error);
    if (!v || CFGetTypeID(v.get()) != CFBooleanGetTypeID()) return std::nullopt;
    return CFBooleanGetValue(v.get()) != 0;
}

std::optional<WindowInfo> info(AXUIElementRef window, AXError* error) {
    WindowInfo w;
    AXError err = kAXErrorSuccess;
    auto f = frame(window, &err);
    if (error) *error = err;
    if (!f) return std::nullopt;  // the first call tells whether the app answers at all
    w.frame = *f;
    w.cgid = window_id(window);
    if (auto t = copy_attr<CFStringRef>(window, kAXTitleAttribute, nullptr)) w.title = to_string(t.get());
    if (auto s = copy_attr<CFStringRef>(window, kAXSubroleAttribute, nullptr)) w.subrole = to_string(s.get());
    w.minimized = get_bool(window, kAXMinimizedAttribute, nullptr).value_or(false);
    w.fullscreen = get_bool(window, kFullScreen, nullptr).value_or(false);
    Boolean settable = false;
    if (AXUIElementIsAttributeSettable(window, kAXSizeAttribute, &settable) == kAXErrorSuccess) w.resizable = settable;
    return w;
}

AXError set_position(AXUIElementRef window, Point p) {
    CGPoint cp{double(p.x), double(p.y)};
    CFRef<AXValueRef> v(AXValueCreate(kAXValueTypeCGPoint, &cp));
    return AXUIElementSetAttributeValue(window, kAXPositionAttribute, v.get());
}

AXError set_size(AXUIElementRef window, Size s) {
    CGSize cs{double(s.width), double(s.height)};
    CFRef<AXValueRef> v(AXValueCreate(kAXValueTypeCGSize, &cs));
    return AXUIElementSetAttributeValue(window, kAXSizeAttribute, v.get());
}

AXError set_bool(AXUIElementRef element, CFStringRef attribute, bool value) {
    return AXUIElementSetAttributeValue(element, attribute, value ? kCFBooleanTrue : kCFBooleanFalse);
}

AXError raise(AXUIElementRef window) { return AXUIElementPerformAction(window, kAXRaiseAction); }

AXError press_close_button(AXUIElementRef window) {
    AXError err = kAXErrorSuccess;
    auto button = copy_attr<AXUIElementRef>(window, kAXCloseButtonAttribute, &err);
    if (!button) return err == kAXErrorSuccess ? kAXErrorAttributeUnsupported : err;
    // Elements do not inherit a messaging timeout; the press goes to the
    // same application the window call just answered for.
    AXUIElementSetMessagingTimeout(button.get(), 1.0f);
    return AXUIElementPerformAction(button.get(), kAXPressAction);
}

}  // namespace brocompositor::mac::ax
