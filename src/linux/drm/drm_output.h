#pragma once

#include "linux/drm/drm_device.h"
#include "linux/wlr.h"

#include "brodmabuf/kms.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace brocompositor::wl::drm {

class DrmBackend;

class DrmOutput {
public:
    wlr_output base{};  // Base wlroots output must be at offset 0

    DrmOutput(DrmBackend* backend, std::shared_ptr<brodmabuf::KmsDevice> dev,
              DrmPipelineInfo pipeline, wl_event_loop* loop);
    ~DrmOutput();

    DrmOutput(const DrmOutput&) = delete;
    DrmOutput& operator=(const DrmOutput&) = delete;

    [[nodiscard]] const DrmPipelineInfo& pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] uint32_t crtc_id() const noexcept { return pipeline_.crtc_id; }
    [[nodiscard]] uint32_t connector_id() const noexcept { return pipeline_.connector_id; }

    bool test(const wlr_output_state* state);
    bool commit(const wlr_output_state* state);
    void destroy();
    const wlr_drm_format_set* get_primary_formats(uint32_t caps);
    size_t get_gamma_size();

    void on_page_flip(unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec);
    void on_buffer_destroy(wlr_buffer* buffer);

    void suspend();
    void resume();

private:
    DrmBackend* backend_ = nullptr;
    std::shared_ptr<brodmabuf::KmsDevice> dev_;
    DrmPipelineInfo pipeline_;
    wl_event_loop* loop_ = nullptr;

    uint32_t mode_blob_id_ = 0;
    bool active_modeset_ = false;
    wlr_drm_format_set primary_formats_{};

    wlr_buffer* current_buffer_ = nullptr;
    brodmabuf::KmsFramebuffer* current_fb_ = nullptr;

    std::unordered_map<wlr_buffer*, std::unique_ptr<brodmabuf::KmsFramebuffer>> fb_cache_;
    std::unordered_map<wlr_buffer*, std::unique_ptr<Listener>> buffer_listeners_;

    uint32_t last_commit_seq_ = 0;
    uint32_t refresh_ns_ = 0;

    brodmabuf::KmsFramebuffer* get_or_create_fb(wlr_buffer* buffer);
    void setup_primary_formats();
    drmModeModeInfo find_mode(int width, int height, int refresh_mhz);
};

bool is_direct_drm_output(wlr_output* output);

}  // namespace brocompositor::wl::drm
