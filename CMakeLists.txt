cmake_minimum_required(VERSION 3.24)

project(brocompositor VERSION 0.2.0 LANGUAGES CXX)

# Targets
#   brocompositor::core    portable window-management core (pure, no OS calls)
#   brocompositor::win     Windows shell backend + Windows.Graphics.Capture (WIN32)
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

# ---------------------------------------------------------------- Windows
if(WIN32)
    add_library(brocompositor_win STATIC
        src/win/util.cpp
        src/win/monitors.cpp
        src/win/window_query.cpp
        src/win/shell_backend.cpp
        src/win/shell_tracking.cpp
        src/win/window_ops.cpp
        src/win/appbar.cpp
        src/win/capture_device.cpp
        src/win/window_capture.cpp
    )
    add_library(brocompositor::win ALIAS brocompositor_win)
    target_link_libraries(brocompositor_win
        PUBLIC brocompositor_core
        PRIVATE user32 shell32 dwmapi shcore d3d11 dxgi windowsapp)
    target_compile_definitions(brocompositor_win PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_include_directories(brocompositor_win PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    brocompositor_warnings(brocompositor_win)
    if(MSVC)
        # C++/WinRT headers (Windows.Graphics.Capture) are template-heavy.
        target_compile_options(brocompositor_win PRIVATE /bigobj)
    endif()
    target_link_libraries(brocompositor INTERFACE brocompositor_win)
endif()

# ---------------------------------------------------------------- Vulkan importer
# Header-only dependency on the Vulkan headers; the loader is never linked
# (the host passes vkGetInstanceProcAddr), so bro keeps owning its loader.
set(_bc_vk_default OFF)
find_package(Vulkan QUIET)
find_path(BROCOMPOSITOR_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS "${Vulkan_INCLUDE_DIR}" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
if(WIN32 AND BROCOMPOSITOR_VULKAN_INCLUDE_DIR)
    set(_bc_vk_default ON)
endif()
option(BROCOMPOSITOR_WITH_VULKAN "Build the Vulkan surface importer (Windows for now)" ${_bc_vk_default})

if(BROCOMPOSITOR_WITH_VULKAN)
    if(NOT BROCOMPOSITOR_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "BROCOMPOSITOR_WITH_VULKAN needs the Vulkan headers (VULKAN_SDK or CMAKE_PREFIX_PATH)")
    endif()
    if(NOT WIN32)
        message(FATAL_ERROR "The Vulkan importer currently implements the Win32 handle types only")
    endif()
    add_library(brocompositor_vulkan STATIC src/vulkan/importer.cpp)
    add_library(brocompositor::vulkan ALIAS brocompositor_vulkan)
    target_include_directories(brocompositor_vulkan PUBLIC
        $<BUILD_INTERFACE:${BROCOMPOSITOR_VULKAN_INCLUDE_DIR}>)
    target_compile_definitions(brocompositor_vulkan PUBLIC VK_NO_PROTOTYPES
        PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(brocompositor_vulkan PUBLIC brocompositor_core)
    brocompositor_warnings(brocompositor_vulkan)
    target_link_libraries(brocompositor INTERFACE brocompositor_vulkan)
endif()

if(BROCOMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
