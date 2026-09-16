include_guard(GLOBAL)

# Keep the host layout used by Microsoft's native NuGet targets: an emulated
# application still needs the OpenConsole executable for the native OS CPU.
function(vnm_terminal_conpty_runtime_files output)
    get_property(package_dir GLOBAL PROPERTY vnm_terminal_conpty_package_dir)
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" architecture)
    if(NOT architecture)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" architecture)
    endif()
    if(architecture MATCHES "^(arm64|aarch64)$")
        set(architecture arm64)
        set(hosts arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(architecture x64)
        set(hosts x64 arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(architecture x86)
        set(hosts x86 x64 arm64)
    else()
        message(FATAL_ERROR "Unsupported ConPTY target architecture: ${architecture}")
    endif()
    set(files "runtimes/win-${architecture}/native/conpty.dll|conpty.dll")
    foreach(host IN LISTS hosts)
        list(APPEND files "build/native/runtimes/${host}/OpenConsole.exe|${host}/OpenConsole.exe")
    endforeach()
    list(APPEND files "LICENSE|licenses/conpty/LICENSE")
    foreach(file IN LISTS files)
        string(REPLACE "|" ";" parts "${file}")
        list(GET parts 0 source)
        if(NOT EXISTS "${package_dir}/${source}")
            message(FATAL_ERROR "ConPTY package is missing ${package_dir}/${source}")
        endif()
    endforeach()
    set(${output} "${files}" PARENT_SCOPE)
endfunction()

# The backend loads conpty.dll from the directory of the running process's main
# module, so the runtime belongs beside whichever executable hosts a terminal.
# Without DESTINATION that is the target's own output directory, staged from a
# POST_BUILD command. DESTINATION covers the two cases a POST_BUILD command
# cannot: an executable that runs from a private runtime directory rather than
# its build output directory, and a target created in another directory, which
# add_custom_command(TARGET) rejects. The destination is spelled out rather than
# derived from $<TARGET_FILE_DIR:${target}> because the staging target has to
# run before ${target} is considered built, and reading the target's own
# location would close that dependency into a cycle.
function(vnm_terminal_deploy_conpty target)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(arg "" "DESTINATION" "" ${ARGN})
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "vnm_terminal_deploy_conpty: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    get_property(package_dir GLOBAL PROPERTY vnm_terminal_conpty_package_dir)
    vnm_terminal_conpty_runtime_files(files)
    set(runtime_dir "${arg_DESTINATION}")
    if(NOT runtime_dir)
        set(runtime_dir "$<TARGET_FILE_DIR:${target}>")
    endif()
    set(commands)
    foreach(file IN LISTS files)
        string(REPLACE "|" ";" parts "${file}")
        list(GET parts 0 source)
        list(GET parts 1 destination)
        get_filename_component(directory "${destination}" DIRECTORY)
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${runtime_dir}/${directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${package_dir}/${source}" "${runtime_dir}/${destination}")
    endforeach()
    if(arg_DESTINATION)
        add_custom_target(${target}_conpty_runtime ${commands}
            COMMENT "Staging the ConPTY runtime for ${target}"
            VERBATIM)
        add_dependencies(${target} ${target}_conpty_runtime)
    else()
        add_custom_command(TARGET ${target} POST_BUILD ${commands} VERBATIM)
    endif()
endfunction()

function(vnm_terminal_install_conpty)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(arg "EXCLUDE_FROM_ALL" "DESTINATION;COMPONENT" "" ${ARGN})
    set(options)
    if(arg_COMPONENT)
        list(APPEND options COMPONENT "${arg_COMPONENT}")
    endif()
    if(arg_EXCLUDE_FROM_ALL)
        list(APPEND options EXCLUDE_FROM_ALL)
    endif()
    get_property(package_dir GLOBAL PROPERTY vnm_terminal_conpty_package_dir)
    vnm_terminal_conpty_runtime_files(files)
    foreach(file IN LISTS files)
        string(REPLACE "|" ";" parts "${file}")
        list(GET parts 0 source)
        list(GET parts 1 destination)
        get_filename_component(directory "${destination}" DIRECTORY)
        install(FILES "${package_dir}/${source}" DESTINATION "${arg_DESTINATION}/${directory}" ${options})
    endforeach()
endfunction()
