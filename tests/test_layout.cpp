#include "brocompositor/layout.h"
#include "layout_engine.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_layout] Running layout tests...\n";

    brocompositor::LayoutEngine engine;
    brocompositor::Rect work_area{0, 0, 1920, 1080};
    brocompositor::LayoutConfig config;
    config.gap_outer = 0;
    config.gap_inner = 0;
    config.master_ratio = 0.5f;

    // 1. Master-Stack with 1 window
    {
        std::vector<brocompositor::WindowId> wins = {101};
        auto res = engine.compute_layout(brocompositor::LayoutMode::MasterStack, work_area, wins, config);
        assert(res.contains(101));
        assert(res.get_rect(101) == work_area);
    }

    // 2. Master-Stack with 2 windows
    {
        std::vector<brocompositor::WindowId> wins = {101, 102};
        auto res = engine.compute_layout(brocompositor::LayoutMode::MasterStack, work_area, wins, config);
        assert(res.contains(101));
        assert(res.contains(102));

        brocompositor::Rect r1 = res.get_rect(101);
        brocompositor::Rect r2 = res.get_rect(102);

        // 50% split width: 960 each
        assert(r1.width == 960);
        assert(r1.height == 1080);
        assert(r1.x == 0);

        assert(r2.width == 960);
        assert(r2.height == 1080);
        assert(r2.x == 960);
        assert(!r1.intersects(r2));
    }

    // 3. Master-Stack with 3 windows and gaps
    {
        brocompositor::LayoutConfig gap_config;
        gap_config.gap_outer = 10;
        gap_config.gap_inner = 10;
        gap_config.master_ratio = 0.5f;

        std::vector<brocompositor::WindowId> wins = {1, 2, 3};
        auto res = engine.compute_layout(brocompositor::LayoutMode::MasterStack, work_area, wins, gap_config);
        assert(res.contains(1));
        assert(res.contains(2));
        assert(res.contains(3));

        brocompositor::Rect m = res.get_rect(1);
        brocompositor::Rect s1 = res.get_rect(2);
        brocompositor::Rect s2 = res.get_rect(3);

        // Verify none overlap
        assert(!m.intersects(s1));
        assert(!m.intersects(s2));
        assert(!s1.intersects(s2));

        // Master on left, slaves on right
        assert(m.x == 10);
        assert(s1.x == m.right() + 10);
        assert(s2.x == m.right() + 10);
        assert(s2.y == s1.bottom() + 10);
    }

    // 4. Columns layout
    {
        std::vector<brocompositor::WindowId> wins = {1, 2, 3};
        auto res = engine.compute_layout(brocompositor::LayoutMode::Columns, work_area, wins, config);
        assert(res.contains(1) && res.contains(2) && res.contains(3));

        brocompositor::Rect c1 = res.get_rect(1);
        brocompositor::Rect c2 = res.get_rect(2);
        brocompositor::Rect c3 = res.get_rect(3);

        assert(c1.width == 640);
        assert(c2.width == 640);
        assert(c3.width == 640);
        assert(c2.x == c1.right());
        assert(c3.x == c2.right());
        assert(c3.right() == 1920);
    }

    // 5. Grid layout
    {
        std::vector<brocompositor::WindowId> wins = {1, 2, 3, 4};
        auto res = engine.compute_layout(brocompositor::LayoutMode::Grid, work_area, wins, config);
        assert(res.placements.size() == 4);

        brocompositor::Rect g1 = res.get_rect(1);
        brocompositor::Rect g2 = res.get_rect(2);
        brocompositor::Rect g3 = res.get_rect(3);
        brocompositor::Rect g4 = res.get_rect(4);

        assert(g1.width == 960 && g1.height == 540);
        assert(g2.x == 960 && g2.y == 0);
        assert(g3.x == 0 && g3.y == 540);
        assert(g4.x == 960 && g4.y == 540);
    }

    // 6. BSP layout
    {
        std::vector<brocompositor::WindowId> wins = {10, 20, 30};
        auto res = engine.compute_layout(brocompositor::LayoutMode::BSP, work_area, wins, config);
        assert(res.placements.size() == 3);

        brocompositor::Rect b1 = res.get_rect(10);
        brocompositor::Rect b2 = res.get_rect(20);
        brocompositor::Rect b3 = res.get_rect(30);

        assert(!b1.empty() && !b2.empty() && !b3.empty());
        assert(!b1.intersects(b2));
        assert(!b1.intersects(b3));
        assert(!b2.intersects(b3));
    }

    // 7. Floating Magnetic Snapping
    {
        // Near left screen edge (x = 8, snap_distance = 16)
        brocompositor::Rect win{8, 100, 400, 300};
        brocompositor::Rect snapped = engine.snap_window(win, work_area, {}, 16);
        assert(snapped.x == 0);
        assert(snapped.y == 100);

        // Near right screen edge (right = 1912, within 16 of 1920)
        brocompositor::Rect win_right{1920 - 400 - 8, 100, 400, 300};
        snapped = engine.snap_window(win_right, work_area, {}, 16);
        assert(snapped.right() == 1920);

        // Snap to neighbor window
        brocompositor::Rect neighbor{100, 100, 300, 300};
        brocompositor::Rect moving{408, 105, 200, 200}; // left is 408 (neighbor right is 400, diff 8)
        snapped = engine.snap_window(moving, work_area, {neighbor}, 16);
        assert(snapped.x == 400); // snapped to neighbor's right edge
        assert(snapped.y == 100); // snapped to neighbor's top edge
    }

    std::cout << "[test_layout] PASSED\n";
    return 0;
}
