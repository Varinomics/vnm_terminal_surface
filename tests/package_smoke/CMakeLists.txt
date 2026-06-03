cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_surface_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vnm_terminal_surface CONFIG REQUIRED)

if(NOT TARGET Qt6::GuiPrivate)
    message(FATAL_ERROR
        "vnm_terminal_surface package must find Qt6::GuiPrivate for exported private Qt API use")
endif()

if(TARGET Qt6::ShaderTools)
    message(FATAL_ERROR
        "vnm_terminal_surface package must not require Qt6::ShaderTools")
endif()

if(NOT DEFINED VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR OR
    NOT DEFINED VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR)
    message(FATAL_ERROR
        "vnm_terminal_surface package must record the Qt private API major/minor")
endif()

if((NOT "${Qt6_VERSION_MAJOR}" STREQUAL "${VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR}") OR
    (NOT "${Qt6_VERSION_MINOR}" STREQUAL "${VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR}"))
    message(FATAL_ERROR
        "vnm_terminal_surface package Qt private API minor check did not match the resolved Qt")
endif()

get_target_property(vnm_terminal_surface_interface
    vnm_terminal_surface::vnm_terminal_surface
    INTERFACE_LINK_LIBRARIES)

if(NOT vnm_terminal_surface_interface MATCHES "Qt6::GuiPrivate")
    message(FATAL_ERROR
        "vnm_terminal_surface package must export the Qt6::GuiPrivate private link dependency")
endif()

if(vnm_terminal_surface_interface MATCHES "ShaderTools")
    message(FATAL_ERROR
        "vnm_terminal_surface package must not export Qt6::ShaderTools")
endif()

add_executable(vnm_terminal_surface_package_smoke
    main.cpp
)

target_link_libraries(vnm_terminal_surface_package_smoke PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)
