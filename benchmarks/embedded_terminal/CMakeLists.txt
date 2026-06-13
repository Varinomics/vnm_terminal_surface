add_executable(vnm_terminal_embedded_benchmark
    embedded_terminal_benchmark.cpp
)

find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)

target_link_libraries(vnm_terminal_embedded_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

if(WIN32)
    target_link_libraries(vnm_terminal_embedded_benchmark PRIVATE
        psapi
    )
endif()

set_target_properties(vnm_terminal_embedded_benchmark PROPERTIES
    AUTOMOC OFF
)

if(MSVC)
    target_compile_options(vnm_terminal_embedded_benchmark PRIVATE
        /FS
    )
endif()

if(WIN32)
    set(vnm_terminal_benchmark_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_benchmark_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_benchmark_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

function(vnm_terminal_benchmark_require_qt_plugin_file plugin_target purpose)
    if(NOT TARGET ${plugin_target})
        message(FATAL_ERROR
            "${purpose} requires Qt plugin target ${plugin_target}")
    endif()

    get_target_property(plugin_target_type ${plugin_target} TYPE)
    if(NOT plugin_target_type MATCHES "^(MODULE_LIBRARY|SHARED_LIBRARY)$")
        message(FATAL_ERROR
            "${purpose} requires ${plugin_target} to be a dynamic plugin; "
            "target type is ${plugin_target_type}")
    endif()
endfunction()

function(vnm_terminal_benchmark_qt_offscreen_test_environment out_var)
    set(${out_var}
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        PARENT_SCOPE)
endfunction()

function(vnm_terminal_benchmark_qt_readback_test_environment out_var)
    if(WIN32)
        set(${out_var}
            "QT_QPA_PLATFORM=windows"
            "QSG_RENDER_LOOP=threaded"
            "QSG_RHI_BACKEND=d3d11"
            PARENT_SCOPE)
    else()
        set(${out_var}
            ${vnm_terminal_benchmark_qt_offscreen_environment}
            PARENT_SCOPE)
    endif()
endfunction()

vnm_terminal_benchmark_require_qt_plugin_file(
    Qt6::QOffscreenIntegrationPlugin
    "Embedded benchmark CTest offscreen run")
vnm_terminal_benchmark_qt_offscreen_test_environment(
    vnm_terminal_benchmark_qt_offscreen_environment)
vnm_terminal_benchmark_qt_readback_test_environment(
    vnm_terminal_benchmark_qt_readback_environment)

function(vnm_terminal_benchmark_add_readback_test test_name)
    if(WIN32)
        set(vnm_terminal_benchmark_executable_command
            cmd.exe /d /c call
            "$<TARGET_FILE:vnm_terminal_embedded_benchmark>"
            ${ARGN}
        )
    else()
        set(vnm_terminal_benchmark_executable_command
            "$<TARGET_FILE:vnm_terminal_embedded_benchmark>"
            ${ARGN}
        )
    endif()

    add_test(
        NAME ${test_name}
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_benchmark_qt_readback_environment}
            "QT_SCALE_FACTOR=1"
            "QT_SCREEN_SCALE_FACTORS="
            "QT_AUTO_SCREEN_SCALE_FACTOR=0"
            "QT_DEVICE_PIXEL_RATIO="
            "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            "${vnm_terminal_benchmark_qt_runtime_path}"
            ${vnm_terminal_benchmark_executable_command}
    )
endfunction()

if(vnm_terminal_surface_testing_enabled AND TARGET vnm_terminal_embedded_benchmark)
    vnm_terminal_benchmark_add_readback_test(
        vnm_terminal_embedded_benchmark_validate
        "--iterations" "7"
        "--warmup" "3"
        "--grid" "48x160"
        "--window-size" "1280x768"
        "--dirty-rows" "5"
        "--dirty-row-stride" "3"
        "--scenario" "dense_repaint"
        "--scenario" "ascii_full_dirty_reuse_only"
        "--scenario" "ascii_full_dirty_same_content"
        "--scenario" "styled_ascii_full_dirty_reuse_only"
        "--scenario" "mixed_text_full_dirty_reuse_only"
        "--scenario" "scroll_burst"
        "--scenario" "resize_bounce"
        "--scenario" "unicode_wide_row"
        "--scenario" "cursor_repaint_toggle"
        "--scenario" "single_row_geometry_update"
        "--scenario" "surface_session_wheel_scroll"
        "--scenario" "surface_session_viewport_pan"
        "--scenario" "surface_session_sustained_output"
        "--scenario" "surface_session_scrollback_limit"
        "--scenario" "surface_session_alternate_churn"
        "--scenario" "surface_session_paste_write"
        "--scenario" "surface_session_output_high_water"
        "--scenario" "surface_session_write_high_water"
        "--scenario" "surface_session_sparse_ascii_output"
        "--scenario" "surface_session_sparse_block_graphics_output"
        "--scenario" "surface_session_cursor_overlay"
        "--scenario" "surface_session_selection_snapshot"
        "--scenario" "surface_session_resize_smoke_boundary"
        "--scenario" "surface_session_viewport_change_smoke_boundary"
        "--scenario" "surface_session_alternate_buffer_smoke_boundary"
        "--scenario" "surface_session_style_color_mode_smoke_boundary"
        "--scenario" "surface_session_hyperlink_smoke_boundary"
        "--scenario" "surface_session_public_projection_boundary"
        "--validate-json"
    )

    set_tests_properties(vnm_terminal_embedded_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark"
        TIMEOUT 240
    )

    vnm_terminal_benchmark_add_readback_test(
        vnm_terminal_embedded_benchmark_renderer_gate
        "--iterations" "3"
        "--warmup" "1"
        "--scenario" "dense_repaint"
        "--scenario" "ascii_full_dirty_reuse_only"
        "--scenario" "scroll_burst"
        "--scenario" "cursor_repaint_toggle"
        "--scenario" "single_row_geometry_update"
        "--output" "${CMAKE_CURRENT_BINARY_DIR}/renderer_gate_output.json"
        "--quiet"
        "--validate-json"
    )

    set_tests_properties(vnm_terminal_embedded_benchmark_renderer_gate PROPERTIES
        LABELS "readiness;benchmark;renderer"
        TIMEOUT 90
    )

    vnm_terminal_benchmark_add_readback_test(
        vnm_terminal_embedded_benchmark_single_bmp_render_cell_gate
        "--iterations" "1"
        "--warmup" "1"
        "--window-size" "240x120"
        "--grid" "6x20"
        "--scenario" "block_graphics_full_dirty_reuse_only"
        "--scenario" "box_graphics_full_dirty_reuse_only"
        "--scenario" "cjk_full_dirty_reuse_only"
        "--scenario" "mixed_non_ascii_full_dirty_reuse_only"
        "--scenario" "surface_session_block_graphics_output"
        "--scenario" "surface_session_box_graphics_output"
        "--scenario" "surface_session_cjk_output"
        "--scenario" "surface_session_mixed_non_ascii_output"
        "--output" "${CMAKE_CURRENT_BINARY_DIR}/single_bmp_render_cell_gate_output.json"
        "--quiet"
        "--validate-json"
    )

    set_tests_properties(vnm_terminal_embedded_benchmark_single_bmp_render_cell_gate PROPERTIES
        LABELS "readiness;benchmark;renderer"
        TIMEOUT 90
    )

    if(VNM_TERMINAL_ENABLE_PROFILING)
        vnm_terminal_benchmark_add_readback_test(
            vnm_terminal_embedded_benchmark_profile_validate
            "--iterations" "1"
            "--warmup" "0"
            "--grid" "48x160"
            "--window-size" "1280x768"
            "--dirty-rows" "5"
            "--dirty-row-stride" "3"
            "--scenario" "surface_session_sparse_ascii_output"
            "--scenario" "surface_session_sparse_block_graphics_output"
            "--scenario" "surface_session_cursor_overlay"
            "--scenario" "surface_session_selection_snapshot"
            "--scenario" "surface_session_resize_smoke_boundary"
            "--scenario" "surface_session_viewport_change_smoke_boundary"
            "--scenario" "surface_session_alternate_buffer_smoke_boundary"
            "--scenario" "surface_session_style_color_mode_smoke_boundary"
            "--scenario" "surface_session_hyperlink_smoke_boundary"
            "--scenario" "surface_session_public_projection_boundary"
            "--profile"
            "--profile-json" "${CMAKE_CURRENT_BINARY_DIR}/profile_validate_profile.json"
            "--profile-text" "${CMAKE_CURRENT_BINARY_DIR}/profile_validate_profile.txt"
            "--output" "${CMAKE_CURRENT_BINARY_DIR}/profile_validate_output.json"
            "--quiet"
            "--validate-json"
        )

        set_tests_properties(vnm_terminal_embedded_benchmark_profile_validate PROPERTIES
            LABELS "readiness;benchmark;profile"
            TIMEOUT 180
        )
    endif()
endif()
