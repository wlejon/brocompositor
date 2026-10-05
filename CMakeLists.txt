cmake_minimum_required(VERSION 3.24)

project(brocompositor VERSION 0.2.0 LANGUAGES CXX)

# Targets
#   brocompositor::core    portable window-management core (pure, no OS calls)
#   brocompositor::win     Windows shell backend + Windows.Graphics.Capture (WIN32)
#   brocompositor::mac     macOS shell backend + ScreenCaptureKit capture (APPLE)
#   brocompositor::wayland Linux Wayland server role (wlroots 0.18)
#   brocompositor::vulkan  optional Vulkan importer for the surface contract
#   brocompositor::brocompositor  everything available on this platform

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_bc_top_level OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_bc_top_level ON)
endif()

option(BROCOMPOSITOR_BUILD_TESTS "Build the brocompositor tests" ${_bc_top_level})

# Coverage. Scoped to a top-level build: a consumer that add_subdirectory()s
# brocompositor has no business having its own build instrumented from in here.
option(BROCOMPOSITOR_COVERAGE "Instrument the build for gcov coverage (GCC/Clang)" OFF)
if(BROCOMPOSITOR_COVERAGE AND _bc_top_level)
    if(MSVC)
        message(FATAL_ERROR "BROCOMPOSITOR_COVERAGE requires GCC or Clang (gcov instrumentation)")
    endif()
    # Atomic counters: the server, the workers and the tests run threads, and
    # racing plain counters leave negative branch counts gcov cannot read.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
    message(STATUS "brocompositor: coverage instrumentation ON (-O0)")
endif()

if(MSVC AND _bc_top_level)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(brocompositor_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ---------------------------------------------------------------- core
add_library(brocompositor_core STATIC
    src/core/layout.cpp
    src/core/window_manager.cpp
)
add_library(brocompositor::core ALIAS brocompositor_core)
target_include_directories(brocompositor_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
brocompositor_warnings(brocompositor_core)

add_library(brocompositor INTERFACE)
add_library(brocompositor::brocompositor ALIAS brocompositor)
target_link_libraries(brocompositor INTERFACE brocompositor_core)

# ---------------------------------------------------------------- shell-role common
# Shared by the shell backends (Windows, macOS): the crash-recovery journal
# and the per-process operation workers. Portable C++ (std::filesystem,
# std::thread); private to the backends, built (and unit-tested) everywhere.
find_package(Threads REQUIRED)
add_library(brocompositor_shell_common STATIC
    src/shell/journal.cpp
    src/shell/serial_workers.cpp
)
target_include_directories(brocompositor_shell_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries(brocompositor_shell_common PUBLIC brocompositor_core Threads::Threads)
set_target_properties(brocompositor_shell_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
brocompositor_warnings(brocompositor_shell_common)

# ---------------------------------------------------------------- brodisplays
# Display topology. On Windows and macOS the OS owns the displays and the
# shell backends observe them through brodisplays (which ones exist, their
# geometry, DPI, primary, and change events). On Linux brocompositor is the
# display server: it owns its outputs and publishes them (wl_output,
# wlr-output-management, wlr-gamma-control), and brodisplays is one of its
# clients; there it is needed only by the integration test.
# Resolved like every bro sibling: an existing `brodisplays` target (a
# superbuild already added it) wins; otherwise a standalone checkout beside
# the top-level project (../brodisplays, overridable with
# -DBRODISPLAYS_DIR=<path>); otherwise the top-level project's
# third_party/brodisplays submodule (`git submodule update --init --recursive`).
set(_bc_need_brodisplays OFF)
if(WIN32 OR APPLE)
    set(_bc_need_brodisplays ON)
endif()
set(_bc_brodisplays_src "")
if(NOT TARGET brodisplays)
    set(BRODISPLAYS_DIR "${CMAKE_SOURCE_DIR}/../brodisplays" CACHE PATH
        "Path to standalone brodisplays repo (set to a missing path to force submodule fallback)")
    if(EXISTS "${BRODISPLAYS_DIR}/CMakeLists.txt")
        set(_bc_brodisplays_src "${BRODISPLAYS_DIR}")
        set(_bc_brodisplays_how "")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/brodisplays/CMakeLists.txt")
        set(_bc_brodisplays_src "${CMAKE_SOURCE_DIR}/third_party/brodisplays")
        set(_bc_brodisplays_how " (submodule)")
    endif()
    if(_bc_need_brodisplays)
        if(NOT _bc_brodisplays_src)
            message(FATAL_ERROR
                "brodisplays not found at ${BRODISPLAYS_DIR} or ${CMAKE_SOURCE_DIR}/third_party/brodisplays. "
                "Clone https://github.com/wlejon/brodisplays next to this repo, run "
                "`git submodule update --init --recursive`, or override with -DBRODISPLAYS_DIR=...")
        endif()
        message(STATUS "brocompositor: brodisplays from ${_bc_brodisplays_src}${_bc_brodisplays_how}")
        set(BRODISPLAYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_bc_brodisplays_src}" "${CMAKE_BINARY_DIR}/brodisplays" EXCLUDE_FROM_ALL)
    endif()
endif()

# ---------------------------------------------------------------- Windows
if(WIN32)
    add_library(brocompositor_win STATIC
        src/win/util.cpp
        src/win/monitors.cpp
        src/win/window_query.cpp
        src/win/shell_backend.cpp
        src/win/shell_tracking.cpp
        src/win/window_ops.cpp
        src/win/recovery.cpp
        src/win/appbar.cpp
        src/win/capture_device.cpp
        src/win/window_capture.cpp
    )
    add_library(brocompositor::win ALIAS brocompositor_win)
    target_link_libraries(brocompositor_win
        PUBLIC brocompositor_core
        PRIVATE brocompositor_shell_common brodisplays::brodisplays
                user32 shell32 ole32 dwmapi shcore d3d11 dxgi windowsapp)
    target_compile_definitions(brocompositor_win PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_include_directories(brocompositor_win PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    brocompositor_warnings(brocompositor_win)
    if(MSVC)
        # C++/WinRT headers (Windows.Graphics.Capture) are template-heavy.
        target_compile_options(brocompositor_win PRIVATE /bigobj)
    endif()
    target_link_libraries(brocompositor INTERFACE brocompositor_win)
endif()

# ---------------------------------------------------------------- macOS
# Shell role over WindowServer (CGWindowList + Accessibility) and
# ScreenCaptureKit capture into IOSurface/Metal. Objective-C++ stays in the
# .mm files (appkit, metal_ring, window_capture); the rest is C++ over the
# CoreFoundation / CoreGraphics / AX C APIs.
if(APPLE)
    enable_language(OBJCXX)
    add_library(brocompositor_mac STATIC
        src/mac/system.cpp
        src/mac/appkit.mm
        src/mac/displays.cpp
        src/mac/permissions.cpp
        src/mac/ax.cpp
        src/mac/app_worker.cpp
        src/mac/shell_backend.cpp
        src/mac/shell_tracking.cpp
        src/mac/window_ops.cpp
        src/mac/recovery.cpp
        src/mac/metal_ring.mm
        src/mac/window_capture.mm
    )
    add_library(brocompositor::mac ALIAS brocompositor_mac)
    set_target_properties(brocompositor_mac PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(brocompositor_mac PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_include_directories(brocompositor_mac PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(brocompositor_mac
        PUBLIC brocompositor_core
        PRIVATE brocompositor_shell_common brodisplays::brodisplays
        "-framework CoreFoundation" "-framework CoreGraphics" "-framework ApplicationServices"
        "-framework AppKit" "-framework IOSurface" "-framework Metal" "-framework CoreMedia"
        "-framework CoreVideo" "-weak_framework ScreenCaptureKit")
    brocompositor_warnings(brocompositor_mac)
    target_link_libraries(brocompositor INTERFACE brocompositor_mac)
endif()

# ---------------------------------------------------------------- Linux: Wayland server role
# wlroots 0.18 as a protocol / backend / input / session toolkit (no
# wlr_renderer, no wlr_scene). Protocol headers are generated here.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(BC_WLR QUIET IMPORTED_TARGET
            wlroots-0.18 wayland-server wayland-client xkbcommon pixman-1 libdrm gbm)
        pkg_get_variable(BC_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    endif()
    find_program(BC_WAYLAND_SCANNER wayland-scanner)
    set(_bc_wl_default OFF)
    if(BC_WLR_FOUND AND BC_WAYLAND_SCANNER AND BC_WAYLAND_PROTOCOLS_DIR)
        set(_bc_wl_default ON)
    endif()
    option(BROCOMPOSITOR_WITH_WAYLAND "Build the Wayland server backend (wlroots 0.18)" ${_bc_wl_default})
endif()

if(BROCOMPOSITOR_WITH_WAYLAND)
    if(NOT BC_WLR_FOUND OR NOT BC_WAYLAND_SCANNER OR NOT BC_WAYLAND_PROTOCOLS_DIR)
        message(FATAL_ERROR "BROCOMPOSITOR_WITH_WAYLAND needs wlroots-0.18, wayland-protocols, wayland-scanner, "
                            "xkbcommon, pixman-1, libdrm and gbm (pkg-config)")
    endif()
    enable_language(C)
    find_package(Threads REQUIRED)
    set(BC_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
    file(MAKE_DIRECTORY ${BC_PROTO_DIR})
    set(BC_PROTO_SOURCES "")
    # bc_wl_protocol(<xml> <base>): <base>-protocol.h (server), <base>-client-protocol.h,
    # <base>-protocol.c (interfaces, for clients; wlroots carries its own).
    function(bc_wl_protocol xml base)
        add_custom_command(
            OUTPUT ${BC_PROTO_DIR}/${base}-protocol.h ${BC_PROTO_DIR}/${base}-client-protocol.h
                   ${BC_PROTO_DIR}/${base}-protocol.c
            COMMAND ${BC_WAYLAND_SCANNER} server-header ${xml} ${BC_PROTO_DIR}/${base}-protocol.h
            COMMAND ${BC_WAYLAND_SCANNER} client-header ${xml} ${BC_PROTO_DIR}/${base}-client-protocol.h
            COMMAND ${BC_WAYLAND_SCANNER} private-code ${xml} ${BC_PROTO_DIR}/${base}-protocol.c
            DEPENDS ${xml} VERBATIM)
        set(BC_PROTO_SOURCES ${BC_PROTO_SOURCES} ${BC_PROTO_DIR}/${base}-protocol.h
            ${BC_PROTO_DIR}/${base}-client-protocol.h ${BC_PROTO_DIR}/${base}-protocol.c PARENT_SCOPE)
    endfunction()
    set(_wp ${BC_WAYLAND_PROTOCOLS_DIR})
    bc_wl_protocol(${_wp}/stable/xdg-shell/xdg-shell.xml xdg-shell)
    bc_wl_protocol(${_wp}/stable/viewporter/viewporter.xml viewporter)
    bc_wl_protocol(${_wp}/stable/presentation-time/presentation-time.xml presentation-time)
    bc_wl_protocol(${_wp}/stable/linux-dmabuf/linux-dmabuf-v1.xml linux-dmabuf-v1)
    bc_wl_protocol(${_wp}/staging/fractional-scale/fractional-scale-v1.xml fractional-scale-v1)
    bc_wl_protocol(${_wp}/staging/cursor-shape/cursor-shape-v1.xml cursor-shape-v1)
    bc_wl_protocol(${_wp}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml xdg-decoration-unstable-v1)
    bc_wl_protocol(${_wp}/unstable/tablet/tablet-unstable-v2.xml tablet-v2)
    bc_wl_protocol(${_wp}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
        pointer-constraints-unstable-v1)
    bc_wl_protocol(${_wp}/unstable/relative-pointer/relative-pointer-unstable-v1.xml relative-pointer-unstable-v1)
    bc_wl_protocol(${_wp}/unstable/text-input/text-input-unstable-v3.xml text-input-unstable-v3)
    bc_wl_protocol(${_wp}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml
        keyboard-shortcuts-inhibit-unstable-v1)
    bc_wl_protocol(${_wp}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml idle-inhibit-unstable-v1)
    bc_wl_protocol(${_wp}/staging/ext-idle-notify/ext-idle-notify-v1.xml ext-idle-notify-v1)
    bc_wl_protocol(${_wp}/staging/ext-session-lock/ext-session-lock-v1.xml ext-session-lock-v1)
    bc_wl_protocol(${_wp}/staging/xdg-activation/xdg-activation-v1.xml xdg-activation-v1)
    bc_wl_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1)
    bc_wl_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/input-method-unstable-v2.xml input-method-unstable-v2)

    # Protocols the server implements itself (wlroots 0.18 has no
    # renderer-free screencopy, no ext-image-copy-capture, and its gamma
    # control needs a hardware LUT): their interface code is linked into the
    # server.
    set(BC_SERVER_PROTO_SOURCES "")
    set(_bc_proto_before ${BC_PROTO_SOURCES})
    bc_wl_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1)
    bc_wl_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-gamma-control-unstable-v1.xml
        wlr-gamma-control-unstable-v1)
    bc_wl_protocol(${_wp}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
        ext-foreign-toplevel-list-v1)
    bc_wl_protocol(${_wp}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
        ext-image-capture-source-v1)
    bc_wl_protocol(${_wp}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml ext-image-copy-capture-v1)
    list(REMOVE_ITEM BC_PROTO_SOURCES ${_bc_proto_before})
    set(BC_SERVER_PROTO_SOURCES ${BC_PROTO_SOURCES})
    set(BC_PROTO_SOURCES ${_bc_proto_before} ${BC_SERVER_PROTO_SOURCES})

    add_library(brocompositor_wl_server_protocols STATIC ${BC_SERVER_PROTO_SOURCES})
    target_include_directories(brocompositor_wl_server_protocols PUBLIC ${BC_PROTO_DIR})
    target_link_libraries(brocompositor_wl_server_protocols PUBLIC PkgConfig::BC_WLR)
    set_target_properties(brocompositor_wl_server_protocols PROPERTIES LINKER_LANGUAGE C
        POSITION_INDEPENDENT_CODE ON)

    # Client-side protocol code for test clients and hosts that speak Wayland.
    add_library(brocompositor_wl_protocols STATIC ${_bc_proto_before})
    target_include_directories(brocompositor_wl_protocols PUBLIC ${BC_PROTO_DIR})
    target_link_libraries(brocompositor_wl_protocols PUBLIC PkgConfig::BC_WLR brocompositor_wl_server_protocols)
    set_target_properties(brocompositor_wl_protocols PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)

    add_library(brocompositor_wayland STATIC
        src/linux/dispatcher.cpp
        src/linux/drm_util.cpp
        src/linux/cpu_mapping.cpp
        src/linux/client_surface.cpp
        src/linux/surfaces.cpp
        src/linux/xdg.cpp
        src/linux/layers.cpp
        src/linux/outputs.cpp
        src/linux/output_images.cpp
        src/linux/output_manager.cpp
        src/linux/seat.cpp
        src/linux/selection.cpp
        src/linux/setup.cpp
        src/linux/server.cpp
        src/linux/server_session.cpp
        src/linux/windows.cpp
        src/linux/xwayland.cpp
        src/linux/foreign_toplevel.cpp
        src/linux/touch_tablet.cpp
        src/linux/pointer_extras.cpp
        src/linux/text_input.cpp
        src/linux/session_lock.cpp
        src/linux/idle.cpp
        src/linux/capture.cpp
        src/linux/screencopy.cpp
        src/linux/image_copy_capture.cpp
        src/linux/gamma.cpp
    )
    add_library(brocompositor::wayland ALIAS brocompositor_wayland)
    add_dependencies(brocompositor_wayland brocompositor_wl_protocols brocompositor_wl_server_protocols)
    target_include_directories(brocompositor_wayland PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${BC_PROTO_DIR})
    target_compile_definitions(brocompositor_wayland PRIVATE WLR_USE_UNSTABLE)
    pkg_get_variable(BC_WLR_HAVE_XWAYLAND wlroots-0.18 have_xwayland)
    if(BC_WLR_HAVE_XWAYLAND STREQUAL "true")
        target_compile_definitions(brocompositor_wayland PRIVATE BC_HAVE_XWAYLAND=1)
    endif()
    target_link_libraries(brocompositor_wayland PUBLIC brocompositor_core
        PRIVATE PkgConfig::BC_WLR brocompositor_wl_server_protocols Threads::Threads)
    brocompositor_warnings(brocompositor_wayland)
    target_link_libraries(brocompositor INTERFACE brocompositor_wayland)
endif()

# ---------------------------------------------------------------- Vulkan importer
# Header-only dependency on the Vulkan headers; the loader is never linked
# (the host passes vkGetInstanceProcAddr), so bro keeps owning its loader.
set(_bc_vk_default OFF)
find_package(Vulkan QUIET)
find_path(BROCOMPOSITOR_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS "${Vulkan_INCLUDE_DIR}" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
if((WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND BROCOMPOSITOR_VULKAN_INCLUDE_DIR)
    set(_bc_vk_default ON)
endif()
option(BROCOMPOSITOR_WITH_VULKAN
    "Build the Vulkan surface importer (Win32 D3D11 handles; Linux dmabuf + sync_file; macOS IOSurface + MTLSharedEvent via MoltenVK)"
    ${_bc_vk_default})

if(BROCOMPOSITOR_WITH_VULKAN)
    if(NOT BROCOMPOSITOR_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "BROCOMPOSITOR_WITH_VULKAN needs the Vulkan headers (VULKAN_SDK or CMAKE_PREFIX_PATH)")
    endif()
    if(WIN32)
        set(_bc_vk_platform src/vulkan/importer_win32.cpp)
    elseif(APPLE)
        set(_bc_vk_platform src/vulkan/importer_apple.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_bc_vk_platform src/vulkan/importer_linux.cpp)
    else()
        message(FATAL_ERROR "The Vulkan importer implements Win32, Linux and Apple handle types only")
    endif()
    add_library(brocompositor_vulkan STATIC src/vulkan/importer.cpp ${_bc_vk_platform})
    add_library(brocompositor::vulkan ALIAS brocompositor_vulkan)
    target_include_directories(brocompositor_vulkan PUBLIC
        $<BUILD_INTERFACE:${BROCOMPOSITOR_VULKAN_INCLUDE_DIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(brocompositor_vulkan PUBLIC VK_NO_PROTOTYPES)
    if(WIN32)
        target_compile_definitions(brocompositor_vulkan PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_link_libraries(brocompositor_vulkan PUBLIC brocompositor_core)
    brocompositor_warnings(brocompositor_vulkan)
    if(NOT MSVC)
        # `VkFoo info{VK_STRUCTURE_TYPE_FOO}` value-initializes the rest; the
        # -Wextra warning about it is noise for Vulkan code.
        target_compile_options(brocompositor_vulkan PRIVATE -Wno-missing-field-initializers)
    endif()
    target_link_libraries(brocompositor INTERFACE brocompositor_vulkan)
endif()

# Linux: brodisplays only for the test that runs it as a client of this
# server (skipped when the repo or its X11 dependencies are missing).
if(BROCOMPOSITOR_BUILD_TESTS AND BROCOMPOSITOR_WITH_WAYLAND AND NOT TARGET brodisplays
   AND _bc_brodisplays_src)
    pkg_check_modules(BC_BRODISPLAYS_DEPS QUIET xcb xcb-randr xau)
    if(BC_BRODISPLAYS_DEPS_FOUND)
        message(STATUS "brocompositor: brodisplays (tests) from ${_bc_brodisplays_src}${_bc_brodisplays_how}")
        set(BRODISPLAYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_bc_brodisplays_src}" "${CMAKE_BINARY_DIR}/brodisplays" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brocompositor: test_wl_brodisplays skipped (brodisplays needs xcb, xcb-randr, xau)")
    endif()
endif()

if(BROCOMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
