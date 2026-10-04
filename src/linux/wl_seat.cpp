#include "wl_seat.h"
#include "wl_backend.h"
#include <cmath>

namespace brocompositor {

WlSeat::WlSeat() = default;

WlSeat::~WlSeat() {
    shutdown();
}

bool WlSeat::initialize(WlBackend& backend, const Config& config) {
    if (initialized_) return true;

    name_ = config.seat_name;
    capabilities_ = config.capabilities;
    cursor_theme_ = config.cursor_theme;
    cursor_size_ = config.cursor_size;
    swipe_threshold_ = config.swipe_threshold;

#if defined(BRO_HAS_WAYLAND)
    if (backend.get_display()) {
        wlr_seat_ = wlr_seat_create(backend.get_display(), name_.c_str());
        wlr_cursor_ = wlr_cursor_create();
        xcursor_mgr_ = wlr_xcursor_manager_create(cursor_theme_.c_str(), cursor_size_);

        uint32_t caps = 0;
        if ((capabilities_ & SeatCapability::Pointer) != SeatCapability::None) {
            caps |= WL_SEAT_CAPABILITY_POINTER;
        }
        if ((capabilities_ & SeatCapability::Keyboard) != SeatCapability::None) {
            caps |= WL_SEAT_CAPABILITY_KEYBOARD;
        }
        if ((capabilities_ & SeatCapability::Touch) != SeatCapability::None) {
            caps |= WL_SEAT_CAPABILITY_TOUCH;
        }
        if (wlr_seat_) {
            wlr_seat_set_capabilities(wlr_seat_, caps);
        }
    }
#else
    (void)backend;
    wlr_seat_ = reinterpret_cast<wlr_seat*>(static_cast<uintptr_t>(0x3000));
    wlr_cursor_ = reinterpret_cast<wlr_cursor*>(static_cast<uintptr_t>(0x3100));
    xcursor_mgr_ = reinterpret_cast<wlr_xcursor_manager*>(static_cast<uintptr_t>(0x3200));
#endif

    initialized_ = true;
    return true;
}

void WlSeat::shutdown() {
    if (!initialized_) return;

    clear_focus();

#if defined(BRO_HAS_WAYLAND)
    if (xcursor_mgr_) {
        wlr_xcursor_manager_destroy(xcursor_mgr_);
        xcursor_mgr_ = nullptr;
    }
    if (wlr_cursor_) {
        wlr_cursor_destroy(wlr_cursor_);
        wlr_cursor_ = nullptr;
    }
    if (wlr_seat_) {
        wlr_seat_destroy(wlr_seat_);
        wlr_seat_ = nullptr;
    }
#else
    xcursor_mgr_ = nullptr;
    wlr_cursor_ = nullptr;
    wlr_seat_ = nullptr;
#endif

    initialized_ = false;
}

Point WlSeat::get_cursor_position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {static_cast<int32_t>(std::round(cursor_x_)), static_cast<int32_t>(std::round(cursor_y_))};
}

void WlSeat::set_cursor_position(double x, double y) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cursor_x_ = x;
        cursor_y_ = y;
    }
    notify_pointer_motion(x, y, 0);
}

void WlSeat::set_cursor_shape(const std::string& shape_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    cursor_shape_ = shape_name;
#if defined(BRO_HAS_WAYLAND)
    if (xcursor_mgr_ && wlr_cursor_) {
        wlr_xcursor_manager_set_cursor_image(xcursor_mgr_, shape_name.c_str(), wlr_cursor_);
    }
#endif
}

void WlSeat::set_cursor_visible(bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    cursor_visible_ = visible;
}

void WlSeat::notify_pointer_motion(double x, double y, uint32_t /*time_ms*/) {
    PointerMotionCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cursor_x_ = x;
        cursor_y_ = y;
        cb = motion_cb_;
    }

#if defined(BRO_HAS_WAYLAND)
    if (wlr_cursor_) {
        wlr_cursor_warp_absolute(wlr_cursor_, nullptr, x, y);
    }
#endif

    if (cb) {
        cb(x, y);
    }
}

void WlSeat::notify_pointer_button(PointerButton button, ButtonState state, uint32_t /*time_ms*/) {
    PointerButtonCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = button_cb_;
    }

#if defined(BRO_HAS_WAYLAND)
    if (wlr_seat_) {
        wlr_seat_pointer_notify_button(
            wlr_seat_,
            0,
            static_cast<uint32_t>(button),
            state == ButtonState::Pressed ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED
        );
    }
#endif

    if (cb) {
        cb(button, state);
    }
}

void WlSeat::notify_pointer_axis(double dx, double dy, uint32_t /*time_ms*/) {
#if defined(BRO_HAS_WAYLAND)
    if (wlr_seat_) {
        if (dx != 0.0) {
            wlr_seat_pointer_notify_axis(wlr_seat_, 0, WLR_AXIS_ORIENTATION_HORIZONTAL, dx, 0, WLR_AXIS_SOURCE_WHEEL);
        }
        if (dy != 0.0) {
            wlr_seat_pointer_notify_axis(wlr_seat_, 0, WLR_AXIS_ORIENTATION_VERTICAL, dy, 0, WLR_AXIS_SOURCE_WHEEL);
        }
    }
#else
    (void)dx;
    (void)dy;
#endif
}

WindowId WlSeat::get_focused_window() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return focused_window_;
}

bool WlSeat::set_focus(WindowId id) {
    WindowId old_focus = InvalidWindowId;
    FocusCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (focused_window_ == id) {
            return true;
        }
        old_focus = focused_window_;
        focused_window_ = id;
        cb = focus_cb_;
    }

    if (cb) {
        cb(old_focus, id);
    }
    return true;
}

void WlSeat::clear_focus() {
    set_focus(InvalidWindowId);
}

void WlSeat::set_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    std::lock_guard<std::mutex> lock(mutex_);
    modifiers_ = depressed | latched | locked;
#if defined(BRO_HAS_WAYLAND)
    if (wlr_seat_) {
        wlr_seat_keyboard_notify_modifiers(wlr_seat_, depressed, latched, locked, group);
    }
#else
    (void)group;
#endif
}

void WlSeat::notify_key(uint32_t keycode, ButtonState state, uint32_t time_ms) {
#if defined(BRO_HAS_WAYLAND)
    if (wlr_seat_) {
        wlr_seat_keyboard_notify_key(
            wlr_seat_,
            time_ms,
            keycode,
            state == ButtonState::Pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
        );
    }
#else
    (void)keycode;
    (void)state;
    (void)time_ms;
#endif
}

void WlSeat::notify_swipe_begin(int fingers) {
    GestureCallback g_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        swipe_active_ = true;
        swipe_fingers_ = fingers;
        swipe_accum_dx_ = 0.0;
        swipe_accum_dy_ = 0.0;
        g_cb = gesture_cb_;
    }

    if (g_cb) {
        g_cb(GestureType::SwipeBegin, fingers, 0.0, 0.0, 1.0);
    }
}

void WlSeat::notify_swipe_update(double dx, double dy) {
    GestureCallback g_cb;
    int fingers = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!swipe_active_) return;
        swipe_accum_dx_ += dx;
        swipe_accum_dy_ += dy;
        fingers = swipe_fingers_;
        g_cb = gesture_cb_;
    }

    if (g_cb) {
        g_cb(GestureType::SwipeUpdate, fingers, dx, dy, 1.0);
    }
}

void WlSeat::notify_swipe_end(bool cancelled) {
    GestureCallback g_cb;
    SwipeActionCallback s_cb;
    int fingers = 0;
    double accum_x = 0.0;
    double accum_y = 0.0;
    double threshold = 0.0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!swipe_active_) return;
        swipe_active_ = false;
        fingers = swipe_fingers_;
        accum_x = swipe_accum_dx_;
        accum_y = swipe_accum_dy_;
        threshold = swipe_threshold_;
        g_cb = gesture_cb_;
        s_cb = swipe_action_cb_;
    }

    if (g_cb) {
        g_cb(GestureType::SwipeEnd, fingers, accum_x, accum_y, 1.0);
    }

    if (!cancelled && s_cb) {
        double abs_x = std::abs(accum_x);
        double abs_y = std::abs(accum_y);
        if (abs_x >= threshold || abs_y >= threshold) {
            if (abs_x >= abs_y) {
                Edge dir = (accum_x > 0) ? Edge::Right : Edge::Left;
                s_cb(fingers, dir);
            } else {
                Edge dir = (accum_y > 0) ? Edge::Bottom : Edge::Top;
                s_cb(fingers, dir);
            }
        }
    }
}

void WlSeat::notify_pinch_begin(int fingers) {
    GestureCallback g_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pinch_active_ = true;
        pinch_fingers_ = fingers;
        pinch_accum_scale_ = 1.0;
        g_cb = gesture_cb_;
    }
    if (g_cb) {
        g_cb(GestureType::PinchBegin, fingers, 0.0, 0.0, 1.0);
    }
}

void WlSeat::notify_pinch_update(double scale, double rotation, double dx, double dy) {
    GestureCallback g_cb;
    int fingers = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pinch_active_) return;
        pinch_accum_scale_ *= scale;
        fingers = pinch_fingers_;
        g_cb = gesture_cb_;
    }
    (void)rotation;
    if (g_cb) {
        g_cb(GestureType::PinchUpdate, fingers, dx, dy, scale);
    }
}

void WlSeat::notify_pinch_end(bool /*cancelled*/) {
    GestureCallback g_cb;
    int fingers = 0;
    double scale = 1.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pinch_active_) return;
        pinch_active_ = false;
        fingers = pinch_fingers_;
        scale = pinch_accum_scale_;
        g_cb = gesture_cb_;
    }
    if (g_cb) {
        g_cb(GestureType::PinchEnd, fingers, 0.0, 0.0, scale);
    }
}

void WlSeat::notify_hold_begin(int fingers) {
    GestureCallback g_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        g_cb = gesture_cb_;
    }
    if (g_cb) {
        g_cb(GestureType::HoldBegin, fingers, 0.0, 0.0, 1.0);
    }
}

void WlSeat::notify_hold_end(bool /*cancelled*/) {
    GestureCallback g_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        g_cb = gesture_cb_;
    }
    if (g_cb) {
        g_cb(GestureType::HoldEnd, 0, 0.0, 0.0, 1.0);
    }
}

void WlSeat::set_focus_callback(FocusCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    focus_cb_ = std::move(cb);
}

void WlSeat::set_pointer_motion_callback(PointerMotionCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    motion_cb_ = std::move(cb);
}

void WlSeat::set_pointer_button_callback(PointerButtonCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    button_cb_ = std::move(cb);
}

void WlSeat::set_gesture_callback(GestureCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    gesture_cb_ = std::move(cb);
}

void WlSeat::set_swipe_action_callback(SwipeActionCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    swipe_action_cb_ = std::move(cb);
}

} // namespace brocompositor
