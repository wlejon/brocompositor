# brocompositor

Window-management and compositing substrate for a desktop environment built on
the bro runtime. A standalone C++20 library: no dependency on bro or bronze, no
JS binding, its own CMake and ctest.

## Structure

The library is honest about the two roles it plays:

| Platform | Role | What the backend does |
|----------|------|-----------------------|
| Windows  | **shell** over DWM | manages top-level windows owned by other processes |
| macOS    | **shell** over WindowServer | manages other applications' windows (window-server list + Accessibility) |
| Linux    | **display server** (Wayland, wlroots 0.18) | owns clients, surfaces and outputs; the host renders the outputs |

All roles share one portable core and one surface contract; the two shell
backends also share their asynchronous operation model and crash-recovery
journal (`shell.h`, `src/shell/`). Nothing else is pretended to be common.

```
include/brocompositor/
  geometry.h        Rect/Point/Size/Margins/Edge/Direction
  events.h          facts: WindowAdded/Removed/Changed, FocusChanged, MoveSize*, MonitorsChanged, ReservationChanged
  commands.h        decisions: PlaceWindow, SetWindowVisible, FocusWindow, CloseWindow
  event_queue.h     MPSC queue the host drains on its own thread
  layout.h          pure layouts: BSP (dwindle), master-stack, columns, grid; snapping
  window_manager.h  the policy core (monitors, workspaces, focus history, tiling)
  surface.h         SurfaceSource: shareable GPU images + sync, leased per frame
  shell.h           shell-role vocabulary: Completion<T>, FocusResult, RecoveryReport
  win/shell_backend.h   Windows shell backend
  win/capture.h         Windows.Graphics.Capture -> SurfaceSource
  mac/shell_backend.h   macOS shell backend, permission query
  mac/capture.h         ScreenCaptureKit -> SurfaceRing (IOSurface + MTLSharedEvent) -> SurfaceSource
  linux/server.h        Wayland server role (ServerBackend, ClientSurface, outputs, input routing)
  linux/server_events.h server-role events: output frames/presents, surface trees, layers, raw input
  linux/cpu_mapping.h   CPU view of shm / LINEAR dmabuf images (tests, software hosts)
  vulkan/importer.h     optional Vulkan import: D3D11 handles (Windows), dmabuf + sync_file (Linux),
                        IOSurface + MTLSharedEvent via MoltenVK (macOS)
```

Targets: `brocompositor::core` (portable, pure), `brocompositor::win` (WIN32),
`brocompositor::mac` (APPLE), `brocompositor_wayland` (Linux),
`brocompositor::vulkan` (optional), `brocompositor::brocompositor` (all
available).

### Shell role: asynchronous by construction

Every operation on another process's window can block on that process
(SetWindowPos and ShowWindow send messages the target's thread must answer;
every Accessibility call is served by the target app's main thread). The
shell backends therefore never run one on the caller's thread: `place`,
`set_visible`, `focus` and `close` queue the operation to a worker that
serves only the target process and return a `Completion<T>`
(`std::shared_future`) the host may wait on, poll or drop. A hung
application stalls its own worker and nothing else; operations on one
process run in issue order; a later `focus()` supersedes an earlier one
(`FocusResult::Superseded`). `execute(Command)` is the fire-and-forget form
for the core's commands. On Windows, edge reservations are negotiated with
Explorer on the shell thread and granted through `ReservationChanged`.

### Crash recovery

State a shell backend changes outlives it: windows it parked off-screen or
minimized for a hidden workspace, and (Windows) appbar reservations held by
Explorer. The destructor undoes all of it (waiting at most
`shutdown_timeout` for unresponsive applications). Against crashes and hard
kills each instance mirrors that state in a journal file
(`<journal_dir>/<pid>-<start time>.journal`, rewritten atomically on every
change, removed on a clean shutdown; default `%LOCALAPPDATA%\brocompositor\journal`
/ `~/Library/Application Support/brocompositor/journal`). A backend starting
with `recover = true` claims the journals of dead instances (pid gone or
reused: the start time is part of the name), puts each journaled window back
if it is still the same window of the same process and still where the dead
instance left it, removes leaked appbars, and reports the outcome as
`recovery()`.

What a hard kill (TerminateProcess, SIGKILL, power loss) can and cannot
leave behind:

* Parked / minimized windows stay hidden until the next backend starts with
  the same journal directory; nothing undoes them sooner. If no backend ever
  starts again, `rescue_offscreen_windows()` (a heuristic for state no
  journal describes) or the user (Window > Move to Display, Win+Shift+Arrow)
  is the remedy.
* Windows: Explorer itself reclaims the screen strip of an appbar whose
  window died with its process (verified on Windows 11 by
  test_win_recovery); the journal's `ABM_REMOVE` is the fallback for shells
  that do not. macOS reservations are virtual and die with the process.
* A window that its application closed, or moved back on screen itself,
  since the kill is left alone (counted as skipped). A journal written by an
  instance without Accessibility (macOS) cannot be acted on and is handed
  back for a later instance that has it.
* Process-wide `win::emergency_release_reservations()` removes reservations
  from a console control handler or an unhandled-exception filter, before
  the process dies, where the platform still runs such code.

### macOS: the shell role

`mac::ShellBackend` manages other applications' windows over WindowServer,
within what macOS permits without SIP changes or private Spaces APIs:

* **Discovery** needs no permission: the window server's list
  (CGWindowList, normal layer, regular applications), polled on the
  backend's thread and re-read at once on NSWorkspace launch / terminate /
  activate notifications and on Accessibility notifications (window
  created, destroyed, moved, resized, retitled, (de)minimized). Without
  Screen Recording other applications' titles come from Accessibility, or
  are empty without it too.
* **Operations** (place, hide/show, focus, close) need Accessibility. Each
  application gets a worker thread with its own run loop and AXObserver;
  every AX call has a messaging timeout (`ax_timeout`, 1 s), so a hung
  application fails its own operations after a timeout and stalls nothing
  else. Without Accessibility every operation completes at once with
  false / `FocusResult::Unavailable`.
* **Workspaces** are emulated by parking: a hidden window is moved so that
  only its top-left point lies on a display corner (macOS keeps a sliver of
  every window reachable; the backend reads back where the window really
  went and falls back to minimizing when more than 64x64 pt stayed visible).
  Fullscreen windows live in their own Space and are not hidden.
* **Displays**: menu-bar- and Dock-aware work areas (NSScreen.visibleFrame),
  Retina scale as dpi = 96 x backingScaleFactor, coordinates in Quartz
  global points (see geometry.h). Edge reservations are virtual (the
  reported work area shrinks; other applications see no change); there is
  no appbar protocol on macOS.
* **Focus** tracking needs no permission (frontmost application + its
  front-most window in the window server's order); focusing a window makes
  its application frontmost and raises it (AX). There are no
  MoveSizeStarted/Ended events.
* **Capture**: `mac::WindowCapture` streams one window with ScreenCaptureKit
  (Screen Recording permission) and copies each frame with a Metal blit into
  a `mac::SurfaceRing` of IOSurface-backed textures, signalling a
  MTLSharedEvent timeline; the Vulkan importer imports both through MoltenVK
  (`VK_EXT_metal_objects`). A `SurfaceRing` can be fed IOSurfaces directly.
* **Permissions** are queried, never requested (`mac::query_permissions()`
  does not prompt or open System Settings), and are granted to the
  *responsible* process it names: the app bundle or terminal that launched
  the host (`/usr/libexec/sshd-session` for programs started over ssh).

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
* **XWayland** — started lazily on the first X11 connection (`XwaylandMode`).
  X11 windows join the same model: WM_CLASS -> app_id, WM_TRANSIENT_FOR ->
  owner, `_NET_WM_PID`, title, maximize / fullscreen states, focus,
  close via WM_DELETE_WINDOW; override-redirect windows (menus, tooltips)
  are `unmanaged_surfaces()`. X surfaces are ClientSurfaces like any other.
  CLIPBOARD and PRIMARY are bridged to the Wayland selections. HiDPI: X
  windows live in layout coordinates 1:1 with scale-1 buffers, so on a
  scaled output the host scales them up (right size, soft) — X11 has no
  per-window scale; GDK_SCALE / QT_SCALE_FACTOR stay the user's choice.
* **Session** — ext-session-lock: while locked the server withholds every
  non-lock surface (hit tests, focus, frame callbacks, `window_surfaces()`),
  ends grabs, and sends `locked` only after each output presented a frame
  containing no client surface, so even a host that keeps drawing windows
  cannot leak them; only the lock client unlocks (a crashed locker leaves
  the session locked until a new locker takes over). Input that clients
  synthesize acts on nothing while locked: virtual keyboards / pointers
  (wtype, wlrctl, remote-desktop tools) are muted at the source, keys and
  buttons they produced just before the lock that the host routes after it
  are recognised and refused, and the input method is neither activated for
  the lock surface nor shown its text, and its commits go nowhere — unless
  the host allows a client (`ServerConfig::locked_virtual_input`,
  `set_locked_virtual_input()`, e.g. an on-screen keyboard). Input events
  carry `InputOrigin` (Device / Host / Client + pid). Screen capture
  (wlr-screencopy v3, ext-image-copy-capture with output and toplevel
  sources) copies from host-presented output images, on the CPU when it can
  and otherwise as a `CaptureRequest` the host answers; damage-paced frames
  follow the host's present damage (`{Rect{}}` = nothing changed). Gamma
  control goes to the hardware LUT or, without one, to the host as
  `GammaChanged`. Idle notify + idle inhibit, foreign-toplevel list and
  management (taskbars: requests arrive as `WindowRequest`, foreign = true),
  xdg-activation.
* **Protocols** — xdg-shell (popups, wm capabilities, bounds), subcompositor,
  wl_seat (xkbcommon, touch), tablet-v2 (tools, pads), pointer-constraints +
  relative-pointer, keyboard-shortcuts-inhibit, text-input-v3 +
  input-method-v2 (IME popups join the focused window's tree),
  virtual-keyboard / virtual-pointer, data-device / primary-selection /
  data-control, layer-shell, xdg-decoration, viewporter, fractional-scale,
  presentation-time, linux-dmabuf v4 (feedback from the host's importable
  formats), cursor-shape, xdg-activation, xdg-output, output-management,
  ext-session-lock, wlr-screencopy, ext-image-copy-capture (+ output and
  foreign-toplevel capture sources), ext-foreign-toplevel-list,
  wlr-foreign-toplevel-management, wlr-gamma-control, ext-idle-notify,
  idle-inhibit.
* **Placement** — a new window opens centred in the work area; one larger
  than the work area is configured down to it (X11 and Wayland alike) unless
  its minimum size is larger, and then starts at the work area's corner.

### Host loop

```cpp
auto shell = win::ShellBackend::create(cfg, &err);   // or mac::ShellBackend; starts its threads
WindowManager wm(wm_cfg);                            // pure, single-threaded
// on the host thread, whenever the queue's wake hook fires:
for (auto& e : shell->events().drain())
    shell->execute(wm.handle(e));                    // queued; never blocks on another process
// host actions return commands too:
shell->execute(wm.activate_workspace(ws));
// or individually, with a result to wait on / poll / drop:
Completion<FocusResult> f = shell->focus(id);
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
  Window operations run on per-process worker threads (per-monitor-v2 DPI);
  workers start on demand and exit when idle.
* macOS: one tracking thread per `ShellBackend` reads the window server's
  list; one worker thread per managed application runs a CFRunLoop with its
  AXObserver and the application's operation queue.
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
* macOS: IOSurface-backed images (`ImageHandleType::IOSurface`) ordered by a
  `MTLSharedEvent` timeline (`SyncHandleType::MetalSharedEvent`), waited
  like the Windows fence. MoltenVK imports them with `VK_EXT_metal_objects`
  (`VkImportMetalIOSurfaceInfoEXT`, `VkImportMetalSharedEventInfoEXT`);
  there is no queue-family transfer (`VK_QUEUE_FAMILY_IGNORED`). Adapters
  match by `MTLDevice.registryID`, which MoltenVK reports in `deviceLUID`.

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
    libpixman-1-dev libdrm-dev libgbm-dev libvulkan-dev libxcb1-dev xwayland
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release
ctest --test-dir build-release
```

`BROCOMPOSITOR_WITH_WAYLAND` (default ON when wlroots-0.18 is found) builds
the server role; protocol headers are generated with wayland-scanner at
build time.

macOS (Apple clang, Command Line Tools are enough; macOS 12.3+ for capture):

```bash
brew install cmake ninja vulkan-loader molten-vk vulkan-headers   # Vulkan only for the importer + its tests
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build-release && ctest --test-dir build-release
```

## Tests

Real ctests (failures counted in every configuration, no `assert`). The
Windows tests drive the real OS against windows owned by `bc_test_app`, a
child process they spawn, with the backend's `process_filter` confined to it:
the user's windows are never moved, hidden or closed, edge reservations are
removed on every exit path (release, destructor, console control handler,
unhandled exception) and the work area of every monitor is compared with its
original at the end, and the user's foreground window is restored.
Every Windows test uses a private journal directory. `test_win_focus` skips
(exit 77) while the input desktop is not the user's Default desktop (screen
saver, lock screen), since nothing can take the foreground then; a focus
step that fails while low-level hooks saw real (non-injected) user input in
the same moment is retried after the user goes quiet, never counted as a
pass, and the test skips with that reason if the user never does.
`test_win_ops` hangs a test application and checks that host calls return
at once, other processes' operations go ahead, teardown is bounded and the
hung window stays journaled; `test_win_recovery` TerminateProcess-es a
shell host that parked a window and reserved an edge and checks the next
backend's recovery.

The macOS tests drive windows of `bc_mac_test_app` (an AppKit child) the same
way, confined by `process_filter`; the runner should hold a user-activity
assertion (`caffeinate -u`) so a sleeping display wakes. What they can prove
depends on the permissions of the *responsible* process (printed by every
test): without them the tests that need them skip with the permission and
the binary to grant it to.

| Test | Needs | Covers |
|------|-------|--------|
| test_mac_permissions | — | permission query, honest Unavailable/false results without Accessibility, nothing moves |
| test_mac_lifecycle | awake display | WindowAdded identity/geometry/monitor/dpi, moves, titles (with a permission), focus tracking + hand-back (unlocked), minimize, close, report_existing |
| test_mac_displays | awake display | menu-bar/Dock work areas, dpi, virtual reservations stacking / renegotiating / invisible to others |
| test_mac_recovery | awake display (+ Accessibility for the kill) | stale journal handling; SIGKILLed host's parked window restored by the next backend |
| test_mac_ops | Accessibility | place, park/show (journaled), minimize hiding, close, teardown restore, hung application |
| test_mac_focus | Accessibility, unlocked | focus, AlreadyFocused, Superseded, focus-none -> Finder, tracking |
| test_mac_surface_vulkan | MoltenVK | IOSurface -> SurfaceRing -> Vulkan pixel check, timeline, leases, resize, adapter match |
| test_mac_capture | Screen Recording, unlocked | ScreenCaptureKit window -> Vulkan pixel check; refusal without the permission |

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
| test_wl_lock | session lock vs a hostile victim (pointer/keyboard/popup/grab, pixels) with lock-aware and naive hosts, locker crash, takeover; wtype / wlrctl / IME commits and pre-lock client keys refused while locked, host allow-list; swaylock |
| test_wl_xwayland | X11 client (WM_CLASS, transient, override-redirect, states, focus, WM_DELETE_WINDOW), xterm typing, xeyes, GTK 3 on X11, oversize placement |
| test_wl_xselection | xclip / xsel <-> wl-copy / wl-paste, CLIPBOARD and PRIMARY |
| test_wl_input_protocols | touch, tablet tool + pad, pointer lock/confine + relative pointer, shortcuts inhibit, wtype, wlrctl pointer |
| test_wl_text_input | text-input-v3 <-> input-method-v2: preedit, commit, keyboard grab (bypassed while locked), IME popup, foot |
| test_wl_capture | grim (full + region) pixels, ext-image-copy-capture output frames paced by damage, toplevel source via the host, host_capture_copies |
| test_wl_taskbar | ext-foreign-toplevel-list, wlrctl toplevel list/find/focus/minimize/close, xdg-activation |
| test_wl_session | swayidle idle/resume, idle inhibitor, wlsunset gamma ramps + restore, oversize Wayland placement |

The XWayland tests set `XWAYLAND_NO_GLAMOR=1`: glamor cannot render into
LINEAR dmabufs on NVIDIA, and software rendering is enough for the checks.
They need XWayland in wlroots and `Xwayland` installed, else they skip.
Extra clients used when installed: xterm, xeyes, xclip, xsel, swaylock,
grim, wtype, wlrctl, wlsunset, swayidle, dbus-run-session.

`test_wl_drm` needs DRM master, so it skips unless `BROCOMPOSITOR_DRM_DEVICE`
names an unused KMS card (load `vkms` and use its card node) and a libseat
backend is reachable (e.g. `seatd -g video` with the user in `video`, then
`LIBSEAT_BACKEND=seatd`).
