add_executable(vnm_terminal_phase7_public_scroll_benchmark
    phase7_public_scroll_benchmark.cpp
)

target_link_libraries(vnm_terminal_phase7_public_scroll_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::Core
)

if(WIN32)
    target_link_libraries(vnm_terminal_phase7_public_scroll_benchmark PRIVATE
        psapi
    )
endif()

set_target_properties(vnm_terminal_phase7_public_scroll_benchmark PROPERTIES
    AUTOMOC OFF
)

if(MSVC)
    target_compile_options(vnm_terminal_phase7_public_scroll_benchmark PRIVATE
        /FS
    )
endif()

if(WIN32)
    set(vnm_terminal_phase7_benchmark_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_phase7_benchmark_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_phase7_benchmark_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

if(BUILD_TESTING AND TARGET vnm_terminal_phase7_public_scroll_benchmark)
    add_test(
        NAME vnm_terminal_phase7_public_scroll_benchmark_validate
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "${vnm_terminal_phase7_benchmark_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_phase7_public_scroll_benchmark>"
            "--iterations" "3"
            "--warmup" "1"
            "--scrollback-limit" "10000"
            "--rows" "24"
            "--columns" "80"
            "--wheel-events" "240"
            "--validate-json"
            "--quiet"
    )

    set_tests_properties(vnm_terminal_phase7_public_scroll_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark;phase7"
        TIMEOUT 120
    )
endif()
