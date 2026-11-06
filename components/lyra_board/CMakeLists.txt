idf_component_register(
    SRCS
        "lyra_board.cpp"
        "lyra_axs15231b.cpp"
        "lyra_lvgl_memory.cpp"
    INCLUDE_DIRS
        "include"
        "../../board/JC3248W535EN/include"
    REQUIRES
        esp_lcd
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        freertos
        lvgl
    PRIV_REQUIRES
        esp_common
        esp_timer
        heap
        log
)

# LVGL references its allocator but nothing else in this component does.
# Pull the object in explicitly so the custom LVGL heap never depends on
# library link order.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=lv_malloc_core")
