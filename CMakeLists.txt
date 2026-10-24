cmake_minimum_required(VERSION 3.16)
project(KolosLinux LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Build KolosLinux on Linux; the tested target is ARM64 Debian")
endif()

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

add_library(kolos_prototype STATIC
    src/vulkan_context.cpp
    src/ahb_import.cpp
    src/buffer_pool.cpp
    src/diagnostic_renderer.cpp
    src/graphics_wire.cpp
    src/graphics_ahb_import.cpp
)
target_compile_features(kolos_prototype PUBLIC cxx_std_17)
target_include_directories(kolos_prototype PUBLIC include protocol)
target_link_libraries(kolos_prototype PUBLIC Vulkan::Vulkan)

add_executable(kolos_ahb_stream tools/kolos_ahb_stream.cpp)
target_link_libraries(kolos_ahb_stream PRIVATE kolos_prototype Threads::Threads)

add_executable(kolos_ahb_import tools/kolos_ahb_import.cpp)
target_compile_features(kolos_ahb_import PRIVATE cxx_std_17)
target_link_libraries(kolos_ahb_import PRIVATE Vulkan::Vulkan)

add_executable(kolos_graphics_smoke tools/kolos_graphics_smoke.cpp)
target_link_libraries(kolos_graphics_smoke PRIVATE kolos_prototype Threads::Threads)
