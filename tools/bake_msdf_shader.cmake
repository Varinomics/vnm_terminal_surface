cmake_minimum_required(VERSION 3.21)

if(NOT EXISTS "${QSB}" OR NOT EXISTS "${MSDF_SHADER_DIR}/lcd_filter.glsl" OR NOT STAGING_DIR)
    message(FATAL_ERROR "Provide QSB, MSDF_SHADER_DIR from vnm_msdf_text, and STAGING_DIR")
endif()
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(MAKE_DIRECTORY "${STAGING_DIR}")
configure_file("${_root}/resources/shaders/atlas_msdf_text.frag"
    "${STAGING_DIR}/atlas_msdf_text.frag" COPYONLY)
configure_file("${MSDF_SHADER_DIR}/lcd_filter.glsl"
    "${STAGING_DIR}/lcd_filter.glsl" COPYONLY)
execute_process(COMMAND "${QSB}"
    --glsl "100 es,120,150" --hlsl 50 --msl 12
    -o "${_root}/resources/shaders/atlas_msdf_text.frag.qsb"
    "${STAGING_DIR}/atlas_msdf_text.frag"
    COMMAND_ERROR_IS_FATAL ANY)
