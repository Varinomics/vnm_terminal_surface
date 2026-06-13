add_executable(vnm_terminal_surface_stress_benchmark
    terminal_surface_stress_benchmark.cpp
)

set_target_properties(vnm_terminal_surface_stress_benchmark PROPERTIES
    AUTOMOC OFF
)

target_link_libraries(vnm_terminal_surface_stress_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)

if(vnm_terminal_surface_testing_enabled AND TARGET vnm_terminal_surface_stress_benchmark)
    add_test(
        NAME vnm_terminal_surface_stress_descriptor_unavailable_contract
        COMMAND
            "${CMAKE_COMMAND}"
            "-DBENCHMARK_EXE=$<TARGET_FILE:vnm_terminal_surface_stress_benchmark>"
            "-DQT_RUNTIME_DIR=$<TARGET_FILE_DIR:Qt6::Core>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/validate_surface_stress_descriptor_contract.cmake"
    )

    set_tests_properties(vnm_terminal_surface_stress_descriptor_unavailable_contract PROPERTIES
        LABELS "readiness;benchmark;schema"
        TIMEOUT 30
    )
endif()
