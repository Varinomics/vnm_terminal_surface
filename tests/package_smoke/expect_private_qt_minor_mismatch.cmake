foreach(required_var IN ITEMS
    package_binary_dir
    install_dir
    mismatch_install_dir
    consumer_source_dir
    consumer_binary_dir
    qt_private_api_version_major
    qt_private_api_version_minor)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing required package smoke variable: ${required_var}")
    endif()
endforeach()

set(build_args
    --build "${package_binary_dir}"
    --target vnm_terminal_surface)

set(install_args
    --install "${package_binary_dir}"
    --prefix "${install_dir}")

if(DEFINED install_config AND NOT "${install_config}" STREQUAL "")
    list(APPEND build_args
        --config "${install_config}")
    list(APPEND install_args
        --config "${install_config}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${build_args}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)

if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to build vnm_terminal_surface before package smoke install.\n"
        "${build_stdout}${build_stderr}")
endif()

file(REMOVE_RECURSE
    "${install_dir}"
    "${mismatch_install_dir}"
    "${consumer_binary_dir}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${install_args}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install vnm_terminal_surface for package smoke.\n"
        "${install_stdout}${install_stderr}")
endif()

file(MAKE_DIRECTORY "${mismatch_install_dir}")
file(COPY "${install_dir}/"
    DESTINATION "${mismatch_install_dir}")

if(qt_private_api_version_minor GREATER 0)
    math(EXPR mismatch_minor "${qt_private_api_version_minor} - 1")
else()
    math(EXPR mismatch_minor "${qt_private_api_version_minor} + 1")
endif()

file(GLOB_RECURSE package_config_paths
    "${mismatch_install_dir}/vnm_terminal_surfaceConfig.cmake")

list(LENGTH package_config_paths package_config_count)
if(NOT package_config_count EQUAL 1)
    message(FATAL_ERROR
        "Expected one installed vnm_terminal_surfaceConfig.cmake, found ${package_config_count}")
endif()

list(GET package_config_paths 0 package_config_path)
file(READ "${package_config_path}" package_config_text)
string(REGEX REPLACE
    "set\\(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR[ \t\r\n]+\"[0-9]+\"\\)"
    "set(VNM_TERMINAL_SURFACE_QT_PRIVATE_API_VERSION_MINOR\n    \"${mismatch_minor}\")"
    mismatched_package_config_text
    "${package_config_text}")

if(mismatched_package_config_text STREQUAL package_config_text)
    message(FATAL_ERROR
        "Failed to synthesize Qt private API minor mismatch in ${package_config_path}")
endif()

file(WRITE "${package_config_path}" "${mismatched_package_config_text}")

set(configure_args)
if(DEFINED generator AND NOT "${generator}" STREQUAL "")
    list(APPEND configure_args -G "${generator}")
endif()

if(DEFINED generator_platform AND NOT "${generator_platform}" STREQUAL "")
    list(APPEND configure_args -A "${generator_platform}")
endif()

if(DEFINED generator_toolset AND NOT "${generator_toolset}" STREQUAL "")
    list(APPEND configure_args -T "${generator_toolset}")
endif()

if(DEFINED make_program AND NOT "${make_program}" STREQUAL "")
    list(APPEND configure_args "-DCMAKE_MAKE_PROGRAM=${make_program}")
endif()

if(DEFINED qt6_dir AND NOT "${qt6_dir}" STREQUAL "")
    list(APPEND configure_args "-DQt6_DIR=${qt6_dir}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        ${configure_args}
        -S "${consumer_source_dir}"
        -B "${consumer_binary_dir}"
        "-DCMAKE_PREFIX_PATH=${mismatch_install_dir}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)

set(configure_output "${configure_stdout}${configure_stderr}")

if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "Expected package smoke configure to fail with Qt private API minor mismatch, "
        "but it succeeded.\n${configure_output}")
endif()

string(CONCAT expected_regex
    "vnm_terminal_surface was built against Qt "
    "${qt_private_api_version_major}\\.${mismatch_minor}\\.x private APIs")

if(NOT configure_output MATCHES "${expected_regex}")
    message(FATAL_ERROR
        "Expected package smoke configure failure output to match '${expected_regex}'.\n"
        "Actual output:\n${configure_output}")
endif()

message(STATUS "Package smoke rejected synthetic Qt private API minor mismatch.")
