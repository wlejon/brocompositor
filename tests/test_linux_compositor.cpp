#include "wl_compositor.h"
#include "wl_backend.h"
#include "wl_seat.h"
#include "wl_xdg_shell.h"
#include "wl_layer_shell.h"
#include "wl_dmabuf_vulkan.h"
#include "test_common.h"

#include <cassert>
#include <iostream>
#include <atomic>

void test_lifecycle() {
    std::cout << "  [Subtest] Lifecycle...\n";
    auto comp = brocompositor::create_linux_compositor();
    assert(comp != nullptr);
    assert(!comp->is_running());

    brocompositor::CompositorContext ctx;
    ctx.auto_tile_new_windows = true;

    bool ok = comp->initialize(ctx);
    assert(ok);
    assert(comp->is_running());

    brocompositor::WorkspaceId active_ws = comp->get_active_workspace(brocompositor::PrimaryMonitorId);
    assert(active_ws != brocompositor::InvalidWorkspaceId);

    const auto* ws = comp->get_workspace(active_ws);
    assert(ws != nullptr);
    assert(ws->name == "Default");

    comp->shutdown();
    assert(!comp->is_running());
    std::cout << "  [Subtest] Lifecycle PASSED\n";
}

void test_backend_and_outputs() {
    std::cout << "  [Subtest] Backend & Outputs...\n";
    brocompositor::WlBackend backend;
    brocompositor::WlBackend::Config cfg;
    cfg.socket_name = "wayland-bro-test";
    cfg.start_thread = true;

    assert(!backend.is_running());
    bool ok = backend.initialize(cfg);
    assert(ok);
    assert(backend.is_running());
    assert(backend.get_socket_name() == "wayland-bro-test");

    // Add a secondary output
    std::atomic<bool> output_added{false};
    backend.set_output_callback([&](brocompositor::MonitorId id, bool added, const brocompositor::WlOutputInfo* info) {
        if (id == 2 && added && info != nullptr) {
            output_added.store(true);
        }
    });

    brocompositor::WlOutputInfo sec_out;
    sec_out.monitor_id = 2;
    sec_out.name = "DP-2";
    sec_out.bounds = {1920, 0, 2560, 1440};
    sec_out.scale = 1.5;
    sec_out.refresh_rate_hz = 144;
    backend.add_output(sec_out);

    assert(output_added.load());
    const auto* queried = backend.get_output(2);
    assert(queried != nullptr);
    assert(queried->name == "DP-2");
    assert(queried->bounds.width == 2560);
    assert(queried->scale == 1.5);

    // Event loop task execution
    std::atomic<bool> task_executed{false};
    backend.dispatch_on_loop([&]() {
        task_executed.store(true);
    });
    backend.process_pending();
    assert(task_executed.load());

    backend.shutdown();
    assert(!backend.is_running());
    std::cout << "  [Subtest] Backend & Outputs PASSED\n";
}

void test_seat_input_and_gestures() {
    std::cout << "  [Subtest] Seat, Input & Gestures...\n";
    brocompositor::WlBackend backend;
    backend.initialize();

    brocompositor::WlSeat seat;
    brocompositor::WlSeat::Config cfg;
    cfg.seat_name = "seat0";
    cfg.swipe_threshold = 30.0;

    assert(!seat.is_initialized());
    bool ok = seat.initialize(backend, cfg);
    assert(ok);
    assert(seat.is_initialized());
    assert(seat.get_seat_name() == "seat0");

    // Pointer motion
    std::atomic<bool> motion_fired{false};
    seat.set_pointer_motion_callback([&](double x, double y) {
        if (x == 640.0 && y == 480.0) {
            motion_fired.store(true);
        }
    });
    seat.notify_pointer_motion(640.0, 480.0);
    assert(motion_fired.load());
    assert(seat.get_cursor_position().x == 640);
    assert(seat.get_cursor_position().y == 480);

    // Focus switching
    brocompositor::WindowId target_win = 101;
    seat.set_focus(target_win);
    assert(seat.get_focused_window() == target_win);
    seat.clear_focus();
    assert(seat.get_focused_window() == brocompositor::InvalidWindowId);

    // 3-Finger Touchpad Swipe Gesture
    std::atomic<bool> swipe_action_fired{false};
    brocompositor::Edge swipe_dir = brocompositor::Edge::Top;
    seat.set_swipe_action_callback([&](int fingers, brocompositor::Edge dir) {
        if (fingers == 3) {
            swipe_action_fired.store(true);
            swipe_dir = dir;
        }
    });

    seat.notify_swipe_begin(3);
    seat.notify_swipe_update(50.0, 0.0); // Right swipe beyond 30.0 threshold
    seat.notify_swipe_end(false);

    assert(swipe_action_fired.load());
    assert(swipe_dir == brocompositor::Edge::Right);

    // Pinch Gesture
    std::atomic<bool> pinch_fired{false};
    seat.set_gesture_callback([&](brocompositor::GestureType type, int fingers, double, double, double scale) {
        if (type == brocompositor::GestureType::PinchUpdate && fingers == 2 && scale > 1.0) {
            pinch_fired.store(true);
        }
    });
    seat.notify_pinch_begin(2);
    seat.notify_pinch_update(1.25, 0.0, 0.0, 0.0);
    seat.notify_pinch_end(false);
    assert(pinch_fired.load());

    seat.shutdown();
    backend.shutdown();
    std::cout << "  [Subtest] Seat, Input & Gestures PASSED\n";
}

void test_xdg_shell_foreign_windows() {
    std::cout << "  [Subtest] XDG Shell Foreign Windows...\n";
    brocompositor::WlBackend backend;
    backend.initialize();

    brocompositor::WlXdgShell shell;
    bool ok = shell.initialize(backend);
    assert(ok);

    std::atomic<bool> created_fired{false};
    shell.set_window_created_callback([&](brocompositor::WindowId, const brocompositor::WindowInfo& info) {
        if (info.app_id == "org.mozilla.firefox") {
            created_fired.store(true);
        }
    });

    // Create foreign toplevel window
    brocompositor::WindowId win = shell.create_toplevel("org.mozilla.firefox", "Mozilla Firefox", {100, 100, 1000, 700});
    assert(win != brocompositor::InvalidWindowId);

    // Map surface
    shell.map_surface(win);
    assert(created_fired.load());

    // Create child popup (e.g. bookmarks dropdown)
    brocompositor::WindowId popup = shell.create_popup(win, {10, 40, 200, 300});
    assert(popup != brocompositor::InvalidWindowId);
    shell.map_surface(popup);

    auto popups = shell.get_popups_for_parent(win);
    assert(popups.size() == 1);
    assert(popups[0] == popup);

    // Client requests maximize
    shell.on_client_request_maximize(win, true);
    const auto* top = shell.get_toplevel(win);
    assert(top != nullptr);
    assert(brocompositor::has_state(top->state, brocompositor::WindowState::Maximized));

    // Compositor configure placement
    brocompositor::Rect new_geom{0, 0, 1920, 1040};
    shell.configure_toplevel(win, new_geom, top->state);
    top = shell.get_toplevel(win);
    assert(top->geometry == new_geom);

    // Destroy toplevel cleans up child popups
    shell.destroy_surface(win);
    assert(shell.get_toplevel(win) == nullptr);
    assert(shell.get_popup(popup) == nullptr);

    shell.shutdown();
    backend.shutdown();
    std::cout << "  [Subtest] XDG Shell Foreign Windows PASSED\n";
}

void test_layer_shell_and_exclusive_zones() {
    std::cout << "  [Subtest] Layer Shell & Exclusive Zones...\n";
    brocompositor::WlBackend backend;
    backend.initialize();

    brocompositor::WlLayerShell layer_shell;
    bool ok = layer_shell.initialize(backend);
    assert(ok);

    std::atomic<bool> zone_cb_called{false};
    layer_shell.set_exclusive_zone_callback([&](brocompositor::AppBarId, brocompositor::MonitorId, bool active, const brocompositor::AppBarInfo* bar) {
        if (active && bar && bar->edge == brocompositor::Edge::Top && bar->thickness == 32) {
            zone_cb_called.store(true);
        }
    });

    // Create desktop top status bar with exclusive zone 32
    brocompositor::AppBarId topbar = layer_shell.create_layer_surface(
        "bro-panel",
        brocompositor::PrimaryMonitorId,
        brocompositor::LayerType::Top,
        brocompositor::LayerAnchor::Top | brocompositor::LayerAnchor::Left | brocompositor::LayerAnchor::Right,
        {1920, 32},
        32
    );
    assert(topbar != brocompositor::InvalidAppBarId);

    // Map surface to activate exclusive zone
    layer_shell.map_layer_surface(topbar);
    assert(zone_cb_called.load());

    // Create bottom dock with exclusive zone 60
    brocompositor::AppBarId dock = layer_shell.create_layer_surface(
        "bro-dock",
        brocompositor::PrimaryMonitorId,
        brocompositor::LayerType::Top,
        brocompositor::LayerAnchor::Bottom | brocompositor::LayerAnchor::Left | brocompositor::LayerAnchor::Right,
        {1920, 60},
        60
    );
    layer_shell.map_layer_surface(dock);

    // Arrange layers on 1920x1080 display
    layer_shell.arrange_layers(brocompositor::PrimaryMonitorId, {0, 0, 1920, 1080});

    const auto* top_info = layer_shell.get_layer_surface(topbar);
    assert(top_info != nullptr);
    assert(top_info->geometry.y == 0);
    assert(top_info->geometry.height == 32);

    const auto* dock_info = layer_shell.get_layer_surface(dock);
    assert(dock_info != nullptr);
    assert(dock_info->geometry.y == 1080 - 60);
    assert(dock_info->geometry.height == 60);

    layer_shell.shutdown();
    backend.shutdown();
    std::cout << "  [Subtest] Layer Shell & Exclusive Zones PASSED\n";
}

void test_dmabuf_vulkan_import() {
    std::cout << "  [Subtest] DMA-BUF Vulkan Zero-Copy Import...\n";
    brocompositor::WlBackend backend;
    backend.initialize();

    brocompositor::WlDmabufVulkan dmabuf;
    bool ok = dmabuf.initialize(backend);
    assert(ok);

    assert(dmabuf.is_format_supported(brocompositor::DRM_FOURCC_ARGB8888));
    assert(dmabuf.is_format_supported(brocompositor::DRM_FOURCC_NV12));
    assert(!dmabuf.is_format_supported(0xFFFFFFFF));

    // Import a client DMA-BUF frame
    brocompositor::DmaBufAttributes attribs;
    attribs.width = 1920;
    attribs.height = 1080;
    attribs.format = brocompositor::DRM_FOURCC_ARGB8888;
    attribs.modifier = brocompositor::DRM_MODIFIER_LINEAR;
    attribs.n_planes = 1;
    attribs.planes[0].fd = 42;
    attribs.planes[0].stride = 1920 * 4;
    attribs.planes[0].offset = 0;

    auto tex = dmabuf.import_dmabuf(attribs);
    assert(tex != nullptr);
    assert(tex->is_valid);
    assert(tex->image_handle != 0);
    assert(tex->width == 1920);
    assert(tex->height == 1080);
    assert(dmabuf.get_active_texture_count() == 1);

    // Synchronize fence
    assert(dmabuf.import_sync_fence(10, tex->image_handle));

    // Release texture
    ok = dmabuf.release_texture(tex->image_handle);
    assert(ok);
    assert(dmabuf.get_active_texture_count() == 0);

    dmabuf.shutdown();
    backend.shutdown();
    std::cout << "  [Subtest] DMA-BUF Vulkan Zero-Copy Import PASSED\n";
}

void test_compositor_integration_tiling_and_exclusive_zones() {
    std::cout << "  [Subtest] Compositor Tiling & Layer Shell Integration...\n";
    auto comp = brocompositor::create_linux_compositor();
    assert(comp != nullptr);

    brocompositor::CompositorContext ctx;
    ctx.auto_tile_new_windows = true;
    ctx.default_layout_config.gap_inner = 0;
    ctx.default_layout_config.gap_outer = 0;
    bool ok = comp->initialize(ctx);
    assert(ok);

    // Initial work area should be full 1920x1080
    brocompositor::WorkArea wa = comp->get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.available_work_area.x == 0);
    assert(wa.available_work_area.y == 0);
    assert(wa.available_work_area.width == 1920);
    assert(wa.available_work_area.height == 1080);

    // Register top panel with thickness 40
    brocompositor::AppBarInfo topbar;
    topbar.id = 501;
    topbar.name = "bro-topbar";
    topbar.edge = brocompositor::Edge::Top;
    topbar.thickness = 40;
    topbar.bounds = {0, 0, 1920, 40};
    comp->register_appbar(topbar);

    // Verify work area updated with exclusive zone deduction
    wa = comp->get_work_area(brocompositor::PrimaryMonitorId);
    assert(wa.reserved_margins.top == 40);
    assert(wa.available_work_area.y == 40);
    assert(wa.available_work_area.height == 1040);

    // Register two foreign client windows via xdg_shell
    brocompositor::WindowId win1 = comp->get_xdg_shell().create_toplevel("alacritty", "Terminal", {100, 100, 800, 600});
    brocompositor::WindowId win2 = comp->get_xdg_shell().create_toplevel("blender", "Blender", {200, 200, 800, 600});

    comp->get_xdg_shell().map_surface(win1);
    comp->get_xdg_shell().map_surface(win2);

    brocompositor::WorkspaceId active_ws = comp->get_active_workspace(brocompositor::PrimaryMonitorId);
    comp->set_workspace_layout_mode(active_ws, brocompositor::LayoutMode::Columns);
    comp->relayout_workspace(active_ws);

    // Verify tiling placement respects topbar exclusive margin (y starts at 40)
    const auto* w1 = comp->get_window_info(win1);
    const auto* w2 = comp->get_window_info(win2);
    assert(w1 != nullptr);
    assert(w2 != nullptr);
    assert(w1->geometry.y == 40);
    assert(w2->geometry.y == 40);
    assert(w1->geometry.height == 1040);
    assert(w2->geometry.height == 1040);

    // Verify zero-copy capture
    ok = comp->enable_capture(win1);
    assert(ok);
    assert(comp->get_shared_texture_handle(win1) != 0);
    comp->disable_capture(win1);
    assert(comp->get_shared_texture_handle(win1) == 0);

    // Test Touchpad 3-Finger Swipe Workspace Switching
    brocompositor::WorkspaceId ws2 = comp->create_workspace("Workspace 2", brocompositor::PrimaryMonitorId);
    assert(ws2 != brocompositor::InvalidWorkspaceId);
    assert(comp->get_active_workspace(brocompositor::PrimaryMonitorId) == active_ws);

    // Trigger 3-finger horizontal swipe right on the seat
    comp->get_seat().notify_swipe_begin(3);
    comp->get_seat().notify_swipe_update(50.0, 0.0);
    comp->get_seat().notify_swipe_end(false);

    // Compositor should have switched to ws2
    assert(comp->get_active_workspace(brocompositor::PrimaryMonitorId) == ws2);

    comp->shutdown();
    std::cout << "  [Subtest] Compositor Tiling & Layer Shell Integration PASSED\n";
}

int main() {
    init_test();
    std::cout << "[test_linux_compositor] Running Linux Wayland compositor tests..." << std::endl;

    std::cout << "Starting test_lifecycle..." << std::endl;
    test_lifecycle();
    std::cout << "Starting test_backend_and_outputs..." << std::endl;
    test_backend_and_outputs();
    std::cout << "Starting test_seat_input_and_gestures..." << std::endl;
    test_seat_input_and_gestures();
    std::cout << "Starting test_xdg_shell_foreign_windows..." << std::endl;
    test_xdg_shell_foreign_windows();
    std::cout << "Starting test_layer_shell_and_exclusive_zones..." << std::endl;
    test_layer_shell_and_exclusive_zones();
    std::cout << "Starting test_dmabuf_vulkan_import..." << std::endl;
    test_dmabuf_vulkan_import();
    std::cout << "Starting test_compositor_integration_tiling_and_exclusive_zones..." << std::endl;
    test_compositor_integration_tiling_and_exclusive_zones();

    std::cout << "[test_linux_compositor] ALL TESTS PASSED!" << std::endl;
    return 0;
}
