#include "workspace_manager.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_workspace] Running workspace tests...\n";

    brocompositor::WorkspaceManager mgr;

    // Verify initial state
    brocompositor::WorkspaceId def_id = mgr.get_active_workspace(brocompositor::PrimaryMonitorId);
    assert(def_id != brocompositor::InvalidWorkspaceId);

    const auto* ws1 = mgr.get_workspace(def_id);
    assert(ws1 != nullptr);
    assert(ws1->name == "Default");
    assert(ws1->monitor_id == brocompositor::PrimaryMonitorId);

    // Create a secondary workspace
    brocompositor::WorkspaceId ws2_id = mgr.create_workspace("Code", brocompositor::PrimaryMonitorId);
    assert(ws2_id != def_id);

    auto workspaces = mgr.get_workspaces_on_monitor(brocompositor::PrimaryMonitorId);
    assert(workspaces.size() == 2);

    // Add windows
    brocompositor::WindowId win1 = 1001;
    brocompositor::WindowId win2 = 1002;
    brocompositor::WindowId win3 = 1003;

    assert(mgr.add_window(win1, def_id));
    assert(mgr.add_window(win2, def_id));
    assert(mgr.add_window(win3, ws2_id));

    assert(mgr.get_window_workspace(win1) == def_id);
    assert(mgr.get_window_workspace(win2) == def_id);
    assert(mgr.get_window_workspace(win3) == ws2_id);

    auto ws1_wins = mgr.get_windows(def_id);
    assert(ws1_wins.size() == 2);
    assert(ws1_wins[0] == win1 && ws1_wins[1] == win2);

    // Move window from def_id to ws2_id
    assert(mgr.move_window_to_workspace(win2, ws2_id));
    assert(mgr.get_window_workspace(win2) == ws2_id);
    assert(mgr.get_windows(def_id).size() == 1);
    assert(mgr.get_windows(ws2_id).size() == 2);

    // Switch workspace
    assert(mgr.switch_workspace(ws2_id));
    assert(mgr.get_active_workspace(brocompositor::PrimaryMonitorId) == ws2_id);

    // Layout mode change
    assert(mgr.set_workspace_layout_mode(ws2_id, brocompositor::LayoutMode::BSP));
    const auto* ws2 = mgr.get_workspace(ws2_id);
    assert(ws2->layout_mode == brocompositor::LayoutMode::BSP);

    // Active window tracking
    assert(mgr.set_active_window(ws2_id, win2));
    assert(mgr.get_active_window(ws2_id) == win2);

    // Removing workspace: windows should migrate to fallback workspace
    assert(mgr.remove_workspace(ws2_id));
    assert(mgr.get_workspace(ws2_id) == nullptr);
    assert(mgr.get_active_workspace(brocompositor::PrimaryMonitorId) == def_id);
    assert(mgr.get_window_workspace(win2) == def_id);
    assert(mgr.get_window_workspace(win3) == def_id);

    std::cout << "[test_workspace] PASSED\n";
    return 0;
}
