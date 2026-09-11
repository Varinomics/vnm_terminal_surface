cmake_minimum_required(VERSION 3.21)

foreach(required_variable IN ITEMS
    source_dir
    producer_binary_dir
    install_dir
    consumer_source_dir
    consumer_binary_dir)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

file(REMOVE_RECURSE
    "${producer_binary_dir}"
    "${install_dir}"
    "${consumer_binary_dir}")

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
if(DEFINED qt6_dir AND NOT "${qt6_dir}" STREQUAL "")
    list(APPEND configure_args "-DQt6_DIR=${qt6_dir}")
endif()

set(smoke_config Debug)
if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    set(smoke_config "${install_config}")
endif()

set(single_config_generator ON)
if(DEFINED generator AND
    "${generator}" MATCHES "Visual Studio|Xcode|Multi-Config")
    set(single_config_generator OFF)
endif()

set(build_type_args)
if(single_config_generator)
    list(APPEND build_type_args "-DCMAKE_BUILD_TYPE=${smoke_config}")
endif()

set(producer_build_args
    --build "${producer_binary_dir}"
    --parallel 1)
set(producer_install_args
    --install "${producer_binary_dir}")
set(consumer_build_args
    --build "${consumer_binary_dir}"
    --parallel 1)
if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND producer_build_args
        --config "${install_config}")
    list(APPEND producer_install_args
        --config "${install_config}")
    list(APPEND consumer_build_args
        --config "${install_config}")
endif()

# The producer configure below is a second, independent configure of this
# project. Hand it the vnm_fonts checkout the outer configure already resolved
# so the smoke neither refetches it nor needs the network.
set(producer_configure_args)
if(DEFINED vnm_fonts_source_dir AND NOT "${vnm_fonts_source_dir}" STREQUAL "")
    list(APPEND producer_configure_args
        "-DVNM_FONTS_SOURCE_DIR=${vnm_fonts_source_dir}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        ${producer_configure_args}
        -S "${source_dir}"
        -B "${producer_binary_dir}"
        -DBUILD_TESTING=OFF
        -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF
        -DVNM_TERMINAL_SURFACE_BUILD_FULL=OFF
        -DVNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER=OFF
        ${build_type_args}
        "-DCMAKE_INSTALL_PREFIX=${install_dir}"
    RESULT_VARIABLE producer_configure_result
    OUTPUT_VARIABLE producer_configure_stdout
    ERROR_VARIABLE producer_configure_stderr)
if(NOT producer_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Renderer-only producer configure failed.\n"
        "${producer_configure_stdout}${producer_configure_stderr}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${producer_build_args}
    RESULT_VARIABLE producer_build_result
    OUTPUT_VARIABLE producer_build_stdout
    ERROR_VARIABLE producer_build_stderr)
if(NOT producer_build_result EQUAL 0)
    message(FATAL_ERROR
        "Renderer-only producer build failed.\n"
        "${producer_build_stdout}${producer_build_stderr}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${producer_install_args}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Renderer-only package install failed.\n"
        "${install_stdout}${install_stderr}")
endif()

foreach(renderer_header IN ITEMS
    terminal_canvas_frame.h
    vnm_terminal_canvas.h)
    if(NOT EXISTS "${install_dir}/include/vnm_terminal/${renderer_header}")
        message(FATAL_ERROR
            "Renderer-only package omitted ${renderer_header}")
    endif()
endforeach()
foreach(full_header IN ITEMS
    backend_output_capture.h
    font_metrics.h
    terminal_canvas_export.h
    terminal_message_submission.h
    vnm_terminal_surface.h)
    if(EXISTS "${install_dir}/include/vnm_terminal/${full_header}")
        message(FATAL_ERROR
            "Renderer-only package installed full-surface header ${full_header}")
    endif()
endforeach()
if(EXISTS "${install_dir}/include/vnm_terminal/diagnostics")
    message(FATAL_ERROR "Renderer-only package installed full diagnostics headers")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${consumer_source_dir}"
        -B "${consumer_binary_dir}"
        ${build_type_args}
        "-DCMAKE_PREFIX_PATH=${install_dir}"
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
        -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE
        -DCMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY=TRUE
    RESULT_VARIABLE consumer_configure_result
    OUTPUT_VARIABLE consumer_configure_stdout
    ERROR_VARIABLE consumer_configure_stderr)
if(NOT consumer_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Renderer-only package consumer configure failed.\n"
        "${consumer_configure_stdout}${consumer_configure_stderr}")
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
        "Renderer-only package consumer build failed.\n"
        "${consumer_build_stdout}${consumer_build_stderr}")
endif()

message(STATUS "Renderer-only installed-package consumer built and linked.")
