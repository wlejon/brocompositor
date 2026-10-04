#pragma once

#include "wl_types.h"
#include <mutex>
#include <functional>

namespace brocompositor {

class WlBackend;

class WlSeat {
public:
    struct Config {
        std::string seat_name = "seat0";
        SeatCapability capabilities = SeatCapability::Pointer | SeatCapability::Keyboard | SeatCapability::Touch;
        std::string cursor_theme = "default";
        int32_t cursor_size = 24;
        double swipe_threshold = 40.0; // minimum pixels to trigger swipe gesture
    };

    using FocusCallback = std::function<void(WindowId old_focus, WindowId new_focus)>;
    using PointerMotionCallback = std::function<void(double x, double y)>;
    using PointerButtonCallback = std::function<void(PointerButton button, ButtonState state)>;
    using GestureCallback = std::function<void(GestureType type, int fingers, double dx, double dy, double scale)>;
    using SwipeActionCallback = std::function<void(int fingers, Edge direction)>;

    WlSeat();
    ~WlSeat();

    WlSeat(const WlSeat&) = delete;
    WlSeat& operator=(const WlSeat&) = delete;

    bool initialize(WlBackend& backend, const Config& config = {});
    void shutdown();
    bool is_initialized() const { return initialized_; }

    const std::string& get_seat_name() const { return name_; }
    SeatCapability get_capabilities() const { return capabilities_; }

    // Pointer & Cursor
    Point get_cursor_position() const;
    void set_cursor_position(double x, double y);
    void set_cursor_shape(const std::string& shape_name);
    void set_cursor_visible(bool visible);
    bool is_cursor_visible() const { return cursor_visible_; }

    void notify_pointer_motion(double x, double y, uint32_t time_ms = 0);
    void notify_pointer_button(PointerButton button, ButtonState state, uint32_t time_ms = 0);
    void notify_pointer_axis(double dx, double dy, uint32_t time_ms = 0);

    // Keyboard & Focus
    WindowId get_focused_window() const;
    bool set_focus(WindowId id);
    void clear_focus();

    uint32_t get_modifiers() const { return modifiers_; }
    void set_modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
    void notify_key(uint32_t keycode, ButtonState state, uint32_t time_ms = 0);

    // Touchpad Gestures
    void notify_swipe_begin(int fingers);
    void notify_swipe_update(double dx, double dy);
    void notify_swipe_end(bool cancelled);

    void notify_pinch_begin(int fingers);
    void notify_pinch_update(double scale, double rotation, double dx, double dy);
    void notify_pinch_end(bool cancelled);

    void notify_hold_begin(int fingers);
    void notify_hold_end(bool cancelled);

    // Callbacks
    void set_focus_callback(FocusCallback cb);
    void set_pointer_motion_callback(PointerMotionCallback cb);
    void set_pointer_button_callback(PointerButtonCallback cb);
    void set_gesture_callback(GestureCallback cb);
    void set_swipe_action_callback(SwipeActionCallback cb);

private:
    bool initialized_ = false;
    std::string name_ = "seat0";
    SeatCapability capabilities_ = SeatCapability::None;
    std::string cursor_theme_ = "default";
    int32_t cursor_size_ = 24;
    double swipe_threshold_ = 40.0;

    wlr_seat* wlr_seat_ = nullptr;
    wlr_cursor* wlr_cursor_ = nullptr;
    wlr_xcursor_manager* xcursor_mgr_ = nullptr;

    mutable std::mutex mutex_;
    double cursor_x_ = 0.0;
    double cursor_y_ = 0.0;
    bool cursor_visible_ = true;
    std::string cursor_shape_ = "default";

    WindowId focused_window_ = InvalidWindowId;
    uint32_t modifiers_ = 0;

    // Gesture tracking state
    bool swipe_active_ = false;
    int swipe_fingers_ = 0;
    double swipe_accum_dx_ = 0.0;
    double swipe_accum_dy_ = 0.0;

    bool pinch_active_ = false;
    int pinch_fingers_ = 0;
    double pinch_accum_scale_ = 1.0;

    FocusCallback focus_cb_;
    PointerMotionCallback motion_cb_;
    PointerButtonCallback button_cb_;
    GestureCallback gesture_cb_;
    SwipeActionCallback swipe_action_cb_;
};

} // namespace brocompositor
