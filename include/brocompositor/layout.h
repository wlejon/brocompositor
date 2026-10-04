// Pure tiling layouts: an ordered window list plus an area in, rectangles out.
#pragma once

#include "brocompositor/events.h"

#include <optional>
#include <vector>

namespace brocompositor {

enum class LayoutMode : uint32_t {
    Floating = 0,     // the core never places windows; they keep their own geometry
    BSP = 1,          // binary split, each new window halves the previous one (dwindle)
    MasterStack = 2,  // master column(s) on the left, the rest stacked on the right
    Columns = 3,
    Grid = 4,
};

const char* to_string(LayoutMode mode);

enum class SplitDirection : uint32_t { Auto = 0, Vertical = 1, Horizontal = 2 };

struct LayoutConfig {
    int32_t gap_inner = 8;
    int32_t gap_outer = 8;
    float master_ratio = 0.5f;
    int32_t master_count = 1;
    SplitDirection bsp_split = SplitDirection::Auto;  // Auto alternates by depth
};

struct Placement {
    WindowId id = kNoWindow;
    Rect rect;
    bool operator==(const Placement&) const = default;
};

struct LayoutResult {
    std::vector<Placement> placements;  // same order as the input window list

    std::optional<Rect> find(WindowId id) const {
        for (const auto& p : placements)
            if (p.id == id) return p.rect;
        return std::nullopt;
    }
};

// Floating yields no placements.
LayoutResult compute_layout(LayoutMode mode, const Rect& area, const std::vector<WindowId>& windows,
                            const LayoutConfig& config);

// Magnetic snapping of a floating rect against the area edges and neighbours.
Rect snap_rect(const Rect& rect, const Rect& area, const std::vector<Rect>& others,
               int32_t snap_distance);

}  // namespace brocompositor
