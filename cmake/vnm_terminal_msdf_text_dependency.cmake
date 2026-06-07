include_guard(GLOBAL)

include(FetchContent)

set(VNM_TERMINAL_MSDF_TEXT_VERSION 0.2.0)
set(VNM_TERMINAL_MSDF_TEXT_GIT_TAG
    04b6989483c280e862633489bda318f34dca05ec)

function(vnm_terminal_msdf_text_set_fetchcontent_build_dir name dir_name)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _fc_compiler_tag)
    set(_fc_config_suffix "-${_fc_compiler_tag}")
    if(CMAKE_BUILD_TYPE)
        string(TOLOWER "${CMAKE_BUILD_TYPE}" _fc_bt_lower)
        string(APPEND _fc_config_suffix "-${_fc_bt_lower}")
    endif()

    set(FETCHCONTENT_BINARY_DIR_${name}
        "${FETCHCONTENT_BASE_DIR}/${dir_name}-build${_fc_config_suffix}"
        CACHE PATH "" FORCE)
endfunction()

function(vnm_terminal_msdf_text_make_available out_var)
    set(_vnm_terminal_msdf_text_options
        ALLOW_EXISTING_TARGET)
    set(_vnm_terminal_msdf_text_one_value_args
        CONTEXT
        REQUIRED_BY
        SOURCE_DIR_VAR
        SOURCE_DIR_OPTION
        USE_SYSTEM_LIBS)
    cmake_parse_arguments(PARSE_ARGV 1 _vnm_terminal_msdf_text
        "${_vnm_terminal_msdf_text_options}"
        "${_vnm_terminal_msdf_text_one_value_args}"
        "")

    if(NOT _vnm_terminal_msdf_text_CONTEXT)
        set(_vnm_terminal_msdf_text_CONTEXT "vnm_terminal_surface")
    endif()
    if(NOT _vnm_terminal_msdf_text_REQUIRED_BY)
        set(_vnm_terminal_msdf_text_REQUIRED_BY "vnm_terminal_surface")
    endif()
    if(NOT _vnm_terminal_msdf_text_SOURCE_DIR_OPTION)
        set(_vnm_terminal_msdf_text_SOURCE_DIR_OPTION
            "${_vnm_terminal_msdf_text_SOURCE_DIR_VAR}")
    endif()

    set(${out_var} OFF PARENT_SCOPE)

    if(TARGET vnm_msdf_text::vnm_msdf_text)
        get_property(
            _vnm_terminal_msdf_text_target_created_by_helper
            GLOBAL
            PROPERTY VNM_TERMINAL_MSDF_TEXT_TARGET_CREATED_BY_HELPER)
        if(NOT _vnm_terminal_msdf_text_target_created_by_helper AND
            NOT _vnm_terminal_msdf_text_ALLOW_EXISTING_TARGET)
            message(FATAL_ERROR
                "Pre-existing vnm_msdf_text::vnm_msdf_text target was not "
                "created by vnm_terminal_msdf_text_make_available. Pass "
                "ALLOW_EXISTING_TARGET only for an explicit development "
                "override.")
        endif()

        message(STATUS "${_vnm_terminal_msdf_text_CONTEXT}: Using existing vnm_msdf_text target")
        set(${out_var} ON PARENT_SCOPE)
        return()
    endif()

    if(_vnm_terminal_msdf_text_USE_SYSTEM_LIBS)
        set(_vnm_terminal_msdf_text_fetch_deps OFF)
    else()
        set(_vnm_terminal_msdf_text_fetch_deps ON)
    endif()
    unset(VNM_MSDF_TEXT_USE_SYSTEM_LIBS CACHE)
    set(VNM_MSDF_TEXT_FETCH_DEPS
        "${_vnm_terminal_msdf_text_fetch_deps}"
        CACHE BOOL "" FORCE)

    if(_vnm_terminal_msdf_text_USE_SYSTEM_LIBS)
        find_package(vnm_msdf_text ${VNM_TERMINAL_MSDF_TEXT_VERSION}
            CONFIG QUIET)
        if(TARGET vnm_msdf_text::vnm_msdf_text)
            message(STATUS "${_vnm_terminal_msdf_text_CONTEXT}: Using vnm_msdf_text from find_package")
            set_property(
                GLOBAL
                PROPERTY VNM_TERMINAL_MSDF_TEXT_TARGET_CREATED_BY_HELPER TRUE)
            set(${out_var} ON PARENT_SCOPE)
            return()
        endif()
    endif()

    set(_vnm_terminal_msdf_text_source_is_explicit OFF)
    set(_vnm_terminal_msdf_text_requested_source_dir "")
    if(_vnm_terminal_msdf_text_SOURCE_DIR_VAR)
        set(_vnm_terminal_msdf_text_requested_source_dir
            "${${_vnm_terminal_msdf_text_SOURCE_DIR_VAR}}")
    endif()

    if(NOT "${_vnm_terminal_msdf_text_requested_source_dir}" STREQUAL "")
        set(_vnm_terminal_msdf_text_source_is_explicit ON)
        set(_vnm_terminal_msdf_text_source
            "${_vnm_terminal_msdf_text_requested_source_dir}")
    else()
        get_filename_component(_vnm_terminal_msdf_text_source
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../bsd_licensed/vnm_msdf_text"
            ABSOLUTE)
    endif()

    if(_vnm_terminal_msdf_text_source_is_explicit AND
        (NOT EXISTS "${_vnm_terminal_msdf_text_source}/CMakeLists.txt"))
        message(FATAL_ERROR
            "${_vnm_terminal_msdf_text_SOURCE_DIR_OPTION} does not contain "
            "vnm_msdf_text CMakeLists.txt: ${_vnm_terminal_msdf_text_source}")
    endif()

    vnm_terminal_msdf_text_set_fetchcontent_build_dir(vnm_msdf_text vnm_msdf_text)
    if(EXISTS "${_vnm_terminal_msdf_text_source}/CMakeLists.txt")
        if(NOT _vnm_terminal_msdf_text_source_is_explicit)
            find_package(Git QUIET)
            if(NOT Git_FOUND)
                message(FATAL_ERROR
                    "Implicit vnm_msdf_text source checkout cannot be verified "
                    "without Git: ${_vnm_terminal_msdf_text_source}")
            endif()

            execute_process(
                COMMAND
                    "${GIT_EXECUTABLE}" -C "${_vnm_terminal_msdf_text_source}"
                    rev-parse HEAD
                RESULT_VARIABLE _vnm_terminal_msdf_text_git_result
                OUTPUT_VARIABLE _vnm_terminal_msdf_text_git_head
                ERROR_VARIABLE _vnm_terminal_msdf_text_git_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE)
            if(NOT _vnm_terminal_msdf_text_git_result EQUAL 0)
                message(FATAL_ERROR
                    "Implicit vnm_msdf_text source checkout is not a verifiable "
                    "Git checkout: ${_vnm_terminal_msdf_text_source}; "
                    "${_vnm_terminal_msdf_text_git_error}")
            endif()

            if(NOT "${_vnm_terminal_msdf_text_git_head}" STREQUAL
                "${VNM_TERMINAL_MSDF_TEXT_GIT_TAG}")
                message(FATAL_ERROR
                    "Implicit vnm_msdf_text source checkout is at "
                    "${_vnm_terminal_msdf_text_git_head}; expected "
                    "${VNM_TERMINAL_MSDF_TEXT_GIT_TAG}. Set "
                    "${_vnm_terminal_msdf_text_SOURCE_DIR_OPTION} explicitly to "
                    "use a development checkout.")
            endif()

            execute_process(
                COMMAND
                    "${GIT_EXECUTABLE}" -C "${_vnm_terminal_msdf_text_source}"
                    status --porcelain --untracked-files=all
                RESULT_VARIABLE _vnm_terminal_msdf_text_status_result
                OUTPUT_VARIABLE _vnm_terminal_msdf_text_status
                ERROR_VARIABLE _vnm_terminal_msdf_text_status_error
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE)
            if(NOT _vnm_terminal_msdf_text_status_result EQUAL 0)
                message(FATAL_ERROR
                    "Implicit vnm_msdf_text source checkout status could not be "
                    "verified: ${_vnm_terminal_msdf_text_status_error}")
            endif()
            if(NOT "${_vnm_terminal_msdf_text_status}" STREQUAL "")
                message(FATAL_ERROR
                    "Implicit vnm_msdf_text source checkout has local "
                    "modifications. Set ${_vnm_terminal_msdf_text_SOURCE_DIR_OPTION} "
                    "explicitly to use a development checkout.")
            endif()
        endif()

        message(STATUS
            "${_vnm_terminal_msdf_text_CONTEXT}: Using vnm_msdf_text source: "
            "${_vnm_terminal_msdf_text_source}")
        FetchContent_Declare(vnm_msdf_text
            SOURCE_DIR "${_vnm_terminal_msdf_text_source}"
            BINARY_DIR "${FETCHCONTENT_BINARY_DIR_vnm_msdf_text}"
        )
    else()
        message(STATUS "${_vnm_terminal_msdf_text_CONTEXT}: Fetching vnm_msdf_text")
        FetchContent_Declare(vnm_msdf_text
            GIT_REPOSITORY https://github.com/imakris/vnm_msdf_text.git
            GIT_TAG "${VNM_TERMINAL_MSDF_TEXT_GIT_TAG}"
            GIT_SHALLOW FALSE
            BINARY_DIR "${FETCHCONTENT_BINARY_DIR_vnm_msdf_text}"
        )
    endif()

    FetchContent_MakeAvailable(vnm_msdf_text)
    if(NOT TARGET vnm_msdf_text::vnm_msdf_text)
        message(FATAL_ERROR
            "vnm_msdf_text was requested for ${_vnm_terminal_msdf_text_REQUIRED_BY} "
            "but did not define vnm_msdf_text::vnm_msdf_text")
    endif()

    set_property(
        GLOBAL
        PROPERTY VNM_TERMINAL_MSDF_TEXT_TARGET_CREATED_BY_HELPER TRUE)
    set(${out_var} ON PARENT_SCOPE)
endfunction()
