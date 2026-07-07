cmake_minimum_required(VERSION 3.13)
if (NOT TARGET pico_battery_op)
    add_library(pico_battery_op INTERFACE)

    target_sources(pico_battery_op INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/power_management.cpp
    )

    target_include_directories(pico_battery_op INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
    )

    target_link_libraries(pico_battery_op INTERFACE
        pico_stdlib
        hardware_adc
        hardware_sleep
        hardware_uart
        hardware_watchdog
        pico_runtime_init
        pico_stdio_usb
    )
endif()
