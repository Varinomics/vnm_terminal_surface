set(separator_index -1)
math(EXPR last_index "${CMAKE_ARGC} - 1")
foreach(index RANGE 0 ${last_index})
    if(separator_index LESS 0 AND "${CMAKE_ARGV${index}}" STREQUAL "--")
        set(separator_index ${index})
    endif()
endforeach()

if(separator_index LESS 0)
    message(FATAL_ERROR "expected embedded benchmark command after --")
endif()

math(EXPR command_start "${separator_index} + 1")
if(command_start GREATER last_index)
    message(FATAL_ERROR "expected embedded benchmark command after --")
endif()

set(command_args)
foreach(index RANGE ${command_start} ${last_index})
    set(command_arg "${CMAKE_ARGV${index}}")
    string(REPLACE ";" "\\;" command_arg "${command_arg}")
    list(APPEND command_args "${command_arg}")
endforeach()

execute_process(
    COMMAND ${command_args}
    RESULT_VARIABLE benchmark_result
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error
)

if(NOT benchmark_result STREQUAL "1")
    message(FATAL_ERROR
        "expected requested-grid mismatch to fail with exit code 1, "
        "got ${benchmark_result}\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

if(NOT benchmark_error MATCHES "requested grid [0-9]+x[0-9]+ but surface produced")
    message(FATAL_ERROR
        "requested-grid mismatch diagnostic missing\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

if(NOT benchmark_output MATCHES "\"requested_grid_required\"[ \r\n\t]*:[ \r\n\t]*true")
    message(FATAL_ERROR
        "requested_grid_required schema value missing\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

if(NOT benchmark_output MATCHES "\"actual_grid_matches_request\"[ \r\n\t]*:[ \r\n\t]*false")
    message(FATAL_ERROR
        "actual_grid_matches_request mismatch schema value missing\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

if(NOT benchmark_output MATCHES "\"status\"[ \r\n\t]*:[ \r\n\t]*\"failed\"")
    message(FATAL_ERROR
        "failed status schema value missing\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

message(STATUS "Requested-grid mismatch failed with expected diagnostic.")
