#include "linux/drm/drm_output.h"

#include <drm_fourcc.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace brocompositor::wl::drm {

namespace {

DrmOutput* output_of(wlr_output* base) {
    return reinterpret_cast<DrmOutput*>(base);
}

void output_destroy(wlr_output* base) {
    output_of(base)->destroy();
}

bool output_test(wlr_output* base, const wlr_output_state* state) {
    return output_of(base)->test(state);
}

bool output_commit(wlr_output* base, const wlr_output_state* state) {
    return output_of(base)->commit(state);
}

size_t output_get_gamma_size(wlr_output* base) {
    return output_of(base)->get_gamma_size();
}

const wlr_drm_format_set* output_get_primary_formats(wlr_output* base, uint32_t caps) {
    return output_of(base)->get_primary_formats(caps);
}

const struct wlr_output_impl kDirectDrmOutputImpl = {
    .set_cursor = nullptr,
    .move_cursor = nullptr,
    .destroy = output_destroy,
    .test = output_test,
    .commit = output_commit,
    .get_gamma_size = output_get_gamma_size,
    .get_cursor_formats = nullptr,
    .get_cursor_sizes = nullptr,
    .get_primary_formats = output_get_primary_formats,
};

}  // namespace

bool is_direct_drm_output(wlr_output* output) {
    return output != nullptr && output->impl == &kDirectDrmOutputImpl;
}

DrmOutput::DrmOutput(DrmBackend* backend, std::shared_ptr<brodmabuf::KmsDevice> dev,
                     DrmPipelineInfo pipeline, wl_event_loop* loop)
    : backend_(backend), dev_(std::move(dev)), pipeline_(std::move(pipeline)), loop_(loop) {
    wlr_output_init(&base, nullptr, &kDirectDrmOutputImpl, loop_, nullptr);

    base.name = ::strdup(pipeline_.name.c_str());
    base.make = ::strdup("DRM");
    base.model = ::strdup("DirectKMS");
    base.description = ::strdup(pipeline_.description.c_str());
    base.phys_width = int32_t(pipeline_.mm_width);
    base.phys_height = int32_t(pipeline_.mm_height);
    base.subpixel = static_cast<wl_output_subpixel>(pipeline_.subpixel);

    bool has_preferred = false;
    for (const auto& mode : pipeline_.modes) {
        auto* m = static_cast<wlr_output_mode*>(std::calloc(1, sizeof(wlr_output_mode)));
        if (!m) continue;
        m->width = int32_t(mode.hdisplay);
        m->height = int32_t(mode.vdisplay);
        m->refresh = int32_t(mode.vrefresh * 1000);
        m->preferred = (mode.type & DRM_MODE_TYPE_PREFERRED) != 0;
        if (m->preferred) has_preferred = true;
        wl_list_insert(&base.modes, &m->link);
    }

    if (!has_preferred && !wl_list_empty(&base.modes)) {
        wlr_output_mode* first = wl_container_of(base.modes.next, first, link);
        first->preferred = true;
    }

    uint32_t hz = pipeline_.mode.vrefresh ? pipeline_.mode.vrefresh : 60;
    refresh_ns_ = 1000000000u / hz;

    setup_primary_formats();
}

DrmOutput::~DrmOutput() {
    destroy();
}

void DrmOutput::destroy() {
    if (active_modeset_ && dev_->valid()) {
        brodmabuf::KmsAtomicReq req;
        req.add_property(pipeline_.plane_id, pipeline_.plane_props.fb_id, 0);
        req.add_property(pipeline_.plane_id, pipeline_.plane_props.crtc_id, 0);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.active, 0);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.mode_id, 0);
        req.add_property(pipeline_.connector_id, pipeline_.connector_props.crtc_id, 0);
        (void)req.commit(dev_->fd(), DRM_MODE_ATOMIC_ALLOW_MODESET);
        active_modeset_ = false;
    }

    if (mode_blob_id_ != 0 && dev_->valid()) {
        dev_->destroy_mode_blob(mode_blob_id_);
        mode_blob_id_ = 0;
    }

    buffer_listeners_.clear();
    fb_cache_.clear();
    wlr_drm_format_set_finish(&primary_formats_);
}

void DrmOutput::setup_primary_formats() {
    wlr_drm_format_set_finish(&primary_formats_);
    static const uint32_t fmts[] = {
        DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888};

    // Query IN_FORMATS property on the primary plane if available
    uint32_t in_formats_prop = 0;
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(dev_->fd(), pipeline_.plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
        for (uint32_t i = 0; i < props->count_props; ++i) {
            drmModePropertyPtr p = drmModeGetProperty(dev_->fd(), props->props[i]);
            if (!p) continue;
            if (std::strcmp(p->name, "IN_FORMATS") == 0) {
                in_formats_prop = props->prop_values[i];
                drmModeFreeProperty(p);
                break;
            }
            drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }

    bool added_any = false;
    if (in_formats_prop != 0) {
        drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(dev_->fd(), in_formats_prop);
        if (blob && blob->data) {
            auto* header = static_cast<const drm_format_modifier_blob*>(blob->data);
            auto* blob_fmts = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(blob->data) + header->formats_offset);
            auto* blob_mods = reinterpret_cast<const drm_format_modifier*>(
                static_cast<const uint8_t*>(blob->data) + header->modifiers_offset);

            for (uint32_t i = 0; i < header->count_modifiers; ++i) {
                const auto& mod = blob_mods[i];
                for (uint64_t j = 0; j < 64; ++j) {
                    if (mod.formats & (1ULL << j)) {
                        uint32_t f_idx = mod.offset + uint32_t(j);
                        if (f_idx < header->count_formats) {
                            wlr_drm_format_set_add(&primary_formats_, blob_fmts[f_idx], mod.modifier);
                            added_any = true;
                        }
                    }
                }
            }
            drmModeFreePropertyBlob(blob);
        }
    }

    if (!added_any) {
        for (uint32_t f : fmts) {
            wlr_drm_format_set_add(&primary_formats_, f, DRM_FORMAT_MOD_LINEAR);
        }
    }
}

const wlr_drm_format_set* DrmOutput::get_primary_formats(uint32_t caps) {
    if (!(caps & WLR_BUFFER_CAP_DMABUF)) return nullptr;
    return &primary_formats_;
}

size_t DrmOutput::get_gamma_size() {
    drmModeCrtcPtr crtc = drmModeGetCrtc(dev_->fd(), pipeline_.crtc_id);
    if (!crtc) return 0;
    size_t size = size_t(crtc->gamma_size);
    drmModeFreeCrtc(crtc);
    return size;
}

drmModeModeInfo DrmOutput::find_mode(int width, int height, int refresh_mhz) {
    for (const auto& m : pipeline_.modes) {
        if (int(m.hdisplay) == width && int(m.vdisplay) == height) {
            if (refresh_mhz <= 0 || int(m.vrefresh * 1000) == refresh_mhz) {
                return m;
            }
        }
    }
    return make_mode_info(uint16_t(width), uint16_t(height), uint32_t(refresh_mhz));
}

brodmabuf::KmsFramebuffer* DrmOutput::get_or_create_fb(wlr_buffer* buffer) {
    if (!buffer) return nullptr;
    auto it = fb_cache_.find(buffer);
    if (it != fb_cache_.end()) {
        return it->second.get();
    }

    wlr_dmabuf_attributes dmabuf_attrs{};
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf_attrs)) {
        return nullptr;
    }

    brodmabuf::DmaBufAttributes attrs;
    attrs.width = uint32_t(dmabuf_attrs.width);
    attrs.height = uint32_t(dmabuf_attrs.height);
    attrs.drm_format = dmabuf_attrs.format;
    attrs.modifier = dmabuf_attrs.modifier;
    for (int i = 0; i < dmabuf_attrs.n_planes; ++i) {
        brodmabuf::DmaBufPlane plane;
        plane.fd = brodmabuf::UniqueFd(::dup(dmabuf_attrs.fd[i]));
        plane.offset = dmabuf_attrs.offset[i];
        plane.stride = dmabuf_attrs.stride[i];
        attrs.planes.push_back(std::move(plane));
    }

    auto fb_res = brodmabuf::KmsFramebuffer::create_from_dmabuf(dev_->fd(), attrs);
    if (!fb_res.ok()) {
        return nullptr;
    }

    auto fb = std::move(fb_res.value());
    auto* raw_fb = fb.get();
    fb_cache_[buffer] = std::move(fb);

    auto listener = std::make_unique<Listener>();
    listener->connect(&buffer->events.destroy, [this, buffer](void*) { on_buffer_destroy(buffer); });
    buffer_listeners_[buffer] = std::move(listener);

    return raw_fb;
}

void DrmOutput::on_buffer_destroy(wlr_buffer* buffer) {
    buffer_listeners_.erase(buffer);
    fb_cache_.erase(buffer);
    if (current_buffer_ == buffer) {
        current_buffer_ = nullptr;
        current_fb_ = nullptr;
    }
}

bool DrmOutput::test(const wlr_output_state* state) {
    if (!state) return false;
    bool enabling = (state->committed & WLR_OUTPUT_STATE_ENABLED) ? state->enabled : base.enabled;
    if (!enabling) return true;

    brodmabuf::KmsAtomicReq req;
    req.add_property(pipeline_.connector_id, pipeline_.connector_props.crtc_id, pipeline_.crtc_id);
    req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.active, 1);

    uint32_t test_mode_blob = mode_blob_id_;
    uint32_t temp_blob = 0;
    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        drmModeModeInfo new_mode{};
        if (state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            new_mode = make_mode_info(uint16_t(state->custom_mode.width), uint16_t(state->custom_mode.height),
                                      uint32_t(state->custom_mode.refresh));
        } else if (state->mode) {
            new_mode = find_mode(state->mode->width, state->mode->height, state->mode->refresh);
        }
        auto b = dev_->create_mode_blob(new_mode);
        if (b.ok()) {
            temp_blob = b.value();
            test_mode_blob = temp_blob;
        }
    }
    if (test_mode_blob != 0) {
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.mode_id, test_mode_blob);
    }

    if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
        auto* fb = get_or_create_fb(state->buffer);
        if (fb) {
            int32_t w = pipeline_.mode.hdisplay;
            int32_t h = pipeline_.mode.vdisplay;
            req.set_plane(pipeline_.plane_props, pipeline_.plane_id, pipeline_.crtc_id, fb->fb_id(),
                          0, 0, w, h, 0, 0, w, h);
        }
    }

    auto res = req.commit(dev_->fd(), DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET);
    if (temp_blob != 0) dev_->destroy_mode_blob(temp_blob);
    return res.ok();
}

bool DrmOutput::commit(const wlr_output_state* state) {
    if (!state) return false;

    bool enabling = (state->committed & WLR_OUTPUT_STATE_ENABLED) ? state->enabled : base.enabled;
    if (!enabling) {
        brodmabuf::KmsAtomicReq req;
        req.add_property(pipeline_.plane_id, pipeline_.plane_props.fb_id, 0);
        req.add_property(pipeline_.plane_id, pipeline_.plane_props.crtc_id, 0);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.active, 0);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.mode_id, 0);
        req.add_property(pipeline_.connector_id, pipeline_.connector_props.crtc_id, 0);
        auto res = req.commit(dev_->fd(), DRM_MODE_ATOMIC_ALLOW_MODESET);
        base.enabled = false;
        active_modeset_ = false;
        return res.ok();
    }

    bool modeset = !active_modeset_;
    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        drmModeModeInfo new_mode{};
        if (state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
            new_mode = make_mode_info(uint16_t(state->custom_mode.width), uint16_t(state->custom_mode.height),
                                      uint32_t(state->custom_mode.refresh));
        } else if (state->mode) {
            new_mode = find_mode(state->mode->width, state->mode->height, state->mode->refresh);
        }
        if (std::memcmp(&new_mode, &pipeline_.mode, sizeof(new_mode)) != 0 || !active_modeset_) {
            if (mode_blob_id_ != 0) {
                dev_->destroy_mode_blob(mode_blob_id_);
                mode_blob_id_ = 0;
            }
            auto blob = dev_->create_mode_blob(new_mode);
            if (!blob.ok()) return false;
            mode_blob_id_ = blob.value();
            pipeline_.mode = new_mode;
            modeset = true;
        }
    }

    if (mode_blob_id_ == 0) {
        auto blob = dev_->create_mode_blob(pipeline_.mode);
        if (!blob.ok()) return false;
        mode_blob_id_ = blob.value();
        modeset = true;
    }

    brodmabuf::KmsFramebuffer* fb = nullptr;
    if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
        fb = get_or_create_fb(state->buffer);
        if (!fb) return false;
        current_buffer_ = state->buffer;
        current_fb_ = fb;
    } else {
        fb = current_fb_;
    }

    if (!fb) return false;

    brodmabuf::KmsAtomicReq req;
    if (modeset) {
        req.add_property(pipeline_.connector_id, pipeline_.connector_props.crtc_id, pipeline_.crtc_id);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.mode_id, mode_blob_id_);
        req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.active, 1);
    }

    int32_t w = pipeline_.mode.hdisplay;
    int32_t h = pipeline_.mode.vdisplay;
    req.set_plane(pipeline_.plane_props, pipeline_.plane_id, pipeline_.crtc_id, fb->fb_id(),
                  0, 0, w, h, 0, 0, w, h);

    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (modeset) {
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    } else {
        flags |= DRM_MODE_ATOMIC_NONBLOCK;
    }

    int32_t out_fence = -1;
    if (pipeline_.crtc_props.out_fence_ptr != 0 && !modeset) {
        req.set_out_fence_ptr(pipeline_.crtc_props, pipeline_.crtc_id, &out_fence);
    }

    auto res = req.commit(dev_->fd(), flags);
    if (!res.ok()) return false;

    active_modeset_ = true;
    base.enabled = true;
    last_commit_seq_ = base.commit_seq;

    uint32_t hz = pipeline_.mode.vrefresh ? pipeline_.mode.vrefresh : 60;
    refresh_ns_ = 1000000000u / hz;

    if (modeset) {
        wlr_output_schedule_frame(&base);
    }

    return true;
}

void DrmOutput::on_page_flip(unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec) {
    if (!base.enabled) return;

    struct timespec when {
        .tv_sec = static_cast<time_t>(tv_sec),
        .tv_nsec = static_cast<long>(tv_usec) * 1000L
    };

    wlr_output_event_present event{};
    event.output = &base;
    event.when = &when;
    event.seq = sequence;
    event.refresh = int(refresh_ns_);
    event.commit_seq = last_commit_seq_;
    event.presented = true;

    wlr_output_send_present(&base, &event);
    wlr_output_send_frame(&base);
}

void DrmOutput::suspend() {
    if (!active_modeset_ || !dev_->valid()) return;
    brodmabuf::KmsAtomicReq req;
    req.add_property(pipeline_.plane_id, pipeline_.plane_props.fb_id, 0);
    req.add_property(pipeline_.plane_id, pipeline_.plane_props.crtc_id, 0);
    req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.active, 0);
    req.add_property(pipeline_.crtc_id, pipeline_.crtc_props.mode_id, 0);
    req.add_property(pipeline_.connector_id, pipeline_.connector_props.crtc_id, 0);
    (void)req.commit(dev_->fd(), DRM_MODE_ATOMIC_ALLOW_MODESET);
}

void DrmOutput::resume() {
    active_modeset_ = false;
    wlr_output_schedule_frame(&base);
}

}  // namespace brocompositor::wl::drm
