if (NOT TARGET pico-ssd1306)
    add_library(pico-ssd1306 INTERFACE)

    target_sources(pico-ssd1306 INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/ssd1306.c
    )

    target_link_libraries(pico-ssd1306 INTERFACE pico_stdlib hardware_i2c)
endif()
