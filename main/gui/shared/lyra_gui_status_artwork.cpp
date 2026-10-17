/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

lv_obj_t *s_status_wifi_signal_bars[kWifiSignalBarCount]{};
lv_timer_t *s_status_wifi_signal_timer;

lv_color_t status_wifi_signal_color()
{
    if (is_aura_theme()) return kTextOnAccent;
    return s_neon_hud ? kAccent : kTextSecondary;
}

lv_color_t status_wifi_signal_inactive_color()
{
    return is_aura_theme() ? lv_color_hex(0x5C96BC) : kDivider;
}

void refresh_status_wifi_signal()
{
    if (!s_status_wifi_signal_bars[0]) return;
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const int active_bars = wifi.connected && wifi.has_connected_rssi ?
        wifi_signal_bar_count(wifi.connected_rssi) : 0;
    const lv_color_t active_color = status_wifi_signal_color();
    const lv_color_t inactive_color = status_wifi_signal_inactive_color();
    for (size_t index = 0; index < kWifiSignalBarCount; ++index) {
        const bool active = static_cast<int>(index) < active_bars;
        lv_obj_set_style_bg_color(s_status_wifi_signal_bars[index],
                                  active ? active_color : inactive_color, 0);
        lv_obj_set_style_bg_opa(s_status_wifi_signal_bars[index],
                                 active ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

void status_wifi_signal_timer_cb(lv_timer_t *)
{
    refresh_status_wifi_signal();
}

void status_bar_delete_cb(lv_event_t *)
{
    for (lv_obj_t *&signal_bar : s_status_wifi_signal_bars) signal_bar = nullptr;
    if (s_status_wifi_signal_timer) {
        lv_timer_delete(s_status_wifi_signal_timer);
        s_status_wifi_signal_timer = nullptr;
    }
}

} // namespace

int wifi_signal_bar_count(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

void make_wifi_signal_bars(lv_obj_t *parent, int x, int y, int signal_bars,
                           lv_color_t active_color, lv_color_t inactive_color,
                           lv_obj_t **bars_out)
{
    static constexpr int kBarHeights[kWifiSignalBarCount] = {4, 7, 10, 13};
    const int clamped_signal_bars = std::clamp(signal_bars, 0,
                                                static_cast<int>(kWifiSignalBarCount));
    for (size_t index = 0; index < kWifiSignalBarCount; ++index) {
        const bool active = static_cast<int>(index) < clamped_signal_bars;
        lv_obj_t *signal_bar = make_box(parent, x + static_cast<int>(index) * 4,
            y + 13 - kBarHeights[index], 2, kBarHeights[index],
            active ? active_color : inactive_color, 1);
        lv_obj_set_style_bg_opa(signal_bar, active ? LV_OPA_COVER : LV_OPA_50, 0);
        if (bars_out) bars_out[index] = signal_bar;
    }
}

void update_status_time_label()
{
    if (!s_status_time_label) return;
    const lyra::clock::DateTime current = lyra::clock::now();
    char value[16];
    format_clock_time(current, value, sizeof(value));
    set_label_text_if_changed(s_status_time_label, value);
}

static lv_obj_t *make_placeholder_shape(lv_obj_t *parent, int x, int y,
                                        int width, int height,
                                        lv_color_t color, int radius,
                                        lv_color_t border = lv_color_hex(0x000000),
                                        int border_width = 0)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_style_bg_color(shape, color, 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shape, border_width, 0);
    if (border_width > 0) lv_obj_set_style_border_color(shape, border, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_set_style_pad_all(shape, 0, 0);
    lv_obj_clear_flag(shape, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
    return shape;
}

void make_artwork_placeholder(lv_obj_t *art, int width, int height)
{
    if (!art || width <= 0 || height <= 0) return;
    const int size = std::min(width, height);
    const int center_x = width / 2;
    const int center_y = height / 2;
    const int scale = std::max(1, size / 100);
    const int inset = std::max(2, size / 12);

    lv_color_t background = kArtworkSurface;
    if (is_standard_theme()) background = lv_color_hex(s_dark_mode ? 0x111923 : 0xE9EDF2);
    else if (is_cute_theme()) background = lv_color_hex(0xF9E7F4);
    else if (is_zeno_theme()) background = lv_color_hex(0x170F1A);
    else if (is_aura_theme()) background = lv_color_hex(0xE1F4FF);
    else if (s_neon_hud) background = lv_color_hex(0x07131E);
    lv_obj_set_style_bg_color(art, background, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);

    if (is_standard_theme()) {
        // A centered record gives the neutral theme a familiar album-cover cue.
        const int disc = std::max(12, size * 68 / 100);
        const int x = center_x - disc / 2;
        const int y = center_y - disc / 2;
        make_placeholder_shape(art, x, y, disc, disc, kSurfaceRaised,
                               LV_RADIUS_CIRCLE, kAccent, scale * 2);
        const int ring = disc * 68 / 100;
        make_placeholder_shape(art, center_x - ring / 2, center_y - ring / 2,
                               ring, ring, background, LV_RADIUS_CIRCLE,
                               kTextMuted, scale);
        const int label = std::max(6, disc * 30 / 100);
        make_placeholder_shape(art, center_x - label / 2, center_y - label / 2,
                               label, label, kAccent, LV_RADIUS_CIRCLE);
        const int hole = std::max(2, scale * 2);
        make_placeholder_shape(art, center_x - hole / 2, center_y - hole / 2,
                               hole, hole, kTextPrimary, LV_RADIUS_CIRCLE);
        return;
    }

    if (s_neon_hud) {
        // Cyan and magenta bars sit on a compact HUD grid.
        const int bar_width = std::max(2, size / 14);
        const int gap = std::max(2, size / 18);
        const int count = 5;
        const int group_width = count * bar_width + (count - 1) * gap;
        const int base_y = center_y + size * 22 / 100;
        const int heights[] = {size * 24 / 100, size * 42 / 100,
                               size * 31 / 100, size * 50 / 100,
                               size * 34 / 100};
        const int grid_y = center_y - size * 28 / 100;
        make_placeholder_shape(art, inset, grid_y, size - inset * 2,
                               std::max(1, scale), kDivider, 0);
        make_placeholder_shape(art, inset, base_y, size - inset * 2,
                               std::max(1, scale), kDivider, 0);
        for (int i = 0; i < count; ++i) {
            const int x = center_x - group_width / 2 + i * (bar_width + gap);
            const lv_color_t color = (i % 2) ? kAccent : kTextSecondary;
            make_placeholder_shape(art, x, base_y - heights[i], bar_width,
                                   heights[i], color, scale);
        }
        return;
    }

    if (is_cute_theme()) {
        // Draw a paired-note mark with a continuous beam. Keep the joins
        // overlapping so the small library thumbnails do not show broken bars.
        const int group_width = size * 44 / 100;
        const int dot = std::max(5, size * 15 / 100);
        const int stem = std::max(2, size * 6 / 100);
        const int left_x = center_x - group_width / 2;
        const int right_x = left_x + group_width * 58 / 100;
        const int left_stem_x = left_x + dot - stem;
        const int right_stem_x = right_x + dot - stem;
        const int left_dot_y = center_y + size * 10 / 100;
        const int right_dot_y = left_dot_y - std::max(1, size * 6 / 100);
        const int beam_y = center_y - size * 27 / 100;
        const int beam_height = std::max(2, size * 6 / 100);
        const int stem_y = beam_y + beam_height - std::max(1, scale / 2);
        const int beam_width = right_stem_x + stem - left_stem_x;
        const int left_stem_height = left_dot_y + dot / 3 - stem_y;
        const int right_stem_height = right_dot_y + dot / 3 - stem_y;

        make_placeholder_shape(art, left_stem_x, beam_y, beam_width,
                               beam_height, kAccentDark, scale);
        make_placeholder_shape(art, left_stem_x, stem_y, stem,
                               std::max(stem, left_stem_height), kAccent, scale);
        make_placeholder_shape(art, right_stem_x, stem_y, stem,
                               std::max(stem, right_stem_height), kAccentDark, scale);
        make_placeholder_shape(art, left_x, left_dot_y, dot, dot,
                               kAccent, LV_RADIUS_CIRCLE);
        make_placeholder_shape(art, right_x, right_dot_y, dot, dot,
                               kAccentDark, LV_RADIUS_CIRCLE);
        return;
    }

    if (is_zeno_theme()) {
        // Pixel blocks form a compact waveform for the list-first theme.
        const int bar_width = std::max(2, size / 14);
        const int gap = std::max(1, size / 30);
        const int levels[] = {2, 4, 3, 5, 4, 2, 3};
        const int count = static_cast<int>(sizeof(levels) / sizeof(levels[0]));
        const int pitch = bar_width + gap;
        const int group_width = count * bar_width + (count - 1) * gap;
        const int base_y = center_y + size * 24 / 100;
        for (int column = 0; column < count; ++column) {
            const int x = center_x - group_width / 2 + column * pitch;
            const int bar_height = levels[column] * std::max(2, size / 12);
            make_placeholder_shape(art, x, base_y - bar_height, bar_width,
                                   bar_height, (column % 2) ? kTextPrimary : kAccent,
                                   0);
        }
        return;
    }

    // Aura uses concentric, precisely centered rings and a small orbiting point.
    const int orbit = size * 70 / 100;
    make_placeholder_shape(art, center_x - orbit / 2, center_y - orbit / 2,
                           orbit, orbit, background, LV_RADIUS_CIRCLE,
                           lv_color_hex(0x72BCEB), scale * 2);
    const int orb = size * 48 / 100;
    make_placeholder_shape(art, center_x - orb / 2, center_y - orb / 2,
                           orb, orb, kAccentSurface, LV_RADIUS_CIRCLE,
                           lv_color_hex(0x1286D2), scale);
    const int highlight = std::max(4, orb * 20 / 100);
    make_placeholder_shape(art, center_x - orb / 5, center_y - orb / 5,
                           highlight, highlight, lv_color_hex(0xF5FCFF),
                           LV_RADIUS_CIRCLE);
    const int satellite = std::max(3, size * 7 / 100);
    make_placeholder_shape(art, center_x + orbit * 30 / 100 - satellite / 2,
                           center_y - orbit * 30 / 100 - satellite / 2,
                           satellite, satellite, kAccent, LV_RADIUS_CIRCLE);
}

void make_status_bar()
{
    lv_obj_t *bar = make_box(s_screen, 0, 0, kScreenWidth, kStatusHeight, kBackground);
    if (is_cute_theme()) lv_obj_set_style_bg_color(bar, lv_color_hex(0xD8EEFF), 0);
    if (is_aura_theme()) {
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x07549E), 0);
        lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x177FCC), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    }
    const lyra::audio::Status audio_status = lyra::audio::status();
    lv_obj_t *volume_button = make_button(bar, 0, 0, 84, kStatusHeight, kBackground, 0);
    lv_obj_set_style_bg_opa(volume_button, LV_OPA_TRANSP, 0);
    s_status_volume_label = make_label(volume_button, "", kTextSecondary);
    lv_obj_align(s_status_volume_label, LV_ALIGN_LEFT_MID, 9, 0);
    update_status_volume_label(audio_status.volume_percent);
    lv_obj_add_event_cb(volume_button, show_volume_popup_cb, LV_EVENT_CLICKED, nullptr);
    s_status_time_label = make_label(bar, "", s_neon_hud ? kTextSecondary : kTextPrimary);
    lv_obj_align(s_status_time_label, LV_ALIGN_CENTER, 0, 0);
    update_status_time_label();
    // Battery ADC calibration is not yet available on the reference board, so
    // this status-bar value is a UI placeholder rather than a voltage reading.
    lv_obj_t *battery = make_label(bar, "100%  " LV_SYMBOL_BATTERY_FULL, kTextSecondary);
    lv_obj_align(battery, LV_ALIGN_RIGHT_MID, -9, 0);
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const int active_wifi_bars = wifi.connected && wifi.has_connected_rssi ?
        wifi_signal_bar_count(wifi.connected_rssi) : 0;
    make_wifi_signal_bars(bar, 224, 7, active_wifi_bars,
                          status_wifi_signal_color(), status_wifi_signal_inactive_color(),
                          s_status_wifi_signal_bars);
    lv_obj_add_event_cb(bar, status_bar_delete_cb, LV_EVENT_DELETE, nullptr);
    if (!s_status_wifi_signal_timer) {
        s_status_wifi_signal_timer = lv_timer_create(status_wifi_signal_timer_cb, 1000, nullptr);
    }
    if (is_aura_theme()) {
        lv_obj_set_style_text_color(s_status_volume_label, kTextOnAccent, 0);
        lv_obj_set_style_text_color(s_status_time_label, kTextOnAccent, 0);
        lv_obj_set_style_text_color(battery, kTextOnAccent, 0);
        make_box(bar, 0, kStatusHeight - 1, kScreenWidth, 1, lv_color_hex(0xE6FAFF));
    }
    if (s_neon_hud) make_box(bar, 8, kStatusHeight - 1, 304, 1, kAccent);
    refresh_status_wifi_signal();
}

lv_obj_t *make_header(const char *title, View back, bool show_back, const char *right,
                      lv_event_cb_t custom_back)
{
    const bool queue_add_toggle = !s_playlist_add_mode &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::AlbumDetail || s_view == View::ArtistDetail ||
         s_view == View::TrackList || s_view == View::Folders ||
         s_view == View::FolderDetail || s_view == View::FolderPlaylist ||
         s_view == View::PlaylistDetail);
    lv_obj_t *header = make_box(s_screen, 0, kStatusHeight, kScreenWidth, 44, kBackground);
    lv_obj_t *divider = make_box(header, 8, 43, 304, 1, s_neon_hud ? kAccent : kDivider);
    (void)divider;
    if (show_back) {
        lv_obj_t *back_button = make_button(header, 4, 4, 42, 36, kBackground, 0);
        if (is_cute_theme()) {
            lv_obj_set_style_radius(back_button, 12, 0);
            lv_obj_set_style_bg_color(back_button, kSurface, 0);
            lv_obj_set_style_border_width(back_button, 1, 0);
            add_cute_plastic_finish(back_button);
        }
        if (is_zeno_theme()) {
            lv_obj_set_size(back_button, 36, 36);
            lv_obj_set_style_radius(back_button, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(back_button, 1, 0);
            lv_obj_set_style_border_color(back_button, kTextSecondary, 0);
        }
        lv_obj_t *icon = make_label(back_button, LV_SYMBOL_LEFT, !is_standard_theme() ? kAccent : kTextPrimary);
        lv_obj_center(icon);
        if (custom_back) lv_obj_add_event_cb(back_button, custom_back, LV_EVENT_CLICKED, nullptr);
        else lv_obj_add_event_cb(back_button, [](lv_event_t *event) {
            const View fallback = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
            navigate_back(fallback);
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(back)));
    }
    lv_obj_t *label = is_zeno_theme() ? make_zeno_label(header, title) :
        make_label(header, title, s_neon_hud ? kAccent : kTextPrimary);
    lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
    if (is_zeno_theme()) lv_obj_set_style_text_font(label, zeno_heading_font(), 0);
    const bool show_right_label = right != nullptr && (!queue_add_toggle || right[0] != '\0');
    make_marquee(label, queue_add_toggle ? (show_right_label ? 150 : 180) :
                 (right ? 180 : 245));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, show_back ? 45 : 10, 0);
    if (show_right_label) {
        lv_obj_t *right_label = make_label(header, right, s_neon_hud ? kTextSecondary : kAccent);
        make_marquee(right_label, queue_add_toggle ? 64 : 84);
        lv_obj_align(right_label, LV_ALIGN_RIGHT_MID,
                     queue_add_toggle ? -52 : -12, 0);
    }
    if (queue_add_toggle) make_queue_add_mode_toggle(header, 274, 4);
    if (is_cute_theme() && !show_right_label && !queue_add_toggle) {
        make_cute_header_mascot(header);
    }
    return header;
}

void queue_add_mode_toggle_cb(lv_event_t *)
{
    s_queue_add_mode = !s_queue_add_mode;
    render(s_view);
}

void make_queue_add_mode_toggle(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *button = make_button(parent, x, y, width, height,
        s_queue_add_mode ? kAccentDark : kBackground, 5);
    lv_obj_t *icon = make_label(button, LV_SYMBOL_LIST,
                                s_queue_add_mode ? kTextOnAccent : kTextSecondary);
    lv_obj_center(icon);
    lv_obj_add_event_cb(button, queue_add_mode_toggle_cb, LV_EVENT_CLICKED, nullptr);
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
    lv_obj_t *title = make_label(dialog, title_text, s_neon_hud ? kAccent : kTextPrimary);
    lv_obj_set_width(title, 232);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
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
    if (!is_aura_theme())
        lv_obj_set_style_bg_color(row, !is_standard_theme() ? kAccentSurface : kSurfaceRaised, LV_STATE_PRESSED);
    const int text_x = icon_text ? 45 : 12;
    if (icon_text) {
        lv_obj_t *icon = make_label(row, icon_text, s_neon_hud ? kTextSecondary : kAccent);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, subtitle == nullptr ? 0 : -1);
    }
    lv_obj_t *title_label = make_label(row, title, s_neon_hud ? kTextSecondary : kTextPrimary);
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
    if (is_zeno_theme() && subtitle == nullptr) {
        lv_obj_set_style_text_font(title_label, zeno_heading_font(), 0);
        if (icon_text) lv_obj_set_style_text_color(lv_obj_get_child(row, 0), kTextPrimary, 0);
    }
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
    static bool cached_dark_mode = true;
    const bool dark = uses_dark_palette();
    if (s_brand_logo_pixels && cached_dark_mode == dark) return true;
    if (s_brand_logo_pixels) {
        heap_caps_free(s_brand_logo_pixels);
        s_brand_logo_pixels = nullptr;
        s_brand_logo_descriptor = {};
    }

    const uint8_t *png_start = dark ? boot_png_start : boot_light_png_start;
    const uint8_t *png_end = dark ? boot_png_end : boot_light_png_end;
    constexpr uint32_t kLightBootBackgroundRgb = 0xF8FAFC;
    const uint32_t background_rgb = dark ? kBootBackgroundRgb :
                                                  kLightBootBackgroundRgb;
    const size_t logo_bytes = static_cast<size_t>(kBootImageWidth) *
        kBootImageHeight * sizeof(uint16_t);
    auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(
        logo_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !lyra::media::decode_png(png_start,
                                             static_cast<size_t>(png_end - png_start),
                                             pixels, kBootImageWidth, kBootImageHeight,
                                             false, background_rgb)) {
        heap_caps_free(pixels);
        return false;
    }
    s_brand_logo_pixels = reinterpret_cast<uint8_t *>(pixels);
    cached_dark_mode = dark;
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
    if (s_player_art_pixels && std::strcmp(s_player_art_path, track.path) == 0 &&
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
    copy_ui_text(s_player_art_path, sizeof(s_player_art_path), track.path);
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

void make_artwork_contents(lv_obj_t *art, int width, int height,
                           const lyra::media::Track &track, int radius,
                           bool preserve_aspect)
{
    if (!art) return;
    make_artwork_placeholder(art, width, height);
    lv_obj_t *image = lv_image_create(art);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, preserve_aspect ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_radius(image, s_neon_hud ? 0 : radius, 0);
    if (load_player_art(track)) {
        lv_image_set_src(image, &s_player_art_descriptor);
    } else {
        lyra::media::request_artwork(track, lyra::media::ArtworkSize::Player);
    }
}

lv_obj_t *make_artwork(lv_obj_t *parent, int x, int y, int width, int height,
                       const lyra::media::Track &track, int radius,
                       bool preserve_aspect)
{
    lv_obj_t *art = make_box(parent, x, y, width, height, kArtworkSurface, radius);
    make_artwork_contents(art, width, height, track, radius, preserve_aspect);
    return art;
}

} // namespace lyra::gui::internal
