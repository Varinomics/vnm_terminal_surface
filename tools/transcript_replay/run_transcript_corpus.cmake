if(NOT DEFINED replay_executable OR replay_executable STREQUAL "")
    message(FATAL_ERROR "replay_executable is required")
endif()

if(NOT EXISTS "${replay_executable}")
    message(FATAL_ERROR "replay_executable does not exist: ${replay_executable}")
endif()

if(NOT DEFINED corpus_dir OR corpus_dir STREQUAL "")
    message(FATAL_ERROR "corpus_dir is required")
endif()

if(NOT IS_DIRECTORY "${corpus_dir}")
    message(FATAL_ERROR "corpus_dir is not a directory: ${corpus_dir}")
endif()

file(GLOB transcript_files
    "${corpus_dir}/*.ndjson"
)
list(SORT transcript_files)

list(LENGTH transcript_files transcript_count)
if(transcript_count EQUAL 0)
    message(FATAL_ERROR "corpus_dir contains no .ndjson transcripts: ${corpus_dir}")
endif()

function(vnm_first_nonempty_lines out_value text max_lines)
    string(REPLACE "\r\n" "\n" normalized_text "${text}")
    string(REPLACE "\r" "\n" normalized_text "${normalized_text}")
    string(REPLACE "\n" ";" text_lines "${normalized_text}")

    set(summary "")
    set(line_count 0)
    foreach(line IN LISTS text_lines)
        string(STRIP "${line}" stripped_line)
        if(NOT stripped_line STREQUAL "")
            string(APPEND summary "    ${stripped_line}\n")
            math(EXPR line_count "${line_count} + 1")
            if(line_count GREATER_EQUAL max_lines)
                break()
            endif()
        endif()
    endforeach()

    set(${out_value} "${summary}" PARENT_SCOPE)
endfunction()

function(vnm_first_diagnostic_lines out_value stdout_text stderr_text max_lines)
    set(summary "")

    vnm_first_nonempty_lines(stderr_summary "${stderr_text}" "${max_lines}")
    if(NOT stderr_summary STREQUAL "")
        string(APPEND summary "  stderr:\n${stderr_summary}")
    endif()

    vnm_first_nonempty_lines(stdout_summary "${stdout_text}" "${max_lines}")
    if(NOT stdout_summary STREQUAL "")
        string(APPEND summary "  stdout:\n${stdout_summary}")
    endif()

    if(summary STREQUAL "")
        set(summary "  <no stdout/stderr>\n")
    endif()
    set(${out_value} "${summary}" PARENT_SCOPE)
endfunction()

set(failed_transcripts "")
set(failed_transcript_summaries "")
foreach(transcript_file IN LISTS transcript_files)
    execute_process(
        COMMAND "${replay_executable}" "${transcript_file}"
        RESULT_VARIABLE replay_result
        OUTPUT_VARIABLE replay_stdout
        ERROR_VARIABLE replay_stderr
    )

    if(NOT replay_result EQUAL 0)
        list(APPEND failed_transcripts "${transcript_file}")
        get_filename_component(transcript_name "${transcript_file}" NAME)
        vnm_first_diagnostic_lines(
            diagnostic_summary
            "${replay_stdout}"
            "${replay_stderr}"
            12)
        string(APPEND failed_transcript_summaries
            "\n${transcript_name}: exit ${replay_result}\n${diagnostic_summary}")
        message(STATUS "Replay failed for ${transcript_file}")
        message(STATUS "stdout:\n${replay_stdout}")
        message(STATUS "stderr:\n${replay_stderr}")
    endif()
endforeach()

list(LENGTH failed_transcripts failed_count)
if(failed_count GREATER 0)
    message(STATUS
        "Transcript replay corpus diagnostic summaries:${failed_transcript_summaries}")
    message(FATAL_ERROR
        "Transcript replay corpus failed for ${failed_count} of ${transcript_count} transcript(s); "
        "curate or regenerate failing transcripts before using this corpus as a gate")
endif()

message(STATUS
    "Transcript replay corpus passed for ${transcript_count} transcript(s) in ${corpus_dir}")
