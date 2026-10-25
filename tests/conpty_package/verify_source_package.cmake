cmake_minimum_required(VERSION 3.21)

if(DEFINED child_source)
    include("${helper}")
    vnm_terminal_stage_conpty_package("${child_source}" "${stage}")
    return()
endif()

set(source "${test_root}/source")
set(stage "${test_root}/stage")
file(MAKE_DIRECTORY "${source}/runtimes/win-x64/native"
    "${source}/build/native/runtimes/x64" "${source}/build/native/runtimes/arm64")
set(paths conpty.dll x64/OpenConsole.exe arm64/OpenConsole.exe)
set(package_paths runtimes/win-x64/native/conpty.dll
    build/native/runtimes/x64/OpenConsole.exe build/native/runtimes/arm64/OpenConsole.exe)
set(files "[]")
set(index 0)
foreach(path package_path IN ZIP_LISTS paths package_paths)
    file(WRITE "${source}/${package_path}" "fixture-${path}")
    file(SHA256 "${source}/${package_path}" hash)
    file(SIZE "${source}/${package_path}" size)
    string(JSON files SET "${files}" ${index}
        "{\"path\":\"${path}\",\"size\":${size},\"sha256\":\"${hash}\"}")
    math(EXPR index "${index} + 1")
endforeach()
file(WRITE "${source}/LICENSE" "complete source notices")
file(SHA256 "${source}/LICENSE" license_hash)
set(receipt "{\"schema_version\":1,\"capability_id\":\"conpty-source-build\",
    \"upstream_commit\":\"5a830b2bf7c053d5c7ac22208fe5a346cb5dd3dc\",
    \"package_version\":\"1.24.260710001\",\"configuration\":\"Release\",
    \"release_policy\":{\"debug_symbols\":false,\"hybrid_crt\":true},
    \"license_sha256\":\"${license_hash}\",\"files\":${files}}")
file(WRITE "${source}/conpty-source-build.json" "${receipt}")

function(check_stage expected_result expected_message)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-Dchild_source=${source}"
        "-Dstage=${stage}" "-Dhelper=${helper}" -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_result STREQUAL "pass")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "ConPTY source package rejected: ${output}${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_message}")
        message(FATAL_ERROR "Expected ${expected_message}; received ${result}: ${output}${error}")
    endif()
endfunction()

check_stage(pass "")
file(READ "${stage}/LICENSE" staged_license)
if(NOT staged_license STREQUAL "complete source notices" OR
    NOT EXISTS "${stage}/conpty-source-build.json")
    message(FATAL_ERROR "Source package lost its complete notices or receipt")
endif()

file(APPEND "${source}/build/native/runtimes/arm64/OpenConsole.exe" "changed")
check_stage(fail "receipt does not match")
file(WRITE "${source}/build/native/runtimes/arm64/OpenConsole.exe" "fixture-arm64/OpenConsole.exe")
file(WRITE "${source}/LICENSE" "incomplete notices")
check_stage(fail "notices do not match")
file(WRITE "${source}/LICENSE" "complete source notices")
string(JSON wrong_configuration SET "${receipt}" configuration "\"RelWithDebInfo\"")
file(WRITE "${source}/conpty-source-build.json" "${wrong_configuration}")
check_stage(fail "Unsupported ConPTY source-build receipt")

set(marked_receipt "${receipt}")
foreach(index RANGE 1 2)
    list(GET package_paths ${index} package_path)
    file(APPEND "${source}/${package_path}" "VNM_CONPTY_SHARED_SHADERS_V1")
    file(SHA256 "${source}/${package_path}" hash)
    file(SIZE "${source}/${package_path}" size)
    string(JSON marked_receipt SET "${marked_receipt}" files ${index} sha256 "\"${hash}\"")
    string(JSON marked_receipt SET "${marked_receipt}" files ${index} size "${size}")
endforeach()
file(WRITE "${source}/conpty-source-build.json" "${marked_receipt}")
check_stage(fail "hosts require their resource capability")
file(REMOVE "${source}/conpty-source-build.json")
check_stage(fail "hosts require their resource capability")

file(MAKE_DIRECTORY "${source}/build/native/runtimes/conpty-shaders")
set(resources "[]")
set(index 0)
foreach(name IN ITEMS shader_vs shader_ps custom_shader_vs custom_shader_ps)
    set(resource "${source}/build/native/runtimes/conpty-shaders/${name}.dxbc")
    file(WRITE "${resource}" "fixture-${name}")
    file(SHA256 "${resource}" hash)
    file(SIZE "${resource}" size)
    string(JSON resources SET "${resources}" ${index}
        "{\"path\":\"conpty-shaders/${name}.dxbc\",\"size\":${size},\"sha256\":\"${hash}\"}")
    math(EXPR index "${index} + 1")
endforeach()
string(JSON marked_receipt SET "${marked_receipt}" shared_shader_resources
    "{\"schema_version\":1,\"files\":${resources}}")
file(WRITE "${source}/conpty-source-build.json" "${marked_receipt}")
check_stage(pass "")
file(APPEND "${resource}" "changed")
check_stage(fail "shared shader does not match")
file(REMOVE "${resource}")
check_stage(fail "shared shader is missing")
foreach(index RANGE 1 2)
    list(GET paths ${index} path)
    list(GET package_paths ${index} package_path)
    file(WRITE "${source}/${package_path}" "fixture-${path}")
endforeach()

# Reconfiguring the same staging destination with a vendor package must remove
# the source-build admission and restore the established vendor notices.
file(REMOVE "${source}/conpty-source-build.json" "${source}/LICENSE")
check_stage(pass "")
file(READ "${stage}/LICENSE" vendor_license)
if(EXISTS "${stage}/conpty-source-build.json" OR NOT vendor_license MATCHES "Microsoft Corporation")
    message(FATAL_ERROR "Vendor intake retained source admission or lost vendor notices")
endif()

# The installed staging helper needs its own fallback notices when consuming a
# vendor archive that has no root LICENSE file.
get_filename_component(helper_directory "${helper}" DIRECTORY)
set(install_build "${test_root}/install-build")
set(install_prefix "${test_root}/install")
execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${CMAKE_CURRENT_LIST_DIR}/install" -B "${install_build}"
    "-DHELPER_DIRECTORY=${helper_directory}"
    "-DVNM_TERMINAL_CONPTY_PACKAGE_DIR=${source}"
    RESULT_VARIABLE configure_result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "ConPTY module installation configure failed: ${output}${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${install_build}" --prefix "${install_prefix}"
    RESULT_VARIABLE install_result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "ConPTY module installation failed: ${output}${error}")
endif()
set(helper "${install_prefix}/lib/cmake/vnm_terminal_surface/vnm_terminal_conpty_source.cmake")
set(stage "${test_root}/installed-helper-stage")
check_stage(pass "")
file(READ "${stage}/LICENSE" installed_vendor_license)
if(NOT installed_vendor_license STREQUAL vendor_license)
    message(FATAL_ERROR "Installed ConPTY staging did not preserve the exact vendor notices")
endif()
message(STATUS "ConPTY source receipt, required shared shaders, byte integrity and vendor transition passed")
