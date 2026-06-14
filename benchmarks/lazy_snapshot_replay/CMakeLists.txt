add_executable(vnm_terminal_lazy_snapshot_replay_benchmark
    lazy_snapshot_replay_benchmark.cpp
)

target_link_libraries(vnm_terminal_lazy_snapshot_replay_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

set_target_properties(vnm_terminal_lazy_snapshot_replay_benchmark PROPERTIES
    AUTOMOC OFF
)

if(MSVC)
    target_compile_options(vnm_terminal_lazy_snapshot_replay_benchmark PRIVATE
        /FS
    )
endif()

if(WIN32)
    set(vnm_terminal_lazy_snapshot_replay_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_lazy_snapshot_replay_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_lazy_snapshot_replay_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

if(vnm_terminal_surface_testing_enabled)
    add_test(
        NAME vnm_terminal_lazy_snapshot_replay_benchmark_validate
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_lazy_snapshot_replay_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_lazy_snapshot_replay_benchmark>"
            "--output" "${CMAKE_CURRENT_BINARY_DIR}/lazy_snapshot_replay_validate_output.json"
            "--quiet"
            "--validate-json"
    )

    set_tests_properties(vnm_terminal_lazy_snapshot_replay_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark;lazy-snapshot;replay"
        TIMEOUT 60
    )
endif()
