include_guard(GLOBAL)

# Apple's forkpty backend owns its child directly and does not use this leaf.
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

set(VNM_PROCESS_CUSTODY_SOURCE_DIR "" CACHE PATH
    "Explicit path to the standalone process_custody leaf")

function(vnm_terminal_require_owner_start_deadline)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        get_target_property(has_deadline vnm_process_custody::vnm_process_custody
            VNM_PROCESS_CUSTODY_OWNER_START_DEADLINE)
        if(NOT has_deadline)
            message(FATAL_ERROR
                "Selected process_custody provider lacks deadline-aware owner START. "
                "Update the maintained source or installed provider to expose send_owner_start_until "
                "and the exported VNM_PROCESS_CUSTODY_OWNER_START_DEADLINE capability. "
                "No blocking or whole-request-retry fallback is supported.")
        endif()
    endif()
endfunction()

if(TARGET vnm_process_custody::vnm_process_custody)
    vnm_terminal_require_owner_start_deadline()
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
        vnm_terminal_require_owner_start_deadline()
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

vnm_terminal_require_owner_start_deadline()
