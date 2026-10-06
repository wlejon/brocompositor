#include "linux/drm/drm_backend.h"

#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

#include <cstring>

namespace brocompositor::wl::drm {

namespace {

DrmBackend* backend_of(wlr_backend* base) {
    return reinterpret_cast<DrmBackend*>(base);
}

const struct wlr_backend_impl kDirectDrmBackendImpl = {
    .start = [](wlr_backend* b) { return backend_of(b)->start(); },
    .destroy = [](wlr_backend* b) { backend_of(b)->destroy(); },
    .get_drm_fd = [](wlr_backend* b) { return backend_of(b)->get_drm_fd(); },
    .get_buffer_caps = [](wlr_backend* b) { return backend_of(b)->get_buffer_caps(); },
    .test = nullptr,
    .commit = nullptr,
};

}  // namespace

DrmBackend::DrmBackend(wl_event_loop* loop, const ServerConfig& config)
    : loop_(loop), config_(config) {
    wlr_backend_init(&base, &kDirectDrmBackendImpl);
}

DrmBackend::~DrmBackend() {
    if (drm_source_) {
        wl_event_source_remove(drm_source_);
        drm_source_ = nullptr;
    }
    if (seat_source_) {
        wl_event_source_remove(seat_source_);
        seat_source_ = nullptr;
    }
}

bool DrmBackend::init(std::string* error) {
    broseat::SeatConfig seat_cfg;
    seat_cfg.backend = broseat::SeatBackendType::Auto;
    std::string seat_err;
    seat_ = broseat::Seat::create(seat_cfg, &seat_err);

    std::string card_path = find_drm_card_path();
    if (card_path.empty()) {
        if (error) *error = "No accessible DRM card node found";
        return false;
    }

    int drm_fd = -1;
    if (seat_) {
        std::string dev_err;
        seat_dev_ = seat_->open_device(card_path, &dev_err);
        if (seat_dev_ && seat_dev_->is_valid()) {
            drm_fd = seat_dev_->fd();
        }
    }

    if (drm_fd < 0) {
        drm_fd = ::open(card_path.c_str(), O_RDWR | O_CLOEXEC);
        if (drm_fd < 0) {
            if (error) {
                *error = "Failed to open DRM card " + card_path + ": " + std::strerror(errno);
            }
            return false;
        }
        owned_drm_fd_ = brodmabuf::UniqueFd(drm_fd);
    }

    auto kms_res = brodmabuf::KmsDevice::wrap_fd(brodmabuf::UniqueFd(::dup(drm_fd)));
    if (!kms_res.ok()) {
        if (error) {
            *error = "KmsDevice initialization failed on " + card_path + ": " +
                     std::string(kms_res.error_message());
        }
        return false;
    }
    kms_dev_ = std::move(kms_res.value());

    if (!kms_dev_->is_atomic_supported()) {
        if (error) {
            *error = "DRM device " + card_path + " does not support DRM atomic modesetting";
        }
        return false;
    }

    input_backend_ = std::make_unique<DrmInputBackend>(&base, seat_.get());
    setup_seat_signals();
    return true;
}

void DrmBackend::setup_seat_signals() {
    if (!seat_) return;
    seat_->set_event_callback([this](const broseat::Event& ev) {
        if (auto* s = std::get_if<broseat::SeatActiveChanged>(&ev)) {
            if (s->active) {
                if (input_backend_) input_backend_->resume();
                for (auto& out : outputs_) out->resume();
            } else {
                if (input_backend_) input_backend_->suspend();
                for (auto& out : outputs_) out->suspend();
            }
        }
    });
}

bool DrmBackend::start() {
    if (started_) return true;

    drm_source_ = wl_event_loop_add_fd(
        loop_, kms_dev_->fd(), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            static_cast<DrmBackend*>(data)->handle_drm_readable();
            return 0;
        },
        this);

    if (seat_ && seat_->poll_fd() >= 0) {
        seat_source_ = wl_event_loop_add_fd(
            loop_, seat_->poll_fd(), WL_EVENT_READABLE,
            [](int, uint32_t, void* data) {
                auto* self = static_cast<DrmBackend*>(data);
                if (self->seat_) self->seat_->dispatch(0);
                return 0;
            },
            this);
    }

    auto pipelines = discover_drm_pipelines(*kms_dev_);
    for (auto& pipe : pipelines) {
        uint32_t crtc_id = pipe.crtc_id;
        auto out = std::make_unique<DrmOutput>(this, kms_dev_, std::move(pipe), loop_);
        crtc_map_[crtc_id] = out.get();
        wl_signal_emit_mutable(&base.events.new_output, &out->base);
        outputs_.push_back(std::move(out));
    }

    if (input_backend_) {
        input_backend_->start(loop_);
    }

    started_ = true;
    return true;
}

void DrmBackend::destroy() {
    if (drm_source_) {
        wl_event_source_remove(drm_source_);
        drm_source_ = nullptr;
    }
    if (seat_source_) {
        wl_event_source_remove(seat_source_);
        seat_source_ = nullptr;
    }

    if (input_backend_) {
        input_backend_->stop();
        input_backend_.reset();
    }

    for (auto& out : outputs_) {
        wl_signal_emit_mutable(&out->base.events.destroy, &out->base);
    }
    outputs_.clear();
    crtc_map_.clear();

    wlr_backend_finish(&base);
    delete this;
}

int DrmBackend::get_drm_fd() const noexcept {
    return kms_dev_ ? kms_dev_->fd() : -1;
}

uint32_t DrmBackend::get_buffer_caps() const noexcept {
    return WLR_BUFFER_CAP_DMABUF;
}

void DrmBackend::handle_drm_readable() {
    if (!kms_dev_ || !kms_dev_->valid()) return;

    drmEventContext evctx{};
    evctx.version = 3;
    evctx.page_flip_handler2 = [](int /*fd*/, unsigned int seq, unsigned int sec, unsigned int usec,
                                  unsigned int crtc, void* data) {
        static_cast<DrmBackend*>(data)->handle_page_flip(crtc, seq, sec, usec);
    };

    drmHandleEvent(kms_dev_->fd(), &evctx);
}

void DrmBackend::handle_page_flip(uint32_t crtc_id, unsigned int sequence, unsigned int tv_sec,
                                  unsigned int tv_usec) {
    auto it = crtc_map_.find(crtc_id);
    if (it != crtc_map_.end() && it->second) {
        it->second->on_page_flip(sequence, tv_sec, tv_usec);
    }
}

wlr_backend* create_direct_drm_backend(wl_event_loop* loop, const ServerConfig& config,
                                       std::string* error) {
    auto* backend = new DrmBackend(loop, config);
    if (!backend->init(error)) {
        backend->destroy();
        return nullptr;
    }
    return &backend->base;
}

}  // namespace brocompositor::wl::drm
