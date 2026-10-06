#include "linux/drm/drm_input.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace brocompositor::wl::drm {

namespace {

int open_restricted(const char* path, int /*flags*/, void* user_data) {
    return static_cast<DrmInputBackend*>(user_data)->open_device(path);
}

void close_restricted(int fd, void* user_data) {
    static_cast<DrmInputBackend*>(user_data)->close_device(fd);
}

const struct libinput_interface kLibinputInterface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

const struct wlr_keyboard_impl kKeyboardImpl = {
    .name = "drm-keyboard",
    .led_update = nullptr,
};

const struct wlr_pointer_impl kPointerImpl = {
    .name = "drm-pointer",
};

const struct wlr_touch_impl kTouchImpl = {
    .name = "drm-touch",
};

uint32_t usec_to_msec(uint64_t usec) {
    return static_cast<uint32_t>(usec / 1000);
}

}  // namespace

DrmInputBackend::DrmInputBackend(wlr_backend* backend, broseat::Seat* seat)
    : backend_(backend), seat_(seat) {}

DrmInputBackend::~DrmInputBackend() {
    stop();
}

int DrmInputBackend::open_device(const char* path) {
    if (seat_) {
        std::string err;
        auto dev = seat_->open_device(path, &err);
        if (dev && dev->is_valid()) {
            int fd = dev->fd();
            int dev_id = dev->device_id();
            fd_to_dev_id_[fd] = dev_id;
            dev->release_fd();
            return fd;
        }
    }
    return ::open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
}

void DrmInputBackend::close_device(int fd) {
    auto it = fd_to_dev_id_.find(fd);
    if (it != fd_to_dev_id_.end()) {
        int dev_id = it->second;
        fd_to_dev_id_.erase(it);
        if (seat_) {
            seat_->close_device(dev_id);
            return;
        }
    }
    ::close(fd);
}

bool DrmInputBackend::start(wl_event_loop* loop) {
    loop_ = loop;
    udev_ = udev_new();
    if (!udev_) return false;

    li_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
    if (!li_) {
        udev_unref(udev_);
        udev_ = nullptr;
        return false;
    }

    std::string seat_name = seat_ ? seat_->seat_name() : "seat0";
    if (libinput_udev_assign_seat(li_, seat_name.c_str()) != 0) {
        libinput_unref(li_);
        li_ = nullptr;
        udev_unref(udev_);
        udev_ = nullptr;
        return false;
    }

    int fd = libinput_get_fd(li_);
    li_source_ = wl_event_loop_add_fd(
        loop_, fd, WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            static_cast<DrmInputBackend*>(data)->dispatch();
            return 0;
        },
        this);

    dispatch();
    return true;
}

void DrmInputBackend::stop() {
    if (li_source_) {
        wl_event_source_remove(li_source_);
        li_source_ = nullptr;
    }

    for (auto& [handle, list] : devices_) {
        for (auto& d : list) {
            wl_signal_emit_mutable(&d->base.events.destroy, &d->base);
        }
    }
    devices_.clear();

    if (li_) {
        libinput_unref(li_);
        li_ = nullptr;
    }
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
}

void DrmInputBackend::suspend() {
    if (li_) libinput_suspend(li_);
}

void DrmInputBackend::resume() {
    if (li_) {
        libinput_resume(li_);
        dispatch();
    }
}

void DrmInputBackend::dispatch() {
    if (!li_) return;
    int ret = libinput_dispatch(li_);
    if (ret != 0) return;

    while (struct libinput_event* ev = libinput_get_event(li_)) {
        process_event(ev);
        libinput_event_destroy(ev);
    }
}

void DrmInputBackend::add_device(struct libinput_device* dev) {
    const char* name = libinput_device_get_name(dev);
    std::vector<std::unique_ptr<DrmInputDevice>> list;

    if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        auto d = std::make_unique<DrmInputDevice>();
        d->handle = dev;
        d->is_keyboard = true;
        d->base.type = WLR_INPUT_DEVICE_KEYBOARD;
        wlr_keyboard_init(&d->keyboard, &kKeyboardImpl, name);
        wl_signal_emit_mutable(&backend_->events.new_input, &d->base);
        list.push_back(std::move(d));
    }

    if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
        auto d = std::make_unique<DrmInputDevice>();
        d->handle = dev;
        d->is_pointer = true;
        d->base.type = WLR_INPUT_DEVICE_POINTER;
        wlr_pointer_init(&d->pointer, &kPointerImpl, name);
        wl_signal_emit_mutable(&backend_->events.new_input, &d->base);
        list.push_back(std::move(d));
    }

    if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
        auto d = std::make_unique<DrmInputDevice>();
        d->handle = dev;
        d->is_touch = true;
        d->base.type = WLR_INPUT_DEVICE_TOUCH;
        wlr_touch_init(&d->touch, &kTouchImpl, name);
        wl_signal_emit_mutable(&backend_->events.new_input, &d->base);
        list.push_back(std::move(d));
    }

    if (!list.empty()) {
        devices_[dev] = std::move(list);
    }
}

void DrmInputBackend::remove_device(struct libinput_device* dev) {
    auto it = devices_.find(dev);
    if (it != devices_.end()) {
        for (auto& d : it->second) {
            wl_signal_emit_mutable(&d->base.events.destroy, &d->base);
        }
        devices_.erase(it);
    }
}

void DrmInputBackend::process_event(struct libinput_event* ev) {
    enum libinput_event_type type = libinput_event_get_type(ev);
    struct libinput_device* dev = libinput_event_get_device(ev);

    switch (type) {
        case LIBINPUT_EVENT_DEVICE_ADDED: add_device(dev); break;
        case LIBINPUT_EVENT_DEVICE_REMOVED: remove_device(dev); break;
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            auto it = devices_.find(dev);
            if (it == devices_.end()) break;
            for (auto& d : it->second) {
                if (!d->is_keyboard) continue;
                auto* ke = libinput_event_get_keyboard_event(ev);
                wlr_keyboard_key_event we{};
                we.time_msec = usec_to_msec(libinput_event_keyboard_get_time_usec(ke));
                we.keycode = libinput_event_keyboard_get_key(ke);
                we.update_state = true;
                we.state = (libinput_event_keyboard_get_key_state(ke) == LIBINPUT_KEY_STATE_PRESSED)
                               ? WL_KEYBOARD_KEY_STATE_PRESSED
                               : WL_KEYBOARD_KEY_STATE_RELEASED;
                wlr_keyboard_notify_key(&d->keyboard, &we);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION: {
            auto it = devices_.find(dev);
            if (it == devices_.end()) break;
            for (auto& d : it->second) {
                if (!d->is_pointer) continue;
                auto* pe = libinput_event_get_pointer_event(ev);
                wlr_pointer_motion_event we{};
                we.pointer = &d->pointer;
                we.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pe));
                we.delta_x = libinput_event_pointer_get_dx(pe);
                we.delta_y = libinput_event_pointer_get_dy(pe);
                we.unaccel_dx = libinput_event_pointer_get_dx_unaccelerated(pe);
                we.unaccel_dy = libinput_event_pointer_get_dy_unaccelerated(pe);
                wl_signal_emit_mutable(&d->pointer.events.motion, &we);
                wl_signal_emit_mutable(&d->pointer.events.frame, &d->pointer);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            auto it = devices_.find(dev);
            if (it == devices_.end()) break;
            for (auto& d : it->second) {
                if (!d->is_pointer) continue;
                auto* pe = libinput_event_get_pointer_event(ev);
                wlr_pointer_motion_absolute_event we{};
                we.pointer = &d->pointer;
                we.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pe));
                we.x = libinput_event_pointer_get_absolute_x_transformed(pe, 1);
                we.y = libinput_event_pointer_get_absolute_y_transformed(pe, 1);
                wl_signal_emit_mutable(&d->pointer.events.motion_absolute, &we);
                wl_signal_emit_mutable(&d->pointer.events.frame, &d->pointer);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            auto it = devices_.find(dev);
            if (it == devices_.end()) break;
            for (auto& d : it->second) {
                if (!d->is_pointer) continue;
                auto* pe = libinput_event_get_pointer_event(ev);
                wlr_pointer_button_event we{};
                we.pointer = &d->pointer;
                we.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pe));
                we.button = libinput_event_pointer_get_button(pe);
                we.state = (libinput_event_pointer_get_button_state(pe) == LIBINPUT_BUTTON_STATE_PRESSED)
                               ? WL_POINTER_BUTTON_STATE_PRESSED
                               : WL_POINTER_BUTTON_STATE_RELEASED;
                wl_signal_emit_mutable(&d->pointer.events.button, &we);
                wl_signal_emit_mutable(&d->pointer.events.frame, &d->pointer);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_AXIS: {
            auto it = devices_.find(dev);
            if (it == devices_.end()) break;
            for (auto& d : it->second) {
                if (!d->is_pointer) continue;
                auto* pe = libinput_event_get_pointer_event(ev);
                for (auto orientation : {LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                                         LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL}) {
                    if (!libinput_event_pointer_has_axis(pe, orientation)) continue;
                    wlr_pointer_axis_event we{};
                    we.pointer = &d->pointer;
                    we.time_msec = usec_to_msec(libinput_event_pointer_get_time_usec(pe));
                    we.orientation = (orientation == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
                                         ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                         : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
                    we.delta = libinput_event_pointer_get_axis_value(pe, orientation);
                    we.delta_discrete = static_cast<int32_t>(
                        libinput_event_pointer_get_axis_value_discrete(pe, orientation) * 120);
                    we.source = (libinput_event_pointer_get_axis_source(pe) == LIBINPUT_POINTER_AXIS_SOURCE_WHEEL)
                                    ? WL_POINTER_AXIS_SOURCE_WHEEL
                                    : WL_POINTER_AXIS_SOURCE_FINGER;
                    we.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL;
                    wl_signal_emit_mutable(&d->pointer.events.axis, &we);
                }
                wl_signal_emit_mutable(&d->pointer.events.frame, &d->pointer);
            }
            break;
        }
        default: break;
    }
}

}  // namespace brocompositor::wl::drm
