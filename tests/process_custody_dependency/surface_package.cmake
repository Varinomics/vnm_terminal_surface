# Exercise the installed surface's actual config without compiling unrelated
# renderer dependencies. The selected custody target keeps its real capability
# properties from this fixture's existing-target or installed-provider mode.
include(CMakePackageConfigHelpers)
get_filename_component(surface_root "${DEPENDENCY_MODULE}/../.." ABSOLUTE)
set(package_root "${fixture}/surface")
file(MAKE_DIRECTORY "${package_root}")

foreach(package IN ITEMS vnm_fonts Qt6 vnm_qt_dispatch vnm_msdf_text)
    set(${package}_DIR "${fixture}/${package}")
    file(MAKE_DIRECTORY "${${package}_DIR}")
    file(WRITE "${${package}_DIR}/${package}Config.cmake" "set(${package}_FOUND TRUE)\n")
endforeach()
file(APPEND "${Qt6_DIR}/Qt6Config.cmake"
    "set(Qt6_VERSION_MAJOR 6)\nset(Qt6_VERSION_MINOR 12)\n")
write_basic_package_version_file("${Qt6_DIR}/Qt6ConfigVersion.cmake"
    VERSION 6.12.0 COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
write_basic_package_version_file("${vnm_qt_dispatch_DIR}/vnm_qt_dispatchConfigVersion.cmake"
    VERSION 2.0.0 COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)

set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR 6)
set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR 12)
set(VNM_TERMINAL_SURFACE_FULL_TARGET_AVAILABLE ON)
set(VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER OFF)
set(vnm_terminal_surface_process_custody_required ON)
configure_package_config_file(
    "${surface_root}/cmake/vnm_terminal_surfaceConfig.cmake.in"
    "${package_root}/vnm_terminal_surfaceConfig.cmake"
    INSTALL_DESTINATION "lib/cmake/vnm_terminal_surface")
configure_file("${surface_root}/cmake/vnm_terminal_process_custody_contract.cmake"
    "${package_root}/vnm_terminal_process_custody_contract.cmake" COPYONLY)
file(WRITE "${package_root}/vnm_terminal_surfaceTargets.cmake"
    "add_library(vnm_terminal_surface::vnm_terminal_surface INTERFACE IMPORTED)\n")
include("${package_root}/vnm_terminal_surfaceConfig.cmake")
if(NOT TARGET vnm_terminal_surface::vnm_terminal_surface)
    message(FATAL_ERROR "The surface package did not finish importing its target")
endif()
