/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {

namespace {

lv_obj_t *s_status_wifi_signal_bars[kWifiSignalBarCount]{};
lv_timer_t *s_status_wifi_signal_timer;
int s_status_wifi_signal_shown = -1;

// The status bar lies over artwork on Now Playing, where it is always white.
bool status_bar_over_art()
{
    return s_view == View::Player;
}

lv_color_t status_wifi_signal_color()
{
    return status_bar_over_art() ? lv_color_white() : kTextSecondary;
}

lv_color_t status_wifi_signal_inactive_color()
{
    return status_bar_over_art() ? lv_color_hex(0x8A8A8A) : kDivider;
}

lv_obj_t *s_status_bar;
lv_obj_t *s_status_play_icon;

void refresh_status_play_icon()
{
    if (!s_status_play_icon) return;
    const lyra::audio::Status audio = lyra::audio::status();
    if (!audio.playing || audio.eof) {
        lv_obj_add_flag(s_status_play_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_status_play_icon, LV_OBJ_FLAG_HIDDEN);
    set_label_text_if_changed(s_status_play_icon, audio.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void refresh_status_wifi_signal()
{
    if (!s_status_wifi_signal_bars[0]) return;
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const int active_bars = wifi.connected && wifi.has_connected_rssi ?
        wifi_signal_bar_count(wifi.connected_rssi) : 0;
    // Setting a style invalidates the bar even when the value is unchanged;
    // skip the once-a-second status-bar redraw unless the signal moved.
    if (active_bars == s_status_wifi_signal_shown) return;
    s_status_wifi_signal_shown = active_bars;
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

void status_bar_timer_cb(lv_timer_t *)
{
    update_status_time_label();
    refresh_status_wifi_signal();
    refresh_status_play_icon();
}

lv_obj_t *s_toast;
lv_timer_t *s_toast_timer;
constexpr uint32_t kToastDurationMs = 1800;

void toast_timer_cb(lv_timer_t *)
{
    s_toast_timer = nullptr; // A repeat count of one deletes the timer itself.
    if (s_toast) lv_obj_delete(s_toast);
}

void toast_delete_cb(lv_event_t *)
{
    s_toast = nullptr;
    if (s_toast_timer) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = nullptr;
    }
}

void header_title_cb(lv_event_t *)
{
    if (s_scroll_body) lv_obj_scroll_to_y(s_scroll_body, 0, LV_ANIM_ON);
}

void status_bar_delete_cb(lv_event_t *)
{
    s_status_bar = nullptr;
    s_status_play_icon = nullptr;
    for (lv_obj_t *&signal_bar : s_status_wifi_signal_bars) signal_bar = nullptr;
    s_status_wifi_signal_shown = -1;
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
        // Taps pass through to whatever the bars sit on (the status bar).
        lv_obj_remove_flag(signal_bar, LV_OBJ_FLAG_CLICKABLE);
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

void make_artwork_placeholder(lv_obj_t *art, int width, int height)
{
    if (!art || width <= 0 || height <= 0) return;
    const int size = std::min(width, height);
    lv_obj_set_style_bg_color(art, kArtworkSurface, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    if (size < 48) {
        lv_obj_t *note = make_icon(art, Icon::Music, kTextMuted, std::max(12, size * 55 / 100));
        lv_obj_center(note);
        return;
    }
    // The Music tile badge, scaled to the cover: the same mark everywhere a
    // song has no artwork of its own.
    const int badge = size * 34 / 100;
    const int footprint = badge + badge / 3;
    make_tile_badge(art, (width - footprint) / 2, (height - footprint) / 2, badge,
                    Icon::Music, 0xF2B320);
}

void make_status_bar()
{
    const bool over_art = status_bar_over_art();
    lv_obj_t *bar = make_box(s_screen, 0, 0, kScreenWidth, kStatusHeight, kBackground);
    s_status_bar = bar;
    if (over_art) {
        // A soft scrim keeps white status text readable on any cover.
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_opa(bar, LV_OPA_60, 0);
        lv_obj_set_style_bg_grad_opa(bar, LV_OPA_TRANSP, 0);
    }
    const lv_color_t primary = over_art ? lv_color_white() : kTextPrimary;
    const lv_color_t secondary = over_art ? lv_color_white() : kTextSecondary;
    // Pulling the bar down draws out the control centre; a tap does nothing,
    // so a near miss on Back never opens it. Page swipes do not start here.
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    control_centre_pull_events(bar);

    const lyra::audio::Status audio_status = lyra::audio::status();
    s_status_volume_label = make_label(bar, "", secondary);
    update_status_volume_label(audio_status.volume_percent);
    s_status_play_icon = make_label(bar, LV_SYMBOL_PLAY, secondary);
    refresh_status_play_icon();
    if (s_clock_position != ClockPosition::Hidden) {
        s_status_time_label = make_label(bar, "", primary);
        update_status_time_label();
    }

    // Battery ADC calibration is not yet available on the reference board, so
    // this status-bar value is a UI placeholder rather than a voltage reading.
    lv_obj_t *battery = make_label(bar, "100% " LV_SYMBOL_BATTERY_FULL, secondary);
    lv_obj_align(battery, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_update_layout(bar);
    const int wifi_x = lv_obj_get_x(battery) - 24;
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const int active_wifi_bars = wifi.connected && wifi.has_connected_rssi ?
        wifi_signal_bar_count(wifi.connected_rssi) : 0;
    make_wifi_signal_bars(bar, wifi_x, 7, active_wifi_bars,
                          status_wifi_signal_color(), status_wifi_signal_inactive_color(),
                          s_status_wifi_signal_bars);

    // Volume and play state lead on the left unless the clock takes that place.
    int left_x = 10;
    if (s_status_time_label && s_clock_position == ClockPosition::Left) {
        lv_obj_align(s_status_time_label, LV_ALIGN_LEFT_MID, left_x, 0);
        lv_obj_update_layout(bar);
        left_x += lv_obj_get_width(s_status_time_label) + 12;
    }
    lv_obj_align(s_status_volume_label, LV_ALIGN_LEFT_MID, left_x, 0);
    lv_obj_update_layout(bar);
    lv_obj_align_to(s_status_play_icon, s_status_volume_label, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    if (s_status_time_label && s_clock_position == ClockPosition::Centre) {
        lv_obj_align(s_status_time_label, LV_ALIGN_CENTER, 0, 0);
    } else if (s_status_time_label && s_clock_position == ClockPosition::Right) {
        lv_obj_align(s_status_time_label, LV_ALIGN_RIGHT_MID, wifi_x - 12 - kScreenWidth, 0);
    }

    lv_obj_add_event_cb(bar, status_bar_delete_cb, LV_EVENT_DELETE, nullptr);
    if (!s_status_wifi_signal_timer) {
        s_status_wifi_signal_timer = lv_timer_create(status_bar_timer_cb, 1000, nullptr);
    }
    refresh_status_wifi_signal();
}

void raise_status_bar()
{
    if (s_status_bar) lv_obj_move_foreground(s_status_bar);
}

lv_obj_t *make_header(const char *title, View back, bool show_back, const char *right,
                      lv_event_cb_t custom_back, int action_width)
{
    const bool queue_add_toggle = !s_playlist_add_mode &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::AlbumDetail || s_view == View::ArtistDetail ||
         s_view == View::TrackList || s_view == View::Folders ||
         s_view == View::FolderDetail || s_view == View::FolderPlaylist ||
         s_view == View::PlaylistDetail);
    lv_obj_t *header = make_box(s_screen, 0, kStatusHeight, kScreenWidth, kHeaderHeight,
                                kBackground);
    if (show_back) {
        lv_obj_t *back_button = make_symbol_button(header, 2, 2, 40, LV_SYMBOL_LEFT, kTextPrimary);
        if (custom_back) {
            lv_obj_add_event_cb(back_button, custom_back, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_add_event_cb(back_button, [](lv_event_t *event) {
                const View fallback = static_cast<View>(
                    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
                navigate_back(fallback);
            }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(back)));
        }
    }
    lv_obj_t *label = make_label(header, title, kTextPrimary);
    lv_obj_set_style_text_font(label, title_font(), 0);
    const bool show_right_label = right != nullptr && (!queue_add_toggle || right[0] != '\0');
    const int title_x = show_back ? 44 : 16;
    const int right_reserve = std::max(queue_add_toggle ? (show_right_label ? 120 : 52) :
                                       right ? 96 : 12, action_width + 8);
    make_marquee(label, kScreenWidth - title_x - right_reserve, MarqueeMode::Focus);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, title_x, 0);
    // Tapping a page title returns a long list to its first row.
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(label, 6);
    lv_obj_add_event_cb(label, header_title_cb, LV_EVENT_CLICKED, nullptr);
    if (show_right_label) {
        lv_obj_t *right_label = make_label(header, right, kTextMuted);
        make_marquee(right_label, queue_add_toggle ? 64 : 84);
        lv_obj_set_style_text_align(right_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(right_label, LV_ALIGN_RIGHT_MID, queue_add_toggle ? -52 : -14, 0);
    }
    if (queue_add_toggle) make_queue_add_mode_toggle(header, 274, 2, 40, 40);
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
        s_queue_add_mode ? kAccent : kSurfaceRaised, LV_RADIUS_CIRCLE);
    if (!s_queue_add_mode) lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_t *icon = make_icon(button, Icon::Queue, s_queue_add_mode ? kTextOnAccent : kTextPrimary);
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
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    constexpr int kMinimumHeight = 180;
    constexpr int kButtonHeight = 44;
    constexpr int kDialogWidth = 280;
    constexpr int kTextWidth = kDialogWidth - 40;
    lv_obj_t *dialog = make_box(overlay, (kScreenWidth - kDialogWidth) / 2, 150, kDialogWidth,
                                kMinimumHeight, kSurface, 20);
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_set_style_text_font(title, title_font(), 0);
    lv_obj_set_width(title, kTextWidth);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *message = make_label(dialog, message_text, kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, kTextWidth);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    // Grow with the wrapped text so longer explanations and translations never
    // run into the title or the button.
    lv_obj_update_layout(dialog);
    const int title_bottom = 22 + lv_obj_get_height(title);
    const int message_height = lv_obj_get_height(message);
    const int height = std::clamp(title_bottom + 12 + message_height + 22 + kButtonHeight + 18,
                                  kMinimumHeight, kScreenHeight - 32);
    const int button_y = height - kButtonHeight - 18;
    lv_obj_set_height(dialog, height);
    lv_obj_set_y(dialog, (kScreenHeight - height) / 2);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0,
                 title_bottom + std::max(12, (button_y - title_bottom - message_height) / 2));
    lv_obj_t *close = make_button(dialog, 20, button_y, kDialogWidth - 40, kButtonHeight,
                                  kAccent, LV_RADIUS_CIRCLE);
    lv_obj_t *close_label = make_label(close, tr(lyra::i18n::StringId::Ok), kTextOnAccent);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
}

void show_toast(const char *text)
{
    if (!s_screen || !text || !text[0]) return;
    if (s_toast) lv_obj_delete(s_toast);
    // A brief, non-modal confirmation. It ignores touches so the page under
    // it stays usable, and render() removes it along with the old screen.
    s_toast = make_box(s_screen, 30, content_bottom() - 56, kScreenWidth - 60, 40,
                       kSurfaceRaised, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = make_label(s_toast, text, kTextPrimary);
    make_marquee(label, kScreenWidth - 92);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(s_toast, toast_delete_cb, LV_EVENT_DELETE, nullptr);
    s_toast_timer = lv_timer_create(toast_timer_cb, kToastDurationMs, nullptr);
    if (s_toast_timer) lv_timer_set_repeat_count(s_toast_timer, 1);
}

static lv_obj_t *make_row_with(lv_obj_t *parent, int y, const char *icon_text, const Icon *icon,
                               const char *title, const char *subtitle, View target, int height)
{
    constexpr int kRowWidth = kScreenWidth - 2 * kPageInset;
    lv_obj_t *row = make_button(parent, kPageInset, y, kRowWidth, height, kSurface, kRowRadius);
    const bool has_icon = icon_text || icon;
    const int text_x = has_icon ? 48 : 16;
    if (icon) {
        lv_obj_t *glyph = make_icon(row, *icon, kAccent);
        lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 14, 0);
    } else if (icon_text) {
        lv_obj_t *glyph = make_label(row, icon_text, kAccent);
        lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 16, 0);
    }
    const int text_width = kRowWidth - text_x - 32;
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    make_marquee(title_label, text_width);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, text_x, subtitle == nullptr ? 0 : -10);
    if (subtitle != nullptr) {
        lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
        make_marquee(sub, text_width);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, text_x, 11);
    }
    lv_obj_t *chevron = make_label(row, LV_SYMBOL_RIGHT, kTextMuted);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -14, 0);
    add_route(row, target);
    return row;
}

lv_obj_t *make_row(lv_obj_t *parent, int y, const char *icon_text, const char *title,
                   const char *subtitle, View target, int height)
{
    return make_row_with(parent, y, icon_text, nullptr, title, subtitle, target, height);
}

lv_obj_t *make_row(lv_obj_t *parent, int y, Icon icon, const char *title,
                   const char *subtitle, View target, int height)
{
    return make_row_with(parent, y, nullptr, &icon, title, subtitle, target, height);
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
    const bool dark = s_dark_mode;
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

    if (s_player_art_pixels) lv_image_cache_drop(&s_player_art_descriptor);
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

lv_obj_t *make_artwork_contents(lv_obj_t *art, int width, int height,
                                const lyra::media::Track &track, int radius,
                                bool preserve_aspect)
{
    if (!art) return nullptr;
    make_artwork_placeholder(art, width, height);
    lv_obj_t *image = lv_image_create(art);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_size(image, width, height);
    lv_image_set_inner_align(image, preserve_aspect ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_style_radius(image, radius, 0);
    if (load_player_art(track)) {
        lv_image_set_src(image, &s_player_art_descriptor);
    } else {
        lyra::media::request_artwork(track, lyra::media::ArtworkSize::Player);
    }
    return image;
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
