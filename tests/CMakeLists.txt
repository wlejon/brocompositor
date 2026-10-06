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
# Shell-role plumbing (crash-recovery journal, per-process workers): portable.
bc_test(test_shell_common SOURCES core/test_shell_common.cpp LIBS brocompositor_shell_common)

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
    target_include_directories(bc_wl_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
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

    bc_test(test_direct_drm SOURCES linux/test_direct_drm.cpp
        LIBS bc_wl_harness brodmabuf::brodmabuf broseat::broseat brocompositor_wl_server_protocols PkgConfig::BC_WLR)
    target_compile_definitions(test_direct_drm PRIVATE WLR_USE_UNSTABLE)

    if(TARGET brodisplays)
        # brodisplays as a client of this server: topology, mode changes,
        # test-then-revert, night light as gamma.
        bc_test(test_wl_brodisplays SOURCES linux/test_wl_brodisplays.cpp LIBS bc_wl_harness brodisplays::brodisplays)
    endif()

    if(TARGET brocompositor_vulkan)
        # dmabuf <-> Vulkan on every capable device (GPU, lavapipe).
        bc_test(test_wl_vulkan SOURCES linux/test_wl_vulkan.cpp linux/vk_host.cpp
            LIBS bc_wl_harness brocompositor_vulkan PkgConfig::BC_WLR ${CMAKE_DL_LIBS})
        target_compile_options(test_wl_vulkan PRIVATE -Wno-missing-field-initializers)
    endif()
endif()

if(APPLE)
    # A regular AppKit application that owns the windows the backend tests
    # manage (every test works on foreign windows it created itself).
    add_executable(bc_mac_test_app mac/test_app.mm)
    target_compile_options(bc_mac_test_app PRIVATE -fobjc-arc)
    set_target_properties(bc_mac_test_app PROPERTIES OBJCXX_STANDARD 20)
    target_link_libraries(bc_mac_test_app PRIVATE "-framework AppKit")

    add_library(bc_mac_harness STATIC mac/harness.cpp)
    target_include_directories(bc_mac_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(bc_mac_harness PUBLIC BC_MAC_TEST_APP="$<TARGET_FILE:bc_mac_test_app>")
    target_link_libraries(bc_mac_harness PUBLIC brocompositor_mac "-framework CoreGraphics"
        "-framework ApplicationServices")
    add_dependencies(bc_mac_harness bc_mac_test_app)
    brocompositor_warnings(bc_mac_harness)

    # A shell host the recovery test kills with SIGKILL.
    add_executable(bc_mac_shell_child mac/shell_child.cpp)
    target_include_directories(bc_mac_shell_child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(bc_mac_shell_child PRIVATE brocompositor_mac)
    brocompositor_warnings(bc_mac_shell_child)

    foreach(t permissions lifecycle displays ops focus recovery)
        bc_test(test_mac_${t} SOURCES mac/test_mac_${t}.cpp LIBS bc_mac_harness)
        # They drive real desktop state (windows, focus); one at a time.
        set_tests_properties(test_mac_${t} PROPERTIES RUN_SERIAL TRUE)
    endforeach()
    target_compile_definitions(test_mac_recovery PRIVATE BC_MAC_SHELL_CHILD="$<TARGET_FILE:bc_mac_shell_child>")
    add_dependencies(test_mac_recovery bc_mac_shell_child)

    if(TARGET brocompositor_vulkan)
        bc_test(test_mac_surface_vulkan SOURCES mac/test_mac_surface_vulkan.cpp mac/vk_mac.cpp
            LIBS brocompositor_mac brocompositor_vulkan "-framework IOSurface" "-framework CoreFoundation")
        bc_test(test_mac_capture SOURCES mac/test_mac_capture.cpp mac/vk_mac.cpp
            LIBS bc_mac_harness brocompositor_vulkan)
        foreach(t test_mac_surface_vulkan test_mac_capture)
            target_compile_options(${t} PRIVATE -Wno-missing-field-initializers)
            set_tests_properties(${t} PROPERTIES RUN_SERIAL TRUE)
        endforeach()
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

    # A shell host the recovery test kills with TerminateProcess.
    add_executable(bc_win_shell_child win/shell_child.cpp)
    target_include_directories(bc_win_shell_child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(bc_win_shell_child PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(bc_win_shell_child PRIVATE brocompositor_win)
    brocompositor_warnings(bc_win_shell_child)

    foreach(t lifecycle ops focus appbar integration capture recovery)
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
    target_compile_definitions(test_win_recovery PRIVATE BC_WIN_SHELL_CHILD="$<TARGET_FILE:bc_win_shell_child>")
    target_link_libraries(test_win_recovery PRIVATE shell32)
    add_dependencies(test_win_recovery bc_win_shell_child)

    if(TARGET brocompositor_vulkan)
        bc_test(test_vulkan_import SOURCES win/test_vulkan_import.cpp win/vk_context.cpp
            LIBS bc_win_harness brocompositor_vulkan)
        set_tests_properties(test_vulkan_import PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

if(BROCOMPOSITOR_ENABLE_API)
    add_executable(brocompositor_test_api
        test_compositor_api.cpp
    )
    target_include_directories(brocompositor_test_api PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/api
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(brocompositor_test_api PRIVATE
        brocompositor_api
        brocompositor_core
        bronze_runtime_shared
    )
    brocompositor_warnings(brocompositor_test_api)
    if(WIN32)
        target_compile_definitions(brocompositor_test_api PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        target_link_libraries(brocompositor_test_api PRIVATE user32 shcore)
        add_custom_command(TARGET brocompositor_test_api POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:brocompositor_test_api>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared>
                $<TARGET_FILE_DIR:brocompositor_test_api>
        )
    endif()

    if(NOT BRASS_HOST_BACKEND)
        message(STATUS "brocompositor: brocompositor_test_api not registered (brass has no code generator for this target)")
    else()
        add_test(NAME brocompositor_test_api COMMAND brocompositor_test_api)
        set_tests_properties(brocompositor_test_api PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

        # A collection at every allocation over a Debug bronze (-O0, and
        # instrumented in the coverage job) takes minutes, not seconds. The
        # limit is there to catch a hang, not to time the run.
        add_test(NAME brocompositor_test_api_gc_stress COMMAND brocompositor_test_api)
        set_tests_properties(brocompositor_test_api_gc_stress PROPERTIES
            TIMEOUT 600
            SKIP_RETURN_CODE 77
            ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1"
        )
        if(WIN32)
            set_tests_properties(brocompositor_test_api PROPERTIES
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:brocompositor_test_api>;$ENV{PATH}")
            set_property(TEST brocompositor_test_api_gc_stress APPEND PROPERTY
                ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bronze_runtime_shared>;$<TARGET_FILE_DIR:brocompositor_test_api>;$ENV{PATH}")
        endif()
    endif()
endif()

# Every test executable links no_dialogs.cpp (see the file).
if(WIN32)
    get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _test_targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            target_sources(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no_dialogs.cpp)
        endif()
    endforeach()
endif()

