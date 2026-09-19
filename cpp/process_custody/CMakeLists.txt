cmake_minimum_required(VERSION 3.21)
project(vnm_process_custody LANGUAGES CXX)
include(GNUInstallDirs)

set(_native_sources)
if(WIN32)
    list(APPEND _native_sources src/process_pipe_win.cpp src/process_channel_win.cpp
        src/process_spawn_win.cpp src/process_tree_win.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _native_sources src/process_pipe_posix.cpp src/process_channel_posix.cpp
        src/process_spawn_posix.cpp src/process_tree_posix.cpp)
else()
    message(FATAL_ERROR "vnm_process_custody currently supports Windows and Linux")
endif()

add_library(vnm_process_custody STATIC ${_native_sources})
add_library(vnm_process_custody::vnm_process_custody ALIAS vnm_process_custody)
target_compile_features(vnm_process_custody PUBLIC cxx_std_20)
target_include_directories(vnm_process_custody PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(vnm_process_custody PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(vnm_process_custody PRIVATE NOMINMAX)
    option(VNM_PROCESS_CUSTODY_ENABLE_TEST_HOOKS "Enable the maintained Windows native observation fixtures" OFF)
    if(VNM_PROCESS_CUSTODY_ENABLE_TEST_HOOKS)
        target_compile_definitions(vnm_process_custody PRIVATE
            LOGONOMIC_ENABLE_PROCESS_TREE_TEST_HOOKS LOGONOMIC_ENABLE_PROCESS_SPAWN_TEST_HOOKS)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(vnm_process_custody PRIVATE src/owner_protocol.cpp)
    add_executable(vnm_process_custody_owner src/owner_main_posix.cpp)
    target_link_libraries(vnm_process_custody_owner PRIVATE vnm_process_custody)
    set_target_properties(vnm_process_custody_owner PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    option(VNM_PROCESS_CUSTODY_BUILD_TESTS "Build the standalone native owner tests" OFF)
    if(VNM_PROCESS_CUSTODY_BUILD_TESTS)
        enable_testing()
        add_executable(vnm_process_custody_tests tests/owner_tests.cpp)
        target_link_libraries(vnm_process_custody_tests PRIVATE vnm_process_custody)
        set_target_properties(vnm_process_custody_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        add_test(NAME vnm_process_custody_tests
            COMMAND vnm_process_custody_tests $<TARGET_FILE:vnm_process_custody_owner>)
        set_tests_properties(vnm_process_custody_tests PROPERTIES TIMEOUT 60)
    endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/vnm_process_custody_install.cmake")
vnm_process_custody_register_source_package()
