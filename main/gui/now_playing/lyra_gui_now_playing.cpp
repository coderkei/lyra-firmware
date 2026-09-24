/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void update_player_progress();

namespace {

constexpr size_t kMaximumLyricsRows = 256;
constexpr uint32_t kUntimedLyric = UINT32_MAX;
constexpr int32_t kFlipScale = 256;

struct LyricsRow {
    uint32_t time_ms;
    const char *text;
    lv_obj_t *label;
};

enum class FlipPhase : uint8_t { None, ToOpenEdge, FromOpenEdge, ToCloseEdge, FromCloseEdge };

lv_obj_t *s_player_art_face;
lv_obj_t *s_player_lyrics_scroll;
lyra::media::Track s_player_lyrics_track{};
int s_player_art_size;
bool s_player_lyrics_visible;
bool s_player_flip_animating;
FlipPhase s_player_flip_phase;
char *s_player_lyrics_buffer;
lyra::media::LyricsSource s_player_lyrics_source;
LyricsRow s_player_lyrics_rows[kMaximumLyricsRows]{};
size_t s_player_lyrics_row_count;
int s_player_current_lyric = -1;
int64_t s_player_manual_scroll_until_us;

void player_art_click_cb(lv_event_t *event);
void player_lyrics_close_cb(lv_event_t *event);
void player_art_flip_exec(void *object, int32_t scale);
void player_art_flip_finished(lv_anim_t *animation);

bool parse_lrc_timestamp(const char *text, size_t length, size_t *consumed,
                         uint32_t *time_ms)
{
    if (!text || length < 6 || text[0] != '[' || !consumed || !time_ms) return false;
    size_t cursor = 1;
    uint64_t minutes = 0;
    size_t minute_digits = 0;
    while (cursor < length && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
        minutes = std::min<uint64_t>(minutes * 10 + (text[cursor] - '0'), UINT32_MAX);
        ++cursor;
        ++minute_digits;
    }
    if (minute_digits == 0 || cursor >= length || text[cursor++] != ':') return false;
    if (cursor + 2 > length || !std::isdigit(static_cast<unsigned char>(text[cursor])) ||
        !std::isdigit(static_cast<unsigned char>(text[cursor + 1]))) return false;
    const uint32_t seconds = static_cast<uint32_t>((text[cursor] - '0') * 10 +
                                                    (text[cursor + 1] - '0'));
    cursor += 2;
    if (seconds >= 60) return false;
    uint32_t fraction_ms = 0;
    if (cursor < length && (text[cursor] == '.' || text[cursor] == ':')) {
        ++cursor;
        const size_t fraction_start = cursor;
        uint32_t fraction = 0;
        while (cursor < length && std::isdigit(static_cast<unsigned char>(text[cursor])) &&
               cursor - fraction_start < 3) {
            fraction = fraction * 10 + static_cast<uint32_t>(text[cursor] - '0');
            ++cursor;
        }
        const size_t digits = cursor - fraction_start;
        if (digits == 0) return false;
        fraction_ms = digits == 1 ? fraction * 100 : digits == 2 ? fraction * 10 : fraction;
        while (cursor < length && std::isdigit(static_cast<unsigned char>(text[cursor]))) ++cursor;
    }
    if (cursor >= length || text[cursor] != ']') return false;
    *consumed = cursor + 1;
    const uint64_t total = minutes * 60000u + seconds * 1000u + fraction_ms;
    *time_ms = static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX - 1));
    return true;
}

bool parse_lrc_offset(const char *line, int32_t *offset_ms)
{
    if (!line || !offset_ms || std::strncmp(line, "[offset:", 8) != 0) return false;
    const char *value = line + 8;
    char *end = nullptr;
    const long offset = std::strtol(value, &end, 10);
    if (end == value || !end || *end != ']') return false;
    *offset_ms = static_cast<int32_t>(std::clamp<long>(offset, -600000, 600000));
    return true;
}

bool is_lrc_metadata_line(const char *line)
{
    if (!line || line[0] != '[') return false;
    const char *colon = std::strchr(line, ':');
    const char *close = std::strchr(line, ']');
    return colon && close && colon < close;
}

size_t parse_player_lyrics(char *text, bool parse_timestamps)
{
    s_player_lyrics_row_count = 0;
    s_player_current_lyric = -1;
    if (!text || !text[0]) return 0;
    int32_t offset_ms = 0;
    char *line = text;
    while (line && *line) {
        char *next = std::strchr(line, '\n');
        if (next) *next++ = '\0';
        while (*line && std::isspace(static_cast<unsigned char>(*line))) ++line;
        size_t line_length = std::strlen(line);
        while (line_length > 0 && std::isspace(static_cast<unsigned char>(line[line_length - 1]))) {
            line[--line_length] = '\0';
        }
        if (!line_length) {
            line = next;
            continue;
        }
        if (parse_timestamps && parse_lrc_offset(line, &offset_ms)) {
            line = next;
            continue;
        }

        uint32_t timestamps[16]{};
        size_t timestamp_count = 0;
        size_t cursor = 0;
        while (parse_timestamps && timestamp_count < sizeof(timestamps) / sizeof(timestamps[0])) {
            size_t consumed = 0;
            uint32_t timestamp = 0;
            if (!parse_lrc_timestamp(line + cursor, line_length - cursor,
                                     &consumed, &timestamp)) break;
            timestamps[timestamp_count++] = timestamp;
            cursor += consumed;
        }
        if (timestamp_count > 0) {
            char *lyric_text = line + cursor;
            while (*lyric_text && std::isspace(static_cast<unsigned char>(*lyric_text))) ++lyric_text;
            if (*lyric_text) {
                for (size_t i = 0; i < timestamp_count &&
                     s_player_lyrics_row_count < kMaximumLyricsRows; ++i) {
                    const int64_t adjusted = static_cast<int64_t>(timestamps[i]) + offset_ms;
                    const uint32_t time_ms = static_cast<uint32_t>(std::clamp<int64_t>(
                        adjusted, 0, UINT32_MAX - 1));
                    s_player_lyrics_rows[s_player_lyrics_row_count++] = {time_ms, lyric_text, nullptr};
                }
            }
        } else if (!parse_timestamps || !is_lrc_metadata_line(line)) {
            if (s_player_lyrics_row_count < kMaximumLyricsRows) {
                s_player_lyrics_rows[s_player_lyrics_row_count++] = {
                    kUntimedLyric, line, nullptr};
            }
        }
        line = next;
    }
    return s_player_lyrics_row_count;
}

void player_lyrics_scroll_begin_cb(lv_event_t *)
{
    s_player_manual_scroll_until_us = esp_timer_get_time() + 3000000;
}

void set_player_lyric_style(LyricsRow &row, bool active)
{
    if (!row.label) return;
    lv_obj_set_style_text_color(row.label, active ? kTextPrimary : kTextSecondary, 0);
    lv_obj_set_style_bg_color(row.label, kAccentSurface, 0);
    lv_obj_set_style_bg_opa(row.label, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void update_player_lyrics()
{
    if (!s_player_lyrics_visible || !s_player_lyrics_scroll || s_player_lyrics_row_count == 0) return;
    const uint32_t position_ms = lyra::audio::status().position_ms;
    int current = -1;
    uint32_t current_time = 0;
    for (size_t i = 0; i < s_player_lyrics_row_count; ++i) {
        const uint32_t time_ms = s_player_lyrics_rows[i].time_ms;
        if (time_ms != kUntimedLyric && time_ms <= position_ms &&
            (current < 0 || time_ms >= current_time)) {
            current = static_cast<int>(i);
            current_time = time_ms;
        }
    }
    if (current == s_player_current_lyric) return;
    if (s_player_current_lyric >= 0 &&
        static_cast<size_t>(s_player_current_lyric) < s_player_lyrics_row_count) {
        set_player_lyric_style(s_player_lyrics_rows[s_player_current_lyric], false);
    }
    s_player_current_lyric = current;
    if (current >= 0) {
        LyricsRow &row = s_player_lyrics_rows[current];
        set_player_lyric_style(row, true);
        if (esp_timer_get_time() >= s_player_manual_scroll_until_us && row.label) {
            lv_obj_scroll_to_view(row.label, LV_ANIM_OFF);
        }
    }
}

void build_player_lyrics_face()
{
    lv_obj_t *face = s_player_art_face;
    if (!face) return;
    lv_obj_clean(face);
    lv_obj_set_style_bg_color(face, kArtworkSurface, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face, 0, 0);
    lv_obj_set_style_radius(face, 13, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_event_cb(face, player_art_click_cb);

    lv_obj_t *title = make_label(face, tr(lyra::i18n::StringId::Lyrics), kTextPrimary);
    lv_obj_set_width(title, s_player_art_size - 62);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_t *close = make_button(face, s_player_art_size - 42, 4, 36, 36,
                                  kSurfaceRaised, 18);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextPrimary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, player_lyrics_close_cb, LV_EVENT_CLICKED, nullptr);

    const int scroll_width = s_player_art_size - 16;
    const int scroll_height = s_player_art_size - 50;
    lv_obj_t *scroll = lv_obj_create(face);
    lv_obj_set_pos(scroll, 8, 42);
    lv_obj_set_size(scroll, scroll_width, scroll_height);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 4, 0);
    lv_obj_set_style_radius(scroll, 6, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(scroll, player_lyrics_scroll_begin_cb,
                        LV_EVENT_SCROLL_BEGIN, nullptr);
    s_player_lyrics_scroll = scroll;

    const bool parse_timestamps = s_player_lyrics_source == lyra::media::LyricsSource::Lrc ||
                                  s_player_lyrics_source == lyra::media::LyricsSource::Embedded;
    const size_t row_count = s_player_lyrics_buffer ?
        parse_player_lyrics(s_player_lyrics_buffer, parse_timestamps) : 0;
    if (s_player_lyrics_source == lyra::media::LyricsSource::None || row_count == 0) {
        lv_obj_t *plain = make_label(scroll,
                                     tr(lyra::i18n::StringId::NoLyricsFound),
                                     kTextPrimary);
        lv_obj_set_width(plain, scroll_width - 16);
        lv_label_set_long_mode(plain, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(plain, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(plain, 4, 4);
    } else {
        int y = 2;
        for (size_t i = 0; i < row_count; ++i) {
            const char *text = s_player_lyrics_rows[i].text;
            lv_obj_t *row = make_label(scroll, text ? text : "", kTextSecondary);
            lv_obj_set_width(row, scroll_width - 16);
            lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(row, 6, 0);
            lv_obj_set_style_pad_hor(row, 5, 0);
            lv_obj_set_style_pad_ver(row, 7, 0);
            lv_obj_set_pos(row, 4, y);
            lv_obj_update_layout(row);
            int row_height = lv_obj_get_height(row);
            if (row_height < 34) {
                row_height = 34;
                lv_obj_set_height(row, row_height);
            }
            y += row_height + 3;
            s_player_lyrics_rows[i].label = row;
        }
        lv_obj_update_layout(scroll);
    }
    if (s_player_lyrics_buffer) {
        heap_caps_free(s_player_lyrics_buffer);
        s_player_lyrics_buffer = nullptr;
    }
    s_player_lyrics_visible = true;
    s_player_manual_scroll_until_us = 0;
    update_player_lyrics();
}

void restore_player_art_face()
{
    lv_obj_t *face = s_player_art_face;
    if (!face) return;
    lv_obj_clean(face);
    lv_obj_set_style_bg_color(face, kArtworkSurface, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face, 0, 0);
    lv_obj_set_style_radius(face, 13, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    make_artwork(face, 0, 0, s_player_art_size, s_player_art_size,
                 s_player_lyrics_track, 13);
    lv_obj_add_event_cb(face, player_art_click_cb, LV_EVENT_CLICKED, nullptr);
    s_player_lyrics_scroll = nullptr;
    s_player_lyrics_row_count = 0;
    s_player_current_lyric = -1;
    s_player_lyrics_visible = false;
}

void player_art_flip_exec(void *object, int32_t scale)
{
    lv_obj_t *face = static_cast<lv_obj_t *>(object);
    if (face) lv_obj_set_style_transform_scale_x(face, scale, 0);
}

void start_player_art_flip(FlipPhase phase, int32_t from, int32_t to)
{
    if (!s_player_art_face) return;
    s_player_flip_phase = phase;
    s_player_flip_animating = true;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_player_art_face);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&animation, player_art_flip_exec);
    lv_anim_set_completed_cb(&animation, player_art_flip_finished);
    lv_anim_start(&animation);
}

void player_art_flip_finished(lv_anim_t *)
{
    switch (s_player_flip_phase) {
        case FlipPhase::ToOpenEdge:
            build_player_lyrics_face();
            s_player_flip_phase = FlipPhase::FromOpenEdge;
            start_player_art_flip(FlipPhase::FromOpenEdge, 0, kFlipScale);
            break;
        case FlipPhase::ToCloseEdge:
            restore_player_art_face();
            s_player_flip_phase = FlipPhase::FromCloseEdge;
            start_player_art_flip(FlipPhase::FromCloseEdge, 0, kFlipScale);
            break;
        case FlipPhase::FromOpenEdge:
        case FlipPhase::FromCloseEdge:
            s_player_flip_animating = false;
            s_player_flip_phase = FlipPhase::None;
            break;
        case FlipPhase::None:
            s_player_flip_animating = false;
            break;
    }
}

void player_art_click_cb(lv_event_t *)
{
    if (!s_player_art_face || s_player_lyrics_visible || s_player_flip_animating) return;
    s_player_lyrics_buffer = static_cast<char *>(heap_caps_malloc(
        lyra::media::kMaxLyricsBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_player_lyrics_buffer) {
        s_player_lyrics_buffer = static_cast<char *>(std::malloc(
            lyra::media::kMaxLyricsBytes + 1));
    }
    if (!s_player_lyrics_buffer) return;
    s_player_lyrics_buffer[0] = '\0';
    if (!lyra::media::load_lyrics(s_player_lyrics_track, s_player_lyrics_buffer,
                                  lyra::media::kMaxLyricsBytes + 1,
                                  &s_player_lyrics_source)) {
        s_player_lyrics_source = lyra::media::LyricsSource::None;
    }
    start_player_art_flip(FlipPhase::ToOpenEdge, kFlipScale, 0);
}

void player_lyrics_close_cb(lv_event_t *)
{
    if (!s_player_art_face || !s_player_lyrics_visible || s_player_flip_animating) return;
    s_player_flip_animating = true;
    lv_obj_remove_event_cb(s_player_art_face, player_art_click_cb);
    start_player_art_flip(FlipPhase::ToCloseEdge, kFlipScale, 0);
}

} // namespace

void reset_player_lyrics_state()
{
    if (s_player_art_face) lv_anim_delete(s_player_art_face, nullptr);
    if (s_player_lyrics_buffer) {
        heap_caps_free(s_player_lyrics_buffer);
        s_player_lyrics_buffer = nullptr;
    }
    s_player_art_face = nullptr;
    s_player_lyrics_scroll = nullptr;
    s_player_lyrics_source = lyra::media::LyricsSource::None;
    s_player_lyrics_row_count = 0;
    s_player_current_lyric = -1;
    s_player_manual_scroll_until_us = 0;
    s_player_lyrics_visible = false;
    s_player_flip_animating = false;
    s_player_flip_phase = FlipPhase::None;
}

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
    update_player_lyrics();
}

void render_player()
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
    s_player_art_size = art_size;
    s_player_lyrics_track = track;
    s_player_art_face = make_artwork(body, art_x, art_y, art_size, art_size, track, 13);
    lv_obj_add_flag(s_player_art_face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(s_player_art_face, art_size / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_player_art_face, art_size / 2, 0);
    lv_obj_add_event_cb(s_player_art_face, player_art_click_cb, LV_EVENT_CLICKED, nullptr);

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
