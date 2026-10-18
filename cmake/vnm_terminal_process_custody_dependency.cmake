include_guard(GLOBAL)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE)
    return()
endif()

set(VNM_PROCESS_CUSTODY_SOURCE_DIR "" CACHE PATH
    "Explicit path to the standalone process_custody leaf")

include("${CMAKE_CURRENT_LIST_DIR}/vnm_terminal_process_custody_contract.cmake")

if(TARGET vnm_process_custody::vnm_process_custody)
    vnm_terminal_require_process_custody()
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
    set(_custody_source "${VNM_PROCESS_CUSTODY_SOURCE_DIR}")
else()
    find_package(vnm_process_custody CONFIG QUIET)
    if(TARGET vnm_process_custody::vnm_process_custody)
        vnm_terminal_require_process_custody()
        return()
    endif()
    if(vnm_process_custody_FOUND)
        message(FATAL_ERROR "The installed process custody package has no public library target")
    endif()
    set(_custody_source "${CMAKE_CURRENT_LIST_DIR}/../cpp/process_custody")
endif()
FetchContent_Declare(vnm_terminal_process_custody SOURCE_DIR "${_custody_source}")
FetchContent_MakeAvailable(vnm_terminal_process_custody)
unset(_custody_source)
if(NOT TARGET vnm_process_custody::vnm_process_custody)
    message(FATAL_ERROR "The selected process custody provider did not define its public target")
endif()

vnm_terminal_require_process_custody()
