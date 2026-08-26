cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_surface_renderer_only_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vnm_terminal_surface CONFIG REQUIRED)

if(VNM_TERMINAL_SURFACE_FULL_TARGET_AVAILABLE)
    message(FATAL_ERROR "Renderer-only package reports the full target available")
endif()
if(TARGET vnm_terminal_surface::vnm_terminal_surface)
    message(FATAL_ERROR "Renderer-only package exported the full surface target")
endif()
if(TARGET vnm_qt_dispatch::vnm_qt_dispatch)
    message(FATAL_ERROR "Renderer-only package resolved vnm_qt_dispatch")
endif()
if(NOT TARGET vnm_terminal_surface::vnm_terminal_surface_renderer)
    message(FATAL_ERROR "Renderer-only package omitted its renderer target")
endif()

add_executable(vnm_terminal_surface_renderer_only_package_smoke
    main.cpp
)

target_link_libraries(
    vnm_terminal_surface_renderer_only_package_smoke
    PRIVATE
        vnm_terminal_surface::vnm_terminal_surface_renderer
)
