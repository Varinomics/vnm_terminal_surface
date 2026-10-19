cmake_minimum_required(VERSION 3.21)
project(process_custody_runtime_contract LANGUAGES NONE)

# The fixture resolves and stages imported files without invoking a compiler.
set(CMAKE_SYSTEM_NAME "${TEST_SYSTEM_NAME}")
set(provider "${CMAKE_CURRENT_BINARY_DIR}/provider")
file(MAKE_DIRECTORY "${provider}/share/licenses/vnm_process_custody")
file(WRITE "${provider}/share/licenses/vnm_process_custody/LICENSE" "fixture license\n")
file(WRITE "${provider}/share/licenses/vnm_process_custody/NOTICE" "fixture notice\n")
set(targets "add_executable(vnm_process_custody::vnm_process_custody_owner IMPORTED GLOBAL)\n")
foreach(configuration IN ITEMS Debug Release)
    string(TOUPPER "${configuration}" upper)
    set(source "${provider}/owner-${configuration}")
    file(WRITE "${source}" "${configuration} selected custody owner\n")
    string(APPEND targets
        "set_property(TARGET vnm_process_custody::vnm_process_custody_owner APPEND PROPERTY IMPORTED_CONFIGURATIONS ${upper})\n"
        "set_property(TARGET vnm_process_custody::vnm_process_custody_owner PROPERTY IMPORTED_LOCATION_${upper} \"${source}\")\n")
endforeach()

if(MODE STREQUAL "package")
    include(CMakePackageConfigHelpers)
    set(CMAKE_INSTALL_DATADIR share)
    set(package_directory lib/cmake/vnm_process_custody)
    set(vnm_process_custody_DIR "${provider}/${package_directory}")
    file(MAKE_DIRECTORY "${vnm_process_custody_DIR}")
    file(WRITE "${vnm_process_custody_DIR}/vnm_process_custody_targets.cmake" "${targets}")
    configure_file("${RUNTIME_MODULE}" "${vnm_process_custody_DIR}/vnm_process_custody_runtime.cmake" COPYONLY)
    get_filename_component(module_directory "${RUNTIME_MODULE}" DIRECTORY)
    configure_package_config_file("${module_directory}/vnm_process_custody-config.cmake.in"
        "${vnm_process_custody_DIR}/vnm_process_custody-config.cmake"
        INSTALL_DESTINATION "${package_directory}"
        INSTALL_PREFIX "${provider}"
        PATH_VARS CMAKE_INSTALL_DATADIR)
    find_package(vnm_process_custody CONFIG REQUIRED NO_DEFAULT_PATH)
else()
    include("${RUNTIME_MODULE}")
    if(MODE STREQUAL "existing")
        cmake_language(EVAL CODE "${targets}")
        set_property(GLOBAL PROPERTY VNM_PROCESS_CUSTODY_LICENSE_FILES
            "${provider}/share/licenses/vnm_process_custody/LICENSE"
            "${provider}/share/licenses/vnm_process_custody/NOTICE")
    endif()
endif()

vnm_process_custody_runtime_files(files)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(files)
        message(FATAL_ERROR "Windows custody has no separate runtime owner")
    endif()
    return()
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime-$<CONFIG>.txt" CONTENT "${files}\n")
add_custom_target(host)
vnm_process_custody_deploy_owner(host DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/stage/$<CONFIG>")
vnm_process_custody_install_owner(DESTINATION bin COMPONENT runtime-stage)
