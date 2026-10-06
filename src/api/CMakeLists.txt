add_library(brocompositor_api STATIC
    api.cpp
    host_class.cpp
    native_windows.cpp
    native_workspaces.cpp
    native_events.cpp
)

target_include_directories(brocompositor_api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)

target_link_libraries(brocompositor_api PUBLIC
    brocompositor_core
    bronze_runtime_shared
)

target_compile_features(brocompositor_api PUBLIC cxx_std_20)

brocompositor_warnings(brocompositor_api)
