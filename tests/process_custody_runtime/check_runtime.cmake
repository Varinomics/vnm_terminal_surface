cmake_minimum_required(VERSION 3.21)

function(run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Custody runtime command failed (${result}): ${ARGV}\n${output}${error}")
    endif()
endfunction()

function(require_content path expected)
    file(READ "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Unexpected custody runtime content at ${path}: ${actual}")
    endif()
endfunction()

foreach(system_name IN ITEMS Linux Darwin)
    foreach(mode IN ITEMS existing package)
        set(build "${TEST_ROOT}/${system_name}-${mode}")
        run_checked("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}" -B "${build}"
            -G "Ninja Multi-Config" "-DCMAKE_MAKE_PROGRAM=${NINJA_EXECUTABLE}"
            "-DTEST_SYSTEM_NAME=${system_name}" "-DMODE=${mode}"
            "-DRUNTIME_MODULE=${RUNTIME_MODULE}")
        foreach(configuration IN ITEMS Debug Release)
            set(source "${build}/provider/owner-${configuration}")
            require_content("${build}/runtime-${configuration}.txt"
                "${source}|vnm_process_custody_owner\n")
            run_checked("${CMAKE_COMMAND}" --build "${build}"
                --config "${configuration}" --target host --parallel 1)
            require_content("${build}/stage/${configuration}/vnm_process_custody_owner"
                "${configuration} selected custody owner\n")
            run_checked("${CMAKE_COMMAND}" --install "${build}"
                --config "${configuration}" --prefix "${build}/install-${configuration}"
                --component runtime-stage)
            require_content("${build}/install-${configuration}/bin/vnm_process_custody_owner"
                "${configuration} selected custody owner\n")
            require_content("${build}/install-${configuration}/share/licenses/vnm_process_custody/LICENSE"
                "fixture license\n")
            file(APPEND "${source}" "updated owner\n")
            run_checked("${CMAKE_COMMAND}" --build "${build}"
                --config "${configuration}" --target host --parallel 1)
            require_content("${build}/stage/${configuration}/vnm_process_custody_owner"
                "${configuration} selected custody owner\nupdated owner\n")
        endforeach()
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}"
        -B "${TEST_ROOT}/${system_name}-missing" -G Ninja "-DCMAKE_MAKE_PROGRAM=${NINJA_EXECUTABLE}"
        "-DTEST_SYSTEM_NAME=${system_name}" -DMODE=missing "-DRUNTIME_MODULE=${RUNTIME_MODULE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "has no POSIX owner executable")
        message(FATAL_ERROR "Missing ${system_name} custody owner was not rejected: ${output}${error}")
    endif()
endforeach()
run_checked("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}" -B "${TEST_ROOT}/Windows"
    -G Ninja "-DCMAKE_MAKE_PROGRAM=${NINJA_EXECUTABLE}"
    -DTEST_SYSTEM_NAME=Windows -DMODE=missing "-DRUNTIME_MODULE=${RUNTIME_MODULE}")
message(STATUS "Custody runtime selection, deployment and installation checks passed")
