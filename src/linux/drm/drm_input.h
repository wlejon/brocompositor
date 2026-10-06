#pragma once

#include "broseat/seat.h"
#include "linux/wlr.h"

#include <libinput.h>
#include <libudev.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brocompositor::wl::drm {

class DrmBackend;

struct DrmInputDevice {
    wlr_input_device base{};
    struct libinput_device* handle = nullptr;
    union {
        wlr_keyboard keyboard;
        wlr_pointer pointer;
        wlr_touch touch;
        wlr_tablet tablet;
        wlr_tablet_pad pad;
    };
    bool is_keyboard = false;
    bool is_pointer = false;
    bool is_touch = false;
    bool is_tablet = false;
    bool is_pad = false;
};

class DrmInputBackend {
public:
    DrmInputBackend(wlr_backend* backend, broseat::Seat* seat);
    ~DrmInputBackend();

    DrmInputBackend(const DrmInputBackend&) = delete;
    DrmInputBackend& operator=(const DrmInputBackend&) = delete;

    bool start(wl_event_loop* loop);
    void stop();

    void suspend();
    void resume();

    int open_device(const char* path);
    void close_device(int fd);

private:
    wlr_backend* backend_ = nullptr;
    broseat::Seat* seat_ = nullptr;
    wl_event_loop* loop_ = nullptr;
    struct libinput* li_ = nullptr;
    struct udev* udev_ = nullptr;
    wl_event_source* li_source_ = nullptr;

    std::unordered_map<int, int> fd_to_dev_id_;
    std::unordered_map<struct libinput_device*, std::vector<std::unique_ptr<DrmInputDevice>>> devices_;

    void dispatch();
    void process_event(struct libinput_event* ev);
    void add_device(struct libinput_device* dev);
    void remove_device(struct libinput_device* dev);
};

}  // namespace brocompositor::wl::drm
