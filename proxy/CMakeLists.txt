cmake_minimum_required(VERSION 3.20)
project(roboblitz_remix_proxy LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "RoboBlitz is a 32-bit game: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the proxy must not depend on a VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /permissive-)

# Game-independent logic, shared with the tests
add_library(proxy_core STATIC
    src/shader_info.cpp
    src/camera.cpp
    src/material_albedo.cpp
    src/material_map.cpp
    src/light_conversion.cpp
    src/dds.cpp
)
target_include_directories(proxy_core PUBLIC src)

# The d3d9.dll proxy. Do not link d3d9.lib: the real d3d9 is loaded at runtime.
add_library(d3d9_proxy SHARED
    src/main.cpp
    src/hooks.cpp
    src/compiler_hook.cpp
    src/config.cpp
    src/log.cpp
    src/material_dump.cpp
    src/texture_capture.cpp
    d3d9.def
)
set_target_properties(d3d9_proxy PROPERTIES OUTPUT_NAME d3d9)
target_link_libraries(d3d9_proxy PRIVATE proxy_core)

enable_testing()
add_executable(proxy_tests tests/proxy_tests.cpp)
target_link_libraries(proxy_tests PRIVATE proxy_core)
add_test(NAME proxy_tests COMMAND proxy_tests)
