#include "appbar_manager.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_appbar] Running appbar tests...\n";

    brocompositor::AppBarManager mgr;
    mgr.set_monitor_bounds(brocompositor::PrimaryMonitorId, brocompositor::Rect{0, 0, 1920, 1080});

    brocompositor::WorkArea wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.available_work_area == (brocompositor::Rect{0, 0, 1920, 1080}));
    assert(wa.reserved_margins.horizontal() == 0 && wa.reserved_margins.vertical() == 0);

    // 1. Register top bar (thickness 32)
    brocompositor::AppBarInfo top_bar;
    top_bar.id = 1;
    top_bar.name = "TopBar";
    top_bar.edge = brocompositor::Edge::Top;
    top_bar.thickness = 32;
    top_bar.monitor_id = brocompositor::PrimaryMonitorId;

    assert(mgr.register_appbar(top_bar));

    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.top == 32);
    assert(wa.available_work_area.y == 32);
    assert(wa.available_work_area.height == 1048);
    assert(wa.available_work_area.width == 1920);

    // 2. Register bottom dock (thickness 64)
    brocompositor::AppBarInfo bottom_dock;
    bottom_dock.id = 2;
    bottom_dock.name = "BottomDock";
    bottom_dock.edge = brocompositor::Edge::Bottom;
    bottom_dock.thickness = 64;
    bottom_dock.monitor_id = brocompositor::PrimaryMonitorId;

    assert(mgr.register_appbar(bottom_dock));

    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.top == 32);
    assert(wa.reserved_margins.bottom == 64);
    assert(wa.available_work_area.y == 32);
    assert(wa.available_work_area.height == 984);

    // 3. Register left side panel (thickness 50)
    brocompositor::AppBarInfo left_panel;
    left_panel.id = 3;
    left_panel.name = "LeftPanel";
    left_panel.edge = brocompositor::Edge::Left;
    left_panel.thickness = 50;
    left_panel.monitor_id = brocompositor::PrimaryMonitorId;

    assert(mgr.register_appbar(left_panel));

    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.left == 50);
    assert(wa.available_work_area.x == 50);
    assert(wa.available_work_area.width == 1870);

    // 4. Auto-hide appbar should not reduce work area
    brocompositor::AppBarInfo autohide_bar;
    autohide_bar.id = 4;
    autohide_bar.name = "AutoHideDock";
    autohide_bar.edge = brocompositor::Edge::Right;
    autohide_bar.thickness = 80;
    autohide_bar.auto_hide = true;
    autohide_bar.monitor_id = brocompositor::PrimaryMonitorId;

    assert(mgr.register_appbar(autohide_bar));
    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.right == 0); // Not reserved because auto_hide is true

    // 5. Unregister top bar -> top margin restored to 0
    assert(mgr.unregister_appbar(1));
    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.top == 0);
    assert(wa.available_work_area.y == 0);
    assert(wa.available_work_area.height == 1080 - 64);

    // Unregister remaining
    assert(mgr.unregister_appbar(2));
    assert(mgr.unregister_appbar(3));
    assert(mgr.unregister_appbar(4));

    wa = mgr.get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.available_work_area == (brocompositor::Rect{0, 0, 1920, 1080}));

    std::cout << "[test_appbar] PASSED\n";
    return 0;
}
