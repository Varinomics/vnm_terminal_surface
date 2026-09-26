include_guard(GLOBAL)

include(FetchContent)

function(vnm_terminal_msdf_text_make_available out_var)
    set(_vnm_terminal_msdf_text_one_value_args
        CONTEXT
        REQUIRED_BY
        SOURCE_DIR_VAR
        USE_SYSTEM_LIBS)
    cmake_parse_arguments(PARSE_ARGV 1 _vnm_terminal_msdf_text
        ""
        "${_vnm_terminal_msdf_text_one_value_args}"
        "")

    if(NOT _vnm_terminal_msdf_text_CONTEXT)
        set(_vnm_terminal_msdf_text_CONTEXT "vnm_terminal_surface")
    endif()
    if(NOT _vnm_terminal_msdf_text_REQUIRED_BY)
        set(_vnm_terminal_msdf_text_REQUIRED_BY "${_vnm_terminal_msdf_text_CONTEXT}")
    endif()

    set(${out_var} OFF PARENT_SCOPE)

    if(TARGET vnm_msdf_text::qt_lcd AND
       (NOT vnm_terminal_msdf_atlas_required OR TARGET vnm_msdf_text::vnm_msdf_text))
        set(${out_var} ON PARENT_SCOPE)
        return()
    endif()

    if(TARGET vnm_msdf_text::lcd_contract)
        message(FATAL_ERROR
            "The existing vnm_msdf_text provider does not supply the required "
            "qt_lcd/atlas components. Enable those components in its owning "
            "source build or install a package that provides them.")
    endif()

    if(_vnm_terminal_msdf_text_USE_SYSTEM_LIBS)
        set(VNM_MSDF_TEXT_FETCH_DEPS OFF CACHE BOOL "" FORCE)
        set(_components qt_lcd)
        if(vnm_terminal_msdf_atlas_required)
            list(APPEND _components atlas)
        endif()
        find_package(vnm_msdf_text CONFIG REQUIRED COMPONENTS ${_components})
        set(${out_var} ON PARENT_SCOPE)
        return()
    else()
        set(VNM_MSDF_TEXT_FETCH_DEPS ON CACHE BOOL "" FORCE)
    endif()

    set(_vnm_terminal_msdf_text_source_dir "")
    if(_vnm_terminal_msdf_text_SOURCE_DIR_VAR)
        set(_vnm_terminal_msdf_text_source_dir
            "${${_vnm_terminal_msdf_text_SOURCE_DIR_VAR}}")
    endif()

    if(NOT "${_vnm_terminal_msdf_text_source_dir}" STREQUAL "")
        if(NOT EXISTS "${_vnm_terminal_msdf_text_source_dir}/CMakeLists.txt")
            message(FATAL_ERROR
                "${_vnm_terminal_msdf_text_SOURCE_DIR_VAR} does not contain "
                "vnm_msdf_text CMakeLists.txt: "
                "${_vnm_terminal_msdf_text_source_dir}")
        endif()

        message(STATUS
            "${_vnm_terminal_msdf_text_CONTEXT}: Using vnm_msdf_text source: "
            "${_vnm_terminal_msdf_text_source_dir}")
        FetchContent_Declare(vnm_msdf_text
            SOURCE_DIR "${_vnm_terminal_msdf_text_source_dir}")
    else()
        message(STATUS "${_vnm_terminal_msdf_text_CONTEXT}: Fetching vnm_msdf_text")
        FetchContent_Declare(vnm_msdf_text
            GIT_REPOSITORY https://github.com/Varinomics/vnm_msdf_text.git
            # vnm_terminal intentionally tracks vnm_msdf_text master so API
            # breaks fail here instead of hiding behind stale compatibility pins.
            GIT_TAG master)
    endif()

    FetchContent_MakeAvailable(vnm_msdf_text)
    if(NOT TARGET vnm_msdf_text::qt_lcd OR
       (vnm_terminal_msdf_atlas_required AND NOT TARGET vnm_msdf_text::vnm_msdf_text))
        message(FATAL_ERROR
            "vnm_msdf_text was requested for ${_vnm_terminal_msdf_text_REQUIRED_BY} "
            "but did not define its required LCD/atlas components")
    endif()

    set(${out_var} ON PARENT_SCOPE)
endfunction()
