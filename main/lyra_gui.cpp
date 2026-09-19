/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lyra_board.h"
#include "lyra_font.h"
#include "lyra_png.h"

#include "gui/lyra_gui_internal.h"

using namespace lyra::gui::internal;

esp_err_t lyra_gui_show_boot(lv_display_t *display)
{
    if (!display) return ESP_ERR_INVALID_ARG;
    lv_obj_t *screen = lv_display_get_screen_active(display);
    if (!screen) return ESP_FAIL;

    lv_obj_clean(screen);
    release_boot_image();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBootBackgroundRgb), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const size_t boot_image_bytes = static_cast<size_t>(kBootImageWidth) *
        kBootImageHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        boot_image_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) return ESP_ERR_NO_MEM;
    if (!lyra::media::decode_png(boot_png_start,
                                 static_cast<size_t>(boot_png_end - boot_png_start),
                                 pixels, kBootImageWidth, kBootImageHeight,
                                 false, kBootBackgroundRgb)) {
        heap_caps_free(pixels);
        return ESP_FAIL;
    }

    s_boot_image_pixels = reinterpret_cast<uint8_t *>(pixels);
    s_boot_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_boot_image_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    s_boot_image_descriptor.header.w = kBootImageWidth;
    s_boot_image_descriptor.header.h = kBootImageHeight;
    s_boot_image_descriptor.header.stride = kBootImageWidth * sizeof(uint16_t);
    s_boot_image_descriptor.data_size = boot_image_bytes;
    s_boot_image_descriptor.data = s_boot_image_pixels;

    lv_obj_t *image = lv_image_create(screen);
    lv_image_set_src(image, &s_boot_image_descriptor);
    lv_obj_set_pos(image, (kScreenWidth - kBootImageWidth) / 2,
                   (kScreenHeight - kBootImageHeight) / 2);

    ESP_LOGI(kTag, "Embedded boot graphic prepared");
    return ESP_OK;
}

esp_err_t lyra_gui_start(lv_display_t *display)
{
    if (display == nullptr) return ESP_ERR_INVALID_ARG;
    s_screen = lv_display_get_screen_active(display);
    if (s_screen == nullptr) return ESP_FAIL;
    release_boot_image();
    lyra::font::init();

    s_current_track = 0;
    s_selected_playlist = 0;
    s_library_tab = LibraryTab::Songs;
    s_artist_detail_tab = ArtistDetailTab::Songs;
    s_sort_section = lyra::media::SortSection::Songs;
    s_navigation_depth = 0;
    s_playback_scope = PlaybackScope::Single;
    clear_saved_queue();
    s_saved_queue_pending = lyra::media::queue_snapshot_exists();
    s_has_active_queue = false;
    s_queue_position = 0;
    s_queue_position_valid = false;
    copy_ui_text(s_playback_folder_path, sizeof(s_playback_folder_path), "/sdcard");
    reset_shuffle_queue();
    s_audio_eof_seen = false;
    s_gapless = true;
    s_replay_gain = true;
    s_crossfade_seconds = 0;
    s_brightness_percent = 72;
    s_date_format = DateFormat::DayMonthYear;
    s_use_24_hour = true;
    s_dst_enabled = false;
    s_dark_mode = true;
    s_accent_colour = 0;
    s_speaker_output_enabled = lyra::audio::kDefaultSpeakerOutputEnabled;
    s_equalizer_preset = EqualizerPreset::Custom;
    std::memset(s_equalizer_custom_bands, 0, sizeof(s_equalizer_custom_bands));
    s_debug_tab = DebugTab::Info;
    s_debug_status[0] = '\0';
    s_sleep_timer_minutes = 0;
    s_sleep_timer_deadline_us = 0;
    s_clock_input_kind = ClockInputKind::Time;
    std::strcpy(s_clock_input_digits, "0000");
    s_clock_input_cursor = 0;
    s_clock_input_pm = false;
    s_pending_track_advance = false;
    s_pending_track_advance_us = 0;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    load_user_settings();
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title),
                 lyra::i18n::tr(lyra::i18n::StringId::AllSongs));
    apply_theme_palette();
    const esp_err_t speaker_output_result =
        lyra::audio::set_speaker_output_enabled(s_speaker_output_enabled);
    if (speaker_output_result != ESP_OK) {
        ESP_LOGW(kTag, "could not restore on-board speaker output: %s",
                 esp_err_to_name(speaker_output_result));
    }
    apply_equalizer_to_audio();
    const esp_err_t brightness_result = lyra_board_display_set_brightness(s_brightness_percent);
    if (brightness_result != ESP_OK) {
        ESP_LOGW(kTag, "could not restore display brightness: %s",
                 esp_err_to_name(brightness_result));
    }
    s_search_query[0] = '\0';
    s_submitted_search_query[0] = '\0';
    s_library_keyboard_symbols = false;
    s_search_category = lyra::media::SearchCategory::Songs;
    s_submitted_search_category = lyra::media::SearchCategory::Songs;
    s_playlist_name[0] = '\0';
    s_playlist_manage_mode = false;
    const lyra::media::Status status = lyra::media::status();
    s_seen_catalog_generation = status.catalog_generation;
    s_seen_scanning = status.scanning;
    s_seen_artwork_generation = status.artwork_generation;
    s_seen_duration_generation = status.duration_generation;
    s_seen_sorting_generation = status.sorting_generation;
    s_seen_search_generation = lyra::media::search_status().generation;
    render(View::Menu);
    lv_timer_create(player_progress_poll_cb, 250, nullptr);
    // The crossfade envelope must not force full player-screen redraws. Keep
    // its gain work isolated from normal progress/UI refreshes.
    lv_timer_create(crossfade_poll_cb, 100, nullptr);
    lv_timer_create(catalog_poll_cb, 500, nullptr);
    ESP_LOGI(kTag, "Lyra GUI created with MicroSD-backed catalog and native multi-format playback");
    return ESP_OK;
}
