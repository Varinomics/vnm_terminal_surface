cmake_minimum_required(VERSION 3.21)
project(conpty_module_install LANGUAGES NONE)

set(CMAKE_INSTALL_LIBDIR lib)
set(CMAKE_INSTALL_DATADIR share)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
include("${HELPER_DIRECTORY}/vnm_terminal_conpty_dependency.cmake")
