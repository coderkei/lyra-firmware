/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void update_player_progress();

void update_player_progress_touch_value(lv_obj_t *touch)
{
    if (!touch || !lv_indev_active()) return;
    lv_area_t area;
    lv_obj_get_coords(touch, &area);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const int32_t width = std::max<int32_t>(1, area.x2 - area.x1);
    const int32_t x = std::max<int32_t>(0, std::min<int32_t>(point.x - area.x1, width));
    s_player_progress_drag_value = (x * 1000) / width;
}

void update_player_progress_preview()
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (audio_status.duration_ms == 0) return;
    if (s_player_progress_bar) {
        lv_bar_set_value(s_player_progress_bar, s_player_progress_drag_value, LV_ANIM_OFF);
    }
    const uint32_t preview_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(s_player_progress_drag_value) * audio_status.duration_ms) / 1000u);
    char elapsed[16];
    format_playback_time(preview_ms, false, elapsed, sizeof(elapsed));
    if (s_player_elapsed_label) lv_label_set_text(s_player_elapsed_label, elapsed);
}

void player_progress_touch_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *touch = lv_event_get_current_target_obj(event);
    if (code == LV_EVENT_PRESSED) {
        s_player_progress_dragging = true;
        update_player_progress_touch_value(touch);
        update_player_progress_preview();
        return;
    }

    if (code == LV_EVENT_PRESSING && s_player_progress_dragging) {
        update_player_progress_touch_value(touch);
        update_player_progress_preview();
        return;
    }

    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (!s_player_progress_dragging) return;
    update_player_progress_touch_value(touch);
    const lyra::audio::Status audio_status = lyra::audio::status();
    s_player_progress_dragging = false;
    if (audio_status.duration_ms == 0) return;
    const uint32_t target_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(s_player_progress_drag_value) * audio_status.duration_ms) / 1000u);
    const esp_err_t seek_ret = lyra::audio::seek(target_ms);
    if (seek_ret != ESP_OK) {
        ESP_LOGW(kTag, "could not seek playback: %s", esp_err_to_name(seek_ret));
    }
    update_player_progress();
}

lv_obj_t *make_player_progress_touch(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *touch = lv_obj_create(parent);
    lv_obj_set_pos(touch, x, y);
    lv_obj_set_size(touch, width, height);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(touch, 0, 0);
    lv_obj_set_style_pad_all(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, player_progress_touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(touch, player_progress_touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(touch, player_progress_touch_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(touch, player_progress_touch_cb, LV_EVENT_PRESS_LOST, nullptr);
    return touch;
}

void update_player_progress()
{
    if (!s_player_progress_bar && !s_player_elapsed_label && !s_player_duration_label) return;

    const lyra::audio::Status audio_status = lyra::audio::status();
    const uint32_t duration_ms = audio_status.duration_ms;
    const uint32_t position_ms = duration_ms > 0 && audio_status.position_ms > duration_ms ?
                                 duration_ms : audio_status.position_ms;
    if (s_player_progress_dragging) return;
    if (s_player_progress_bar) {
        const int progress = duration_ms > 0 ? static_cast<int>(
            (static_cast<uint64_t>(position_ms) * 1000u) / duration_ms) : 0;
        lv_bar_set_value(s_player_progress_bar, std::min(progress, 1000), LV_ANIM_OFF);
    }
    char elapsed[16];
    char duration[16];
    format_playback_time(position_ms, false, elapsed, sizeof(elapsed));
    format_playback_time(duration_ms, duration_ms == 0, duration, sizeof(duration));
    if (s_player_elapsed_label) lv_label_set_text(s_player_elapsed_label, elapsed);
    if (s_player_duration_label) lv_label_set_text(s_player_duration_label, duration);
}

void render_player(bool fullscreen)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        make_header(tr(lyra::i18n::StringId::NowPlaying), View::Menu, true);
        lv_obj_t *empty = make_label(s_screen, tr(lyra::i18n::StringId::NoTrackSelectedAndScan), kTextMuted);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
        return;
    }
    const bool favorite = lyra::media::is_favorite(s_current_track);
    const int body_height = content_height(kStatusHeight);
    if (fullscreen) {
        lv_obj_t *art = make_box(s_screen, 0, kStatusHeight, 320, body_height, kPlayerArtSurface);
        // Keep the square cover flush with the status bar instead of centering
        // it in the taller fullscreen content area.
        make_album_art(art, 0, 0, kScreenWidth, kScreenWidth, track, true);
        lv_obj_t *exit_hit = make_button(art, 0, 0, 320, body_height, kBackground, 0, false);
        lv_obj_set_style_bg_opa(exit_hit, LV_OPA_TRANSP, 0);
        add_route(exit_hit, View::Player);
        lv_obj_t *overlay = make_box(art, 0, body_height - 132, 320, 132, kOverlay);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
        lv_obj_t *title = make_label(overlay, track.title, kTextOnOverlayPrimary);
        lv_obj_set_style_text_font(title, lyra::font::ui(), 0);
        make_marquee(title, 250);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 20);
        lv_obj_t *artist = make_label(overlay, track.artist, kTextOnOverlaySecondary);
        make_marquee(artist, 280);
        lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 10, 43);
        lv_obj_t *heart = make_label(overlay, favorite ? kHeartFilled : kHeartOutline,
                                     favorite ? kAccent : kTextOnOverlayPrimary);
        lv_obj_align(heart, LV_ALIGN_TOP_RIGHT, -13, 24);
        lv_obj_t *bar = lv_bar_create(overlay);
        lv_obj_set_size(bar, 258, 5);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -32);
        lv_bar_set_range(bar, 0, 1000);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, kAccent, LV_PART_INDICATOR);
        s_player_progress_bar = bar;
        s_player_progress_touch = make_player_progress_touch(overlay, 31, 87, 258, 21);
        s_player_elapsed_label = make_label(overlay, "00:00", kTextOnOverlaySecondary);
        lv_obj_align(s_player_elapsed_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
        s_player_duration_label = make_label(overlay, "--:--", kTextOnOverlaySecondary);
        lv_obj_align(s_player_duration_label, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
        update_player_progress();
        return;
    }

    lv_obj_t *body = make_box(s_screen, 0, kStatusHeight, 320, body_height, kBackground);
    const size_t queue_count = s_has_active_queue ? playback_queue_count() : 0;
    size_t queue_position = 0;
    const bool queue_position_valid = queue_count > 0 && current_queue_position(&queue_position);
    if (queue_position_valid) {
        char position_text[24];
        format_u32_u32(lyra::i18n::StringId::QueuePosition,
                       static_cast<uint32_t>(queue_position + 1),
                       static_cast<uint32_t>(queue_count),
                       position_text, sizeof(position_text));
        lv_obj_t *position = make_label(body, position_text, kTextSecondary);
        lv_obj_set_width(position, kScreenWidth);
        lv_obj_set_style_text_align(position, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(position, LV_ALIGN_TOP_MID, 0, 0);
    }
    const int art_size = s_show_nav ? 224 : 272;
    const int art_x = (kScreenWidth - art_size) / 2;
    const int controls_y = body_height - 54;
    const int progress_y = controls_y - 19;
    const int art_y = 20;
    make_album_art(body, art_x, art_y, art_size, art_size, track);
    lv_obj_t *art_hit = make_button(body, art_x, art_y, art_size, art_size, kBackground, 13, false);
    lv_obj_set_style_bg_opa(art_hit, LV_OPA_TRANSP, 0);
    add_route(art_hit, View::FullscreenArt);

    lv_obj_t *title = make_label(body, track.title, kTextPrimary);
    lv_obj_set_style_text_font(title, lyra::font::ui(), 0);
    make_marquee(title, 240);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, progress_y - 79);
    lv_obj_t *artist = make_label(body, track.artist, kTextSecondary);
    make_marquee(artist, 240);
    lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 20, progress_y - 56);
    lv_obj_t *album = make_label(body, track.album, kTextMuted);
    make_marquee(album, 240);
    lv_obj_align(album, LV_ALIGN_TOP_LEFT, 20, progress_y - 35);

    lv_obj_t *heart_button = make_button(body, 270, progress_y - 79, 36, 36, kBackground, 18);
    lv_obj_t *heart = make_label(heart_button, favorite ? kHeartFilled : kHeartOutline,
                                 favorite ? kAccent : kTextPrimary);
    lv_obj_center(heart);
    lv_obj_add_event_cb(heart_button, toggle_favorite_cb, LV_EVENT_CLICKED, heart);

    lv_obj_t *bar = lv_bar_create(body);
    lv_obj_set_pos(bar, 56, progress_y);
    lv_obj_set_size(bar, 208, 5);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, kAccent, LV_PART_INDICATOR);
    s_player_progress_bar = bar;
    s_player_progress_touch = make_player_progress_touch(body, 56, progress_y - 8, 208, 21);
    s_player_elapsed_label = make_label(body, "00:00", kTextSecondary);
    lv_obj_set_pos(s_player_elapsed_label, 10, progress_y - 8);
    s_player_duration_label = make_label(body, "--:--", kTextSecondary);
    lv_obj_align(s_player_duration_label, LV_ALIGN_TOP_RIGHT, -10, progress_y - 8);
    update_player_progress();

    lv_obj_t *repeat = make_button(body, 16, controls_y + 4, 44, 38, kBackground, 7);
    const bool repeat_enabled = s_repeat_mode != RepeatMode::Off;
    lv_obj_t *repeat_icon = make_label(repeat, LV_SYMBOL_LOOP,
                                       repeat_enabled ? kAccent : kTextSecondary);
    lv_obj_center(repeat_icon);
    if (s_repeat_mode == RepeatMode::Song) {
        lv_obj_t *repeat_one = make_label(repeat, "1", kAccent);
        lv_obj_align(repeat_one, LV_ALIGN_CENTER, 8, 5);
        lv_obj_clear_flag(repeat_one, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(repeat, [](lv_event_t *) {
        switch (s_repeat_mode) {
            case RepeatMode::Off: s_repeat_mode = RepeatMode::All; break;
            case RepeatMode::All: s_repeat_mode = RepeatMode::Song; break;
            case RepeatMode::Song: s_repeat_mode = RepeatMode::Off; break;
        }
        if (s_repeat_mode != RepeatMode::Off && lyra::audio::status().eof) {
            s_audio_eof_seen = false;
        }
        render(s_view);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *equalizer = make_button(body, 76, controls_y + 4, 44, 38, kBackground, 7);
    make_equalizer_icon(equalizer, kTextSecondary);
    add_route(equalizer, View::Equalizer);
    lv_obj_t *playlist_button = make_button(body, 136, controls_y + 4, 44, 38,
                                            kBackground, 7);
    lv_obj_t *playlist_icon = make_label(playlist_button, LV_SYMBOL_LIST, kTextSecondary);
    lv_obj_align(playlist_icon, LV_ALIGN_CENTER, -3, 0);
    lv_obj_t *playlist_plus = make_label(playlist_button, LV_SYMBOL_PLUS, kTextSecondary);
    lv_obj_align(playlist_plus, LV_ALIGN_BOTTOM_RIGHT, -4, -1);
    lv_obj_clear_flag(playlist_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(playlist_plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(playlist_button, show_player_playlist_picker_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *info = make_button(body, 196, controls_y + 4, 44, 38, kBackground, 7);
    lv_obj_t *info_icon = make_label(info, "i", kTextSecondary);
    lv_obj_set_style_text_font(info_icon, &lv_font_montserrat_18, 0);
    lv_obj_center(info_icon);
    lv_obj_add_event_cb(info, [](lv_event_t *) {
        s_track_info_tab = TrackInfoTab::Song;
        navigate_to(View::TrackInfo);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *shuffle = make_button(body, 256, controls_y + 4, 44, 38, kBackground, 7);
    lv_obj_t *shuffle_icon = make_label(shuffle, LV_SYMBOL_SHUFFLE, s_shuffle ? kAccent : kTextSecondary);
    lv_obj_center(shuffle_icon);
    lv_obj_add_event_cb(shuffle, [](lv_event_t *) {
        s_shuffle = !s_shuffle;
        reset_shuffle_queue();
        if (s_shuffle && !build_shuffle_queue()) {
            s_shuffle = false;
            ESP_LOGW(kTag, "could not allocate shuffle queue");
        }
        if (!s_shuffle) s_queue_position_valid = false;
        if (s_shuffle && lyra::audio::status().eof) s_audio_eof_seen = false;
        render(s_view);
    }, LV_EVENT_CLICKED, shuffle_icon);
}

} // namespace lyra::gui::internal
