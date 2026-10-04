#pragma once

#include "brocompositor/layout.h"

namespace brocompositor {

class LayoutEngine : public ILayoutEngine {
public:
    LayoutEngine() = default;
    ~LayoutEngine() override = default;

    LayoutResult compute_layout(
        LayoutMode mode,
        const Rect& work_area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    ) override;

    Rect snap_window(
        const Rect& current_rect,
        const Rect& work_area,
        const std::vector<Rect>& other_windows,
        int32_t snap_distance
    ) override;

private:
    LayoutResult layout_master_stack(
        const Rect& area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    );

    LayoutResult layout_columns(
        const Rect& area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    );

    LayoutResult layout_grid(
        const Rect& area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    );

    LayoutResult layout_bsp(
        const Rect& area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    );

    LayoutResult layout_floating(
        const Rect& area,
        const std::vector<WindowId>& windows,
        const LayoutConfig& config
    );
};

} // namespace brocompositor
