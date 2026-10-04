#pragma once

#include "wl_types.h"
#include <unordered_map>
#include <mutex>
#include <functional>

namespace brocompositor {

class WlBackend;
class IAppBarManager;

struct LayerSurfaceInfo {
    AppBarId id = InvalidAppBarId;
    std::string namespace_id = "bro-panel";
    MonitorId monitor_id = PrimaryMonitorId;
    LayerType layer = LayerType::Top;
    LayerAnchor anchor = LayerAnchor::Top;
    int32_t exclusive_zone = 0;
    Margins margin;
    Size desired_size{0, 32};
    Rect geometry;
    bool mapped = false;
    wlr_layer_surface_v1* wlr_surface = nullptr;

    bool has_exclusive_zone() const { return exclusive_zone > 0; }
    Edge determine_edge() const;
    AppBarInfo to_appbar_info() const;
};

class WlLayerShell {
public:
    using ExclusiveZoneCallback = std::function<void(AppBarId id, MonitorId monitor, bool active, const AppBarInfo* bar)>;

    WlLayerShell();
    ~WlLayerShell();

    WlLayerShell(const WlLayerShell&) = delete;
    WlLayerShell& operator=(const WlLayerShell&) = delete;

    bool initialize(WlBackend& backend);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Surface lifecycle
    AppBarId create_layer_surface(
        const std::string& namespace_id,
        MonitorId monitor_id,
        LayerType layer,
        LayerAnchor anchor,
        const Size& desired_size,
        int32_t exclusive_zone = 0
    );

    bool destroy_layer_surface(AppBarId id);
    bool map_layer_surface(AppBarId id);
    bool unmap_layer_surface(AppBarId id);

    // Configuration updates
    bool set_exclusive_zone(AppBarId id, int32_t zone);
    bool set_anchor(AppBarId id, LayerAnchor anchor);
    bool set_layer(AppBarId id, LayerType layer);
    bool set_margin(AppBarId id, const Margins& margin);
    bool set_desired_size(AppBarId id, const Size& size);

    // Geometry layout
    void arrange_layers(MonitorId monitor_id, const Rect& screen_bounds);

    // Queries
    const LayerSurfaceInfo* get_layer_surface(AppBarId id) const;
    std::vector<AppBarId> get_surfaces_for_monitor(MonitorId monitor_id) const;
    std::vector<AppBarId> get_exclusive_surfaces(MonitorId monitor_id) const;

    // Listeners
    void set_exclusive_zone_callback(ExclusiveZoneCallback cb);

private:
    void notify_exclusive_zone_change(const LayerSurfaceInfo& surf, bool active);

    bool initialized_ = false;
    wlr_layer_shell_v1* layer_shell_ = nullptr;

    mutable std::mutex mutex_;
    AppBarId next_id_ = 100;
    std::unordered_map<AppBarId, LayerSurfaceInfo> surfaces_;
    ExclusiveZoneCallback zone_cb_;
};

} // namespace brocompositor
