# brocompositor

Window-management and compositing substrate for a desktop environment built on
the bro runtime. A standalone C++20 library: no dependency on bro or bronze, no
JS binding, its own CMake and ctest.

## Structure

The library is honest about the two roles it plays:

| Platform | Role | What the backend does |
|----------|------|-----------------------|
| Windows  | **shell** over DWM | manages top-level windows owned by other processes |
| Linux (next) | **display server** (Wayland) | owns clients, surfaces and outputs; the host renders the outputs |

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
  vulkan/importer.h     optional Vulkan import of SharedImage / SharedTimeline
```

Targets: `brocompositor::core` (portable, pure), `brocompositor::win` (WIN32),
`brocompositor::vulkan` (optional), `brocompositor::brocompositor` (all
available).

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
* Linux (next): dmabuf planes + modifier (or shm) with damage, a per-frame
  sync_file, and `presented()` driving frame callbacks.

## Building

Windows (Visual Studio generator, one build dir, config at build time):

```bash
cmake -B build -DCMAKE_PREFIX_PATH=D:/vcpkg/installed/x64-windows   # Vulkan headers for the importer
cmake --build build --config Release
ctest --test-dir build -C Release
```

The Vulkan importer builds when Vulkan headers are found (`VULKAN_SDK` or
`CMAKE_PREFIX_PATH`); it never links a loader. On Linux only the core builds:

```bash
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
ctest --test-dir build-release
```

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
