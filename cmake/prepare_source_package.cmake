cmake_minimum_required(VERSION 3.21)
include("${VNM_TOOLCHAIN_CONTEXT}")
include("${VNM_PACKAGE_DEPENDENCY_CONTEXT}")

set(configure_args -C "${VNM_PACKAGE_DEPENDENCY_CONTEXT}")
vnm_append_toolchain_args(configure_args)
if(NOT VNM_NESTED_CONFIGURATION_TYPES)
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${install_config}")
endif()

function(run_package_step description command_variable)
    # Keep escaped list elements intact until their final command boundary.
    execute_process(COMMAND "${CMAKE_COMMAND}" ${${command_variable}}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed.\n${output}${error}")
    endif()
    message(STATUS "${description} completed")
endfunction()

# The provider owns its package. Build only the LCD component needed by an
# atlas-disabled surface, without FreeType/msdfgen or borrowed source exports.
set(lcd_prefix "${package_root}/lcd-install")
set(command "${configure_args}")
list(APPEND command
    -S "${msdf_source_dir}" -B "${package_root}/lcd-build"
    -DVNM_MSDF_TEXT_BUILD_ATLAS=OFF
    -DVNM_MSDF_TEXT_BUILD_RHI=OFF
    -DVNM_MSDF_TEXT_BUILD_QT_LCD=ON
    -DVNM_MSDF_TEXT_BUILD_TESTS=OFF
    -DVNM_MSDF_TEXT_FETCH_DEPS=OFF
    -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_INSTALL_PREFIX=${lcd_prefix}")
run_package_step("LCD provider configure" command)
set(command --build "${package_root}/lcd-build" --config "${install_config}" --parallel 1)
run_package_step("LCD provider build" command)
set(command --install "${package_root}/lcd-build" --config "${install_config}")
run_package_step("LCD provider install" command)

file(READ "${VNM_PACKAGE_DEPENDENCY_CONTEXT}" dependency_context)
file(WRITE "${package_context}" "${dependency_context}\n"
    "set(vnm_msdf_text_DIR [==[${lcd_prefix}/lib/cmake/vnm_msdf_text]==] CACHE PATH \"\" FORCE)\n")

# Source suites intentionally build the atlas from source. Their package gate
# uses a separate surface or terminal producer with an installed LCD dependency.
set(command "${configure_args}")
list(APPEND command -C "${package_context}"
    -S "${producer_source_dir}" -B "${package_binary_dir}"
    -DBUILD_TESTING=OFF -DVNM_TERMINAL_SURFACE_BUILD_TESTING=OFF
    -DVNM_TERMINAL_SURFACE_BUILD_FULL=ON
    -DVNM_TERMINAL_ENABLE_MSDF_TEXT_RENDERER=OFF
    -DVNM_TERMINAL_MSDF_TEXT_RENDERER_USE_SYSTEM_LIBS=ON)
run_package_step("Package producer configure" command)
set(command --build "${package_binary_dir}" --config "${install_config}" --parallel 1)
run_package_step("Package producer build" command)
