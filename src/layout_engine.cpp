#include "layout_engine.h"
#include "bsp_tree.h"
#include <cmath>
#include <cstdlib>

namespace brocompositor {

namespace {

Rect apply_outer_gap(const Rect& area, int32_t gap) {
    if (gap <= 0) return area;
    Margins m{gap, gap, gap, gap};
    Rect r = area.inset(m);
    return r.empty() ? area : r;
}

std::vector<int32_t> partition_dimension(int32_t total, int32_t count, int32_t gap) {
    std::vector<int32_t> sizes;
    if (count <= 0) return sizes;
    sizes.resize(count);

    int32_t total_gaps = (count - 1) * std::max(0, gap);
    int32_t available = std::max(0, total - total_gaps);
    int32_t base = available / count;
    int32_t remainder = available % count;

    for (int32_t i = 0; i < count; ++i) {
        sizes[i] = base + (i < remainder ? 1 : 0);
    }
    return sizes;
}

} // namespace

LayoutResult LayoutEngine::compute_layout(
    LayoutMode mode,
    const Rect& work_area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& config
) {
    if (windows.empty() || work_area.empty()) {
        return {};
    }

    Rect area = apply_outer_gap(work_area, config.gap_outer);

    switch (mode) {
        case LayoutMode::MasterStack:
            return layout_master_stack(area, windows, config);
        case LayoutMode::Columns:
            return layout_columns(area, windows, config);
        case LayoutMode::Grid:
            return layout_grid(area, windows, config);
        case LayoutMode::BSP:
            return layout_bsp(area, windows, config);
        case LayoutMode::Floating:
        default:
            return layout_floating(area, windows, config);
    }
}

LayoutResult LayoutEngine::layout_master_stack(
    const Rect& area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& config
) {
    LayoutResult result;
    const size_t count = windows.size();
    if (count == 0) return result;

    if (count == 1) {
        result.placements[windows[0]] = area;
        return result;
    }

    int32_t master_target = std::max(1, config.master_count);
    int32_t master_num = std::min(static_cast<int32_t>(count - 1), master_target);
    int32_t slave_num = static_cast<int32_t>(count) - master_num;

    int32_t gap = std::max(0, config.gap_inner);
    float ratio = std::clamp(config.master_ratio, 0.1f, 0.9f);

    int32_t avail_w = std::max(0, area.width - gap);
    int32_t master_w = static_cast<int32_t>(avail_w * ratio);
    int32_t slave_w = avail_w - master_w;

    auto master_heights = partition_dimension(area.height, master_num, gap);
    auto slave_heights = partition_dimension(area.height, slave_num, gap);

    // Place master windows
    int32_t curr_y = area.y;
    for (int32_t i = 0; i < master_num; ++i) {
        result.placements[windows[i]] = Rect{
            area.x,
            curr_y,
            master_w,
            master_heights[i]
        };
        curr_y += master_heights[i] + gap;
    }

    // Place slave windows
    curr_y = area.y;
    int32_t slave_x = area.x + master_w + gap;
    for (int32_t i = 0; i < slave_num; ++i) {
        result.placements[windows[master_num + i]] = Rect{
            slave_x,
            curr_y,
            slave_w,
            slave_heights[i]
        };
        curr_y += slave_heights[i] + gap;
    }

    return result;
}

LayoutResult LayoutEngine::layout_columns(
    const Rect& area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& config
) {
    LayoutResult result;
    const int32_t count = static_cast<int32_t>(windows.size());
    if (count == 0) return result;

    int32_t gap = std::max(0, config.gap_inner);
    auto widths = partition_dimension(area.width, count, gap);

    int32_t curr_x = area.x;
    for (int32_t i = 0; i < count; ++i) {
        result.placements[windows[i]] = Rect{
            curr_x,
            area.y,
            widths[i],
            area.height
        };
        curr_x += widths[i] + gap;
    }

    return result;
}

LayoutResult LayoutEngine::layout_grid(
    const Rect& area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& config
) {
    LayoutResult result;
    const int32_t total = static_cast<int32_t>(windows.size());
    if (total == 0) return result;

    int32_t cols = static_cast<int32_t>(std::ceil(std::sqrt(static_cast<double>(total))));
    if (cols <= 0) cols = 1;
    int32_t rows = static_cast<int32_t>(std::ceil(static_cast<double>(total) / cols));
    if (rows <= 0) rows = 1;

    int32_t gap = std::max(0, config.gap_inner);
    auto row_heights = partition_dimension(area.height, rows, gap);

    int32_t win_idx = 0;
    int32_t curr_y = area.y;

    for (int32_t r = 0; r < rows && win_idx < total; ++r) {
        int32_t remaining = total - win_idx;
        int32_t cols_in_row = std::min(cols, remaining);
        auto col_widths = partition_dimension(area.width, cols_in_row, gap);

        int32_t curr_x = area.x;
        for (int32_t c = 0; c < cols_in_row && win_idx < total; ++c) {
            result.placements[windows[win_idx]] = Rect{
                curr_x,
                curr_y,
                col_widths[c],
                row_heights[r]
            };
            curr_x += col_widths[c] + gap;
            win_idx++;
        }
        curr_y += row_heights[r] + gap;
    }

    return result;
}

LayoutResult LayoutEngine::layout_bsp(
    const Rect& area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& config
) {
    LayoutResult result;
    if (windows.empty()) return result;

    BspTree tree;
    for (WindowId id : windows) {
        tree.insert(id, InvalidWindowId, config.bsp_default_split);
    }

    tree.compute_rects(area, config.gap_inner, result.placements);
    return result;
}

LayoutResult LayoutEngine::layout_floating(
    const Rect& area,
    const std::vector<WindowId>& windows,
    const LayoutConfig& /*config*/) {
    // For floating windows without pre-set geometry, cascade them neatly in work area
    LayoutResult result;
    const int32_t cascade_offset = 32;
    int32_t def_w = std::min(area.width, std::max(300, (area.width * 2) / 3));
    int32_t def_h = std::min(area.height, std::max(200, (area.height * 2) / 3));

    int32_t cur_x = area.x + 40;
    int32_t cur_y = area.y + 40;

    for (WindowId id : windows) {
        result.placements[id] = Rect{cur_x, cur_y, def_w, def_h};
        cur_x += cascade_offset;
        cur_y += cascade_offset;
        if (cur_x + def_w > area.right()) cur_x = area.x + 40;
        if (cur_y + def_h > area.bottom()) cur_y = area.y + 40;
    }
    return result;
}

Rect LayoutEngine::snap_window(
    const Rect& current_rect,
    const Rect& work_area,
    const std::vector<Rect>& other_windows,
    int32_t snap_distance
) {
    if (snap_distance <= 0) return current_rect;

    Rect snapped = current_rect;

    // 1. Snap to work area boundary
    // Left boundary
    if (std::abs(snapped.left() - work_area.left()) <= snap_distance) {
        snapped.x = work_area.left();
    }
    // Right boundary
    if (std::abs(snapped.right() - work_area.right()) <= snap_distance) {
        snapped.x = work_area.right() - snapped.width;
    }
    // Top boundary
    if (std::abs(snapped.top() - work_area.top()) <= snap_distance) {
        snapped.y = work_area.top();
    }
    // Bottom boundary
    if (std::abs(snapped.bottom() - work_area.bottom()) <= snap_distance) {
        snapped.y = work_area.bottom() - snapped.height;
    }

    // 2. Snap to neighbor windows
    for (const auto& other : other_windows) {
        if (other == current_rect) continue;

        // Vertical overlap check for horizontal snapping
        bool v_overlap = !(snapped.bottom() <= other.top() || snapped.top() >= other.bottom());
        if (v_overlap) {
            // snapped right edge to other left edge
            if (std::abs(snapped.right() - other.left()) <= snap_distance) {
                snapped.x = other.left() - snapped.width;
            }
            // snapped left edge to other right edge
            else if (std::abs(snapped.left() - other.right()) <= snap_distance) {
                snapped.x = other.right();
            }
            // align top edges
            if (std::abs(snapped.top() - other.top()) <= snap_distance) {
                snapped.y = other.top();
            }
            // align bottom edges
            if (std::abs(snapped.bottom() - other.bottom()) <= snap_distance) {
                snapped.y = other.bottom() - snapped.height;
            }
        }

        // Horizontal overlap check for vertical snapping
        bool h_overlap = !(snapped.right() <= other.left() || snapped.left() >= other.right());
        if (h_overlap) {
            // snapped bottom edge to other top edge
            if (std::abs(snapped.bottom() - other.top()) <= snap_distance) {
                snapped.y = other.top() - snapped.height;
            }
            // snapped top edge to other bottom edge
            else if (std::abs(snapped.top() - other.bottom()) <= snap_distance) {
                snapped.y = other.bottom();
            }
            // align left edges
            if (std::abs(snapped.left() - other.left()) <= snap_distance) {
                snapped.x = other.left();
            }
            // align right edges
            if (std::abs(snapped.right() - other.right()) <= snap_distance) {
                snapped.x = other.right() - snapped.width;
            }
        }
    }

    return snapped;
}

} // namespace brocompositor
