include("${CMAKE_CURRENT_LIST_DIR}/vnm_terminal_conpty.cmake")

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(VNM_TERMINAL_CONPTY_PACKAGE_DIR "" CACHE PATH
    "Extracted Microsoft.Windows.Console.ConPTY NuGet package; compatible newer packages are supported")
set(VNM_TERMINAL_CONPTY_VERSION "1.24.260710001" CACHE STRING
    "Microsoft.Windows.Console.ConPTY NuGet version to download")
set(VNM_TERMINAL_CONPTY_SHA256 "175640566a3b59c4b132070ee96c2c77e5ab7edd2e92732a5eb3610bbf63d90e"
    CACHE STRING "SHA256 of the selected ConPTY NuGet archive")

if(VNM_TERMINAL_CONPTY_PACKAGE_DIR)
    set(conpty_source_dir "${VNM_TERMINAL_CONPTY_PACKAGE_DIR}")
else()
    FetchContent_Declare(vnm_terminal_conpty
        URL "https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/${VNM_TERMINAL_CONPTY_VERSION}/microsoft.windows.console.conpty.${VNM_TERMINAL_CONPTY_VERSION}.nupkg"
        URL_HASH "SHA256=${VNM_TERMINAL_CONPTY_SHA256}")
    FetchContent_MakeAvailable(vnm_terminal_conpty)
    set(conpty_source_dir "${vnm_terminal_conpty_SOURCE_DIR}")
endif()

# Stage the redistribution license with the package without modifying a supplied
# package directory. Installed consumers use the same self-contained layout.
set(conpty_package_dir "${CMAKE_CURRENT_BINARY_DIR}/conpty")
file(MAKE_DIRECTORY "${conpty_package_dir}")
file(COPY "${conpty_source_dir}/runtimes" "${conpty_source_dir}/build"
    DESTINATION "${conpty_package_dir}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY/conpty/LICENSE"
    "${conpty_package_dir}/LICENSE" COPYONLY)
set_property(GLOBAL PROPERTY vnm_terminal_conpty_package_dir "${conpty_package_dir}")
vnm_terminal_conpty_runtime_files(conpty_files)
install(DIRECTORY "${conpty_package_dir}/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vnm_terminal_surface/conpty")
install(FILES "${CMAKE_CURRENT_LIST_DIR}/vnm_terminal_conpty.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vnm_terminal_surface")
