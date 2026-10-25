cmake_minimum_required(VERSION 3.21)
project(terminal_surface_package_prefix LANGUAGES NONE)
include(CMakePackageConfigHelpers)

set(surface_prefix "${CMAKE_CURRENT_BINARY_DIR}/surface")
set(dependency_prefix "${CMAKE_CURRENT_BINARY_DIR}/dependencies")
set(package_dir "${surface_prefix}/lib/cmake/vnm_terminal_surface")
file(MAKE_DIRECTORY "${package_dir}" "${dependency_prefix}/modules")
set(CMAKE_INSTALL_DATADIR share)
set(VNM_TERMINAL_SURFACE_FULL_TARGET_AVAILABLE TRUE)
set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR 6)
set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR 12)
set(VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER FALSE)
configure_package_config_file(
    "${SURFACE_SOURCE_DIR}/cmake/vnm_terminal_surfaceConfig.cmake.in"
    "${package_dir}/vnm_terminal_surfaceConfig.cmake"
    INSTALL_DESTINATION lib/cmake/vnm_terminal_surface
    INSTALL_PREFIX "${surface_prefix}"
    PATH_VARS CMAKE_INSTALL_DATADIR)
foreach(helper IN ITEMS vnm_terminal_conpty vnm_terminal_conpty_source)
    configure_file("${SURFACE_SOURCE_DIR}/cmake/${helper}.cmake"
        "${package_dir}/${helper}.cmake" COPYONLY)
endforeach()
file(WRITE "${package_dir}/vnm_terminal_surfaceTargets.cmake"
    "add_library(vnm_terminal_surface::vnm_terminal_surface INTERFACE IMPORTED)\n")

# Reproduce the pre-3.30 caller-visible prefix change even on newer test hosts.
file(WRITE "${dependency_prefix}/modules/CMakeFindDependencyMacro.cmake" [=[
macro(find_dependency dependency)
    set(${dependency}_FOUND TRUE)
    if("${dependency}" STREQUAL "Qt6")
        set(Qt6_VERSION_MAJOR 6)
        set(Qt6_VERSION_MINOR 12)
    endif()
    set(PACKAGE_PREFIX_DIR "${dependency_prefix}/${dependency}")
endmacro()
]=])
list(PREPEND CMAKE_MODULE_PATH "${dependency_prefix}/modules")

set(WIN32 TRUE)
set(APPLE FALSE)
set(CMAKE_SYSTEM_NAME Windows)
find_package(vnm_terminal_surface CONFIG REQUIRED
    PATHS "${surface_prefix}" NO_DEFAULT_PATH)
get_property(conpty_package GLOBAL PROPERTY vnm_terminal_conpty_package_dir)
set(expected "${surface_prefix}/share/vnm_terminal_surface/conpty")
if(NOT conpty_package STREQUAL expected)
    message(FATAL_ERROR
        "The installed surface lost its runtime prefix: expected ${expected}, got ${conpty_package}")
endif()
if(NOT COMMAND vnm_terminal_deploy_conpty)
    message(FATAL_ERROR "The installed surface did not load its native runtime deployment helper")
endif()
