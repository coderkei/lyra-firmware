idf_component_register(
    SRCS
        "app_main.cpp"
        "lyra_gui.cpp"
        "lyra_audio.cpp"
        "lyra_media.cpp"
        "lyra_sd.cpp"
        "lyra_progressive_jpeg.cpp"
        "lyra_png.cpp"
        "lyra_jpeg_memory.c"
        "lyra_font.cpp"
        "lyra_unicode_16.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "../graphics/boot.png"
    REQUIRES
        lyra_board
        lvgl
        log
        fatfs
        esp_driver_sdmmc
        esp_driver_i2s
        sdmmc
        esp_system
        esp_timer
        esp_hw_support
        nvs_flash
        libjpeg-turbo
        libpng
        esp_audio_codec
)

# Keep JPEG virtual arrays in PSRAM when they fit. Only oversized progressive
# arrays use a transient, audio-aware SD working file. The media layer may
# separately retain their much smaller final decoded artwork.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=jpeg_get_large"
    "-Wl,--wrap=jpeg_free_large"
    "-Wl,--wrap=jpeg_mem_available"
    "-Wl,--wrap=jpeg_open_backing_store"
    "-Wl,--undefined=__wrap_jpeg_get_large"
    "-Wl,--undefined=__wrap_jpeg_free_large"
    "-Wl,--undefined=__wrap_jpeg_mem_available"
    "-Wl,--undefined=__wrap_jpeg_open_backing_store"
    # A progressive-disabled libjpeg build leaves jdphuff.c empty.  Requiring
    # this implementation symbol turns that silent runtime mismatch into a
    # link failure instead of JERR_NOT_COMPILED on the device.
    "-Wl,--undefined=jinit_phuff_decoder"
)
