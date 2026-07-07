if(NOT vnm_terminal_surface_testing_enabled)
    return()
endif()

if(NOT WIN32)
    return()
endif()

if(NOT TARGET vnm_terminal_input_echo_catchup_benchmark OR
   NOT TARGET vnm_terminal_surface_stress_benchmark)
    return()
endif()

find_program(vnm_terminal_powershell powershell.exe)
if(NOT vnm_terminal_powershell)
    return()
endif()

if(WIN32)
    set(vnm_terminal_benchmark_evidence_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
endif()

# Structural smoke for the evidence runner. This validates runner schema,
# matrix coverage, and benchmark contract plumbing; it encodes no performance
# thresholds and is not decision-grade timing evidence.
add_test(
    NAME vnm_terminal_benchmark_evidence_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        "QT_SCALE_FACTOR=1"
        "QT_SCREEN_SCALE_FACTORS="
        "QT_AUTO_SCREEN_SCALE_FACTOR=0"
        "QT_DEVICE_PIXEL_RATIO="
        "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
        "${vnm_terminal_benchmark_evidence_qt_runtime_path}"
        "${vnm_terminal_powershell}"
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/run_benchmark_evidence.ps1"
        -Mode smoke
        -InputEchoExe "$<TARGET_FILE:vnm_terminal_input_echo_catchup_benchmark>"
        -SurfaceStressExe "$<TARGET_FILE:vnm_terminal_surface_stress_benchmark>"
        -ArtifactDir "${CMAKE_CURRENT_BINARY_DIR}/smoke_$<CONFIG>"
        -OutputJson "${CMAKE_CURRENT_BINARY_DIR}/smoke_$<CONFIG>/benchmark_evidence_smoke.json"
        -BuildDir "${CMAKE_BINARY_DIR}"
        -SurfaceRepo "${PROJECT_SOURCE_DIR}"
)

set_tests_properties(vnm_terminal_benchmark_evidence_smoke PROPERTIES
    LABELS "readiness;benchmark;smoke;structural"
    TIMEOUT 120
)

add_custom_target(vnm_terminal_benchmark_evidence_smoke
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "QT_QPA_PLATFORM=offscreen"
        "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
        "QT_SCALE_FACTOR=1"
        "QT_SCREEN_SCALE_FACTORS="
        "QT_AUTO_SCREEN_SCALE_FACTOR=0"
        "QT_DEVICE_PIXEL_RATIO="
        "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
        "${vnm_terminal_benchmark_evidence_qt_runtime_path}"
        "${vnm_terminal_powershell}"
        -NoProfile
        -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/run_benchmark_evidence.ps1"
        -Mode smoke
        -InputEchoExe "$<TARGET_FILE:vnm_terminal_input_echo_catchup_benchmark>"
        -SurfaceStressExe "$<TARGET_FILE:vnm_terminal_surface_stress_benchmark>"
        -ArtifactDir "${CMAKE_CURRENT_BINARY_DIR}/smoke_$<CONFIG>"
        -OutputJson "${CMAKE_CURRENT_BINARY_DIR}/smoke_$<CONFIG>/benchmark_evidence_smoke.json"
        -BuildDir "${CMAKE_BINARY_DIR}"
        -SurfaceRepo "${PROJECT_SOURCE_DIR}"
    DEPENDS
        vnm_terminal_input_echo_catchup_benchmark
        vnm_terminal_surface_stress_benchmark
    VERBATIM
)
