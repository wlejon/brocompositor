# brocompositor

Window-management and compositing substrate for a desktop environment built on
the bro runtime. A standalone C++20 library: no dependency on bro or bronze, no
JS binding, its own CMake and ctest.

## Structure

The library is honest about the two roles it plays:

| Platform | Role | What the backend does |
|----------|------|-----------------------|
| Windows  | **shell** over DWM | manages top-level windows owned by other processes |
| Linux    | **display server** (Wayland, wlroots 0.18) | owns clients, surfaces and outputs; the host renders the outputs |

Both roles share one portable core and one surface contract; nothing else is
pretended to be common.

```
include/brocompositor/
  geometry.h        Rect/Point/Size/Margins/Edge/Direction
  events.h          facts: WindowAdded/Removed/Changed, FocusChanged, MoveSize*, MonitorsChanged, ReservationChanged
  commands.h        decisions: PlaceWindow, SetWindowVisible, FocusWindow, CloseWindow
  event_queue.h     MPSC queue the host drains on its own thread
  layout.h          pure layouts: BSP (dwindle), master-stack, columns, grid; snapping
  window_manager.h  the policy core (monitors, workspaces, focus history, tiling)
  surface.h         SurfaceSource: shareable GPU images + sync, leased per frame
  win/shell_backend.h   Windows shell backend
  win/capture.h         Windows.Graphics.Capture -> SurfaceSource
  linux/server.h        Wayland server role (ServerBackend, ClientSurface, outputs, input routing)
  linux/server_events.h server-role events: output frames/presents, surface trees, layers, raw input
  linux/cpu_mapping.h   CPU view of shm / LINEAR dmabuf images (tests, software hosts)
  vulkan/importer.h     optional Vulkan import: D3D11 handles (Windows), dmabuf + sync_file (Linux)
```

Targets: `brocompositor::core` (portable, pure), `brocompositor::win` (WIN32),
`brocompositor_wayland` (Linux), `brocompositor::vulkan` (optional),
`brocompositor::brocompositor` (all available).

### Linux: the server role

`wl::ServerBackend` runs a Wayland display server on its own thread, using
wlroots 0.18 for protocols, backends (headless, nested Wayland / X11,
DRM/KMS + libinput + libseat session) and input devices only: no
`wlr_renderer`, no `wlr_scene`. The host renders.

* **Windows** — mapped `xdg_toplevel`s feed the portable core (WindowAdded with
  app_id, title, pid, parent -> owner; WindowChanged; WindowRemoved), and core
  commands act on them: PlaceWindow -> configure, SetWindowVisible -> not
  drawn (and `suspended`), FocusWindow -> keyboard focus + activated,
  CloseWindow -> `xdg_toplevel.close`. Each window is a tree of
  `SurfaceNode`s (root, subsurfaces, popups) for the host to draw.
  xdg-decoration is negotiated (server-side preferred;
  `server_side_decoration(id)` tells the host whether to draw the frame).
* **Outputs** — every output's presentable images are allocated by the server
  (GBM dmabufs with host-importable modifiers, dumb buffers, or shm for
  nested backends). Per `OutputFrame` the host `acquire_output_image()`s,
  renders, and `present_output()`s with damage, a render-done sync_file and
  the surfaces it drew (they get frame callbacks and presentation feedback);
  `OutputPresented` reports the kernel / backend timestamps. Outputs become
  `MonitorsChanged`; layer-shell exclusive zones and `reserve_edge()` become
  work areas and `ReservationChanged`; wlr-output-management (wlr-randr) and
  `configure_output()` change modes, scale and position.
* **Input** — devices feed `PointerMotion`/`PointerButton`/`KeyboardKey`/...
  server events; the host hit-tests (`hit_test()`, `hit_test_layer()`) and
  routes with `pointer_route()` / `pointer_button()` / `keyboard_key()`.
  Keys carry the post-key modifier state so routed modifiers stay in order
  with routed keys. Virtual devices (`inject_*`) drive the same path.
* **Protocols** — xdg-shell (popups, wm capabilities, bounds), subcompositor,
  wl_seat (xkbcommon), data-device / primary-selection / data-control,
  layer-shell, xdg-decoration, viewporter, fractional-scale,
  presentation-time, linux-dmabuf v4 (feedback from the host's importable
  formats), cursor-shape, xdg-activation, xdg-output, output-management.
  XWayland is not supported yet.

### Host loop

```cpp
auto shell = win::ShellBackend::create(cfg, &err);   // starts the shell thread
WindowManager wm(wm_cfg);                            // pure, single-threaded
// on the host thread, whenever the queue's wake hook fires:
for (auto& e : shell->events().drain())
    shell->execute(wm.handle(e));
// host actions return commands too:
shell->execute(wm.activate_workspace(ws));
```

The core never moves a window unless the host opts a workspace into a tiling
layout (`LayoutMode::Floating` is the default).

### Threading

* Backends push value snapshots into an `EventQueue` from their own threads;
  the host drains it on its thread. No callback runs host code except the
  queue's optional wake hook (and `WindowCapture::set_frame_callback`, which
  fires on a WGC thread-pool thread).
* Windows: one shell thread per `ShellBackend` owns the WinEvent hooks, the
  listener window (display / work-area broadcasts) and the appbar windows.
  Window operations run on the caller's thread in a per-monitor-v2 DPI scope.
* Linux: one server thread per `ServerBackend` owns the `wl_display` and every
  wlroots object. Queries read a mutex-guarded mirror; mutations are posted to
  the server thread (an eventfd job queue) and synchronous ones wait for the
  result. Events arrive on two queues: `events()` (portable) and
  `server_events()` (server role).

### Surface contract

A `SurfaceSource` exposes a small set of OS-native shareable images
(`SharedImage`, imported once per `id`) and a sync primitive. Per frame the
host leases the newest frame (`acquire`), GPU-waits its sync, samples, and
`release`s once its GPU work completed. Leased images are never overwritten.

* Windows: D3D11 textures with `SHARED_NTHANDLE`, ordered by a shared
  `ID3D11Fence` imported in Vulkan as a timeline semaphore
  (`VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`, Vulkan
  1.2 timeline semaphores). The producer device is created on the host's
  adapter by LUID.
* Linux: each client buffer is a `SharedImage` — dmabuf planes + DRM fourcc +
  explicit modifier, or a wl_shm fd — with damage since the last acquire, a
  per-frame sync_file (the buffer's implicit fence) and buffer release when
  no lease remains. The Vulkan importer imports dmabufs with
  `VK_EXT_image_drm_format_modifier` (multi-plane, disjoint when planes are
  separate buffers), waits sync_files as temporary SYNC_FD semaphores,
  exports render-done sync_files, and reports `dmabuf_formats()` for the
  server to advertise. shm images are uploaded by the host.

## Building

Windows (Visual Studio generator, one build dir, config at build time):

```bash
cmake -B build -DCMAKE_PREFIX_PATH=D:/vcpkg/installed/x64-windows   # Vulkan headers for the importer
cmake --build build --config Release
ctest --test-dir build -C Release
```

The Vulkan importer builds when Vulkan headers are found (`VULKAN_SDK` or
`CMAKE_PREFIX_PATH`); it never links a loader.

Linux (GCC 12+ or Clang; Debian trixie package names):

```bash
sudo apt install libwlroots-0.18-dev wayland-protocols libwayland-dev libxkbcommon-dev \
    libpixman-1-dev libdrm-dev libgbm-dev libvulkan-dev
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
ctest --test-dir build-release
```

`BROCOMPOSITOR_WITH_WAYLAND` (default ON when wlroots-0.18 is found) builds
the server role; protocol headers are generated with wayland-scanner at
build time.

## Tests

Real ctests (failures counted in every configuration, no `assert`). The
Windows tests drive the real OS against windows owned by `bc_test_app`, a
child process they spawn, with the backend's `process_filter` confined to it:
the user's windows are never moved, hidden or closed, edge reservations are
removed on every exit path (release, destructor, console control handler,
unhandled exception) and the work area of every monitor is compared with its
original at the end, and the user's foreground window is restored.
`test_win_focus` skips (exit 77) while the input desktop is not the user's
Default desktop (screen saver, lock screen), since nothing can take the
foreground then.

The Linux tests run a `ServerBackend` with a test host (`tests/linux/wl_harness`:
WindowManager + hit-test routing + CPU compositor) in a private
`XDG_RUNTIME_DIR`, and drive real clients: `bc_wl_client` (a scripted client
that prints what it receives), plus weston-simple-shm / -damage /
-dmabuf-egl, foot, wl-clipboard, wlr-randr, gtk3-widget-factory, a Qt 6
example and Xvfb when installed (missing programs are skipped, not failed).

| Test | Covers |
|------|--------|
| test_wl_window | map/WindowAdded, content, subsurface + popup tree, place/tiling, workspace hide, close, viewporter, decorations |
| test_wl_input | pointer/keyboard routing, focus, cursor clamping, typing a command into foot |
| test_wl_clipboard | wl-clipboard (data-control) and data-device clients, primary selection |
| test_wl_layer | layer-shell exclusive zones, work area, reserve_edge, exclusive keyboard |
| test_wl_outputs | wlr-randr, scale / fractional scale, add_output, mode change, disable |
| test_wl_frames | frame-callback pacing, presentation feedback, hidden windows, buffer reuse |
| test_wl_clients | weston demos, GTK 3, Qt 6: map, content, animation, close |
| test_wl_nested | Wayland-nested child server (content, input, resize, close); X11 under Xvfb |
| test_wl_vulkan | dmabuf client -> Vulkan import + pixel check (every capable device incl. lavapipe); Vulkan -> output image -> present |
| test_wl_drm | DRM/KMS on an unused card (vkms) via libseat; opt-in |

`test_wl_drm` needs DRM master, so it skips unless `BROCOMPOSITOR_DRM_DEVICE`
names an unused KMS card (load `vkms` and use its card node) and a libseat
backend is reachable (e.g. `seatd -g video` with the user in `video`, then
`LIBSEAT_BACKEND=seatd`).
