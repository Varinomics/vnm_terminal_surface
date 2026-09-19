include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/vnm_process_custody_runtime.cmake")

function(vnm_process_custody_install_package)
    get_property(package_registered GLOBAL PROPERTY VNM_PROCESS_CUSTODY_PACKAGE_REGISTERED)
    if(package_registered)
        return()
    endif()
    cmake_parse_arguments(arg "" "" "PUBLIC_HEADERS" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS OR arg_KEYWORDS_MISSING_VALUES OR NOT arg_PUBLIC_HEADERS)
        message(FATAL_ERROR "vnm_process_custody_install_package requires PUBLIC_HEADERS")
    endif()
    set(package_directory "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_process_custody")
    install(FILES
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../LICENSE"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../NOTICE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/vnm_process_custody"
        COMPONENT vnm_process_custody)
    install(TARGETS vnm_process_custody
        EXPORT vnm_process_custody_targets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT vnm_process_custody
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT vnm_process_custody
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT vnm_process_custody)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        install(TARGETS vnm_process_custody_owner
            EXPORT vnm_process_custody_targets
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            COMPONENT vnm_process_custody)
    endif()
    install(FILES ${arg_PUBLIC_HEADERS}
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vnm_process_custody"
        COMPONENT vnm_process_custody)
    configure_package_config_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/vnm_process_custody-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_process_custody-config.cmake"
        INSTALL_DESTINATION "${package_directory}"
        PATH_VARS CMAKE_INSTALL_DATADIR)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/vnm_process_custody-config.cmake"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/vnm_process_custody_runtime.cmake"
        DESTINATION "${package_directory}"
        COMPONENT vnm_process_custody)
    install(EXPORT vnm_process_custody_targets
        NAMESPACE vnm_process_custody::
        DESTINATION "${package_directory}"
        COMPONENT vnm_process_custody)
    set_property(GLOBAL PROPERTY VNM_PROCESS_CUSTODY_PACKAGE_REGISTERED TRUE)
endfunction()

function(vnm_process_custody_register_source_package)
    set_property(GLOBAL PROPERTY VNM_PROCESS_CUSTODY_LICENSE_FILES
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../LICENSE"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../NOTICE")
    get_target_property(provider_imported vnm_process_custody::vnm_process_custody IMPORTED)
    if(provider_imported)
        return()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       NOT TARGET vnm_process_custody::vnm_process_custody_owner)
        add_executable(vnm_process_custody::vnm_process_custody_owner ALIAS vnm_process_custody_owner)
    endif()
    get_target_property(provider_source vnm_process_custody::vnm_process_custody SOURCE_DIR)
    set(public_headers)
    foreach(header IN ITEMS owner process_channel process_clock process_pipe process_spawn process_tree)
        list(APPEND public_headers "${provider_source}/include/vnm_process_custody/${header}.h")
    endforeach()
    vnm_process_custody_install_package(PUBLIC_HEADERS ${public_headers})
endfunction()
