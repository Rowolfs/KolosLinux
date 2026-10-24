cmake_minimum_required(VERSION 3.16)
project(KolosLinux LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Build KolosLinux on Linux; the tested target is ARM64 Debian")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

add_library(kolos_graphics_wire STATIC src/graphics_wire.cpp)
target_compile_features(kolos_graphics_wire PUBLIC cxx_std_17)
target_include_directories(kolos_graphics_wire PUBLIC protocol)

add_library(kolos_prototype STATIC
    src/vulkan_context.cpp
    src/ahb_import.cpp
    src/buffer_pool.cpp
    src/diagnostic_renderer.cpp
    src/graphics_ahb_import.cpp
)
target_compile_features(kolos_prototype PUBLIC cxx_std_17)
target_include_directories(kolos_prototype PUBLIC include protocol)
target_link_libraries(kolos_prototype PUBLIC Vulkan::Vulkan kolos_graphics_wire)

add_executable(kolos_ahb_stream tools/kolos_ahb_stream.cpp)
target_link_libraries(kolos_ahb_stream PRIVATE kolos_prototype Threads::Threads)

add_executable(kolos_ahb_import tools/kolos_ahb_import.cpp)
target_compile_features(kolos_ahb_import PRIVATE cxx_std_17)
target_link_libraries(kolos_ahb_import PRIVATE Vulkan::Vulkan)

add_executable(kolos_graphics_smoke tools/kolos_graphics_smoke.cpp)
target_link_libraries(kolos_graphics_smoke PRIVATE kolos_prototype Threads::Threads)

# The compositor needs Wayland server only; it does not create a Vulkan device.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner is required for kolos-compositor")
endif()

set(GENERATED_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-protocol")
file(MAKE_DIRECTORY "${GENERATED_PROTOCOL_DIR}")
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(KOLOS_GRAPHICS_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/kolos-graphics-v1.xml")
if(NOT EXISTS "${XDG_SHELL_XML}")
    message(FATAL_ERROR "xdg-shell protocol XML not found: ${XDG_SHELL_XML}")
endif()
foreach(PROTOCOL IN ITEMS xdg-shell kolos-graphics-v1)
    if(PROTOCOL STREQUAL "xdg-shell")
        set(PROTOCOL_XML "${XDG_SHELL_XML}")
    else()
        set(PROTOCOL_XML "${KOLOS_GRAPHICS_XML}")
    endif()
    add_custom_command(
        OUTPUT "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-server-protocol.h"
               "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-protocol.c"
               "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" server-header "${PROTOCOL_XML}"
                "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-server-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" client-header "${PROTOCOL_XML}"
                "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${PROTOCOL_XML}"
                "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-protocol.c"
        DEPENDS "${PROTOCOL_XML}"
        VERBATIM
    )
    list(APPEND GENERATED_PROTOCOL_SOURCES
         "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-protocol.c"
         "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-server-protocol.h"
         "${GENERATED_PROTOCOL_DIR}/${PROTOCOL}-client-protocol.h")
endforeach()

add_library(kolos_android_graphics_bridge STATIC src/android_graphics_bridge.cpp)
target_compile_features(kolos_android_graphics_bridge PUBLIC cxx_std_17)
target_include_directories(kolos_android_graphics_bridge PUBLIC include)
target_link_libraries(kolos_android_graphics_bridge PUBLIC kolos_graphics_wire Threads::Threads)

add_executable(kolos-compositor src/kolos_compositor.cpp ${GENERATED_PROTOCOL_SOURCES})
target_compile_features(kolos-compositor PRIVATE cxx_std_17)
target_include_directories(kolos-compositor PRIVATE "${GENERATED_PROTOCOL_DIR}")
target_link_libraries(kolos-compositor PRIVATE kolos_android_graphics_bridge PkgConfig::WAYLAND_SERVER)

add_executable(kolos-wayland-probe tools/kolos_wayland_probe.cpp
    "${GENERATED_PROTOCOL_DIR}/xdg-shell-client-protocol.h"
    "${GENERATED_PROTOCOL_DIR}/xdg-shell-protocol.c")
target_compile_features(kolos-wayland-probe PRIVATE cxx_std_17)
target_include_directories(kolos-wayland-probe PRIVATE "${GENERATED_PROTOCOL_DIR}")
target_link_libraries(kolos-wayland-probe PRIVATE PkgConfig::WAYLAND_CLIENT)
