if(WIN32)
    set(vnm_terminal_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)
find_package(Threads REQUIRED)
include("${PROJECT_SOURCE_DIR}/cmake/vnm_terminal_msdf_text_dependency.cmake")

function(vnm_terminal_require_qt_plugin_file plugin_target purpose)
    if(NOT TARGET ${plugin_target})
        message(FATAL_ERROR
            "${purpose} requires Qt plugin target ${plugin_target}")
    endif()

    get_target_property(plugin_target_type ${plugin_target} TYPE)
    if(NOT plugin_target_type MATCHES "^(MODULE_LIBRARY|SHARED_LIBRARY)$")
        message(FATAL_ERROR
            "${purpose} requires ${plugin_target} to be a dynamic plugin; target type is ${plugin_target_type}")
    endif()
endfunction()

function(vnm_terminal_qt_offscreen_test_environment out_var)
    set(${out_var}
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        PARENT_SCOPE)
endfunction()

function(vnm_terminal_qt_readback_test_environment out_var)
    if(WIN32)
        set(${out_var}
            "QT_QPA_PLATFORM=windows"
            "QSG_RENDER_LOOP=threaded"
            "QSG_RHI_BACKEND=d3d11"
            PARENT_SCOPE)
    else()
        set(${out_var}
            ${vnm_terminal_qt_offscreen_environment}
            PARENT_SCOPE)
    endif()
endfunction()

vnm_terminal_require_qt_plugin_file(
    Qt6::QOffscreenIntegrationPlugin
    "CTest offscreen runs")
vnm_terminal_qt_offscreen_test_environment(vnm_terminal_qt_offscreen_environment)
vnm_terminal_qt_readback_test_environment(vnm_terminal_qt_readback_environment)

if(WIN32)
    set(vnm_terminal_qt_windows_qpa_environment
        "QT_QPA_PLATFORM=windows"
    )
else()
    set(vnm_terminal_qt_windows_qpa_environment)
endif()

set(vnm_terminal_qt_scale_environment
    "QT_SCREEN_SCALE_FACTORS="
    "QT_AUTO_SCREEN_SCALE_FACTOR=0"
    "QT_DEVICE_PIXEL_RATIO="
    "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
)

set(vnm_terminal_qt_unscaled_environment
    "QT_SCALE_FACTOR=1"
    ${vnm_terminal_qt_scale_environment}
)

set(vnm_terminal_qt_scaled_environment
    "QT_SCALE_FACTOR=1.5"
    ${vnm_terminal_qt_scale_environment}
)

set(vnm_terminal_qt_fractional_environment
    "QT_SCALE_FACTOR=1.25"
    ${vnm_terminal_qt_scale_environment}
)


find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME vnm_terminal_primary_backing_phase_0b_guard
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/primary_backing_phase_0b_guard.py"
        --repo-root
        "${CMAKE_SOURCE_DIR}"
)

set_tests_properties(vnm_terminal_primary_backing_phase_0b_guard PROPERTIES
    LABELS "primary-backing;static"
)

add_test(
    NAME vnm_terminal_qsg_atlas_warm_set_generator
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/qsg_atlas_warm_set/generate_qsg_atlas_warm_set.py"
        --self-test
)

add_test(
    NAME vnm_terminal_qsg_atlas_warm_set_drift
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/qsg_atlas_warm_set/generate_qsg_atlas_warm_set.py"
        --check
        "${CMAKE_SOURCE_DIR}/tools/qsg_atlas_warm_set/qsg_atlas_warm_set.toml"
        "${CMAKE_SOURCE_DIR}/include/vnm_terminal/internal/qsg_atlas_warm_set.h"
)

set_tests_properties(
    vnm_terminal_qsg_atlas_warm_set_generator
    vnm_terminal_qsg_atlas_warm_set_drift
    PROPERTIES
        LABELS "qsg;atlas;warm-set;static"
)

set(vnm_terminal_default_capture_replay_dir "")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/conformance/captures")
    set(vnm_terminal_default_capture_replay_dir
        "${CMAKE_CURRENT_SOURCE_DIR}/conformance/captures")
endif()

set(VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR "" CACHE PATH
    "Unicode data directory used by vnm_terminal_unicode_data_conformance.")
set(VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE OFF CACHE BOOL
    "Add optional Unicode conformance checks for full GraphemeBreakTest and emoji ZWJ sequences.")
set(VNM_TERMINAL_CAPTURE_REPLAY_DIR "${vnm_terminal_default_capture_replay_dir}" CACHE PATH
    "Directory of local .raw/.vnm_capture captures for replay conformance.")
set(VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR "" CACHE PATH
    "Directory of graphics-protocol captures for replay conformance.")
set(VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR "${vnm_terminal_default_capture_replay_dir}" CACHE PATH
    "Directory of seed files for parser fuzz-smoke replay.")
set(VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR "" CACHE PATH
    "Optional directory of .ndjson transcript files replayed by vnm_terminal_transcript_replay.")
set(VNM_TERMINAL_BUILD_LIBFUZZER OFF CACHE BOOL
    "Build the parser libFuzzer harness target when the compiler supports it.")
set(VNM_TERMINAL_LIBVTERM_DIFF_COMMAND "" CACHE STRING
    "Semicolon-separated external libvterm differential command.")
set(VNM_TERMINAL_LIBVTERM_INCLUDE_DIR "" CACHE PATH
    "libvterm include directory for the optional linked differential harness.")
set(VNM_TERMINAL_LIBVTERM_LIBRARY "" CACHE FILEPATH
    "libvterm library path for the optional linked differential harness.")
set(VNM_TERMINAL_LIBVTERM_CORPUS_DIR "" CACHE PATH
    "Directory of .raw/.vnm_capture files for the linked libvterm differential harness.")
set(VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND "" CACHE STRING
    "Semicolon-separated external graphics conformance command.")
set(VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT "300" CACHE STRING
    "CTest timeout, in seconds, for external conformance commands.")
set(VNM_TERMINAL_ENABLE_GPU_ATLAS_SMOKE OFF CACHE BOOL
    "Add the canonical atlas QRhi render-node hardware smoke CTests.")
set(VNM_TERMINAL_ENABLE_GPU_ATLAS_PIXEL_PARITY OFF CACHE BOOL
    "Add the canonical atlas pixel-parity hardware CTests.")
set(VNM_TERMINAL_ENABLE_GPU_ATLAS_LAYOUT_PARITY OFF CACHE BOOL
    "Add the canonical atlas layout-parity hardware CTests.")
set(VNM_TERMINAL_ENABLE_GPU_ATLAS_REPORT OFF CACHE BOOL
    "Add the canonical atlas hardware report CTests.")
option(VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_TEXT
    "Make vnm_msdf_text available only to the qsg atlas test executable."
    ON)
option(VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_USE_SYSTEM_LIBS
    "Prefer find_package dependencies for the qsg atlas vnm_msdf_text test dependency."
    OFF)
set(VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_SOURCE_DIR "" CACHE PATH
    "Optional vnm_msdf_text source checkout used only by the qsg atlas tests.")
mark_as_advanced(
    VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_TEXT
    VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_SOURCE_DIR
    VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_USE_SYSTEM_LIBS
)

function(vnm_terminal_configure_test_process target_name)
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /FIvnm_terminal_test_process.h
            /FS
        )
    endif()
endfunction()

function(vnm_terminal_add_external_conformance_command test_name labels command_value)
    if("${command_value}" STREQUAL "")
        return()
    endif()

    set(vnm_terminal_external_command ${command_value})
    add_test(
        NAME ${test_name}
        COMMAND ${vnm_terminal_external_command}
    )

    set_tests_properties(${test_name} PROPERTIES
        LABELS "${labels}"
        TIMEOUT "${VNM_TERMINAL_EXTERNAL_CONFORMANCE_TIMEOUT}"
    )
endfunction()

add_executable(vnm_terminal_profiler
    profiler/hierarchical_profiler_tests.cpp
)

target_include_directories(vnm_terminal_profiler PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_compile_definitions(vnm_terminal_profiler PRIVATE
    VNM_TERMINAL_PROFILING_ENABLED=1
)

target_link_libraries(vnm_terminal_profiler PRIVATE
    Threads::Threads
)

set_target_properties(vnm_terminal_profiler PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_profiler
    COMMAND
        "$<TARGET_FILE:vnm_terminal_profiler>"
)

add_executable(vnm_terminal_parser_ir
    parser_ir/parser_ir_tests.cpp
)

target_link_libraries(vnm_terminal_parser_ir PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_parser_ir PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_parser_ir
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_parser_ir>"
)

add_executable(vnm_terminal_unicode_width
    unicode_width/unicode_width_tests.cpp
)

target_link_libraries(vnm_terminal_unicode_width PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_unicode_width PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_unicode_width
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_unicode_width>"
)

add_test(
    NAME vnm_terminal_unicode_width_generator
    COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/unicode_width/generate_unicode_width_tables.py"
        --self-test
)

add_executable(vnm_terminal_unicode_width_tables
    unicode_width_tables/unicode_width_tables_tests.cpp
)

target_link_libraries(vnm_terminal_unicode_width_tables PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_unicode_width_tables PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_unicode_width_tables
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_unicode_width_tables>"
)

add_executable(vnm_terminal_history_ring
    history_ring/history_ring_tests.cpp
)

target_link_libraries(vnm_terminal_history_ring PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_history_ring PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_history_ring
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_history_ring>"
)

add_executable(vnm_terminal_history_row_record_codec
    history_row_record_codec/history_row_record_codec_tests.cpp
)

target_link_libraries(vnm_terminal_history_row_record_codec PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_history_row_record_codec PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_history_row_record_codec
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_history_row_record_codec>"
)

add_executable(vnm_terminal_history_row_traversal
    history_row_record_codec/history_row_traversal_tests.cpp
)

target_link_libraries(vnm_terminal_history_row_traversal PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_history_row_traversal PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_history_row_traversal
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_history_row_traversal>"
)

add_executable(vnm_terminal_history_phase8_benchmark
    history_row_record_codec/history_row_record_phase8_benchmark.cpp
)

target_link_libraries(vnm_terminal_history_phase8_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_history_phase8_benchmark PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_history_phase8_benchmark
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_history_phase8_benchmark>"
)

add_executable(vnm_terminal_sequence_matrix
    sequence_matrix/sequence_matrix_tests.cpp
)

set_target_properties(vnm_terminal_sequence_matrix PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_sequence_matrix
    COMMAND
        "$<TARGET_FILE:vnm_terminal_sequence_matrix>"
        "${CMAKE_SOURCE_DIR}/docs/terminal_sequence_matrix.md"
        "${CMAKE_SOURCE_DIR}/docs/terminal_conformance_oracles.md"
)

add_executable(vnm_terminal_canvas_fixture_contract
    terminal_canvas_fixture/terminal_canvas_fixture_tests.cpp
)

add_dependencies(vnm_terminal_canvas_fixture_contract
    vnm_terminal_canvas_fixture
)

target_include_directories(vnm_terminal_canvas_fixture_contract PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(vnm_terminal_canvas_fixture_contract PRIVATE
    Qt6::Core
)

set_target_properties(vnm_terminal_canvas_fixture_contract PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_canvas_fixture_contract
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture_contract>"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
)

add_executable(vnm_terminal_qt_render_smoke
    qt_render_smoke/render_smoke_tests.cpp
)

target_link_libraries(vnm_terminal_qt_render_smoke PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::QuickPrivate
)

set_target_properties(vnm_terminal_qt_render_smoke PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_render_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_readback_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_render_smoke>"
)

# The smoke test compares accelerated-atlas rasterization against a hardware GPU
# reference and reports return code 77 to skip on software/headless RHI backends
# (CI runners' llvmpipe/WARP devices, or an offscreen platform with no GPU).
set_tests_properties(
    vnm_terminal_qt_render_smoke
    PROPERTIES
        SKIP_RETURN_CODE 77
)

add_executable(vnm_terminal_qt_metrics
    qt_metrics/qt_metrics_tests.cpp
)

target_link_libraries(vnm_terminal_qt_metrics PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_qt_metrics PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_metrics
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_metrics>"
        "--expect-normal"
)

add_test(
    NAME vnm_terminal_qt_metrics_scaled
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_scaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qt_metrics>"
        "--expect-scaled"
)

add_executable(vnm_terminal_profile_text
    profile_text/profile_text_tests.cpp
)

target_link_libraries(vnm_terminal_profile_text PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_profile_text PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_profile_text
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_profile_text>"
)

set_tests_properties(vnm_terminal_profile_text PROPERTIES
    TIMEOUT 60
)

# Golden characterization test for the shared text-layout diagnostics block.
# Feeds a fabricated stats fixture through the detail emitters and asserts the
# TEXT output byte-for-byte and the JSON output key/value-string-exact. The
# emitters live in src/diagnostics/metric_descriptor.h (+ the atlas tables in
# src/diagnostics/atlas_metric_descriptors.h), so the test reaches into
# the source tree for those internal headers.
add_executable(vnm_terminal_diagnostics_text_layout
    diagnostics_text_layout/diagnostics_text_layout_tests.cpp
)

target_include_directories(vnm_terminal_diagnostics_text_layout PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(vnm_terminal_diagnostics_text_layout PRIVATE
    VNM_TERMINAL_PROFILING_ENABLED=1
)

target_link_libraries(vnm_terminal_diagnostics_text_layout PRIVATE
    Qt6::Core
    Qt6::Gui
)

set_target_properties(vnm_terminal_diagnostics_text_layout PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_diagnostics_text_layout
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_diagnostics_text_layout>"
)

set_tests_properties(vnm_terminal_diagnostics_text_layout PROPERTIES
    LABELS "diagnostics;metrics;golden"
    TIMEOUT 30
)

add_executable(vnm_terminal_render_cell_text
    render_cell_text/render_cell_text_tests.cpp
)

target_link_libraries(vnm_terminal_render_cell_text PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_render_cell_text PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_render_cell_text
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_render_cell_text>"
)

add_executable(vnm_terminal_render_snapshot
    render_snapshot/render_snapshot_tests.cpp
)

target_link_libraries(vnm_terminal_render_snapshot PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_render_snapshot PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_render_snapshot
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_render_snapshot>"
)

add_executable(vnm_terminal_render_frame
    render_frame/render_frame_tests.cpp
)

target_link_libraries(vnm_terminal_render_frame PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_render_frame PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_render_frame
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_render_frame>"
)

add_executable(vnm_terminal_diagnostics_schema_sync
    diagnostics_schema/schema_sync_tests.cpp
)

target_include_directories(vnm_terminal_diagnostics_schema_sync PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(vnm_terminal_diagnostics_schema_sync PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_diagnostics_schema_sync PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_diagnostics_schema_sync
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_diagnostics_schema_sync>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../docs/diagnostics_schema.md"
)

if(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    add_executable(vnm_terminal_transcript
        transcript/transcript_tests.cpp
    )

    add_dependencies(vnm_terminal_transcript
        vnm_terminal_transcript_replay
    )

    target_link_libraries(vnm_terminal_transcript PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_transcript PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_transcript
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_transcript>"
            "$<TARGET_FILE:vnm_terminal_transcript_replay>"
    )

    if(NOT VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR STREQUAL "")
        if(NOT IS_DIRECTORY "${VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR}")
            message(FATAL_ERROR
                "VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR is not a directory: "
                "${VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR}")
        endif()

        add_test(
            NAME vnm_terminal_transcript_replay_corpus
            COMMAND
                "${CMAKE_COMMAND}"
                "-Dreplay_executable=$<TARGET_FILE:vnm_terminal_transcript_replay>"
                "-Dcorpus_dir=${VNM_TERMINAL_TRANSCRIPT_REPLAY_CORPUS_DIR}"
                -P "${CMAKE_SOURCE_DIR}/tools/transcript_replay/run_transcript_corpus.cmake"
        )

        set_tests_properties(vnm_terminal_transcript_replay_corpus PROPERTIES
            LABELS "transcript;replay;corpus"
            TIMEOUT 300
        )
    endif()
endif()

set(vnm_terminal_qsg_atlas_msdf_text_enabled OFF)
if(VNM_TERMINAL_QSG_ATLAS_ENABLE_MSDF_TEXT)
    vnm_terminal_msdf_text_make_available(
        vnm_terminal_qsg_atlas_msdf_text_enabled
        CONTEXT "vnm_terminal_surface tests"
        REQUIRED_BY "qsg atlas tests"
        SOURCE_DIR_VAR VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_SOURCE_DIR
        SOURCE_DIR_OPTION VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_SOURCE_DIR
        USE_SYSTEM_LIBS "${VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_USE_SYSTEM_LIBS}")
endif()

add_executable(vnm_terminal_qsg_atlas
    qsg_atlas/atlas_tests.cpp
)

target_link_libraries(vnm_terminal_qsg_atlas PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::QuickPrivate
)

if(vnm_terminal_qsg_atlas_msdf_text_enabled)
    target_link_libraries(vnm_terminal_qsg_atlas PRIVATE
        vnm_msdf_text::vnm_msdf_text
    )
endif()

target_compile_definitions(vnm_terminal_qsg_atlas PRIVATE
    VNM_TERMINAL_QSG_ATLAS_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/qsg_atlas_renderer.cpp"
    VNM_TERMINAL_SURFACE_SOURCE_PATH="${PROJECT_SOURCE_DIR}/src/vnm_terminal_surface.cpp"
    VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER}>
    VNM_TERMINAL_QSG_ATLAS_MSDF_TEXT_ENABLED=$<BOOL:${vnm_terminal_qsg_atlas_msdf_text_enabled}>
    VNM_TERMINAL_TEST_BUILD_TYPE="$<CONFIG>"
    VNM_TERMINAL_PROFILING_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_PROFILING}>
)

set_target_properties(vnm_terminal_qsg_atlas PROPERTIES
    AUTOMOC OFF
)

if(WIN32)
    add_custom_command(
        TARGET vnm_terminal_qsg_atlas
        POST_BUILD
        COMMAND "$<TARGET_FILE_DIR:Qt6::Core>/windeployqt.exe"
            --no-compiler-runtime
            --no-translations
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
        VERBATIM
    )
endif()

add_test(
    NAME vnm_terminal_qsg_atlas
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
)

set_tests_properties(vnm_terminal_qsg_atlas PROPERTIES
    LABELS "qsg;atlas"
    TIMEOUT 60
)

if(WIN32)
    set(vnm_terminal_qsg_atlas_dense_grid_environment
        ${vnm_terminal_qt_windows_qpa_environment}
        "QSG_RENDER_LOOP=threaded"
        ${vnm_terminal_qt_fractional_environment}
    )
    add_test(
        NAME vnm_terminal_qsg_atlas_dense_grid_d3d11
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qsg_atlas_dense_grid_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
            "--dense-grid-smoke"
            "--backend"
            "d3d11"
    )

    set_tests_properties(
        vnm_terminal_qsg_atlas_dense_grid_d3d11
        PROPERTIES
            LABELS "qsg;hardware;atlas;cutover;dense-grid"
            SKIP_RETURN_CODE 77
            TIMEOUT 90
    )

    set(vnm_terminal_qsg_atlas_cursor_descender_environment
        ${vnm_terminal_qt_windows_qpa_environment}
        "QSG_RENDER_LOOP=threaded"
        ${vnm_terminal_qt_unscaled_environment}
    )
    add_test(
        NAME vnm_terminal_qsg_atlas_cursor_descender_d3d11
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qsg_atlas_cursor_descender_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
            "--cursor-descender-smoke"
            "--backend"
            "d3d11"
    )

    set_tests_properties(
        vnm_terminal_qsg_atlas_cursor_descender_d3d11
        PROPERTIES
            LABELS "qsg;hardware;atlas;cursor-descender"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    set(vnm_terminal_qsg_atlas_host_state_environment
        ${vnm_terminal_qt_windows_qpa_environment}
        "QSG_RENDER_LOOP=threaded"
        ${vnm_terminal_qt_unscaled_environment}
    )
    add_test(
        NAME vnm_terminal_qsg_atlas_host_state_d3d11
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qsg_atlas_host_state_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
            "--host-state-smoke"
            "--backend"
            "d3d11"
    )

    set_tests_properties(
        vnm_terminal_qsg_atlas_host_state_d3d11
        PROPERTIES
            LABELS "qsg;hardware;atlas;host-state;cutover"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    set(vnm_terminal_qsg_atlas_lcd_probe_environment
        ${vnm_terminal_qt_windows_qpa_environment}
        "QSG_RENDER_LOOP=threaded"
        "QSG_RHI_BACKEND=d3d11"
        "VNM_TERMINAL_LCD_ATLAS_ARTIFACT_DIR=${CMAKE_CURRENT_BINARY_DIR}/lcd_atlas_probe_d3d11"
        ${vnm_terminal_qt_unscaled_environment}
    )
    add_test(
        NAME vnm_terminal_qsg_atlas_lcd_capability_probe_d3d11
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qsg_atlas_lcd_probe_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
            "--lcd-capability-probe"
            "--backend"
            "d3d11"
    )

    set_tests_properties(
        vnm_terminal_qsg_atlas_lcd_capability_probe_d3d11
        PROPERTIES
            LABELS "qsg;hardware;atlas;lcd-probe"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )

    set(vnm_terminal_qsg_atlas_warm_lazy_environment
        ${vnm_terminal_qt_windows_qpa_environment}
        "QSG_RENDER_LOOP=threaded"
        "QSG_RHI_BACKEND=d3d11"
        ${vnm_terminal_qt_unscaled_environment}
    )
    add_test(
        NAME vnm_terminal_qsg_atlas_warm_lazy_d3d11
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qsg_atlas_warm_lazy_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
            "--warm-lazy-smoke"
            "--backend"
            "d3d11"
    )

    set_tests_properties(
        vnm_terminal_qsg_atlas_warm_lazy_d3d11
        PROPERTIES
            LABELS "qsg;hardware;atlas;warm-lazy"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
    )
endif()

if(VNM_TERMINAL_ENABLE_GPU_ATLAS_SMOKE)
    set(vnm_terminal_qsg_atlas_smoke_backends)
    if(WIN32)
        list(APPEND vnm_terminal_qsg_atlas_smoke_backends
            d3d11
            d3d12
            vulkan
            opengl
        )
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        list(APPEND vnm_terminal_qsg_atlas_smoke_backends
            vulkan
            opengl
        )
    elseif(APPLE)
        list(APPEND vnm_terminal_qsg_atlas_smoke_backends
            opengl
        )
    endif()

    foreach(atlas_backend IN LISTS vnm_terminal_qsg_atlas_smoke_backends)
        foreach(atlas_render_loop IN ITEMS threaded basic)
            add_test(
                NAME vnm_terminal_qsg_atlas_${atlas_render_loop}_${atlas_backend}
                COMMAND
                    "${CMAKE_COMMAND}" -E env
                    ${vnm_terminal_qt_windows_qpa_environment}
                    "QSG_RENDER_LOOP=${atlas_render_loop}"
                    ${vnm_terminal_qt_unscaled_environment}
                    "${vnm_terminal_qt_runtime_path}"
                    "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
                    "--render-smoke"
                    "--backend"
                    "${atlas_backend}"
            )

            set_tests_properties(
                vnm_terminal_qsg_atlas_${atlas_render_loop}_${atlas_backend}
                PROPERTIES
                    LABELS "qsg;hardware;atlas"
                    SKIP_RETURN_CODE 77
                    TIMEOUT 60
            )
        endforeach()
    endforeach()

endif()

if(VNM_TERMINAL_ENABLE_GPU_ATLAS_PIXEL_PARITY)
    set(vnm_terminal_qsg_atlas_pixel_parity_backends)
    if(WIN32)
        list(APPEND vnm_terminal_qsg_atlas_pixel_parity_backends
            d3d11
            d3d12
            vulkan
            opengl
        )
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        list(APPEND vnm_terminal_qsg_atlas_pixel_parity_backends
            vulkan
            opengl
        )
    elseif(APPLE)
        list(APPEND vnm_terminal_qsg_atlas_pixel_parity_backends
            opengl
        )
    endif()

    foreach(atlas_pixel_backend IN LISTS vnm_terminal_qsg_atlas_pixel_parity_backends)
        add_test(
            NAME vnm_terminal_qsg_atlas_pixel_parity_${atlas_pixel_backend}
            COMMAND
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_windows_qpa_environment}
                "QSG_RENDER_LOOP=threaded"
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
                "--pixel-parity"
                "--backend"
                "${atlas_pixel_backend}"
        )

        set_tests_properties(
            vnm_terminal_qsg_atlas_pixel_parity_${atlas_pixel_backend}
            PROPERTIES
                LABELS "qsg;hardware;atlas;pixel-parity"
                SKIP_RETURN_CODE 77
                TIMEOUT 90
        )
    endforeach()
endif()

if(VNM_TERMINAL_ENABLE_GPU_ATLAS_LAYOUT_PARITY)
    set(vnm_terminal_qsg_atlas_layout_parity_backends)
    if(WIN32)
        list(APPEND vnm_terminal_qsg_atlas_layout_parity_backends
            d3d11
            d3d12
            vulkan
            opengl
        )
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        list(APPEND vnm_terminal_qsg_atlas_layout_parity_backends
            vulkan
            opengl
        )
    elseif(APPLE)
        list(APPEND vnm_terminal_qsg_atlas_layout_parity_backends
            opengl
        )
    endif()

    foreach(atlas_layout_backend IN LISTS vnm_terminal_qsg_atlas_layout_parity_backends)
        add_test(
            NAME vnm_terminal_qsg_atlas_layout_parity_${atlas_layout_backend}
            COMMAND
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_windows_qpa_environment}
                "QSG_RENDER_LOOP=threaded"
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
                "--layout-parity"
                "--backend"
                "${atlas_layout_backend}"
        )

        set_tests_properties(
            vnm_terminal_qsg_atlas_layout_parity_${atlas_layout_backend}
            PROPERTIES
                LABELS "qsg;hardware;atlas;layout-parity"
                SKIP_RETURN_CODE 77
                TIMEOUT 120
        )
    endforeach()
endif()

if(VNM_TERMINAL_ENABLE_GPU_ATLAS_REPORT)
    set(vnm_terminal_qsg_atlas_report_backends)
    if(WIN32)
        list(APPEND vnm_terminal_qsg_atlas_report_backends
            d3d11
            d3d12
            vulkan
            opengl
        )
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        list(APPEND vnm_terminal_qsg_atlas_report_backends
            vulkan
            opengl
        )
    elseif(APPLE)
        list(APPEND vnm_terminal_qsg_atlas_report_backends
            opengl
        )
    endif()

    foreach(atlas_report_backend IN LISTS vnm_terminal_qsg_atlas_report_backends)
        add_test(
            NAME vnm_terminal_qsg_atlas_report_${atlas_report_backend}
            COMMAND
                "${CMAKE_COMMAND}" -E env
                ${vnm_terminal_qt_windows_qpa_environment}
                "QSG_RENDER_LOOP=threaded"
                ${vnm_terminal_qt_unscaled_environment}
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_qsg_atlas>"
                "--atlas-report"
                "--backend"
                "${atlas_report_backend}"
        )

        set_tests_properties(
            vnm_terminal_qsg_atlas_report_${atlas_report_backend}
            PROPERTIES
                LABELS "qsg;hardware;atlas;report"
                SKIP_RETURN_CODE 77
                TIMEOUT 120
        )
    endforeach()

endif()

add_executable(vnm_terminal_shaping_contract
    shaping_contract/shaping_contract_tests.cpp
)

target_link_libraries(vnm_terminal_shaping_contract PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_shaping_contract PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_shaping_contract
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_shaping_contract>"
)

add_executable(vnm_terminal_surface_host
    surface_host/surface_host_tests.cpp
)

target_link_libraries(vnm_terminal_surface_host PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

target_compile_definitions(vnm_terminal_surface_host PRIVATE
    VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}>
)

set_target_properties(vnm_terminal_surface_host PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_surface_host
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_surface_host>"
)

set_tests_properties(vnm_terminal_surface_host PROPERTIES
    LABELS "conformance;input;mouse;paste;focus"
    TIMEOUT 60
)

add_executable(vnm_terminal_input_encoder
    input_encoder/input_encoder_tests.cpp
)

target_link_libraries(vnm_terminal_input_encoder PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_input_encoder PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_input_encoder
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_input_encoder>"
)

set_tests_properties(vnm_terminal_input_encoder PROPERTIES
    LABELS "conformance;input;mouse;paste;focus"
    TIMEOUT 60
)

add_executable(vnm_terminal_behavior_smoke
    behavior_smoke/behavior_smoke_tests.cpp
)

add_dependencies(vnm_terminal_behavior_smoke
    vnm_terminal_canvas_fixture
)

target_link_libraries(vnm_terminal_behavior_smoke PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

set_target_properties(vnm_terminal_behavior_smoke PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_behavior_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
        "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
)

set_tests_properties(vnm_terminal_behavior_smoke PROPERTIES
    TIMEOUT 60
)

add_test(
    NAME vnm_terminal_surface_behavior_smokes
    COMMAND
        "${CMAKE_COMMAND}" -E env
        ${vnm_terminal_qt_offscreen_environment}
        ${vnm_terminal_qt_unscaled_environment}
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
        "--surface-host"
)

set_tests_properties(vnm_terminal_surface_behavior_smokes PROPERTIES
    TIMEOUT 60
)

if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_test(
        NAME vnm_terminal_native_surface_behavior_smokes
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_behavior_smoke>"
            "--native-surface-host"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_native_surface_behavior_smokes PROPERTIES
        TIMEOUT 120
    )
endif()

if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_executable(vnm_terminal_compat_smoke
        compat_smoke/compat_smoke_tests.cpp
    )

    add_dependencies(vnm_terminal_compat_smoke
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_compat_smoke PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_compat_smoke PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_compat_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            ${vnm_terminal_qt_offscreen_environment}
            ${vnm_terminal_qt_unscaled_environment}
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_compat_smoke>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_compat_smoke PROPERTIES
        TIMEOUT 120
    )
endif()

add_executable(vnm_terminal_parser_randomized
    parser_randomized/parser_randomized_tests.cpp
)

target_link_libraries(vnm_terminal_parser_randomized PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_parser_randomized PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_parser_randomized
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_parser_randomized>"
        "${CMAKE_CURRENT_SOURCE_DIR}/parser_randomized"
)

set_tests_properties(vnm_terminal_parser_randomized PROPERTIES
    LABELS "conformance;fuzz;parser"
    TIMEOUT 60
)

add_executable(vnm_terminal_resource_lifecycle
    resource_lifecycle/resource_lifecycle_tests.cpp
)

target_link_libraries(vnm_terminal_resource_lifecycle PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_resource_lifecycle PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_resource_lifecycle
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_resource_lifecycle>"
)

set_tests_properties(vnm_terminal_resource_lifecycle PROPERTIES
    TIMEOUT 60
)

if(NOT VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR STREQUAL "")
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR is not a directory: "
            "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}")
    endif()

    add_executable(vnm_terminal_unicode_data_conformance
        conformance/unicode_data_conformance_tests.cpp
    )

    target_link_libraries(vnm_terminal_unicode_data_conformance PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
        Qt6::Core
    )

    set_target_properties(vnm_terminal_unicode_data_conformance PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_unicode_data_conformance
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_unicode_data_conformance>"
            "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}"
    )

    set_tests_properties(vnm_terminal_unicode_data_conformance PROPERTIES
        LABELS "conformance;unicode"
        TIMEOUT 120
    )

    if(VNM_TERMINAL_UNICODE_ASPIRATIONAL_CONFORMANCE)
        add_test(
            NAME vnm_terminal_unicode_data_aspirational_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_unicode_data_conformance>"
                "${VNM_TERMINAL_UNICODE_CONFORMANCE_DATA_DIR}"
                "--include-aspirational"
        )

        set_tests_properties(vnm_terminal_unicode_data_aspirational_conformance PROPERTIES
            LABELS "conformance;unicode;aspirational"
            TIMEOUT 120
        )
    endif()
endif()

if((NOT VNM_TERMINAL_CAPTURE_REPLAY_DIR STREQUAL "") OR
    (NOT VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR STREQUAL ""))
    add_executable(vnm_terminal_capture_replay_conformance
        conformance/capture_replay_conformance_tests.cpp
    )

    target_link_libraries(vnm_terminal_capture_replay_conformance PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_capture_replay_conformance PROPERTIES
        AUTOMOC OFF
    )

    if(NOT VNM_TERMINAL_CAPTURE_REPLAY_DIR STREQUAL "")
        if(NOT IS_DIRECTORY "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}")
            message(FATAL_ERROR
                "VNM_TERMINAL_CAPTURE_REPLAY_DIR is not a directory: "
                "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}")
        endif()

        add_test(
            NAME vnm_terminal_capture_replay_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_capture_replay_conformance>"
                "${VNM_TERMINAL_CAPTURE_REPLAY_DIR}"
        )

        set_tests_properties(vnm_terminal_capture_replay_conformance PROPERTIES
            LABELS "conformance;capture-replay"
            TIMEOUT 180
        )
    endif()

    if(NOT VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR STREQUAL "")
        if(NOT IS_DIRECTORY "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}")
            message(FATAL_ERROR
                "VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR is not a directory: "
                "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}")
        endif()

        add_test(
            NAME vnm_terminal_graphics_capture_replay_conformance
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "${vnm_terminal_qt_runtime_path}"
                "$<TARGET_FILE:vnm_terminal_capture_replay_conformance>"
                "${VNM_TERMINAL_GRAPHICS_CAPTURE_REPLAY_DIR}"
        )

        set_tests_properties(vnm_terminal_graphics_capture_replay_conformance PROPERTIES
            LABELS "conformance;graphics;capture-replay"
            TIMEOUT 180
        )
    endif()
endif()

if(NOT VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR STREQUAL "")
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR is not a directory: "
            "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}")
    endif()

    add_executable(vnm_terminal_parser_fuzz_smoke
        conformance/parser_libfuzzer_harness.cpp
    )

    target_link_libraries(vnm_terminal_parser_fuzz_smoke PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_parser_fuzz_smoke PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_parser_fuzz_smoke
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_parser_fuzz_smoke>"
            "${VNM_TERMINAL_FUZZ_SMOKE_CORPUS_DIR}"
    )

    set_tests_properties(vnm_terminal_parser_fuzz_smoke PROPERTIES
        LABELS "conformance;fuzz;parser"
        TIMEOUT 120
    )
endif()

if((NOT VNM_TERMINAL_LIBVTERM_INCLUDE_DIR STREQUAL "") OR
    (NOT VNM_TERMINAL_LIBVTERM_LIBRARY STREQUAL "") OR
    (NOT VNM_TERMINAL_LIBVTERM_CORPUS_DIR STREQUAL ""))
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_INCLUDE_DIR is not a directory: "
            "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${VNM_TERMINAL_LIBVTERM_LIBRARY}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_LIBRARY does not exist: "
            "${VNM_TERMINAL_LIBVTERM_LIBRARY}")
    endif()
    if(NOT IS_DIRECTORY "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}")
        message(FATAL_ERROR
            "VNM_TERMINAL_LIBVTERM_CORPUS_DIR is not a directory: "
            "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}")
    endif()

    add_executable(vnm_terminal_libvterm_differential
        conformance/libvterm_differential_tests.cpp
    )

    target_include_directories(vnm_terminal_libvterm_differential PRIVATE
        "${VNM_TERMINAL_LIBVTERM_INCLUDE_DIR}"
    )

    target_link_libraries(vnm_terminal_libvterm_differential PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
        "${VNM_TERMINAL_LIBVTERM_LIBRARY}"
    )

    set_target_properties(vnm_terminal_libvterm_differential PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_libvterm_differential
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_libvterm_differential>"
            "${VNM_TERMINAL_LIBVTERM_CORPUS_DIR}"
    )

    set_tests_properties(vnm_terminal_libvterm_differential PROPERTIES
        LABELS "conformance;libvterm"
        TIMEOUT 180
    )
endif()

if(VNM_TERMINAL_BUILD_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "VNM_TERMINAL_BUILD_LIBFUZZER requires a Clang compiler")
    endif()

    add_executable(vnm_terminal_parser_libfuzzer
        conformance/parser_libfuzzer_harness.cpp
    )

    target_compile_definitions(vnm_terminal_parser_libfuzzer PRIVATE
        VNM_TERMINAL_LIBFUZZER
    )

    target_compile_options(vnm_terminal_parser_libfuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )

    target_link_options(vnm_terminal_parser_libfuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
    )

    target_link_libraries(vnm_terminal_parser_libfuzzer PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_parser_libfuzzer PROPERTIES
        AUTOMOC OFF
    )
endif()

vnm_terminal_add_external_conformance_command(
    vnm_terminal_external_libvterm_diff
    "conformance;libvterm;external"
    "${VNM_TERMINAL_LIBVTERM_DIFF_COMMAND}")

vnm_terminal_add_external_conformance_command(
    vnm_terminal_external_graphics_conformance
    "conformance;graphics;external"
    "${VNM_TERMINAL_GRAPHICS_CONFORMANCE_COMMAND}")

add_executable(vnm_terminal_qt_posture
    qt_posture/qt_posture_tests.cpp
)

set_target_properties(vnm_terminal_qt_posture PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_qt_posture
    COMMAND
        "$<TARGET_FILE:vnm_terminal_qt_posture>"
        "${CMAKE_COMMAND}"
        "${CMAKE_GENERATOR}"
        "$<IF:$<BOOL:${CMAKE_MAKE_PROGRAM}>,${CMAKE_MAKE_PROGRAM},->"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/qt_posture_cases"
)

add_executable(vnm_terminal_screen_basic
    screen_basic/basic_model_tests.cpp
)

target_link_libraries(vnm_terminal_screen_basic PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_screen_basic PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_basic
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_basic>"
)

add_executable(vnm_terminal_screen_sgr
    screen_sgr/sgr_model_tests.cpp
)

target_link_libraries(vnm_terminal_screen_sgr PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_screen_sgr PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_sgr
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_sgr>"
)

add_executable(vnm_terminal_screen_operations
    screen_operations/model_ops_tests.cpp
)

target_link_libraries(vnm_terminal_screen_operations PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_screen_operations PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_operations
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_operations>"
)

add_executable(vnm_terminal_screen_alternate
    screen_alternate/alternate_screen_tests.cpp
)

target_link_libraries(vnm_terminal_screen_alternate PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_screen_alternate PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_screen_alternate
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_screen_alternate>"
)

add_executable(vnm_terminal_terminal_modes
    terminal_modes/private_modes_tests.cpp
)

target_link_libraries(vnm_terminal_terminal_modes PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_terminal_modes PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_terminal_modes
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_terminal_modes>"
)

add_executable(vnm_terminal_viewport
    viewport/viewport_controller_tests.cpp
)

target_link_libraries(vnm_terminal_viewport PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_viewport PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_viewport
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_viewport>"
)

add_executable(vnm_terminal_backend_session
    backend_session/backend_session_tests.cpp
)

target_link_libraries(vnm_terminal_backend_session PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

target_compile_definitions(vnm_terminal_backend_session PRIVATE
    VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}>
)

set_target_properties(vnm_terminal_backend_session PROPERTIES
    AUTOMOC OFF
)

add_test(
    NAME vnm_terminal_backend_session
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "${vnm_terminal_qt_runtime_path}"
        "$<TARGET_FILE:vnm_terminal_backend_session>"
)

if(WIN32)
    add_executable(vnm_terminal_windows_conpty_backend
        windows_conpty_backend/windows_conpty_backend_tests.cpp
    )

    add_dependencies(vnm_terminal_windows_conpty_backend
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_windows_conpty_backend PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_windows_conpty_backend PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_windows_conpty_backend
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_windows_conpty_backend>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_windows_conpty_backend PROPERTIES
        LABELS "conformance;windows;conpty"
        TIMEOUT 120
    )
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    add_executable(vnm_terminal_posix_pty_backend
        posix_pty_backend/posix_pty_backend_tests.cpp
    )

    add_dependencies(vnm_terminal_posix_pty_backend
        vnm_terminal_canvas_fixture
    )

    target_link_libraries(vnm_terminal_posix_pty_backend PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_posix_pty_backend PROPERTIES
        AUTOMOC OFF
    )

    add_test(
        NAME vnm_terminal_posix_pty_backend
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_posix_pty_backend>"
            "$<TARGET_FILE:vnm_terminal_canvas_fixture>"
    )

    set_tests_properties(vnm_terminal_posix_pty_backend PROPERTIES
        LABELS "conformance;posix;pty"
        TIMEOUT 120
    )
endif()

set(vnm_terminal_test_process_targets
    vnm_terminal_canvas_fixture
    vnm_terminal_profiler
    vnm_terminal_parser_ir
    vnm_terminal_unicode_width
    vnm_terminal_unicode_width_tables
    vnm_terminal_history_ring
    vnm_terminal_history_row_record_codec
    vnm_terminal_history_row_traversal
    vnm_terminal_history_phase8_benchmark
    vnm_terminal_sequence_matrix
    vnm_terminal_canvas_fixture_contract
    vnm_terminal_qt_render_smoke
    vnm_terminal_qt_metrics
    vnm_terminal_profile_text
    vnm_terminal_diagnostics_text_layout
    vnm_terminal_render_cell_text
    vnm_terminal_render_snapshot
    vnm_terminal_render_frame
    vnm_terminal_qsg_atlas
    vnm_terminal_shaping_contract
    vnm_terminal_input_encoder
    vnm_terminal_surface_host
    vnm_terminal_behavior_smoke
    vnm_terminal_parser_randomized
    vnm_terminal_resource_lifecycle
    vnm_terminal_qt_posture
    vnm_terminal_screen_basic
    vnm_terminal_screen_sgr
    vnm_terminal_screen_operations
    vnm_terminal_screen_alternate
    vnm_terminal_terminal_modes
    vnm_terminal_viewport
    vnm_terminal_backend_session
)

if(TARGET vnm_terminal_transcript)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_transcript
    )
endif()

if(WIN32)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_windows_conpty_backend
    )
endif()

if(TARGET vnm_terminal_posix_pty_backend)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_posix_pty_backend
    )
endif()

if(TARGET vnm_terminal_compat_smoke)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_compat_smoke
    )
endif()

if(TARGET vnm_terminal_unicode_data_conformance)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_unicode_data_conformance
    )
endif()

if(TARGET vnm_terminal_capture_replay_conformance)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_capture_replay_conformance
    )
endif()

if(TARGET vnm_terminal_parser_fuzz_smoke)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_parser_fuzz_smoke
    )
endif()

if(TARGET vnm_terminal_libvterm_differential)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_libvterm_differential
    )
endif()

if(TARGET vnm_terminal_parser_libfuzzer)
    list(APPEND vnm_terminal_test_process_targets
        vnm_terminal_parser_libfuzzer
    )
endif()

add_test(
    NAME vnm_terminal_surface_rejects_distribution_transcript_build
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dgenerator=${CMAKE_GENERATOR}"
        "-Dgenerator_platform=${CMAKE_GENERATOR_PLATFORM}"
        "-Dgenerator_toolset=${CMAKE_GENERATOR_TOOLSET}"
        "-Dsource_dir=${PROJECT_SOURCE_DIR}"
        "-Dbinary_dir=${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_surface_distribution_guard"
        "-Dexpected_regex=VNM_TERMINAL_DISTRIBUTION_BUILD=ON requires"
        -P "${PROJECT_SOURCE_DIR}/tools/expect_configure_failure.cmake"
)

set_tests_properties(vnm_terminal_surface_rejects_distribution_transcript_build PROPERTIES
    TIMEOUT 30
)

add_test(
    NAME vnm_terminal_surface_package_consumer_builds
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dgenerator=${CMAKE_GENERATOR}"
        "-Dgenerator_platform=${CMAKE_GENERATOR_PLATFORM}"
        "-Dgenerator_toolset=${CMAKE_GENERATOR_TOOLSET}"
        "-Dmake_program=${CMAKE_MAKE_PROGRAM}"
        "-Dpackage_binary_dir=${CMAKE_BINARY_DIR}"
        "-Dinstall_config=$<CONFIG>"
        "-Dinstall_dir=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_consumer/install"
        "-Dconsumer_source_dir=${PROJECT_SOURCE_DIR}/tests/package_smoke"
        "-Dconsumer_binary_dir=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_consumer/consumer_build"
        "-Dqt6_dir=${Qt6_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tests/package_smoke/expect_consumer_builds.cmake"
)

set_tests_properties(vnm_terminal_surface_package_consumer_builds PROPERTIES
    LABELS "package;posture"
    TIMEOUT 180
)

add_test(
    NAME vnm_terminal_surface_package_rejects_qt_private_api_minor_mismatch
    COMMAND
        "${CMAKE_COMMAND}"
        "-Dgenerator=${CMAKE_GENERATOR}"
        "-Dgenerator_platform=${CMAKE_GENERATOR_PLATFORM}"
        "-Dgenerator_toolset=${CMAKE_GENERATOR_TOOLSET}"
        "-Dmake_program=${CMAKE_MAKE_PROGRAM}"
        "-Dpackage_binary_dir=${CMAKE_BINARY_DIR}"
        "-Dinstall_config=$<CONFIG>"
        "-Dinstall_dir=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_private_qt_minor/install"
        "-Dmismatch_install_dir=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_private_qt_minor/mismatch_install"
        "-Dconsumer_source_dir=${PROJECT_SOURCE_DIR}/tests/package_smoke"
        "-Dconsumer_binary_dir=${CMAKE_CURRENT_BINARY_DIR}/package_smoke_private_qt_minor/consumer_build"
        "-Dqt6_dir=${Qt6_DIR}"
        "-Dqt_private_api_version_major=${VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR}"
        "-Dqt_private_api_version_minor=${VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR}"
        -P "${PROJECT_SOURCE_DIR}/tests/package_smoke/expect_private_qt_minor_mismatch.cmake"
)

set_tests_properties(vnm_terminal_surface_package_rejects_qt_private_api_minor_mismatch PROPERTIES
    LABELS "package;posture"
    TIMEOUT 120
)






foreach(vnm_terminal_test_process_target IN LISTS vnm_terminal_test_process_targets)
    vnm_terminal_configure_test_process(${vnm_terminal_test_process_target})
endforeach()
