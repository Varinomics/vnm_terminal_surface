add_executable(vnm_terminal_surface_stress_benchmark
    terminal_surface_stress_benchmark.cpp
)

set_target_properties(vnm_terminal_surface_stress_benchmark PROPERTIES
    AUTOMOC OFF
)

target_link_libraries(vnm_terminal_surface_stress_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)
