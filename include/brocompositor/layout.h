#pragma once

#include "brocompositor/types.h"
#include <unordered_map>
#include <vector>

namespace brocompositor {

struct LayoutConfig {
    int32_t gap_inner = 8;
    int32_t gap_outer = 8;
    float master_ratio = 0.5f;
    int32_t master_count = 1;
    SplitDirection bsp_default_split = SplitDirection::Auto;
    int32_t snap_distance = 16;
};

struct LayoutResult {
    std::unordered_map<WindowId, Rect> placements;

    bool contains(WindowId id) const {
        return placements.find(id) != placements.end();
    }

    Rect get_rect(WindowId id, const Rect& fallback = {}) const {
        auto it = placements.find(id);
        return it != placements.end() ? it->second : fallback;
    }
};

class ILayoutEngine {
public:
    virtual ~ILayoutEngine() = default;

    virtual LayoutResult compute_layout(
        LayoutMode mode,
        const Rect& work_area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    ) = 0;

    virtual Rect snap_window(
        const Rect& current_rect,
        const Rect& work_area,
        const std::vector<Rect>& other_windows,
        int32_t snap_distance
    ) = 0;
};

} // namespace brocompositor
