#pragma once

#include "brocompositor/linux/server.h"
#include "linux/drm/drm_device.h"
#include "linux/drm/drm_input.h"
#include "linux/drm/drm_output.h"
#include "linux/wlr.h"

#include "brodmabuf/kms.h"
#include "broseat/seat.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brocompositor::wl::drm {

class DrmBackend {
public:
    wlr_backend base{};

    DrmBackend(wl_event_loop* loop, const ServerConfig& config);
    ~DrmBackend();

    DrmBackend(const DrmBackend&) = delete;
    DrmBackend& operator=(const DrmBackend&) = delete;

    bool init(std::string* error);

    bool start();
    void destroy();
    int get_drm_fd() const noexcept;
    uint32_t get_buffer_caps() const noexcept;

    void handle_drm_readable();
    void handle_page_flip(uint32_t crtc_id, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec);

    broseat::Seat* seat() const noexcept { return seat_.get(); }
    brodmabuf::KmsDevice* kms_device() const noexcept { return kms_dev_.get(); }

private:
    wl_event_loop* loop_ = nullptr;
    ServerConfig config_;

    std::unique_ptr<broseat::Seat> seat_;
    std::unique_ptr<broseat::SeatDevice> seat_dev_;
    brodmabuf::UniqueFd owned_drm_fd_;
    std::shared_ptr<brodmabuf::KmsDevice> kms_dev_;

    std::unique_ptr<DrmInputBackend> input_backend_;
    std::vector<std::unique_ptr<DrmOutput>> outputs_;
    std::unordered_map<uint32_t, DrmOutput*> crtc_map_;

    wl_event_source* drm_source_ = nullptr;
    wl_event_source* seat_source_ = nullptr;
    bool started_ = false;

    void setup_seat_signals();
};

wlr_backend* create_direct_drm_backend(wl_event_loop* loop, const ServerConfig& config, std::string* error);

}  // namespace brocompositor::wl::drm
