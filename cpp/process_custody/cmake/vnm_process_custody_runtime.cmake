include_guard(GLOBAL)

# Runtime lookup is relative to the hosting executable. Target locations are
# used only to assemble that runtime, including when the provider is imported.
function(vnm_process_custody_deploy_owner target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    cmake_parse_arguments(arg "" "DESTINATION" "" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS OR arg_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "vnm_process_custody_deploy_owner: invalid arguments")
    endif()
    if(NOT TARGET vnm_process_custody::vnm_process_custody_owner)
        message(FATAL_ERROR "The process custody provider has no Linux owner executable")
    endif()
    set(destination "${arg_DESTINATION}")
    if(NOT destination)
        set(destination "$<TARGET_FILE_DIR:${target}>")
    endif()
    # Directory-only target expressions must not introduce a reverse dependency
    # on the host. An always-run stage also refreshes an imported helper without
    # requiring the host to relink, and can be called across directory scopes.
    cmake_policy(PUSH)
    cmake_policy(SET CMP0112 NEW)
    add_custom_target(${target}_process_owner_runtime
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:vnm_process_custody::vnm_process_custody_owner>"
            "${destination}/vnm_process_custody_owner"
        DEPENDS vnm_process_custody::vnm_process_custody_owner
        VERBATIM)
    cmake_policy(POP)
    add_dependencies(${target} ${target}_process_owner_runtime)
endfunction()

function(vnm_process_custody_install_owner)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    cmake_parse_arguments(arg "" "DESTINATION;COMPONENT" "" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS OR arg_KEYWORDS_MISSING_VALUES OR NOT arg_DESTINATION)
        message(FATAL_ERROR "vnm_process_custody_install_owner requires DESTINATION")
    endif()
    set(options)
    if(arg_COMPONENT)
        list(APPEND options COMPONENT "${arg_COMPONENT}")
    endif()
    install(PROGRAMS "$<TARGET_FILE:vnm_process_custody::vnm_process_custody_owner>"
        DESTINATION "${arg_DESTINATION}" ${options})
    get_property(license_files GLOBAL PROPERTY VNM_PROCESS_CUSTODY_LICENSE_FILES)
    if(NOT license_files)
        message(FATAL_ERROR "The process custody provider has no distribution license notices")
    endif()
    include(GNUInstallDirs)
    install(FILES ${license_files}
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/vnm_process_custody" ${options})
endfunction()
