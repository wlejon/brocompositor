#include "linux/drm/drm_device.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace brocompositor::wl::drm {

std::string find_drm_card_path(const std::string& preferred) {
    if (!preferred.empty() && ::access(preferred.c_str(), R_OK | W_OK) == 0) {
        return preferred;
    }

    if (const char* env = std::getenv("BROCOMPOSITOR_DRM_DEVICE")) {
        if (*env && ::access(env, R_OK | W_OK) == 0) {
            return env;
        }
    }

    if (const char* env = std::getenv("WLR_DRM_DEVICES")) {
        if (*env && ::access(env, R_OK | W_OK) == 0) {
            return env;
        }
    }

    for (int i = 0; i < 64; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (::access(path, R_OK | W_OK) == 0) {
            return path;
        }
    }

    return "";
}

drmModeModeInfo make_mode_info(uint16_t width, uint16_t height, uint32_t refresh_mhz) {
    drmModeModeInfo mode{};
    mode.hdisplay = width;
    mode.vdisplay = height;
    mode.vrefresh = refresh_mhz > 0 ? (refresh_mhz + 500) / 1000 : 60;
    mode.clock = uint32_t(width) * height * mode.vrefresh / 1000;
    mode.hsync_start = width + 16;
    mode.hsync_end = mode.hsync_start + 96;
    mode.htotal = mode.hsync_end + 48;
    mode.vsync_start = height + 10;
    mode.vsync_end = mode.vsync_start + 2;
    mode.vtotal = mode.vsync_end + 33;
    mode.type = DRM_MODE_TYPE_DRIVER;
    std::snprintf(mode.name, sizeof(mode.name), "%ux%u", width, height);
    return mode;
}

std::vector<DrmPipelineInfo> discover_drm_pipelines(const brodmabuf::KmsDevice& dev) {
    std::vector<DrmPipelineInfo> pipelines;
    if (!dev.valid()) return pipelines;

    drmModeResPtr res = drmModeGetResources(dev.fd());
    if (!res) return pipelines;

    drmModePlaneResPtr plane_res = drmModeGetPlaneResources(dev.fd());
    if (!plane_res) {
        drmModeFreeResources(res);
        return pipelines;
    }

    // Map CRTC index -> primary plane ID
    std::vector<uint32_t> crtc_primary_planes(size_t(res->count_crtcs), 0);
    for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
        uint32_t pid = plane_res->planes[i];
        brodmabuf::KmsPlaneProps p_props = dev.query_plane_props(pid);
        if (p_props.type == 0) continue;

        drmModeObjectPropertiesPtr obj_props =
            drmModeObjectGetProperties(dev.fd(), pid, DRM_MODE_OBJECT_PLANE);
        if (!obj_props) continue;

        bool is_primary = false;
        for (uint32_t j = 0; j < obj_props->count_props; ++j) {
            if (obj_props->props[j] == p_props.type &&
                obj_props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) {
                is_primary = true;
                break;
            }
        }
        drmModeFreeObjectProperties(obj_props);

        if (is_primary) {
            drmModePlanePtr plane = drmModeGetPlane(dev.fd(), pid);
            if (plane) {
                for (int c = 0; c < res->count_crtcs; ++c) {
                    if ((plane->possible_crtcs & (1u << c)) && crtc_primary_planes[size_t(c)] == 0) {
                        crtc_primary_planes[size_t(c)] = pid;
                    }
                }
                drmModeFreePlane(plane);
            }
        }
    }

    std::unordered_set<uint32_t> assigned_crtcs;

    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(dev.fd(), res->connectors[i]);
        if (!conn) continue;

        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes <= 0) {
            drmModeFreeConnector(conn);
            continue;
        }

        // Pick preferred mode or first mode
        drmModeModeInfo chosen_mode = conn->modes[0];
        for (int m = 0; m < conn->count_modes; ++m) {
            if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                chosen_mode = conn->modes[m];
                break;
            }
        }

        // Find a suitable CRTC and plane
        uint32_t crtc_id = 0;
        uint32_t plane_id = 0;

        // Try existing encoder's CRTC first if unassigned
        if (conn->encoder_id != 0) {
            drmModeEncoderPtr enc = drmModeGetEncoder(dev.fd(), conn->encoder_id);
            if (enc) {
                if (enc->crtc_id != 0 && assigned_crtcs.find(enc->crtc_id) == assigned_crtcs.end()) {
                    for (int c = 0; c < res->count_crtcs; ++c) {
                        if (res->crtcs[c] == enc->crtc_id && crtc_primary_planes[size_t(c)] != 0) {
                            crtc_id = enc->crtc_id;
                            plane_id = crtc_primary_planes[size_t(c)];
                            break;
                        }
                    }
                }
                drmModeFreeEncoder(enc);
            }
        }

        // Otherwise find first compatible unassigned CRTC
        if (crtc_id == 0) {
            for (int e = 0; e < conn->count_encoders; ++e) {
                drmModeEncoderPtr enc = drmModeGetEncoder(dev.fd(), conn->encoders[e]);
                if (!enc) continue;
                for (int c = 0; c < res->count_crtcs; ++c) {
                    uint32_t cand_crtc = res->crtcs[c];
                    if ((enc->possible_crtcs & (1u << c)) &&
                        assigned_crtcs.find(cand_crtc) == assigned_crtcs.end() &&
                        crtc_primary_planes[size_t(c)] != 0) {
                        crtc_id = cand_crtc;
                        plane_id = crtc_primary_planes[size_t(c)];
                        break;
                    }
                }
                drmModeFreeEncoder(enc);
                if (crtc_id != 0) break;
            }
        }

        if (crtc_id == 0 || plane_id == 0) {
            drmModeFreeConnector(conn);
            continue;
        }

        assigned_crtcs.insert(crtc_id);

        const char* type_name = drmModeGetConnectorTypeName(conn->connector_type);
        std::string name = (type_name ? type_name : "DRM") + std::string("-") +
                           std::to_string(conn->connector_type_id);

        DrmPipelineInfo pipe;
        pipe.connector_id = conn->connector_id;
        pipe.crtc_id = crtc_id;
        pipe.plane_id = plane_id;
        pipe.mode = chosen_mode;
        pipe.name = name;
        pipe.description = name;
        pipe.mm_width = conn->mmWidth;
        pipe.mm_height = conn->mmHeight;
        pipe.subpixel = conn->subpixel;

        for (int m = 0; m < conn->count_modes; ++m) {
            pipe.modes.push_back(conn->modes[m]);
        }

        pipe.plane_props = dev.query_plane_props(plane_id);
        pipe.crtc_props = dev.query_crtc_props(crtc_id);
        pipe.connector_props = dev.query_connector_props(conn->connector_id);

        pipelines.push_back(std::move(pipe));
        drmModeFreeConnector(conn);
    }

    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);
    return pipelines;
}

}  // namespace brocompositor::wl::drm
