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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND _native_sources src/process_pipe_posix.cpp src/process_channel_darwin.cpp
        src/process_spawn_darwin.cpp src/process_tree_darwin.cpp)
else()
    message(FATAL_ERROR "vnm_process_custody supports Windows, Linux and Darwin")
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

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    set_property(TARGET vnm_process_custody PROPERTY
        VNM_PROCESS_CUSTODY_OWNER_START_DEADLINE 1)
    set_property(TARGET vnm_process_custody APPEND PROPERTY EXPORT_PROPERTIES
        VNM_PROCESS_CUSTODY_OWNER_START_DEADLINE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set_property(TARGET vnm_process_custody PROPERTY
            VNM_PROCESS_CUSTODY_COMMAND_EXEC_CONFIRMATION 1)
        set_property(TARGET vnm_process_custody APPEND PROPERTY EXPORT_PROPERTIES
            VNM_PROCESS_CUSTODY_COMMAND_EXEC_CONFIRMATION)
    endif()
    target_sources(vnm_process_custody PRIVATE src/owner_protocol.cpp)
    add_executable(vnm_process_custody_owner src/owner_main_posix.cpp)
    target_link_libraries(vnm_process_custody_owner PRIVATE vnm_process_custody)
    set_target_properties(vnm_process_custody_owner PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    set(vnm_process_custody_build_tests_default OFF)
    if(BUILD_TESTING AND VNM_TERMINAL_SURFACE_BUILD_TESTING)
        set(vnm_process_custody_build_tests_default ON)
    endif()
    option(VNM_PROCESS_CUSTODY_BUILD_TESTS
        "Build the standalone native owner tests"
        ${vnm_process_custody_build_tests_default})
    if(VNM_PROCESS_CUSTODY_BUILD_TESTS)
        enable_testing()
        find_package(Threads REQUIRED)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(owner_test_source tests/owner_darwin_tests.cpp)
        else()
            set(owner_test_source tests/owner_tests.cpp)
        endif()
        add_executable(vnm_process_custody_tests ${owner_test_source})
        target_link_libraries(vnm_process_custody_tests PRIVATE vnm_process_custody Threads::Threads)
        set_target_properties(vnm_process_custody_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        add_test(NAME vnm_process_custody_tests
            COMMAND vnm_process_custody_tests $<TARGET_FILE:vnm_process_custody_owner>)
        set_tests_properties(vnm_process_custody_tests PROPERTIES TIMEOUT 60)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            add_test(NAME vnm_process_custody_truncated_rights_tests
                COMMAND vnm_process_custody_tests --truncated-rights)
            set_tests_properties(vnm_process_custody_truncated_rights_tests PROPERTIES TIMEOUT 10)
            add_test(NAME vnm_process_custody_command_exec_tests
                COMMAND vnm_process_custody_tests --command-exec $<TARGET_FILE:vnm_process_custody_owner>)
            set_tests_properties(vnm_process_custody_command_exec_tests PROPERTIES TIMEOUT 30)
            add_executable(vnm_process_custody_darwin_pty_tests tests/owner_darwin_pty_tests.cpp)
            target_link_libraries(vnm_process_custody_darwin_pty_tests PRIVATE vnm_process_custody)
            set_target_properties(vnm_process_custody_darwin_pty_tests PROPERTIES
                AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
            add_dependencies(vnm_process_custody_darwin_pty_tests vnm_process_custody_owner)
            add_test(NAME vnm_process_custody_darwin_pty_tests
                COMMAND vnm_process_custody_darwin_pty_tests $<TARGET_FILE:vnm_process_custody_owner>)
            set_tests_properties(vnm_process_custody_darwin_pty_tests PROPERTIES TIMEOUT 40)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME vnm_process_custody_started_descriptor_tests
                COMMAND vnm_process_custody_tests --started-descriptor)
            set_tests_properties(vnm_process_custody_started_descriptor_tests PROPERTIES TIMEOUT 10)
            add_test(NAME vnm_process_custody_receive_error_tests
                COMMAND vnm_process_custody_tests --receive-error)
            add_test(NAME vnm_process_custody_late_stop_tests
                COMMAND vnm_process_custody_tests --late-stop $<TARGET_FILE:vnm_process_custody_owner>)
            set_tests_properties(vnm_process_custody_receive_error_tests
                vnm_process_custody_late_stop_tests PROPERTIES TIMEOUT 20)
        endif()
        add_executable(vnm_process_custody_start_deadline_tests
            tests/owner_start_deadline_tests.cpp)
        target_link_libraries(vnm_process_custody_start_deadline_tests PRIVATE
            vnm_process_custody Threads::Threads)
        set_target_properties(vnm_process_custody_start_deadline_tests PROPERTIES
            AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        add_test(NAME vnm_process_custody_start_deadline_tests
            COMMAND vnm_process_custody_start_deadline_tests)
        set_tests_properties(vnm_process_custody_start_deadline_tests PROPERTIES TIMEOUT 20)
    endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/vnm_process_custody_install.cmake")
vnm_process_custody_register_source_package()
