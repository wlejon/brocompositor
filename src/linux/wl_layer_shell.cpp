#include "wl_layer_shell.h"
#include "wl_backend.h"

namespace brocompositor {

Edge LayerSurfaceInfo::determine_edge() const {
    bool t = has_anchor(anchor, LayerAnchor::Top);
    bool b = has_anchor(anchor, LayerAnchor::Bottom);
    bool l = has_anchor(anchor, LayerAnchor::Left);
    bool r = has_anchor(anchor, LayerAnchor::Right);

    if (t && !b) return Edge::Top;
    if (b && !t) return Edge::Bottom;
    if (l && !r) return Edge::Left;
    if (r && !l) return Edge::Right;
    return Edge::Top;
}

AppBarInfo LayerSurfaceInfo::to_appbar_info() const {
    AppBarInfo info;
    info.id = id;
    info.name = namespace_id;
    info.monitor_id = monitor_id;
    info.edge = determine_edge();
    info.thickness = (info.edge == Edge::Top || info.edge == Edge::Bottom)
        ? (exclusive_zone > 0 ? exclusive_zone : desired_size.height)
        : (exclusive_zone > 0 ? exclusive_zone : desired_size.width);
    info.bounds = geometry;
    info.auto_hide = false;
    return info;
}

WlLayerShell::WlLayerShell() = default;

WlLayerShell::~WlLayerShell() {
    shutdown();
}

bool WlLayerShell::initialize(WlBackend& backend) {
    if (initialized_) return true;

#if defined(BRO_HAS_WAYLAND)
    if (backend.get_display()) {
        layer_shell_ = wlr_layer_shell_v1_create(backend.get_display(), 4);
    }
#else
    (void)backend;
    layer_shell_ = reinterpret_cast<wlr_layer_shell_v1*>(static_cast<uintptr_t>(0x5000));
#endif

    initialized_ = true;
    return true;
}

void WlLayerShell::shutdown() {
    if (!initialized_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        surfaces_.clear();
    }

#if defined(BRO_HAS_WAYLAND)
    layer_shell_ = nullptr;
#else
    layer_shell_ = nullptr;
#endif

    initialized_ = false;
}

AppBarId WlLayerShell::create_layer_surface(
    const std::string& namespace_id,
    MonitorId monitor_id,
    LayerType layer,
    LayerAnchor anchor,
    const Size& desired_size,
    int32_t exclusive_zone
) {
    std::lock_guard<std::mutex> lock(mutex_);
    AppBarId id = next_id_++;

    LayerSurfaceInfo info;
    info.id = id;
    info.namespace_id = namespace_id;
    info.monitor_id = monitor_id;
    info.layer = layer;
    info.anchor = anchor;
    info.desired_size = desired_size;
    info.exclusive_zone = exclusive_zone;
    info.mapped = false;

    surfaces_[id] = std::move(info);
    return id;
}

bool WlLayerShell::destroy_layer_surface(AppBarId id) {
    LayerSurfaceInfo info_copy;
    bool was_mapped = false;
    bool was_exclusive = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return false;

        info_copy = it->second;
        was_mapped = it->second.mapped;
        was_exclusive = it->second.has_exclusive_zone();
        surfaces_.erase(it);
    }

    if (was_mapped && was_exclusive) {
        notify_exclusive_zone_change(info_copy, false);
    }
    return true;
}

bool WlLayerShell::map_layer_surface(AppBarId id) {
    LayerSurfaceInfo info_copy;
    bool is_exclusive = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return false;

        it->second.mapped = true;
        info_copy = it->second;
        is_exclusive = it->second.has_exclusive_zone();
    }

    if (is_exclusive) {
        notify_exclusive_zone_change(info_copy, true);
    }
    return true;
}

bool WlLayerShell::unmap_layer_surface(AppBarId id) {
    LayerSurfaceInfo info_copy;
    bool was_exclusive = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return false;

        was_exclusive = it->second.has_exclusive_zone() && it->second.mapped;
        it->second.mapped = false;
        info_copy = it->second;
    }

    if (was_exclusive) {
        notify_exclusive_zone_change(info_copy, false);
    }
    return true;
}

bool WlLayerShell::set_exclusive_zone(AppBarId id, int32_t zone) {
    LayerSurfaceInfo info_copy;
    bool is_mapped = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = surfaces_.find(id);
        if (it == surfaces_.end()) return false;

        it->second.exclusive_zone = zone;
        info_copy = it->second;
        is_mapped = it->second.mapped;
    }

    if (is_mapped) {
        notify_exclusive_zone_change(info_copy, zone > 0);
    }
    return true;
}

bool WlLayerShell::set_anchor(AppBarId id, LayerAnchor anchor) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    it->second.anchor = anchor;
    return true;
}

bool WlLayerShell::set_layer(AppBarId id, LayerType layer) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    it->second.layer = layer;
    return true;
}

bool WlLayerShell::set_margin(AppBarId id, const Margins& margin) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    it->second.margin = margin;
    return true;
}

bool WlLayerShell::set_desired_size(AppBarId id, const Size& size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(id);
    if (it == surfaces_.end()) return false;
    it->second.desired_size = size;
    return true;
}

void WlLayerShell::arrange_layers(MonitorId monitor_id, const Rect& screen_bounds) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : surfaces_) {
        auto& surf = pair.second;
        if (surf.monitor_id != monitor_id || !surf.mapped) continue;

        Edge edge = surf.determine_edge();
        int32_t thickness = (surf.exclusive_zone > 0)
            ? surf.exclusive_zone
            : ((edge == Edge::Top || edge == Edge::Bottom) ? surf.desired_size.height : surf.desired_size.width);

        switch (edge) {
            case Edge::Top:
                surf.geometry = {
                    screen_bounds.x + surf.margin.left,
                    screen_bounds.y + surf.margin.top,
                    screen_bounds.width - surf.margin.horizontal(),
                    thickness
                };
                break;
            case Edge::Bottom:
                surf.geometry = {
                    screen_bounds.x + surf.margin.left,
                    screen_bounds.y + screen_bounds.height - thickness - surf.margin.bottom,
                    screen_bounds.width - surf.margin.horizontal(),
                    thickness
                };
                break;
            case Edge::Left:
                surf.geometry = {
                    screen_bounds.x + surf.margin.left,
                    screen_bounds.y + surf.margin.top,
                    thickness,
                    screen_bounds.height - surf.margin.vertical()
                };
                break;
            case Edge::Right:
                surf.geometry = {
                    screen_bounds.x + screen_bounds.width - thickness - surf.margin.right,
                    screen_bounds.y + surf.margin.top,
                    thickness,
                    screen_bounds.height - surf.margin.vertical()
                };
                break;
        }

#if defined(BRO_HAS_WAYLAND)
        if (surf.wlr_surface) {
            wlr_layer_surface_v1_configure(surf.wlr_surface, surf.geometry.width, surf.geometry.height);
        }
#endif
    }
}

const LayerSurfaceInfo* WlLayerShell::get_layer_surface(AppBarId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = surfaces_.find(id);
    return it != surfaces_.end() ? &it->second : nullptr;
}

std::vector<AppBarId> WlLayerShell::get_surfaces_for_monitor(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppBarId> result;
    for (const auto& pair : surfaces_) {
        if (pair.second.monitor_id == monitor_id) {
            result.push_back(pair.first);
        }
    }
    return result;
}

std::vector<AppBarId> WlLayerShell::get_exclusive_surfaces(MonitorId monitor_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AppBarId> result;
    for (const auto& pair : surfaces_) {
        if (pair.second.monitor_id == monitor_id && pair.second.has_exclusive_zone() && pair.second.mapped) {
            result.push_back(pair.first);
        }
    }
    return result;
}

void WlLayerShell::set_exclusive_zone_callback(ExclusiveZoneCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    zone_cb_ = std::move(cb);
}

void WlLayerShell::notify_exclusive_zone_change(const LayerSurfaceInfo& surf, bool active) {
    ExclusiveZoneCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = zone_cb_;
    }
    if (cb) {
        AppBarInfo bar = surf.to_appbar_info();
        cb(surf.id, surf.monitor_id, active, active ? &bar : nullptr);
    }
}

} // namespace brocompositor
