/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void update_player_progress();

namespace {

constexpr size_t kMaximumLyricsRows = 256;
constexpr size_t kLyricsRowsPerTick = 6;
constexpr uint32_t kLyricsRowsTickMs = 10;
constexpr uint32_t kUntimedLyric = UINT32_MAX;
constexpr int32_t kPlayerProgressMax = 1000;
constexpr int32_t kSeekHandleRadius = 6;

void player_seek_handle_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        auto *extra = static_cast<int32_t *>(lv_event_get_param(event));
        *extra = std::max(*extra, kSeekHandleRadius);
        return;
    }

    lv_obj_t *bar = lv_event_get_current_target_obj(event);
    lv_area_t rail;
    lv_obj_get_coords(bar, &rail);
    const int32_t x = rail.x1 + (rail.x2 - rail.x1) * lv_bar_get_value(bar) / kPlayerProgressMax;
    const int32_t y = (rail.y1 + rail.y2) / 2;
    const lv_area_t handle{x - kSeekHandleRadius, y - kSeekHandleRadius,
                           x + kSeekHandleRadius, y + kSeekHandleRadius};
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_white();
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = LV_RADIUS_CIRCLE;
    rect.border_width = 3;
    rect.border_color = kAccent;
    rect.border_opa = LV_OPA_COVER;
    lv_draw_rect(lv_event_get_layer(event), &rect, &handle);
}

struct LyricsRow {
    uint32_t time_ms;
    const char *text;
    lv_obj_t *label;
};

struct LyricsLoadRequest {
    uint32_t generation;
    lyra::media::Track track;
    char *buffer;
    lyra::media::LyricsSource source;
    bool loaded;
};

enum class FlipPhase : uint8_t { None, ToOpenEdge, FromOpenEdge, ToCloseEdge, FromCloseEdge };

int s_player_lyrics_width;
int s_player_lyrics_height;
lv_obj_t *s_player_flip_window;
lv_obj_t *s_player_flip_content;
lv_obj_t *s_player_cover_image;
lv_obj_t *s_player_lyrics_scroll;
lv_obj_t *s_player_lyrics_loading_spinner;
lv_timer_t *s_player_lyrics_rows_timer;
lyra::media::Track s_player_lyrics_track{};
lyra::media::Track s_player_pending_lyrics_track{};
int s_player_art_size;
bool s_player_lyrics_visible;
bool s_player_flip_animating;
bool s_player_auto_scrolling;
bool s_player_lyrics_rows_ready;
bool s_player_lyrics_rows_pending;
FlipPhase s_player_flip_phase;
char *s_player_lyrics_buffer;
lyra::media::LyricsSource s_player_lyrics_source;
LyricsRow s_player_lyrics_rows[kMaximumLyricsRows]{};
size_t s_player_lyrics_row_count;
size_t s_player_lyrics_rows_created;
int s_player_lyrics_next_row_y;
int s_player_current_lyric = -1;
int64_t s_player_manual_scroll_until_us;
uint32_t s_player_lyrics_generation;
bool s_player_seek_preview_active;
bool s_player_seek_preview_release_pending;
uint32_t s_player_seek_preview_position_ms;
uint32_t s_player_seek_preview_duration_ms;
char s_player_seek_preview_path[lyra::audio::kMaxPath];

void player_art_click_cb(lv_event_t *event);
void player_lyrics_close_cb(lv_event_t *event);
void player_art_flip_exec(void *object, int32_t scale);
void player_art_flip_finished(lv_anim_t *animation);
void start_player_lyrics_load();
void player_lyrics_load_task(void *context);
void player_lyrics_loaded_async_cb(void *context);
void player_lyrics_rows_timer_cb(lv_timer_t *timer);
void start_player_lyrics_rows_timer();

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
    if (!s_player_auto_scrolling) {
        s_player_manual_scroll_until_us = esp_timer_get_time() + 3000000;
    }
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
    if (!s_player_lyrics_visible || !s_player_lyrics_scroll ||
        !s_player_lyrics_rows_ready || s_player_lyrics_row_count == 0) return;
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
            s_player_auto_scrolling = true;
            lv_obj_scroll_to_view(row.label, LV_ANIM_OFF);
            s_player_auto_scrolling = false;
        }
    }
}

void build_player_lyrics_face()
{
    lv_obj_t *window = s_player_flip_window;
    if (!window) return;
    if (s_player_flip_content) {
        lv_obj_remove_event_cb(s_player_flip_content, player_art_click_cb);
        s_player_cover_image = nullptr;
    }
    lv_obj_clean(window);
    lv_obj_set_style_bg_color(window, kArtworkSurface, 0);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(window, 0, 0);
    lv_obj_set_style_radius(window, 0, 0);
    lv_obj_set_style_pad_all(window, 0, 0);
    lv_obj_clear_flag(window, LV_OBJ_FLAG_SCROLLABLE);
    s_player_flip_content = make_box(window, 0, 0, s_player_lyrics_width,
                                     s_player_lyrics_height, kArtworkSurface);
    lv_obj_set_style_border_width(s_player_flip_content, 0, 0);

    // The status bar lies over the top of this face.
    constexpr int kTop = kStatusHeight + 6;
    lv_obj_t *title = make_label(s_player_flip_content,
                                 tr(lyra::i18n::StringId::Lyrics), kTextPrimary);
    lv_obj_set_style_text_font(title, title_font(), 0);
    lv_obj_set_width(title, s_player_lyrics_width - 120);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(title, 60, kTop + 8);
    lv_obj_t *close = make_symbol_button(s_player_flip_content, s_player_lyrics_width - 50, kTop,
                                         40, LV_SYMBOL_CLOSE, kTextPrimary);
    lv_obj_add_event_cb(close, player_lyrics_close_cb, LV_EVENT_CLICKED, nullptr);

    const int scroll_width = s_player_lyrics_width - 16;
    const int scroll_height = s_player_lyrics_height - kTop - 50;
    lv_obj_t *scroll = lv_obj_create(s_player_flip_content);
    lv_obj_set_pos(scroll, 8, kTop + 44);
    lv_obj_set_size(scroll, scroll_width, scroll_height);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 4, 0);
    lv_obj_set_style_radius(scroll, 0, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(scroll, player_lyrics_scroll_begin_cb,
                        LV_EVENT_SCROLL_BEGIN, nullptr);
    s_player_lyrics_scroll = scroll;
    s_player_lyrics_loading_spinner = lv_spinner_create(scroll);
    lv_obj_set_size(s_player_lyrics_loading_spinner, 34, 34);
    lv_obj_center(s_player_lyrics_loading_spinner);
    lv_spinner_set_anim_params(s_player_lyrics_loading_spinner, 900, 250);
    lv_obj_set_style_arc_color(s_player_lyrics_loading_spinner, kDivider, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_player_lyrics_loading_spinner, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_player_lyrics_loading_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_player_lyrics_loading_spinner, 4, LV_PART_INDICATOR);
    s_player_lyrics_source = lyra::media::LyricsSource::None;
    s_player_lyrics_row_count = 0;
    s_player_lyrics_rows_created = 0;
    s_player_lyrics_rows_ready = false;
    s_player_lyrics_rows_pending = false;
    s_player_lyrics_next_row_y = 2;
    s_player_current_lyric = -1;
    s_player_lyrics_visible = true;
    s_player_manual_scroll_until_us = 0;
    player_art_flip_exec(window, 0);
}

void free_player_lyrics_buffer()
{
    if (!s_player_lyrics_buffer) return;
    heap_caps_free(s_player_lyrics_buffer);
    s_player_lyrics_buffer = nullptr;
}

void stop_player_lyrics_rows_timer()
{
    if (!s_player_lyrics_rows_timer) return;
    lv_timer_delete(s_player_lyrics_rows_timer);
    s_player_lyrics_rows_timer = nullptr;
}

void free_lyrics_load_request(LyricsLoadRequest *request)
{
    if (!request) return;
    if (request->buffer) heap_caps_free(request->buffer);
    heap_caps_free(request);
}

void remove_player_lyrics_spinner()
{
    if (!s_player_lyrics_loading_spinner) return;
    lv_obj_delete(s_player_lyrics_loading_spinner);
    s_player_lyrics_loading_spinner = nullptr;
}

void show_player_no_lyrics()
{
    remove_player_lyrics_spinner();
    free_player_lyrics_buffer();
    s_player_lyrics_row_count = 0;
    s_player_lyrics_rows_created = 0;
    s_player_lyrics_rows_ready = true;
    if (!s_player_lyrics_scroll) return;
    lv_obj_t *plain = make_label(s_player_lyrics_scroll,
                                 tr(lyra::i18n::StringId::NoLyricsFound),
                                 kTextPrimary);
    lv_obj_set_width(plain, s_player_lyrics_width - 32);
    lv_label_set_long_mode(plain, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(plain, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(plain, 4, 4);
}

void player_lyrics_rows_timer_cb(lv_timer_t *timer)
{
    if (s_player_lyrics_rows_timer != timer) return;
    if (!s_player_lyrics_visible || !s_player_lyrics_scroll) {
        stop_player_lyrics_rows_timer();
        return;
    }

    const int row_width = s_player_lyrics_width - 32;
    size_t created_this_tick = 0;
    while (s_player_lyrics_rows_created < s_player_lyrics_row_count &&
           created_this_tick < kLyricsRowsPerTick) {
        const size_t index = s_player_lyrics_rows_created;
        const char *text = s_player_lyrics_rows[index].text;
        lv_obj_t *row = make_label(s_player_lyrics_scroll, text ? text : "",
                                   kTextSecondary);
        lv_obj_set_width(row, row_width);
        lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, kRowRadius, 0);
        lv_obj_set_style_pad_hor(row, 5, 0);
        lv_obj_set_style_pad_ver(row, 7, 0);
        lv_obj_set_pos(row, 4, s_player_lyrics_next_row_y);
        lv_obj_update_layout(row);
        int row_height = lv_obj_get_height(row);
        if (row_height < 34) {
            row_height = 34;
            lv_obj_set_height(row, row_height);
        }
        s_player_lyrics_next_row_y += row_height + 3;
        s_player_lyrics_rows[index].label = row;
        ++s_player_lyrics_rows_created;
        ++created_this_tick;
    }

    if (s_player_lyrics_rows_created < s_player_lyrics_row_count) return;

    lv_obj_update_layout(s_player_lyrics_scroll);
    s_player_lyrics_rows_ready = true;
    stop_player_lyrics_rows_timer();
    free_player_lyrics_buffer();
    update_player_lyrics();
}

void player_lyrics_loaded_async_cb(void *context)
{
    auto *request = static_cast<LyricsLoadRequest *>(context);
    if (!request) return;
    if (request->generation != s_player_lyrics_generation ||
        !s_player_lyrics_visible || !s_player_lyrics_scroll) {
        free_lyrics_load_request(request);
        return;
    }

    s_player_lyrics_source = request->loaded ? request->source :
        lyra::media::LyricsSource::None;
    s_player_lyrics_buffer = request->buffer;
    request->buffer = nullptr;
    free_lyrics_load_request(request);

    const bool parse_timestamps = s_player_lyrics_source == lyra::media::LyricsSource::Lrc ||
                                  s_player_lyrics_source == lyra::media::LyricsSource::Embedded;
    const size_t row_count = s_player_lyrics_buffer ?
        parse_player_lyrics(s_player_lyrics_buffer, parse_timestamps) : 0;
    if (s_player_lyrics_source == lyra::media::LyricsSource::None || row_count == 0) {
        show_player_no_lyrics();
        return;
    }

    s_player_lyrics_rows_created = 0;
    s_player_lyrics_next_row_y = 2;
    s_player_lyrics_rows_ready = false;
    if (s_player_flip_animating && s_player_flip_phase == FlipPhase::FromOpenEdge) {
        s_player_lyrics_rows_pending = true;
    } else {
        start_player_lyrics_rows_timer();
    }
}

void start_player_lyrics_rows_timer()
{
    if (!s_player_lyrics_visible || s_player_lyrics_row_count == 0 ||
        s_player_lyrics_rows_ready || s_player_lyrics_rows_timer) return;
    s_player_lyrics_rows_pending = false;
    remove_player_lyrics_spinner();
    s_player_lyrics_rows_timer = lv_timer_create(player_lyrics_rows_timer_cb,
                                                  kLyricsRowsTickMs, nullptr);
    if (!s_player_lyrics_rows_timer) show_player_no_lyrics();
}

void player_lyrics_load_task(void *context)
{
    auto *request = static_cast<LyricsLoadRequest *>(context);
    if (!request) {
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    request->loaded = lyra::media::load_lyrics(request->track, request->buffer,
                                               lyra::media::kMaxLyricsBytes + 1,
                                               &request->source);
    if (lv_async_call(player_lyrics_loaded_async_cb, request) != LV_RESULT_OK) {
        free_lyrics_load_request(request);
    }
    vTaskDeleteWithCaps(nullptr);
}

void start_player_lyrics_load()
{
    auto *request = static_cast<LyricsLoadRequest *>(
        heap_caps_calloc(1, sizeof(LyricsLoadRequest), MALLOC_CAP_8BIT));
    if (!request) {
        show_player_no_lyrics();
        return;
    }
    request->generation = s_player_lyrics_generation;
    request->track = s_player_pending_lyrics_track;
    request->source = lyra::media::LyricsSource::None;
    request->buffer = static_cast<char *>(heap_caps_malloc(
        lyra::media::kMaxLyricsBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!request->buffer) {
        request->buffer = static_cast<char *>(std::malloc(lyra::media::kMaxLyricsBytes + 1));
    }
    if (!request->buffer) {
        free_lyrics_load_request(request);
        show_player_no_lyrics();
        return;
    }
    request->buffer[0] = '\0';
    // Lyrics load whenever the player opens, which is exactly when a new
    // track needs internal RAM for its I2S output. Reading metadata and .lrc
    // files never writes flash, so this stack lives in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(player_lyrics_load_task, "lyra_lyrics", 8192,
                                        request, 1, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free_lyrics_load_request(request);
        show_player_no_lyrics();
    }
}

void restore_player_art_face()
{
    lv_obj_t *window = s_player_flip_window;
    if (!window) return;
    stop_player_lyrics_rows_timer();
    remove_player_lyrics_spinner();
    free_player_lyrics_buffer();
    if (s_player_flip_content) {
        lv_obj_remove_event_cb(s_player_flip_content, player_art_click_cb);
    }
    lv_obj_clean(window);
    lv_obj_set_style_bg_color(window, kArtworkSurface, 0);
    lv_obj_set_style_bg_opa(window, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(window, 0, 0);
    lv_obj_set_style_radius(window, 0, 0);
    lv_obj_set_style_pad_all(window, 0, 0);
    lv_obj_clear_flag(window, LV_OBJ_FLAG_SCROLLABLE);
    s_player_flip_content = make_box(window, 0, 0, s_player_art_size,
                                     s_player_art_size, kArtworkSurface);
    lv_obj_set_style_border_width(s_player_flip_content, 0, 0);
    s_player_cover_image = make_artwork_contents(
        s_player_flip_content, s_player_art_size, s_player_art_size, s_player_lyrics_track, 0);
    lv_obj_add_event_cb(s_player_flip_content, player_art_click_cb,
                        LV_EVENT_CLICKED, nullptr);
    s_player_lyrics_scroll = nullptr;
    s_player_lyrics_row_count = 0;
    s_player_lyrics_rows_created = 0;
    s_player_lyrics_rows_ready = false;
    s_player_lyrics_rows_pending = false;
    s_player_current_lyric = -1;
    s_player_lyrics_visible = false;
    player_art_flip_exec(window, 0);
}

void player_art_flip_exec(void *object, int32_t width)
{
    lv_obj_t *window = static_cast<lv_obj_t *>(object);
    if (!window || s_player_art_size <= 0) return;
    const int32_t clipped_width = std::clamp<int32_t>(width, 1, s_player_art_size);
    const int32_t offset = (s_player_art_size - clipped_width) / 2;
    lv_obj_set_x(window, offset);
    lv_obj_set_width(window, clipped_width);
    if (s_player_flip_content) lv_obj_set_x(s_player_flip_content, -offset);
}

void start_player_art_flip(FlipPhase phase, int32_t from, int32_t to)
{
    if (!s_player_flip_window) return;
    s_player_flip_phase = phase;
    s_player_flip_animating = true;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_player_flip_window);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, 200);
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
            start_player_lyrics_load();
            s_player_flip_phase = FlipPhase::FromOpenEdge;
            start_player_art_flip(FlipPhase::FromOpenEdge, 0, s_player_art_size);
            break;
        case FlipPhase::ToCloseEdge:
            restore_player_art_face();
            s_player_flip_phase = FlipPhase::FromCloseEdge;
            start_player_art_flip(FlipPhase::FromCloseEdge, 0, s_player_art_size);
            break;
        case FlipPhase::FromOpenEdge:
            s_player_flip_animating = false;
            s_player_flip_phase = FlipPhase::None;
            if (s_player_lyrics_rows_pending) start_player_lyrics_rows_timer();
            break;
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
    if (!s_player_flip_window || !s_player_flip_content ||
        s_player_lyrics_visible || s_player_flip_animating) return;
    ++s_player_lyrics_generation;
    s_player_pending_lyrics_track = s_player_lyrics_track;
    start_player_art_flip(FlipPhase::ToOpenEdge, s_player_art_size, 0);
}

void player_lyrics_close_cb(lv_event_t *)
{
    if (!s_player_flip_window || !s_player_lyrics_visible || s_player_flip_animating) return;
    ++s_player_lyrics_generation;
    stop_player_lyrics_rows_timer();
    free_player_lyrics_buffer();
    s_player_lyrics_rows_pending = false;
    s_player_flip_animating = true;
    if (s_player_flip_content) {
        lv_obj_remove_event_cb(s_player_flip_content, player_art_click_cb);
    }
    start_player_art_flip(FlipPhase::ToCloseEdge, s_player_art_size, 0);
}

// Now Playing: the cover fills the top of the screen under a translucent
// status bar; below it, a panel tinted with the cover's own colour holds the
// title, the seek bar and the transport.
constexpr int kPlayerArtSize = kScreenWidth;
constexpr int kPlayerPanelTop = kPlayerArtSize;
constexpr int kPlayerInset = 16;

lv_obj_t *s_player_panel;

// The average colour of the current cover, sampled sparsely: enough to tint
// the panel without walking every pixel.
bool player_art_average(lv_color_t *out)
{
    if (!s_player_art_pixels || !out ||
        std::strcmp(s_player_art_path, s_player_lyrics_track.path) != 0) return false;
    const auto *pixels = reinterpret_cast<const uint16_t *>(s_player_art_pixels);
    const uint32_t width = s_player_art_descriptor.header.w;
    const uint32_t height = s_player_art_descriptor.header.h;
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    uint32_t samples = 0;
    for (uint32_t y = 4; y < height; y += 12) {
        for (uint32_t x = 4; x < width; x += 12) {
            const uint16_t pixel = pixels[y * width + x];
            red += (pixel >> 11) & 0x1F;
            green += (pixel >> 5) & 0x3F;
            blue += pixel & 0x1F;
            ++samples;
        }
    }
    if (samples == 0) return false;
    *out = lv_color_make(static_cast<uint8_t>(red * 255 / (31 * samples)),
                         static_cast<uint8_t>(green * 255 / (63 * samples)),
                         static_cast<uint8_t>(blue * 255 / (31 * samples)));
    return true;
}

void tint_player_panel()
{
    if (!s_player_panel) return;
    lv_color_t average;
    if (!player_art_average(&average)) {
        lv_obj_set_style_bg_color(s_player_panel, kSurface, 0);
        return;
    }
    // Keep the cover's hue but hold the panel dark (or light) enough for text.
    const lv_color_t base = s_dark_mode ? lv_color_hex(0x101010) : lv_color_hex(0xF4F4F6);
    lv_obj_set_style_bg_color(s_player_panel,
                              lv_color_mix(average, base, s_dark_mode ? 92 : 70), 0);
}

void player_panel_deleted_cb(lv_event_t *)
{
    s_player_panel = nullptr;
}

} // namespace

void refresh_player_artwork()
{
    if (!s_player_cover_image || lv_image_get_src(s_player_cover_image)) return;
    if (load_player_art(s_player_lyrics_track)) {
        lv_image_set_src(s_player_cover_image, &s_player_art_descriptor);
        tint_player_panel();
    } else {
        lyra::media::request_artwork(s_player_lyrics_track, lyra::media::ArtworkSize::Player);
    }
}

void reset_player_lyrics_state()
{
    ++s_player_lyrics_generation;
    if (s_player_flip_window) lv_anim_delete(s_player_flip_window, nullptr);
    stop_player_lyrics_rows_timer();
    remove_player_lyrics_spinner();
    free_player_lyrics_buffer();
    s_player_lyrics_width = s_player_lyrics_height = 0;
    s_player_flip_window = nullptr;
    s_player_flip_content = nullptr;
    s_player_cover_image = nullptr;
    s_player_lyrics_scroll = nullptr;
    s_player_lyrics_loading_spinner = nullptr;
    s_player_lyrics_source = lyra::media::LyricsSource::None;
    s_player_lyrics_row_count = 0;
    s_player_lyrics_rows_created = 0;
    s_player_lyrics_rows_ready = false;
    s_player_lyrics_rows_pending = false;
    s_player_current_lyric = -1;
    s_player_manual_scroll_until_us = 0;
    s_player_auto_scrolling = false;
    s_player_lyrics_visible = false;
    s_player_flip_animating = false;
    s_player_flip_phase = FlipPhase::None;
}

void update_player_progress_touch_value()
{
    if (!s_player_progress_bar || !lv_indev_active()) return;
    lv_area_t area;
    lv_obj_get_coords(s_player_progress_bar, &area);
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const int32_t width = std::max<int32_t>(1, area.x2 - area.x1);
    const int32_t x = std::max<int32_t>(0, std::min<int32_t>(point.x - area.x1, width));
    s_player_progress_drag_value = (x * kPlayerProgressMax) / width;
}

void update_player_progress_preview()
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (audio_status.duration_ms == 0) return;
    if (s_player_progress_bar) {
        lv_bar_set_value(s_player_progress_bar, s_player_progress_drag_value, LV_ANIM_OFF);
    }
    const uint32_t preview_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(s_player_progress_drag_value) * audio_status.duration_ms) / kPlayerProgressMax);
    char elapsed[16];
    format_playback_time(preview_ms, false, elapsed, sizeof(elapsed));
    if (s_player_elapsed_label) set_label_text_if_changed(s_player_elapsed_label, elapsed);
}

void player_progress_touch_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_player_progress_dragging = true;
        update_player_progress_touch_value();
        update_player_progress_preview();
        return;
    }

    if (code == LV_EVENT_PRESSING && s_player_progress_dragging) {
        update_player_progress_touch_value();
        update_player_progress_preview();
        return;
    }

    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (!s_player_progress_dragging) return;
    update_player_progress_touch_value();
    const lyra::audio::Status audio_status = lyra::audio::status();
    s_player_progress_dragging = false;
    if (audio_status.duration_ms == 0) return;
    const uint32_t target_ms = static_cast<uint32_t>(
        (static_cast<uint64_t>(s_player_progress_drag_value) * audio_status.duration_ms) / kPlayerProgressMax);
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
    update_menu_playback();
    lyra::audio::Status audio_status = lyra::audio::status();
    const bool ab_mode_changed = update_player_ab_loop(audio_status);
    if (ab_mode_changed && s_view == View::Player) render(View::Player);
    audio_status = lyra::audio::status();
    if (!s_player_progress_bar && !s_player_elapsed_label && !s_player_duration_label) return;
    if (s_player_seek_preview_active && s_player_seek_preview_release_pending) {
        const bool path_changed = std::strcmp(audio_status.path, s_player_seek_preview_path) != 0;
        if (path_changed || audio_status.last_error != ESP_OK || audio_status.duration_ms > 0) {
            s_player_seek_preview_active = false;
            s_player_seek_preview_release_pending = false;
        }
    }
    const uint32_t duration_ms = s_player_seek_preview_active ?
        s_player_seek_preview_duration_ms : audio_status.duration_ms;
    const uint32_t requested_position_ms = s_player_seek_preview_active ?
        s_player_seek_preview_position_ms : audio_status.position_ms;
    const uint32_t position_ms = requested_position_ms;
    if (s_player_progress_dragging) return;
    if (s_player_progress_bar) {
        const int progress = duration_ms > 0 ? static_cast<int>(
            (static_cast<uint64_t>(position_ms) * kPlayerProgressMax) / duration_ms) : 0;
        lv_bar_set_value(s_player_progress_bar, std::min<int32_t>(progress, kPlayerProgressMax), LV_ANIM_OFF);
    }
    char elapsed[16];
    char duration[16];
    format_playback_time(position_ms, false, elapsed, sizeof(elapsed));
    format_playback_time(duration_ms, duration_ms == 0, duration, sizeof(duration));
    if (s_player_elapsed_label) set_label_text_if_changed(s_player_elapsed_label, elapsed);
    if (s_player_duration_label) set_label_text_if_changed(s_player_duration_label, duration);
    update_player_lyrics();
}

void player_quick_seek_cb(lv_event_t *event)
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.path[0] || audio_status.duration_ms == 0 ||
        audio_status.last_error != ESP_OK) return;

    constexpr uint32_t kQuickSeekDistanceMs = 30000;
    const uint32_t current_ms = std::min(audio_status.position_ms, audio_status.duration_ms);
    const bool forward = lv_event_get_user_data(event) != nullptr;
    const uint32_t target_ms = forward ? static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(current_ms) + kQuickSeekDistanceMs,
        audio_status.duration_ms)) :
        (current_ms > kQuickSeekDistanceMs ? current_ms - kQuickSeekDistanceMs : 0);
    const esp_err_t seek_ret = lyra::audio::seek(target_ms);
    if (seek_ret != ESP_OK) {
        ESP_LOGW(kTag, "could not quick seek playback: %s", esp_err_to_name(seek_ret));
    }
    update_player_progress();
}

void make_player_quick_seek_button(lv_obj_t *parent, int x, int y, bool forward)
{
    // A dark lozenge over the bottom corner of the cover.
    lv_obj_t *button = make_button(parent, x, y, 56, 36, lv_color_black(), LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(button, LV_OPA_50, 0);
    lv_obj_t *label = make_label(button, tr(forward ?
        lyra::i18n::StringId::QuickSeekForward :
        lyra::i18n::StringId::QuickSeekBackward), lv_color_white());
    lv_obj_center(label);
    lv_obj_add_event_cb(button, player_quick_seek_cb, LV_EVENT_CLICKED,
                        forward ? reinterpret_cast<void *>(static_cast<uintptr_t>(1)) : nullptr);
}

void update_player_progress_seek_preview(uint32_t position_ms, uint32_t duration_ms, bool active)
{
    if (!active) {
        const lyra::audio::Status audio_status = lyra::audio::status();
        s_player_seek_preview_release_pending = s_player_seek_preview_active &&
                                                audio_status.duration_ms == 0;
        if (!s_player_seek_preview_release_pending) s_player_seek_preview_active = false;
        update_player_progress();
        return;
    }

    s_player_seek_preview_active = true;
    s_player_seek_preview_release_pending = false;
    s_player_seek_preview_duration_ms = duration_ms;
    s_player_seek_preview_position_ms = std::min(position_ms, duration_ms);
    const lyra::audio::Status audio_status = lyra::audio::status();
    std::strncpy(s_player_seek_preview_path, audio_status.path,
                 sizeof(s_player_seek_preview_path) - 1);
    s_player_seek_preview_path[sizeof(s_player_seek_preview_path) - 1] = '\0';
    if (s_player_progress_dragging) return;
    if (s_player_progress_bar && duration_ms > 0) {
        const int progress = static_cast<int>(
            (static_cast<uint64_t>(s_player_seek_preview_position_ms) * kPlayerProgressMax) / duration_ms);
        lv_bar_set_value(s_player_progress_bar, std::min<int32_t>(progress, kPlayerProgressMax), LV_ANIM_OFF);
    }
    char elapsed[16];
    format_playback_time(s_player_seek_preview_position_ms, false, elapsed, sizeof(elapsed));
    if (s_player_elapsed_label) set_label_text_if_changed(s_player_elapsed_label, elapsed);
}

namespace {

const char *play_mode_symbol()
{
    return s_shuffle ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP;
}

lv_obj_t *make_transport_button(lv_obj_t *parent, int x, int y, int size, const char *symbol,
                                lv_color_t color)
{
    lv_obj_t *button = make_symbol_button(parent, x, y, size, symbol, color);
    lv_obj_t *glyph = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(glyph, &lv_font_montserrat_28, 0);
    return button;
}

void bind_hold_transport(lv_obj_t *button, lv_event_cb_t callback)
{
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESS_LOST, nullptr);
}

} // namespace

void render_player()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        make_header(tr(lyra::i18n::StringId::NowPlaying), View::Menu, true);
        lv_obj_t *empty = make_label(s_screen, tr(lyra::i18n::StringId::NoTrackSelectedAndScan),
                                     kTextMuted);
        lv_obj_set_width(empty, kScreenWidth - 40);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
        return;
    }
    const bool favorite = lyra::media::is_favorite(s_current_track);

    // ---- Cover (tap for lyrics) ----
    s_player_art_size = kPlayerArtSize;
    s_player_lyrics_width = s_player_lyrics_height = kPlayerArtSize;
    s_player_lyrics_track = track;
    lv_obj_t *art_face = make_box(s_screen, 0, 0, kPlayerArtSize, kPlayerArtSize, kArtworkSurface);
    lv_obj_remove_flag(art_face, LV_OBJ_FLAG_CLICKABLE);
    s_player_flip_window = make_box(art_face, 0, 0, kPlayerArtSize, kPlayerArtSize, kArtworkSurface);
    s_player_flip_content = make_box(s_player_flip_window, 0, 0, kPlayerArtSize, kPlayerArtSize,
                                     kArtworkSurface);
    s_player_cover_image = make_artwork_contents(s_player_flip_content, kPlayerArtSize,
                                                 kPlayerArtSize, track, 0);
    lv_obj_add_event_cb(s_player_flip_content, player_art_click_cb, LV_EVENT_CLICKED, nullptr);

    // Close Now Playing; a swipe to the right does the same.
    lv_obj_t *close = make_button(s_screen, 8, kStatusHeight + 6, 40, 40, lv_color_black(),
                                  LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(close, LV_OPA_40, 0);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_DOWN, lv_color_white());
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { navigate_back(View::Menu); },
                        LV_EVENT_CLICKED, nullptr);

    if (s_quick_seek) {
        make_player_quick_seek_button(s_screen, 12, kPlayerArtSize - 48, false);
        make_player_quick_seek_button(s_screen, kScreenWidth - 68, kPlayerArtSize - 48, true);
    }

    // ---- Panel ----
    lv_obj_t *panel = make_box(s_screen, 0, kPlayerPanelTop, kScreenWidth,
                               kScreenHeight - kPlayerPanelTop, kSurface);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    s_player_panel = panel;
    lv_obj_add_event_cb(panel, player_panel_deleted_cb, LV_EVENT_DELETE, nullptr);
    tint_player_panel();

    lv_obj_t *title = make_label(panel, track.title, kTextPrimary);
    lv_obj_set_style_text_font(title, title_font(), 0);
    make_marquee(title, kScreenWidth - kPlayerInset - 56, MarqueeMode::Focus);
    lv_obj_set_pos(title, kPlayerInset, 10);

    lv_obj_t *heart_button = make_button(panel, kScreenWidth - 50, 4, 40, 40, kSurfaceRaised,
                                         LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(heart_button, LV_OPA_TRANSP, 0);
    lv_obj_t *heart = make_label(heart_button, favorite ? kHeartFilled : kHeartOutline,
                                 favorite ? kAccent : kTextPrimary);
    lv_obj_set_style_text_font(heart, title_font(), 0);
    lv_obj_center(heart);
    lv_obj_add_event_cb(heart_button, toggle_favorite_cb, LV_EVENT_CLICKED, heart);

    char format[48];
    format_track_format(track, format, sizeof(format));
    lv_obj_t *format_label = make_label(panel, format, kTextMuted);
    make_marquee(format_label, 110);
    lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(format_label, LV_ALIGN_TOP_RIGHT, -kPlayerInset, 36);
    lv_obj_t *artist = make_label(panel, track.artist[0] ? track.artist : track.album,
                                  kTextSecondary);
    make_marquee(artist, kScreenWidth - 2 * kPlayerInset - 120, MarqueeMode::Focus);
    lv_obj_set_pos(artist, kPlayerInset, 36);
    // Tap the artist name to open the artist.
    lv_obj_add_flag(artist, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(artist, 6);
    lv_obj_add_event_cb(artist, open_current_track_group_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Artist)));

    // ---- Seek bar ----
    constexpr int kBarY = 70;
    constexpr int kBarWidth = kScreenWidth - 2 * kPlayerInset;
    lv_obj_t *bar = lv_bar_create(panel);
    lv_obj_set_pos(bar, kPlayerInset, kBarY);
    lv_obj_set_size(bar, kBarWidth, 4);
    lv_bar_set_range(bar, 0, kPlayerProgressMax);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, kTextMuted, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, kAccent, LV_PART_INDICATOR);
    // Draw the knob over the rail, including when LVGL refreshes only the
    // pixels beyond the edge of the rail.
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(bar, player_seek_handle_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(bar, player_seek_handle_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_refresh_ext_draw_size(bar);
    s_player_progress_bar = bar;
    s_player_progress_touch = make_player_progress_touch(panel, kPlayerInset - kSeekHandleRadius,
        kBarY - 10, kBarWidth + 2 * kSeekHandleRadius, 24);
    block_page_gestures(s_player_progress_touch);
    s_player_elapsed_label = make_label(panel, "00:00", kTextSecondary);
    lv_obj_set_pos(s_player_elapsed_label, kPlayerInset, kBarY + 10);
    s_player_duration_label = make_label(panel, "--:--", kTextSecondary);
    lv_obj_align(s_player_duration_label, LV_ALIGN_TOP_RIGHT, -kPlayerInset, kBarY + 10);
    if (!make_player_ab_bar(panel, kBarY + 8)) {
        const size_t queue_count = s_has_active_queue ? playback_queue_count() : 0;
        size_t queue_position = 0;
        if (queue_count > 0 && current_queue_position(&queue_position)) {
            char position_text[24];
            std::snprintf(position_text, sizeof(position_text), "%u/%u",
                          static_cast<unsigned>(queue_position + 1),
                          static_cast<unsigned>(queue_count));
            lv_obj_t *position = make_label(panel, position_text, kTextMuted);
            lv_obj_align(position, LV_ALIGN_TOP_MID, 0, kBarY + 10);
        }
    }
    update_player_progress();

    // ---- Transport ----
    constexpr int kControlsCentre = 128;
    const bool mode_active = s_shuffle || s_repeat_mode != RepeatMode::Off;
    lv_obj_t *mode = make_symbol_button(panel, 10, kControlsCentre - 22, 44, play_mode_symbol(),
                                        mode_active ? kAccent : kTextSecondary);
    if (s_repeat_mode == RepeatMode::Song) {
        lv_obj_t *one = make_label(mode, "1", kAccent);
        lv_obj_align(one, LV_ALIGN_CENTER, 10, 7);
    }
    lv_obj_add_event_cb(mode, [](lv_event_t *) { cycle_play_mode(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *previous = make_transport_button(panel, 66, kControlsCentre - 24, 48,
                                               LV_SYMBOL_PREV, kTextPrimary);
    bind_hold_transport(previous, nav_previous_cb);
    const lyra::audio::Status audio = lyra::audio::status();
    const bool playing = audio.playing && !audio.paused;
    lv_obj_t *play = make_button(panel, (kScreenWidth - 58) / 2, kControlsCentre - 29, 58, 58,
                                 kAccent, LV_RADIUS_CIRCLE);
    lv_obj_t *play_icon = make_label(play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                                     kTextOnAccent);
    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(play_icon);
    lv_obj_add_event_cb(play, nav_play_cb, LV_EVENT_CLICKED, nullptr);
    track_play_icon(play_icon);
    lv_obj_t *next = make_transport_button(panel, kScreenWidth - 66 - 48, kControlsCentre - 24, 48,
                                           LV_SYMBOL_NEXT, kTextPrimary);
    bind_hold_transport(next, nav_next_cb);
    lv_obj_t *more = make_icon_button(panel, kScreenWidth - 54, kControlsCentre - 22, 44,
                                      Icon::More, kTextSecondary);
    lv_obj_add_event_cb(more, [](lv_event_t *) { show_player_more_menu(); },
                        LV_EVENT_CLICKED, nullptr);

    raise_status_bar();
}

} // namespace lyra::gui::internal
