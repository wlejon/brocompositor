#pragma once

#include "brodmabuf/kms.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brocompositor::wl::drm {

struct DrmPipelineInfo {
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    drmModeModeInfo mode{};
    brodmabuf::KmsPlaneProps plane_props;
    brodmabuf::KmsCrtcProps crtc_props;
    brodmabuf::KmsConnectorProps connector_props;
    std::vector<drmModeModeInfo> modes;
    std::string name;
    std::string description;
    uint32_t mm_width = 0;
    uint32_t mm_height = 0;
    uint32_t subpixel = 0;
};

/// Discovers the path to the primary or requested DRM card node.
std::string find_drm_card_path(const std::string& preferred = "");

/// Enumerates connected display pipelines on the given KMS device.
std::vector<DrmPipelineInfo> discover_drm_pipelines(const brodmabuf::KmsDevice& dev);

/// Synthesizes a standard video mode timing structure.
drmModeModeInfo make_mode_info(uint16_t width, uint16_t height, uint32_t refresh_mhz);

}  // namespace brocompositor::wl::drm
