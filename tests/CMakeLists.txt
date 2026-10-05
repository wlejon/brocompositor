function(bc_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    brocompositor_warnings(${name})
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endfunction()

# Portable core: runs everywhere.
bc_test(test_layout SOURCES core/test_layout.cpp LIBS brocompositor_core)
bc_test(test_window_manager SOURCES core/test_window_manager.cpp LIBS brocompositor_core)
bc_test(test_event_queue SOURCES core/test_event_queue.cpp LIBS brocompositor_core)

if(TARGET brocompositor_wayland)
    # The scripted Wayland client the server tests drive (plus real
    # third-party clients when installed: weston demos, foot, wl-clipboard,
    # wlr-randr, GTK).
    add_executable(bc_wl_client linux/wl_client.cpp linux/wl_client_buffers.cpp)
    target_include_directories(bc_wl_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(bc_wl_client PRIVATE brocompositor_wl_protocols PkgConfig::BC_WLR)
    brocompositor_warnings(bc_wl_client)

    # Clients for the desktop-session protocols: newer input protocols on a
    # window, and the session roles (input method, lock screen, toplevel
    # list, image copy capture).
    add_executable(bc_wl_input_client linux/wl_input_client.cpp linux/wl_client_buffers.cpp)
    add_executable(bc_wl_session_client linux/wl_session_client.cpp)
    foreach(c bc_wl_input_client bc_wl_session_client)
        target_include_directories(${c} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${c} PRIVATE brocompositor_wl_protocols PkgConfig::BC_WLR)
        brocompositor_warnings(${c})
    endforeach()

    add_library(bc_wl_harness STATIC linux/wl_harness.cpp linux/wl_harness_desktop.cpp linux/child.cpp)
    target_include_directories(bc_wl_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(bc_wl_harness PUBLIC BC_WL_CLIENT="$<TARGET_FILE:bc_wl_client>"
        BC_WL_INPUT_CLIENT="$<TARGET_FILE:bc_wl_input_client>"
        BC_WL_SESSION_CLIENT="$<TARGET_FILE:bc_wl_session_client>")
    target_link_libraries(bc_wl_harness PUBLIC brocompositor_wayland brocompositor_core Threads::Threads)
    add_dependencies(bc_wl_harness bc_wl_client bc_wl_input_client bc_wl_session_client)
    brocompositor_warnings(bc_wl_harness)

    # A plain-xcb X11 client for the XWayland tests (when xcb is available).
    pkg_check_modules(BC_XCB IMPORTED_TARGET xcb)
    if(BC_XCB_FOUND)
        add_executable(bc_x11_client linux/x11_client.cpp)
        target_link_libraries(bc_x11_client PRIVATE PkgConfig::BC_XCB)
        brocompositor_warnings(bc_x11_client)
        target_compile_definitions(bc_wl_harness PUBLIC BC_X11_CLIENT="$<TARGET_FILE:bc_x11_client>")
        add_dependencies(bc_wl_harness bc_x11_client)
    endif()

    foreach(t window input clipboard layer outputs frames clients nested drm lock xwayland xselection
            input_protocols text_input capture taskbar session)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/linux/test_wl_${t}.cpp)
            continue()
        endif()
        bc_test(test_wl_${t} SOURCES linux/test_wl_${t}.cpp LIBS bc_wl_harness)
    endforeach()

    if(TARGET brocompositor_vulkan)
        # dmabuf <-> Vulkan on every capable device (GPU, lavapipe).
        bc_test(test_wl_vulkan SOURCES linux/test_wl_vulkan.cpp linux/vk_host.cpp
            LIBS bc_wl_harness brocompositor_vulkan PkgConfig::BC_WLR ${CMAKE_DL_LIBS})
        target_compile_options(test_wl_vulkan PRIVATE -Wno-missing-field-initializers)
    endif()
endif()

if(WIN32)
    # A separate process that owns the windows the backend tests manage, so
    # every test exercises the real cross-process (foreign window) paths and
    # never touches a window it did not create.
    add_executable(bc_test_app win/test_app.cpp)
    target_compile_definitions(bc_test_app PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(bc_test_app PRIVATE user32 gdi32 dwmapi)
    brocompositor_warnings(bc_test_app)

    add_library(bc_win_harness STATIC win/harness.cpp)
    target_include_directories(bc_win_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(bc_win_harness PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        BC_TEST_APP="$<TARGET_FILE:bc_test_app>")
    target_link_libraries(bc_win_harness PUBLIC brocompositor_win user32 dwmapi)
    add_dependencies(bc_win_harness bc_test_app)
    brocompositor_warnings(bc_win_harness)

    foreach(t lifecycle ops focus appbar integration capture)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/win/test_win_${t}.cpp)
            continue()
        endif()
        bc_test(test_win_${t} SOURCES win/test_win_${t}.cpp LIBS bc_win_harness)
        # The backend tests drive real desktop state (foreground, work
        # area); run them one at a time.
        set_tests_properties(test_win_${t} PROPERTIES RUN_SERIAL TRUE)
    endforeach()
    if(TARGET test_win_capture)
        target_link_libraries(test_win_capture PRIVATE d3d11 dxgi)
    endif()

    if(TARGET brocompositor_vulkan)
        bc_test(test_vulkan_import SOURCES win/test_vulkan_import.cpp win/vk_context.cpp
            LIBS bc_win_harness brocompositor_vulkan)
        set_tests_properties(test_vulkan_import PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()
