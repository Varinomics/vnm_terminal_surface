include_guard(GLOBAL)

# Apple's forkpty backend owns its child directly and does not use this leaf.
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(VNM_PROCESS_CUSTODY_SOURCE_DIR "" CACHE PATH
    "Explicit path to vnm_framework/cpp/process_custody")

if(TARGET vnm_framework::vnm_process_custody)
    get_target_property(_custody_imported vnm_framework::vnm_process_custody IMPORTED)
    if(NOT _custody_imported)
        get_target_property(_custody_source vnm_framework::vnm_process_custody SOURCE_DIR)
        include("${_custody_source}/../../cmake/vnm_process_custody_install.cmake")
        vnm_process_custody_register_source_package()
        unset(_custody_source)
    endif()
    unset(_custody_imported)
    return()
endif()

include(FetchContent)
if(VNM_PROCESS_CUSTODY_SOURCE_DIR)
    if(NOT EXISTS "${VNM_PROCESS_CUSTODY_SOURCE_DIR}/CMakeLists.txt" OR
       NOT EXISTS "${VNM_PROCESS_CUSTODY_SOURCE_DIR}/include/vnm_process_custody/process_spawn.h")
        message(FATAL_ERROR
            "VNM_PROCESS_CUSTODY_SOURCE_DIR must name the process_custody leaf: "
            "${VNM_PROCESS_CUSTODY_SOURCE_DIR}")
    endif()
    FetchContent_Declare(vnm_terminal_process_custody
        SOURCE_DIR "${VNM_PROCESS_CUSTODY_SOURCE_DIR}")
else()
    find_package(vnm_process_custody CONFIG QUIET)
    if(TARGET vnm_framework::vnm_process_custody)
        return()
    endif()
    if(vnm_process_custody_FOUND)
        message(FATAL_ERROR "The installed process custody package has no public library target")
    endif()
    FetchContent_Declare(vnm_terminal_process_custody
        GIT_REPOSITORY https://github.com/Varinomics/vnm_framework.git
        GIT_TAG master
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR cpp/process_custody)
endif()
FetchContent_MakeAvailable(vnm_terminal_process_custody)
if(NOT TARGET vnm_framework::vnm_process_custody)
    message(FATAL_ERROR "The selected process custody provider did not define its public target")
endif()
get_target_property(_custody_source vnm_framework::vnm_process_custody SOURCE_DIR)
include("${_custody_source}/../../cmake/vnm_process_custody_install.cmake")
vnm_process_custody_register_source_package()
unset(_custody_source)
