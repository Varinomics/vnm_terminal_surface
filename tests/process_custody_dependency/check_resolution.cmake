get_filename_component(surface_root "${DEPENDENCY_MODULE}/../.." ABSOLUTE)
foreach(relative_path IN ITEMS
        CMakeLists.txt
        cmake/vnm_terminal_process_custody_dependency.cmake
        cmake/vnm_terminal_process_custody_contract.cmake
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

foreach(system_name IN ITEMS Linux Darwin)
    foreach(mode IN ITEMS existing installed source fallback invalid old-existing old-installed old-source
            no-retirement-existing no-retirement-installed no-retirement-source no-retirement-fallback
            no-cpu-existing no-cpu-installed no-cpu-source no-cpu-fallback)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -S "${CMAKE_CURRENT_LIST_DIR}"
                -B "${TEST_ROOT}/${system_name}/${mode}"
                "-DTEST_SYSTEM_NAME=${system_name}"
                "-DMODE=${mode}"
                "-DDEPENDENCY_MODULE=${DEPENDENCY_MODULE}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(mode STREQUAL "invalid")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "must name the process_custody leaf")
                message(FATAL_ERROR "Invalid ${system_name} leaf override was not rejected: ${output}${error}")
            endif()
        elseif(mode MATCHES "^old-")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks deadline-aware owner START")
                message(FATAL_ERROR "Legacy ${system_name} custody provider was not rejected: ${output}${error}")
            endif()
        elseif(system_name STREQUAL "Darwin" AND mode MATCHES "^no-retirement-")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks Darwin PTY output retirement")
                message(FATAL_ERROR "Darwin custody provider without output retirement was accepted: ${output}${error}")
            endif()
        elseif(system_name STREQUAL "Darwin" AND mode MATCHES "^no-cpu-")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks Darwin observed-family CPU accounting")
                message(FATAL_ERROR "Darwin custody provider without CPU observation was accepted: ${output}${error}")
            endif()
        elseif(NOT result EQUAL 0)
            message(FATAL_ERROR "Custody resolver ${system_name} ${mode} failed: ${output}${error}")
        endif()
    endforeach()
    foreach(mode IN ITEMS existing installed no-retirement-existing no-retirement-installed
            no-cpu-existing no-cpu-installed)
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                -S "${CMAKE_CURRENT_LIST_DIR}"
                -B "${TEST_ROOT}/${system_name}/package-${mode}"
                "-DTEST_SYSTEM_NAME=${system_name}"
                "-DMODE=${mode}"
                -DSURFACE_PACKAGE=ON
                "-DDEPENDENCY_MODULE=${DEPENDENCY_MODULE}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)
        if(system_name STREQUAL "Darwin" AND mode MATCHES "^no-retirement-")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks Darwin PTY output retirement")
                message(FATAL_ERROR "Surface package accepted Darwin custody without output retirement: ${output}${error}")
            endif()
        elseif(system_name STREQUAL "Darwin" AND mode MATCHES "^no-cpu-")
            if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "lacks Darwin observed-family CPU accounting")
                message(FATAL_ERROR "Surface package accepted Darwin custody without CPU observation: ${output}${error}")
            endif()
        elseif(NOT result EQUAL 0)
            message(FATAL_ERROR "Surface package ${system_name} ${mode} failed: ${output}${error}")
        endif()
    endforeach()
endforeach()
message(STATUS "All compiler-free process custody resolution checks passed for Linux and Darwin")
