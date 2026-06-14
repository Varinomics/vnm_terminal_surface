foreach(required_var IN ITEMS
    package_binary_dir
    install_dir
    consumer_source_dir
    consumer_binary_dir)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing required package smoke variable: ${required_var}")
    endif()
endforeach()

# The library target was already built by the suite build that produced
# package_binary_dir, so this driver installs that build rather than re-driving
# a nested cmake --build (which cannot re-resolve some generators' toolchains).
set(install_args
    --install "${package_binary_dir}"
    --prefix "${install_dir}")

if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND install_args
        --config "${install_config}")
endif()

file(REMOVE_RECURSE
    "${install_dir}"
    "${consumer_binary_dir}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${install_args}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install vnm_terminal_surface for package smoke.\n"
        "${install_stdout}${install_stderr}")
endif()

set(installed_diagnostics_header
    "${install_dir}/include/vnm_terminal/diagnostics/metrics_json.h")
if(NOT EXISTS "${installed_diagnostics_header}")
    message(FATAL_ERROR
        "Package smoke expected installed diagnostics header at "
        "${installed_diagnostics_header}")
endif()

set(installed_font_metrics_header
    "${install_dir}/include/vnm_terminal/font_metrics.h")
if(NOT EXISTS "${installed_font_metrics_header}")
    message(FATAL_ERROR
        "Package smoke expected installed font metrics header at "
        "${installed_font_metrics_header}")
endif()

set(configure_args)
if(DEFINED generator AND NOT "${generator}" STREQUAL "")
    list(APPEND configure_args -G "${generator}")
endif()

if(DEFINED generator_platform AND NOT "${generator_platform}" STREQUAL "")
    list(APPEND configure_args -A "${generator_platform}")
endif()

if(DEFINED generator_toolset AND NOT "${generator_toolset}" STREQUAL "")
    list(APPEND configure_args -T "${generator_toolset}")
endif()

if(DEFINED make_program AND NOT "${make_program}" STREQUAL "")
    list(APPEND configure_args "-DCMAKE_MAKE_PROGRAM=${make_program}")
endif()

set(single_config_generator ON)
if(DEFINED generator AND
    "${generator}" MATCHES "Visual Studio|Xcode|Multi-Config")
    set(single_config_generator OFF)
endif()

if(single_config_generator AND
    DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${install_config}")
endif()

if(DEFINED qt6_dir AND NOT "${qt6_dir}" STREQUAL "")
    list(APPEND configure_args "-DQt6_DIR=${qt6_dir}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${consumer_source_dir}"
        -B "${consumer_binary_dir}"
        "-DCMAKE_PREFIX_PATH=${install_dir}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Package smoke consumer configure failed.\n"
        "${configure_stdout}${configure_stderr}")
endif()

set(consumer_build_args
    --build "${consumer_binary_dir}")
if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND consumer_build_args
        --config "${install_config}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${consumer_build_args}
    RESULT_VARIABLE consumer_build_result
    OUTPUT_VARIABLE consumer_build_stdout
    ERROR_VARIABLE consumer_build_stderr)

if(NOT consumer_build_result EQUAL 0)
    message(FATAL_ERROR
        "Package smoke consumer build failed; the installed diagnostics header "
        "or its builders did not link.\n"
        "${consumer_build_stdout}${consumer_build_stderr}")
endif()

message(STATUS "Package smoke consumer built against the installed public headers.")
