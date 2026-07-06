if(NOT DEFINED BENCHMARK_EXE OR BENCHMARK_EXE STREQUAL "")
    message(FATAL_ERROR "BENCHMARK_EXE is required")
endif()

if(DEFINED QT_RUNTIME_DIR AND NOT QT_RUNTIME_DIR STREQUAL "")
    if(WIN32)
        set(ENV{PATH} "${QT_RUNTIME_DIR};$ENV{PATH}")
    elseif(APPLE)
        set(ENV{DYLD_LIBRARY_PATH} "${QT_RUNTIME_DIR}:$ENV{DYLD_LIBRARY_PATH}")
    else()
        set(ENV{LD_LIBRARY_PATH} "${QT_RUNTIME_DIR}:$ENV{LD_LIBRARY_PATH}")
    endif()
endif()

execute_process(
    COMMAND
        "${BENCHMARK_EXE}"
        --frames 3
        --warmup-frames 1
        --rows 24
        --cols 80
        --dirty-rows 4
        --dirty-row-stride 7
        --text-pattern block
        --graphics-every 0
    RESULT_VARIABLE benchmark_result
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error)

if(NOT benchmark_result EQUAL 0)
    message(FATAL_ERROR
        "surface stress benchmark failed with ${benchmark_result}\n"
        "stdout:\n${benchmark_output}\n"
        "stderr:\n${benchmark_error}")
endif()

if(NOT benchmark_output MATCHES "frame_row_descriptor_counters_available=true")
    message(FATAL_ERROR
        "surface stress descriptor availability contract missing\n"
        "stdout:\n${benchmark_output}")
endif()

if(NOT benchmark_output MATCHES
    "frame_row_descriptor_counter_semantics=frame_qsg_descriptor_reuse_counters")
    message(FATAL_ERROR
        "surface stress descriptor semantics contract missing\n"
        "stdout:\n${benchmark_output}")
endif()

if(NOT benchmark_output MATCHES "frame_row_descriptors_built=[1-9][0-9]*")
    message(FATAL_ERROR
        "surface stress row descriptor counter missing or zero\n"
        "stdout:\n${benchmark_output}")
endif()

if(NOT benchmark_output MATCHES "frame_layer_descriptors_built=[1-9][0-9]*")
    message(FATAL_ERROR
        "surface stress layer descriptor counter missing or zero\n"
        "stdout:\n${benchmark_output}")
endif()
