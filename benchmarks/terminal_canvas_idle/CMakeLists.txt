if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(vnm_terminal_canvas_idle_probe terminal_canvas_idle_probe.cpp)
    target_link_libraries(vnm_terminal_canvas_idle_probe PRIVATE
        vnm_terminal_surface::vnm_terminal_surface_renderer)
    set_target_properties(vnm_terminal_canvas_idle_probe PROPERTIES AUTOMOC OFF)
endif()
