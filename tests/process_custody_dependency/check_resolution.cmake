get_filename_component(surface_root "${DEPENDENCY_MODULE}/../.." ABSOLUTE)
foreach(relative_path IN ITEMS
        CMakeLists.txt
        cmake/vnm_terminal_process_custody_dependency.cmake
        cmake/vnm_terminal_surfaceConfig.cmake.in
        cpp/process_custody/CMakeLists.txt
        cpp/process_custody/cmake/vnm_process_custody_install.cmake
        cpp/process_custody/cmake/vnm_process_custody_runtime.cmake
        cpp/process_custody/cmake/vnm_process_custody-config.cmake.in)
    file(READ "${surface_root}/${relative_path}" contents)
    if(contents MATCHES "vnm_framework")
        message(FATAL_ERROR "Surface custody must be framework-independent: ${relative_path}")
    endif()
endforeach()

foreach(mode IN ITEMS existing installed source fallback invalid darwin old-existing old-installed old-source)
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
    elseif(mode MATCHES "^old-")
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks deadline-aware owner START")
            message(FATAL_ERROR "Legacy custody provider was not rejected: ${output}${error}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "Custody resolver ${mode} failed: ${output}${error}")
    endif()
endforeach()
message(STATUS "All nine compiler-free process custody resolution checks passed")
