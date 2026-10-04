#include "brocompositor/compositor.h"
#include "brocompositor/version.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_smoke] brocompositor version: " << brocompositor::version_string() << "\n";
    assert(!brocompositor::version_string().empty());
    assert(brocompositor::version_major() == BRO_COMPOSITOR_VERSION_MAJOR);
    assert(brocompositor::version_minor() == BRO_COMPOSITOR_VERSION_MINOR);
    assert(brocompositor::version_patch() == BRO_COMPOSITOR_VERSION_PATCH);

    // Create compositor instance
    auto comp = brocompositor::create_compositor();
    assert(comp != nullptr);
    assert(!comp->is_running());

    // Initialize with test context (hooks disabled for headless smoke test)
    brocompositor::CompositorContext ctx;
    ctx.enable_window_hooks = false;
    ctx.enable_wgc_capture = false;
    ctx.auto_tile_new_windows = false;

    bool ok = comp->initialize(ctx);
    assert(ok);
    assert(comp->is_running());

    // Verify default workspace
    brocompositor::WorkspaceId active_ws = comp->get_active_workspace(brocompositor::PrimaryMonitorId);
    assert(active_ws != brocompositor::InvalidWorkspaceId);

    const auto* ws_info = comp->get_workspace(active_ws);
    assert(ws_info != nullptr);
    assert(ws_info->id == active_ws);

    // Verify work area
    brocompositor::WorkArea wa = comp->get_work_area(brocompositor::PrimaryMonitorId);
    assert(!wa.available_work_area.empty());

    // Shutdown
    comp->shutdown();
    assert(!comp->is_running());

    std::cout << "[test_smoke] PASSED\n";
    return 0;
}
