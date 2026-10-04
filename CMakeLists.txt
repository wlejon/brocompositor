cmake_minimum_required(VERSION 3.24)

project(brocompositor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Respect memory-conscious compile & link job bounds
if(NOT DEFINED BRO_COMPILE_JOBS)
    set(BRO_COMPILE_JOBS 4 CACHE STRING "Maximum parallel compile jobs")
endif()
if(NOT DEFINED BRO_LINK_JOBS)
    set(BRO_LINK_JOBS 2 CACHE STRING "Maximum parallel link jobs")
endif()
set_property(GLOBAL PROPERTY JOB_POOLS compile_job_pool=${BRO_COMPILE_JOBS} link_job_pool=${BRO_LINK_JOBS})
set(CMAKE_JOB_POOL_COMPILE compile_job_pool)
set(CMAKE_JOB_POOL_LINK link_job_pool)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
    )
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

set(BROCOMPOSITOR_SOURCES
    src/version.cpp
    src/bsp_tree.cpp
    src/layout_engine.cpp
    src/workspace_manager.cpp
    src/appbar_manager.cpp
)

if(WIN32)
    list(APPEND BROCOMPOSITOR_SOURCES
        src/win/win_window_ops.cpp
        src/win/win_hook_manager.cpp
        src/win/win_appbar.cpp
        src/win/win_virtual_desktops.cpp
        src/win/win_capture.cpp
        src/win/win_compositor.cpp
    )
endif()

add_library(brocompositor STATIC ${BROCOMPOSITOR_SOURCES})
add_library(brocompositor::brocompositor ALIAS brocompositor)
target_compile_definitions(brocompositor PUBLIC BROCOMPOSITOR_STATIC_DEFINE)

target_include_directories(brocompositor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(brocompositor PUBLIC
        user32
        shell32
        dwmapi
        d3d11
        dxgi
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(brocompositor PUBLIC Threads::Threads)
endif()

option(BROCOMPOSITOR_BUILD_TESTS "Build brocompositor test suite" ON)
if(BROCOMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
