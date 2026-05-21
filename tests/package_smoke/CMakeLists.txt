cmake_minimum_required(VERSION 3.21)
project(vnm_terminal_surface_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(vnm_terminal_surface CONFIG REQUIRED)

add_executable(vnm_terminal_surface_package_smoke
    main.cpp
)

target_link_libraries(vnm_terminal_surface_package_smoke PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
)
