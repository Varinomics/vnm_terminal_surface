add_executable(vnm_terminal_input_echo_catchup_benchmark
    input_echo_catchup_benchmark.cpp
)

add_executable(vnm_terminal_input_echo_ordering_benchmark
    input_echo_ordering_benchmark.cpp
)

find_package(Qt6Gui 6.7 REQUIRED COMPONENTS QOffscreenIntegrationPlugin)

target_link_libraries(vnm_terminal_input_echo_catchup_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

target_link_libraries(vnm_terminal_input_echo_ordering_benchmark PRIVATE
    vnm_terminal_surface::vnm_terminal_surface
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

set_target_properties(vnm_terminal_input_echo_catchup_benchmark PROPERTIES
    AUTOMOC OFF
)

set_target_properties(vnm_terminal_input_echo_ordering_benchmark PROPERTIES
    AUTOMOC OFF
)

if(MSVC)
    target_compile_options(vnm_terminal_input_echo_catchup_benchmark PRIVATE
        /FS
    )
    target_compile_options(vnm_terminal_input_echo_ordering_benchmark PRIVATE
        /FS
    )
endif()

if(WIN32)
    set(vnm_terminal_input_echo_qt_runtime_path
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>$<SEMICOLON>$ENV{PATH}")
elseif(APPLE)
    set(vnm_terminal_input_echo_qt_runtime_path
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(vnm_terminal_input_echo_qt_runtime_path
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Qt6::Core>:$ENV{LD_LIBRARY_PATH}")
endif()

if(vnm_terminal_surface_testing_enabled)
    add_test(
        NAME vnm_terminal_input_echo_catchup_benchmark_validate
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
            "QT_SCALE_FACTOR=1"
            "QT_SCREEN_SCALE_FACTORS="
            "QT_AUTO_SCREEN_SCALE_FACTOR=0"
            "QT_DEVICE_PIXEL_RATIO="
            "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            "${vnm_terminal_input_echo_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_input_echo_catchup_benchmark>"
            "--iterations" "4"
            "--warmup" "1"
            "--catchup-budget-us" "0,500,2000"
            "--echo-delay-us" "0,1000,4000"
            "--polish-delay-us" "2000"
            "--pre-echo-backlog" "2"
            "--output" "${CMAKE_CURRENT_BINARY_DIR}/input_echo_catchup_validate_output.json"
            "--quiet"
            "--validate-json"
    )

    set_tests_properties(vnm_terminal_input_echo_catchup_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark;input-echo"
        TIMEOUT 60
    )

    add_test(
        NAME vnm_terminal_input_echo_queue_contract_benchmark_validate
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
            "QT_SCALE_FACTOR=1"
            "QT_SCREEN_SCALE_FACTORS="
            "QT_AUTO_SCREEN_SCALE_FACTOR=0"
            "QT_DEVICE_PIXEL_RATIO="
            "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            "${vnm_terminal_input_echo_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_input_echo_catchup_benchmark>"
            "--queue-contract"
            "--output" "${CMAKE_CURRENT_BINARY_DIR}/input_echo_queue_contract_validate_output.json"
            "--quiet"
            "--validate-json"
    )

    set_tests_properties(vnm_terminal_input_echo_queue_contract_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark;input-echo;queue-contract"
        TIMEOUT 60
    )

    add_test(
        NAME vnm_terminal_input_echo_ordering_benchmark_validate
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
            "QT_SCALE_FACTOR=1"
            "QT_SCREEN_SCALE_FACTORS="
            "QT_AUTO_SCREEN_SCALE_FACTOR=0"
            "QT_DEVICE_PIXEL_RATIO="
            "QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough"
            "${vnm_terminal_input_echo_qt_runtime_path}"
            "$<TARGET_FILE:vnm_terminal_input_echo_ordering_benchmark>"
            "--output" "${CMAKE_CURRENT_BINARY_DIR}/input_echo_ordering_validate_output.json"
            "--quiet"
            "--validate-json"
    )

    set_tests_properties(vnm_terminal_input_echo_ordering_benchmark_validate PROPERTIES
        LABELS "readiness;benchmark;input-echo"
        TIMEOUT 90
    )
endif()
