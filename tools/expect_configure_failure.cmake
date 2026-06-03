set(configure_args)
if(NOT "${generator}" STREQUAL "")
    list(APPEND configure_args -G "${generator}")
endif()

if(NOT "${generator_platform}" STREQUAL "")
    list(APPEND configure_args -A "${generator_platform}")
endif()

if(NOT "${generator_toolset}" STREQUAL "")
    list(APPEND configure_args -T "${generator_toolset}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${source_dir}"
        -B "${binary_dir}"
        -DVNM_TERMINAL_DISTRIBUTION_BUILD=ON
        -DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=ON
        -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)

set(configure_output "${configure_stdout}${configure_stderr}")

if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected configure to fail, but it succeeded.\n${configure_output}")
endif()

if(NOT configure_output MATCHES "${expected_regex}")
    message(FATAL_ERROR
        "Expected configure failure output to match '${expected_regex}'.\n"
        "Actual output:\n${configure_output}")
endif()

message(STATUS "Configure failed with expected diagnostic.")
