cmake_minimum_required(VERSION 3.21)

option(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY
    "Build diagnostic transcript capture/replay recorder, replay tool, and transcript tests"
    OFF)
option(VNM_TERMINAL_DISTRIBUTION_BUILD
    "Configure a distribution build; diagnostic transcript capture/replay must stay disabled"
    OFF)

if(VNM_TERMINAL_DISTRIBUTION_BUILD AND VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    message(FATAL_ERROR
        "VNM_TERMINAL_DISTRIBUTION_BUILD=ON requires "
        "VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=OFF")
endif()

project(vnm_terminal_surface LANGUAGES CXX VERSION 0.1.0)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(vnm_terminal_surface_build_testing_default OFF)
if(PROJECT_IS_TOP_LEVEL)
    set(vnm_terminal_surface_build_testing_default ON)
endif()

option(VNM_TERMINAL_SURFACE_BUILD_TESTING
    "Build vnm_terminal_surface tests"
    ${vnm_terminal_surface_build_testing_default})
option(VNM_TERMINAL_BUILD_REQUIRED_READINESS "Build vnm_terminal_surface readiness validation gates" OFF)
option(VNM_TERMINAL_BUILD_BENCHMARKS "Build vnm_terminal_surface benchmark executables" OFF)
option(VNM_TERMINAL_ENABLE_PROFILING "Compile vnm_terminal_surface profile scope instrumentation" OFF)
option(VNM_TERMINAL_ENABLE_SHADER_GENERATION
    "Require Qt ShaderTools for source-tree shader tooling"
    OFF)
option(VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER
    "Build the MSDF text renderer"
    ON)
set(VNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS OFF CACHE BOOL
    "Prefer find_package dependencies for the MSDF text renderer dependency.")
set(VNM_TERMINAL_MSDF_TEXT_RENDERER_SOURCE_DIR "" CACHE PATH
    "Optional vnm_msdf_text source checkout used by the MSDF text renderer.")
mark_as_advanced(
    VNM_TERMINAL_MSDF_TEXT_RENDERER_SOURCE_DIR
    VNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS
)
if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT VNM_TERMINAL_BUILD_BENCHMARKS)
    set(VNM_TERMINAL_BUILD_BENCHMARKS ON)
endif()
if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT VNM_TERMINAL_ENABLE_PROFILING)
    set(VNM_TERMINAL_ENABLE_PROFILING ON)
endif()

set(vnm_terminal_surface_testing_enabled OFF)
if(BUILD_TESTING AND VNM_TERMINAL_SURFACE_BUILD_TESTING)
    set(vnm_terminal_surface_testing_enabled ON)
endif()

if(VNM_TERMINAL_BUILD_REQUIRED_READINESS AND NOT vnm_terminal_surface_testing_enabled)
    message(FATAL_ERROR
        "VNM_TERMINAL_BUILD_REQUIRED_READINESS requires BUILD_TESTING=ON and "
        "VNM_TERMINAL_SURFACE_BUILD_TESTING=ON")
endif()

set(vnm_terminal_qt_components
    Core
    Gui
    Quick
    QuickPrivate
    GuiPrivate
)

if(VNM_TERMINAL_ENABLE_SHADER_GENERATION)
    list(APPEND vnm_terminal_qt_components ShaderTools)
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS ${vnm_terminal_qt_components})
include(cmake/vnm_terminal_qt_posture.cmake)

set(vnm_terminal_qt_targets
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

vnm_terminal_validate_qt_posture(DIRECT_TARGETS
    ${vnm_terminal_qt_targets}
)

set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MAJOR "${Qt6_VERSION_MAJOR}")
set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR "${Qt6_VERSION_MINOR}")

add_library(vnm_terminal_surface STATIC
    include/vnm_terminal/internal/backend_contract.h
    include/vnm_terminal/internal/csi_parameter_parsing.h
    include/vnm_terminal/internal/ime_contract.h
    include/vnm_terminal/internal/metrics_contract.h
    include/vnm_terminal/internal/parser_action.h
    include/vnm_terminal/internal/qsg_atlas_renderer.h
    include/vnm_terminal/internal/qsg_terminal_render_frame.h
    include/vnm_terminal/internal/qsg_terminal_renderer.h
    include/vnm_terminal/internal/qt_grid_metrics_provider.h
    include/vnm_terminal/internal/render_snapshot.h
    include/vnm_terminal/internal/selection_contract.h
    include/vnm_terminal/internal/session_contract.h
    include/vnm_terminal/internal/terminal_canvas_fixture_contract.h
    include/vnm_terminal/internal/terminal_history_ring.h
    include/vnm_terminal/internal/terminal_history_row_record_codec.h
    include/vnm_terminal/internal/terminal_history_row_traversal.h
    include/vnm_terminal/internal/terminal_byte_stream_parser.h
    include/vnm_terminal/internal/terminal_input_mode.h
    include/vnm_terminal/internal/terminal_input_encoder.h
    include/vnm_terminal/internal/terminal_public_projection.h
    include/vnm_terminal/internal/terminal_repaint_recovery.h
    include/vnm_terminal/internal/terminal_resize_controller.h
    include/vnm_terminal/internal/terminal_render_cell_text.h
    include/vnm_terminal/internal/terminal_screen_model.h
    include/vnm_terminal/internal/terminal_session.h
    include/vnm_terminal/internal/terminal_transcript.h
    include/vnm_terminal/internal/terminal_style.h
    include/vnm_terminal/internal/utf8_scan.h
    include/vnm_terminal/internal/unicode_width.h
    include/vnm_terminal/internal/unicode_width_tables.h
    include/vnm_terminal/internal/vnm_terminal_font.h
    include/vnm_terminal/internal/viewport_contract.h
    include/vnm_terminal/internal/vnm_terminal_surface_render_bridge.h
    include/vnm_terminal/internal/posix_pty_backend.h
    include/vnm_terminal/internal/windows_conpty_backend.h
    include/vnm_terminal/diagnostics/metrics_json.h
    include/vnm_terminal/diagnostics/profile_text.h
    include/vnm_terminal/font_metrics.h
    include/vnm_terminal/vnm_terminal_surface.h
    src/diagnostics/metric_descriptor.h
    src/diagnostics/metrics_json.cpp
    src/diagnostics/profile_text.cpp
    src/font_metrics.cpp
    src/terminal_history_ring.cpp
    src/terminal_history_row_record_codec.cpp
    src/terminal_history_row_traversal.cpp
    src/terminal_input_encoder.cpp
    src/terminal_public_projection.cpp
    src/terminal_repaint_recovery.cpp
    src/terminal_resize_controller.cpp
    src/terminal_session.cpp
    src/terminal_byte_stream_parser.cpp
    src/terminal_screen_model.cpp
    src/terminal_screen_model_snapshot.cpp
    src/metrics_contract.cpp
    src/native_backend_io_core.cpp
    src/qsg_atlas_renderer.cpp
    src/qsg_terminal_renderer.cpp
    src/qt_grid_metrics_provider.cpp
    src/selection_contract.cpp
    src/session_contract.cpp
    src/unicode_width_tables.cpp
    src/viewport_contract.cpp
    src/vnm_terminal_font.cpp
    src/vnm_terminal_surface.cpp
)

qt_add_resources(vnm_terminal_surface_resource_sources
    resources/vnm_terminal_surface.qrc
)

target_sources(vnm_terminal_surface PRIVATE
    ${vnm_terminal_surface_resource_sources}
)

if(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    target_sources(vnm_terminal_surface PRIVATE
        src/terminal_transcript.cpp
    )
else()
    target_sources(vnm_terminal_surface PRIVATE
        src/terminal_transcript_disabled.cpp
    )
endif()

if(WIN32)
    target_sources(vnm_terminal_surface PRIVATE
        src/windows_conpty_backend.cpp
    )
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    target_sources(vnm_terminal_surface PRIVATE
        src/posix_pty_backend.cpp
    )
endif()

target_include_directories(vnm_terminal_surface PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

target_link_libraries(vnm_terminal_surface PUBLIC
    ${vnm_terminal_qt_targets}
)

target_link_libraries(vnm_terminal_surface PRIVATE
    Qt6::GuiPrivate
    Qt6::QuickPrivate
)

target_compile_definitions(vnm_terminal_surface PUBLIC
    VNM_TERMINAL_PROFILING_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_PROFILING}>
)

target_compile_definitions(vnm_terminal_surface PRIVATE
    VNM_TERMINAL_MSDF_TEXT_RENDERER_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER}>
    VNM_TERMINAL_TRANSCRIPT_CAPTURE_REPLAY_ENABLED=$<BOOL:${VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY}>
)

if(VNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vnm_terminal_msdf_text_dependency.cmake")
    vnm_terminal_msdf_text_make_available(
        vnm_terminal_msdf_text_renderer_enabled
        CONTEXT "vnm_terminal_surface"
        REQUIRED_BY "MSDF text renderer"
        SOURCE_DIR_VAR VNM_TERMINAL_MSDF_TEXT_RENDERER_SOURCE_DIR
        SOURCE_DIR_OPTION VNM_TERMINAL_MSDF_TEXT_RENDERER_SOURCE_DIR
        USE_SYSTEM_LIBS "${VNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS}")
    target_link_libraries(vnm_terminal_surface PRIVATE
        "$<BUILD_INTERFACE:vnm_msdf_text::vnm_msdf_text>"
    )
endif()

if(MSVC)
    target_compile_options(vnm_terminal_surface PRIVATE
        /FS
    )
endif()

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    find_library(VNM_TERMINAL_PTY_UTIL_LIBRARY util)
    if(VNM_TERMINAL_PTY_UTIL_LIBRARY)
        target_link_libraries(vnm_terminal_surface PRIVATE
            ${VNM_TERMINAL_PTY_UTIL_LIBRARY}
        )
    endif()
endif()

cmake_language(DEFER CALL vnm_terminal_validate_qt_link_interface vnm_terminal_surface)

add_library(vnm_terminal_surface::vnm_terminal_surface ALIAS vnm_terminal_surface)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # FreeType is statically linked into consumers through vnm_msdf_text/msdfgen.
    # Without this, a consumer executable exports those FreeType symbols from its
    # dynamic symbol table and interposes Qt's system FreeType (Qt's font engine on
    # X11/Wayland), corrupting Qt glyph loading (FT_Load_Glyph err=7) and breaking
    # glyph rasterization. Exclude static-archive symbols from consumers' dynamic
    # exports so Qt keeps using its own FreeType.
    target_link_options(vnm_terminal_surface INTERFACE "-Wl,--exclude-libs,ALL")
endif()

install(TARGETS
    vnm_terminal_surface
    EXPORT vnm_terminal_surfaceTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES
    include/vnm_terminal/font_metrics.h
    include/vnm_terminal/vnm_terminal_surface.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vnm_terminal"
)

install(FILES
    include/vnm_terminal/diagnostics/metrics_json.h
    include/vnm_terminal/diagnostics/profile_text.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vnm_terminal/diagnostics"
)

install(EXPORT vnm_terminal_surfaceTargets
    NAMESPACE vnm_terminal_surface::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_surface"
)

configure_package_config_file(
    cmake/vnm_terminal_surfaceConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_surfaceConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_surface"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_surfaceConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_surfaceConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/vnm_terminal_surfaceConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_surface"
)

add_executable(vnm_terminal_canvas_fixture
    tools/terminal_canvas_fixture/terminal_canvas_fixture.cpp
)

set_target_properties(vnm_terminal_canvas_fixture PROPERTIES
    AUTOMOC OFF
)

target_include_directories(vnm_terminal_canvas_fixture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(VNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY)
    add_executable(vnm_terminal_transcript_replay
        tools/transcript_replay/terminal_transcript_replay.cpp
    )

    target_link_libraries(vnm_terminal_transcript_replay PRIVATE
        vnm_terminal_surface::vnm_terminal_surface
    )

    set_target_properties(vnm_terminal_transcript_replay PROPERTIES
        AUTOMOC OFF
    )

    if(WIN32)
        add_custom_command(TARGET vnm_terminal_transcript_replay POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:vnm_terminal_transcript_replay>
                $<TARGET_FILE_DIR:vnm_terminal_transcript_replay>
            COMMAND_EXPAND_LISTS
        )
    endif()
endif()

if(VNM_TERMINAL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(vnm_terminal_surface_testing_enabled)
    add_subdirectory(tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/more_tests/CMakeLists.txt")
        add_subdirectory(more_tests)
    endif()
endif()
