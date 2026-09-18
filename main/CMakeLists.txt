idf_component_register(
    SRCS
        "app_main.cpp"
        "lyra_boot_test.cpp"
        "lyra_gui.cpp"
        "gui/lyra_gui_state.cpp"
        "gui/shared/lyra_gui_theme_widgets.cpp"
        "gui/shared/lyra_gui_status_artwork.cpp"
        "gui/queue/lyra_gui_queue_core.cpp"
        "gui/queue/lyra_gui_queue_view.cpp"
        "gui/library/lyra_gui_library_rows.cpp"
        "gui/library/lyra_gui_library_screens.cpp"
        "gui/track_details/lyra_gui_track_details.cpp"
        "gui/track_details/lyra_gui_track_details_player_art.cpp"
        "gui/system/lyra_gui_system_debug.cpp"
        "gui/now_playing/lyra_gui_now_playing.cpp"
        "gui/settings/lyra_gui_equalizer.cpp"
        "gui/settings/lyra_gui_settings_system.cpp"
        "gui/search/lyra_gui_search.cpp"
        "gui/lifecycle/lyra_gui_lifecycle.cpp"
        "lyra_gui_settings.cpp"
        "lyra_audio.cpp"
        "audio/lyra_audio_state.cpp"
        "audio/core/lyra_audio_io.cpp"
        "audio/formats/lyra_audio_formats.cpp"
        "audio/playback/lyra_audio_pcm_output.cpp"
        "audio/playback/lyra_audio_decoder_task.cpp"
        "lyra_audio_pcm.cpp"
        "lyra_media.cpp"
        "media/lyra_media_state.cpp"
        "media/catalog/lyra_media_catalog_settings.cpp"
        "media/catalog/lyra_media_catalog_pipeline.cpp"
        "media/metadata/lyra_media_metadata.cpp"
        "media/playlists/lyra_media_playlists_favorites.cpp"
        "lyra_media_artwork.cpp"
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
        esp_psram
        esp_partition
        app_update
        bootloader_support
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
