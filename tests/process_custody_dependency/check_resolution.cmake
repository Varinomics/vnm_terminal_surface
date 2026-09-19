foreach(mode IN ITEMS existing installed source fallback invalid darwin)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_LIST_DIR}"
            -B "${TEST_ROOT}/${mode}"
            "-DMODE=${mode}"
            "-DDEPENDENCY_MODULE=${DEPENDENCY_MODULE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(mode STREQUAL "invalid")
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "must name the process_custody leaf")
            message(FATAL_ERROR "Invalid leaf override was not rejected: ${output}${error}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "Custody resolver ${mode} failed: ${output}${error}")
    endif()
endforeach()
message(STATUS "All six compiler-free process custody resolution checks passed")
