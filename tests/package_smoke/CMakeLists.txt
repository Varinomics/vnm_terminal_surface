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
if(NOT DEFINED VNM_TERMINAL_SURFACE_MSDF_TEXT_RENDERER_ENABLED)
    message(FATAL_ERROR
        "vnm_terminal_surface package must record whether MSDF text is enabled")
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
if(VNM_TERMINAL_SURFACE_MSDF_TEXT_RENDERER_ENABLED)
    if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
        message(FATAL_ERROR
            "vnm_terminal_surface package must find vnm_msdf_text when MSDF is enabled")
    endif()
    if(NOT vnm_terminal_surface_interface MATCHES "vnm_msdf_text::vnm_msdf_text")
        message(FATAL_ERROR
            "vnm_terminal_surface package must export the vnm_msdf_text link dependency")
    endif()
endif()

get_target_property(vnm_terminal_surface_include_dirs
    vnm_terminal_surface::vnm_terminal_surface
    INTERFACE_INCLUDE_DIRECTORIES)

if(NOT vnm_terminal_surface_include_dirs)
    message(FATAL_ERROR
        "vnm_terminal_surface package must export an include directory")
endif()

foreach(vnm_terminal_surface_include_dir IN LISTS vnm_terminal_surface_include_dirs)
    if(EXISTS "${vnm_terminal_surface_include_dir}/vnm_terminal/internal")
        message(FATAL_ERROR
            "vnm_terminal_surface package must not install internal headers")
    endif()
endforeach()

add_executable(vnm_terminal_surface_package_smoke
    main.cpp
)

target_link_libraries(vnm_terminal_surface_package_smoke PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)
