# The scripted Wayland clients the server tests drive (bc_wl_client,
# bc_wl_input_client, bc_wl_session_client). Included by tests/CMakeLists.txt,
# and on its own with BROCOMPOSITOR_BUILD_TEST_CLIENTS for a host whose own
# tests run them against its server (bro's compositor tests), so the clients
# always come from the brocompositor the host was built with.
if(TARGET brocompositor_wayland AND NOT TARGET bc_wl_client)
    set(_bc_clients_dir "${CMAKE_CURRENT_LIST_DIR}")
    set(_bc_tests_dir "${CMAKE_CURRENT_LIST_DIR}/..")

    # The general one (plus real third-party clients when installed: weston
    # demos, foot, wl-clipboard, wlr-randr, GTK).
    add_executable(bc_wl_client ${_bc_clients_dir}/wl_client.cpp ${_bc_clients_dir}/wl_client_buffers.cpp)
    # Clients for the desktop-session protocols: newer input protocols on a
    # window, and the session roles (input method, lock screen, toplevel
    # list, image copy capture).
    add_executable(bc_wl_input_client ${_bc_clients_dir}/wl_input_client.cpp ${_bc_clients_dir}/wl_client_buffers.cpp)
    add_executable(bc_wl_session_client ${_bc_clients_dir}/wl_session_client.cpp)
    foreach(c bc_wl_client bc_wl_input_client bc_wl_session_client)
        target_include_directories(${c} PRIVATE ${_bc_tests_dir})
        target_link_libraries(${c} PRIVATE brocompositor_wl_protocols PkgConfig::BC_WLR)
        brocompositor_warnings(${c})
    endforeach()
endif()
