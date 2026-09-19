/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void make_status_bar()
{
    lv_obj_t *bar = make_box(s_screen, 0, 0, kScreenWidth, kStatusHeight, kBackground);
    const lyra::audio::Status audio_status = lyra::audio::status();
    lv_obj_t *volume_button = make_button(bar, 0, 0, 84, kStatusHeight, kBackground, 0);
    lv_obj_set_style_bg_opa(volume_button, LV_OPA_TRANSP, 0);
    s_status_volume_label = make_label(volume_button, "", kTextSecondary);
    lv_obj_align(s_status_volume_label, LV_ALIGN_LEFT_MID, 9, 0);
    update_status_volume_label(audio_status.volume_percent);
    lv_obj_add_event_cb(volume_button, show_volume_popup_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *time = make_label(bar, "12:30", kTextPrimary);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, 0);
    // Battery ADC calibration is not yet available on the reference board, so
    // this status-bar value is a UI placeholder rather than a voltage reading.
    lv_obj_t *battery = make_label(bar, "100%  " LV_SYMBOL_BATTERY_FULL, kTextSecondary);
    lv_obj_align(battery, LV_ALIGN_RIGHT_MID, -9, 0);
}

lv_obj_t *make_header(const char *title, View back, bool show_back, const char *right,
                      lv_event_cb_t custom_back)
{
    lv_obj_t *header = make_box(s_screen, 0, kStatusHeight, kScreenWidth, 44, kBackground);
    lv_obj_t *divider = make_box(header, 8, 43, 304, 1, kDivider);
    (void)divider;
    if (show_back) {
        lv_obj_t *back_button = make_button(header, 4, 4, 42, 36, kBackground, 0);
        lv_obj_t *icon = make_label(back_button, LV_SYMBOL_LEFT, kTextPrimary);
        lv_obj_center(icon);
        if (custom_back) lv_obj_add_event_cb(back_button, custom_back, LV_EVENT_CLICKED, nullptr);
        else lv_obj_add_event_cb(back_button, [](lv_event_t *event) {
            const View fallback = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
            navigate_back(fallback);
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(back)));
    }
    lv_obj_t *label = make_label(header, title, kTextPrimary);
    lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
    make_marquee(label, right ? 180 : 245);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, show_back ? 45 : 10, 0);
    if (right != nullptr) {
        lv_obj_t *right_label = make_label(header, right, kAccent);
        lv_obj_align(right_label, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    return header;
}

void dismiss_overlay_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (overlay) lv_obj_delete(overlay);
}

void show_notice(const char *title_text, const char *message_text)
{
    if (!s_screen) return;
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                                 kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 24, 142, 272, 196, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *message = make_label(dialog, message_text, kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, 232);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -3);
    lv_obj_t *close = make_button(dialog, 78, 134, 116, 44, kAccentDark, 7);
    lv_obj_t *close_label = make_label(close, tr(lyra::i18n::StringId::Ok), kTextOnAccent);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
}

lv_obj_t *make_row(lv_obj_t *parent, int y, const char *icon_text, const char *title,
                   const char *subtitle, View target, int height)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 6, true);
    lv_obj_set_style_bg_color(row, kSurfaceRaised, LV_STATE_PRESSED);
    const int text_x = icon_text ? 45 : 12;
    if (icon_text) {
        lv_obj_t *icon = make_label(row, icon_text, kAccent);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, subtitle == nullptr ? 0 : -1);
    }
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    lv_obj_set_style_text_font(title_label, lyra::font::ui(), 0);
    make_marquee(title_label, icon_text ? 220 : 253);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, text_x, subtitle == nullptr ? 0 : -10);
    if (subtitle != nullptr) {
        lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
        make_marquee(sub, icon_text ? 220 : 253);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, text_x, 11);
    }
    lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT, kTextMuted);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
    add_route(row, target);
    return row;
}

void release_boot_image()
{
    heap_caps_free(s_boot_image_pixels);
    s_boot_image_pixels = nullptr;
    s_boot_image_descriptor = {};
}

bool load_brand_logo()
{
    if (s_brand_logo_pixels) return true;
    const size_t logo_bytes = static_cast<size_t>(kBootImageWidth) *
        kBootImageHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        logo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !lyra::media::decode_png(boot_png_start,
                                             static_cast<size_t>(boot_png_end - boot_png_start),
                                             pixels, kBootImageWidth, kBootImageHeight,
                                             false, kBootBackgroundRgb)) {
        heap_caps_free(pixels);
        return false;
    }
    s_brand_logo_pixels = reinterpret_cast<uint8_t *>(pixels);
    s_brand_logo_descriptor = {};
    s_brand_logo_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_brand_logo_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    s_brand_logo_descriptor.header.w = kBootImageWidth;
    s_brand_logo_descriptor.header.h = kBootImageHeight;
    s_brand_logo_descriptor.header.stride = kBootImageWidth * sizeof(uint16_t);
    s_brand_logo_descriptor.data_size = logo_bytes;
    s_brand_logo_descriptor.data = s_brand_logo_pixels;
    return true;
}

bool load_player_art(const lyra::media::Track &track)
{
    const lyra::media::Status media_status = lyra::media::status();
    const uint16_t artwork_size = media_status.artwork_size;
    const size_t player_art_pixels = static_cast<size_t>(artwork_size) * artwork_size;
    const size_t player_art_bytes = player_art_pixels * sizeof(uint16_t);
    if (s_player_art_pixels && std::strcmp(s_player_art_album, track.album) == 0 &&
        s_player_art_catalog_generation == media_status.catalog_generation &&
        s_player_art_generation == media_status.artwork_generation) return true;

    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        player_art_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // The GUI copy is CPU-read image data, not a DMA source. Never consume
    // internal SRAM here: the LCD transport and I2S driver need that heap.
    if (!pixels || !lyra::media::copy_artwork(track, pixels, player_art_pixels)) {
        heap_caps_free(pixels);
        return false;
    }

    heap_caps_free(s_player_art_pixels);
    s_player_art_pixels = reinterpret_cast<uint8_t *>(pixels);
    copy_ui_text(s_player_art_album, sizeof(s_player_art_album), track.album);
    s_player_art_catalog_generation = media_status.catalog_generation;
    s_player_art_generation = media_status.artwork_generation;
    s_player_art_descriptor = {};
    s_player_art_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_player_art_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    s_player_art_descriptor.header.w = artwork_size;
    s_player_art_descriptor.header.h = artwork_size;
    s_player_art_descriptor.header.stride = artwork_size * sizeof(uint16_t);
    s_player_art_descriptor.data_size = player_art_bytes;
    s_player_art_descriptor.data = s_player_art_pixels;
    return true;
}

lv_obj_t *make_artwork(lv_obj_t *parent, int x, int y, int width, int height,
                       const lyra::media::Track &track, int radius,
                       bool preserve_aspect)
{
    lv_obj_t *art = make_box(parent, x, y, width, height, kArtworkSurface, radius);
    lv_obj_t *fallback = make_label(art, LV_SYMBOL_AUDIO, kAccent);
    lv_obj_set_style_text_font(fallback, &lv_font_montserrat_18, 0);
    lv_obj_set_style_transform_scale_x(fallback, width > 80 ? 512 : 320, 0);
    lv_obj_set_style_transform_scale_y(fallback, width > 80 ? 512 : 320, 0);
    lv_obj_center(fallback);
    lv_obj_t *image = lv_image_create(art);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, preserve_aspect ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_radius(image, radius, 0);
    if (load_player_art(track)) {
        lv_image_set_src(image, &s_player_art_descriptor);
        lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    } else {
        lyra::media::request_artwork(track, lyra::media::ArtworkSize::Player);
    }
    return art;
}

} // namespace lyra::gui::internal
