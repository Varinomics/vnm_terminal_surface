cmake_minimum_required(VERSION 3.21)

foreach(required_variable inventory_file source_root)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_variable}")
    endif()
endforeach()

if(NOT EXISTS "${inventory_file}")
    message(FATAL_ERROR "Renderer inventory does not exist: ${inventory_file}")
endif()

file(READ "${inventory_file}" inventory)
string(TOLOWER "${inventory}" inventory_lower)

foreach(required_source
    qsg_atlas_font_bytes.cpp
    qsg_atlas_renderer.cpp
    qsg_terminal_renderer.cpp
    qt_grid_metrics_provider.cpp
    vnm_terminal_font.cpp
    vnm_terminal_canvas.cpp)
    if(NOT inventory_lower MATCHES "${required_source}")
        message(FATAL_ERROR
            "Renderer target is missing required source ${required_source}:\n${inventory}")
    endif()
endforeach()

foreach(forbidden_dependency
    terminal_byte_stream_parser
    terminal_history
    terminal_input
    terminal_session
    posix_pty
    windows_conpty
    native_backend
    vnm_qt_dispatch)
    if(inventory_lower MATCHES "${forbidden_dependency}")
        message(FATAL_ERROR
            "Renderer target contains forbidden dependency ${forbidden_dependency}:\n${inventory}")
    endif()
endforeach()

foreach(public_header
    include/vnm_terminal/terminal_canvas_frame.h
    include/vnm_terminal/vnm_terminal_canvas.h)
    file(READ "${source_root}/${public_header}" header_text)
    if(header_text MATCHES "vnm_terminal/internal/")
        message(FATAL_ERROR
            "Public renderer header exposes an internal include: ${public_header}")
    endif()
endforeach()
