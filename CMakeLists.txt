cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

set(project_name "pico_battery_op" C CXX)
project(${project_name})
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(lib/pico-ssd1306)

set(bin_name ${PROJECT_NAME})
add_executable(${bin_name}
    main.cpp
    power_management.cpp
)

#pico_enable_stdio_usb(${bin_name} 1)
#pico_enable_stdio_uart(${bin_name} 1)

target_link_libraries(${bin_name} 
    hardware_adc
    hardware_sleep
    hardware_uart
    hardware_watchdog
    pico_runtime_init
    pico_stdlib
    pico-ssd1306
    pico_stdio_usb
)

target_include_directories(${bin_name} INTERFACE lib/)

pico_add_extra_outputs(${bin_name})