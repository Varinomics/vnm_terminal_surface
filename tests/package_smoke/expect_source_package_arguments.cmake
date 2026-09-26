cmake_minimum_required(VERSION 3.21)

# This compiler-free transport check exercises the real driver's two configure
# boundaries. Actual library/export validation remains in the package consumers.
file(MAKE_DIRECTORY "${test_root}/source")
file(WRITE "${test_root}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(package_argument_transport LANGUAGES NONE)
if(NOT CMAKE_CONFIGURATION_TYPES STREQUAL "Debug;Release;MinSizeRel;RelWithDebInfo")
    message(FATAL_ERROR "Configuration list was split: ${CMAKE_CONFIGURATION_TYPES}")
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" DESTINATION share/argument-check)
]=])
file(WRITE "${test_root}/context.cmake"
    "include([==[${VNM_TOOLCHAIN_CONTEXT}]==])\n"
    "set(VNM_NESTED_CONFIGURATION_TYPES [==[Debug;Release;MinSizeRel;RelWithDebInfo]==])\n"
    "set(VNM_NESTED_FIELDS GENERATOR GENERATOR_PLATFORM GENERATOR_TOOLSET GENERATOR_INSTANCE MAKE_PROGRAM CONFIGURATION_TYPES)\n")
file(WRITE "${test_root}/dependencies.cmake" "# No library dependencies in this transport check.\n")
set(VNM_TOOLCHAIN_CONTEXT "${test_root}/context.cmake")
set(VNM_PACKAGE_DEPENDENCY_CONTEXT "${test_root}/dependencies.cmake")
set(msdf_source_dir "${test_root}/source")
set(producer_source_dir "${test_root}/source")
set(package_root "${test_root}/package")
set(package_binary_dir "${package_root}/producer")
set(package_context "${package_root}/dependencies.cmake")
set(install_config Release)
include("${driver}")
