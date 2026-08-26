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

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${source_dir}"
        -B "${producer_binary_dir}"
        -DBUILD_TESTING=OFF
        -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF
        -DVNM_TERMINAL_SURFACE_BUILD_FULL=OFF
        -DVNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER=OFF
        -DCMAKE_BUILD_TYPE=Debug
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
        --build "${producer_binary_dir}"
        --target vnm_terminal_surface_renderer
        --parallel 1
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
        --install "${producer_binary_dir}"
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
        -DCMAKE_BUILD_TYPE=Debug
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
        --build "${consumer_binary_dir}"
        --parallel 1
    RESULT_VARIABLE consumer_build_result
    OUTPUT_VARIABLE consumer_build_stdout
    ERROR_VARIABLE consumer_build_stderr)
if(NOT consumer_build_result EQUAL 0)
    message(FATAL_ERROR
        "Renderer-only package consumer build failed.\n"
        "${consumer_build_stdout}${consumer_build_stderr}")
endif()

message(STATUS "Renderer-only installed-package consumer built and linked.")
