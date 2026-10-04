# brocompositor

**Cross-Platform Window Management and Display Compositing Substrate for Bro (`bro.wm` / `bro.compositor`).**

`brocompositor` manages window placement, virtual workspaces, edge appbar docking for desktop panels/docks, tiling/floating layout algorithms (BSP, Master-Stack, Columns, Grid), and zero-copy GPU texture ingestion into Bro's Vulkan presenter.

---

## Features

- **Tiling & Layout Engine**:
  - Binary Space Partitioning (BSP) tree-based window splitting.
  - Master-Stack dynamic tiling (customizable master ratio and window counts).
  - Columns and Grid layouts.
  - Floating mode with magnetic snapping against display work areas and adjacent window boundaries.
  - Configurable inner and outer gaps.
- **Virtual Desktops & Workspaces**:
  - Multi-monitor workspace management.
  - Dynamic workspace switching with OS window cloaking/visibility transitions.
  - Window migration and focus retention across workspaces.
  - Window pinning support across all virtual desktops.
- **Edge AppBar Docking**:
  - Reserved Top, Bottom, Left, and Right desktop edges with pixel thickness and exclusive margins.
  - System work area adjustment (`SHAppBarMessage` / `SPI_SETWORKAREA`) ensuring tiled/maximized windows never overlap docks or taskbars.
  - Auto-hide support.
- **Windows Backend (Win32 / DWM / WGC)**:
  - Event hook manager (`SetWinEventHook`) running on a dedicated message thread for real-time window tracking (`EVENT_OBJECT_CREATE`, `EVENT_OBJECT_DESTROY`, `EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MOVESIZEEND`, `EVENT_SYSTEM_MINIMIZESTART`, `EVENT_SYSTEM_MINIMIZEEND`).
  - Window management and DWM styling (`DwmSetWindowAttribute` for immersive dark mode titlebars, window corner rounding preferences, and cloaking).
  - Windows Graphics Capture (WGC) + DXGI NT Shared Handle pipeline for zero-copy Vulkan ingestion (`VK_KHR_external_memory_win32`).

---

## Directory Structure

```
brocompositor/
├── CMakeLists.txt
├── README.md
├── include/
│   └── brocompositor/
│       ├── appbar.h           # Edge docking & work area interface
│       ├── compositor.h       # Master ICompositor facade
│       ├── export.h           # Dynamic/static export macros
│       ├── layout.h           # Layout engine interface & config
│       ├── types.h            # Geometric types, states, enums
│       ├── version.h          # Semantic versioning API
│       ├── window.h           # WindowInfo abstractions
│       └── workspace.h        # Virtual desktop abstractions
├── src/
│   ├── appbar_manager.h/cpp   # AppBar & work area computation
│   ├── bsp_tree.h/cpp         # BSP tree layout implementation
│   ├── layout_engine.h/cpp    # Master-Stack, Columns, Grid, Snapping
│   ├── version.cpp            # Version implementation
│   ├── workspace_manager.h/cpp# Multi-monitor workspace state
│   └── win/
│       ├── win_appbar.h/cpp   # Win32 SHAppBarMessage integration
│       ├── win_capture.h/cpp  # D3D11/DXGI shared NT handle pipeline
│       ├── win_compositor.h/cpp# Windows concrete ICompositor
│       ├── win_hook_manager.h/cpp # WinEventHook background loop
│       ├── win_virtual_desktops.h/cpp # Window cloaking / switching
│       └── win_window_ops.h/cpp   # SetWindowPos, ShowWindow, DWM
└── tests/
    ├── CMakeLists.txt
    ├── test_appbar.cpp
    ├── test_common.h
    ├── test_layout.cpp
    ├── test_smoke.cpp
    ├── test_window_win.cpp
    └── test_workspace.cpp
```

---

## Building & Testing

### Windows (Visual Studio 2022 / Ninja)

```sh
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```
