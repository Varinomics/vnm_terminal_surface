include_guard(GLOBAL)

function(vnm_terminal_require_process_custody)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
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
    if(APPLE)
        get_target_property(observes_cpu vnm_process_custody::vnm_process_custody
            VNM_PROCESS_CUSTODY_DARWIN_CPU_OBSERVATION)
        if(NOT observes_cpu)
            message(FATAL_ERROR
                "Selected process_custody provider lacks Darwin observed-family CPU accounting. "
                "Update the maintained source or installed provider to expose Process_cpu_tracker "
                "and the exported VNM_PROCESS_CUSTODY_DARWIN_CPU_OBSERVATION capability.")
        endif()
        get_target_property(retains_output vnm_process_custody::vnm_process_custody
            VNM_PROCESS_CUSTODY_DARWIN_PTY_OUTPUT_RETIREMENT)
        if(NOT retains_output)
            message(FATAL_ERROR
                "Selected process_custody provider lacks Darwin PTY output retirement. "
                "Update the maintained source or installed provider so its helper retains "
                "the terminal after settlement until client control-channel EOF, with the "
                "exported VNM_PROCESS_CUSTODY_DARWIN_PTY_OUTPUT_RETIREMENT capability.")
        endif()
    endif()
endfunction()
