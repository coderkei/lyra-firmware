/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

#include <sys/stat.h>
#include <unistd.h>

namespace lyra::gui::internal {
namespace {

constexpr size_t kPlayerBookmarkCount = 5;
constexpr uint32_t kPlayerBookmarkMagic = 0x4C594250;
constexpr uint16_t kPlayerBookmarkVersion = 1;
constexpr const char *kPlayerBookmarkDirectory =
    "/sdcard/.lyra/playback-bookmarks";

struct PlayerBookmarks {
    uint32_t magic = kPlayerBookmarkMagic;
    uint16_t version = kPlayerBookmarkVersion;
    uint8_t count = 0;
    uint8_t reserved = 0;
    char path[lyra::audio::kMaxPath]{};
    uint32_t positions_ms[kPlayerBookmarkCount]{};
};

enum class MoreAction : uintptr_t {
    Equalizer,
    Information,
    AddToPlaylist,
    PlaybackSpeed,
    ABPlayMode,
    Bookmarks,
};

enum class ABAction : uintptr_t { SetA, SetB, End };

lv_obj_t *s_more_overlay;
lv_obj_t *s_speed_overlay;
lv_obj_t *s_speed_value_label;
lv_obj_t *s_ab_a_button;
lv_obj_t *s_ab_b_button;
lv_obj_t *s_bookmarks_overlay;
bool s_ab_active;
bool s_ab_has_a;
bool s_ab_has_b;
uint32_t s_ab_a_ms;
uint32_t s_ab_b_ms;
char s_ab_path[lyra::audio::kMaxPath];

bool current_player_track_path(char *path, size_t capacity)
{
    if (!path || capacity == 0) return false;
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track) || !track.path[0] ||
        std::strlen(track.path) >= capacity) return false;
    std::strcpy(path, track.path);
    return true;
}

void close_player_overlay(lv_obj_t **overlay)
{
    if (!overlay || !*overlay) return;
    lv_obj_t *to_delete = *overlay;
    *overlay = nullptr;
    lv_obj_delete(to_delete);
}

void close_more_overlay()
{
    close_player_overlay(&s_more_overlay);
}

void close_speed_overlay()
{
    s_speed_value_label = nullptr;
    close_player_overlay(&s_speed_overlay);
}

void close_bookmarks_overlay()
{
    close_player_overlay(&s_bookmarks_overlay);
}

void player_overlay_background_cb(lv_event_t *event)
{
    auto **slot = static_cast<lv_obj_t **>(lv_event_get_user_data(event));
    close_player_overlay(slot);
}

lv_obj_t *make_player_overlay(lv_obj_t **slot, int dialog_height,
                              lv_obj_t **dialog_out)
{
    if (!slot || !dialog_out || *slot) return nullptr;
    const int overlay_height = content_bottom();
    dialog_height = std::min(dialog_height, overlay_height - 36);
    if (dialog_height < 120) return nullptr;
    *slot = make_box(s_screen, 0, 0, kScreenWidth, overlay_height, kBackground);
    if (!*slot) return nullptr;
    lv_obj_set_style_bg_opa(*slot, LV_OPA_80, 0);
    lv_obj_add_flag(*slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(*slot, player_overlay_background_cb,
                        LV_EVENT_CLICKED, slot);
    *dialog_out = make_box(*slot, 16, (overlay_height - dialog_height) / 2,
                           288, dialog_height, kSurfaceRaised, 10);
    if (!*dialog_out) {
        close_player_overlay(slot);
        return nullptr;
    }
    lv_obj_add_flag(*dialog_out, LV_OBJ_FLAG_CLICKABLE);
    return *slot;
}

void update_ab_button_style()
{
    if (s_ab_a_button) {
        lv_obj_set_style_bg_color(s_ab_a_button,
            s_ab_has_a ? kAccentSurface : kBackground, 0);
        lv_obj_t *label = lv_obj_get_child(s_ab_a_button, 0);
        if (label) lv_obj_set_style_text_color(label,
            s_ab_has_a ? kAccent : kTextPrimary, 0);
    }
    if (s_ab_b_button) {
        lv_obj_set_style_bg_color(s_ab_b_button,
            s_ab_has_b ? kAccentSurface : kBackground, 0);
        lv_obj_t *label = lv_obj_get_child(s_ab_b_button, 0);
        if (label) lv_obj_set_style_text_color(label,
            s_ab_has_b ? kAccent : kTextPrimary, 0);
    }
}

void reset_ab_points()
{
    s_ab_has_a = false;
    s_ab_has_b = false;
    s_ab_a_ms = 0;
    s_ab_b_ms = 0;
    s_ab_path[0] = '\0';
    update_ab_button_style();
}

uint64_t playback_bookmark_hash(const char *track_path)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(track_path); *p; ++p) {
        hash = (hash ^ *p) * 1099511628211ULL;
    }
    return hash;
}

void playback_bookmark_file_path(const char *track_path, char *out, size_t capacity)
{
    const uint64_t hash = playback_bookmark_hash(track_path);
    std::snprintf(out, capacity, "%s/%08lx%08lx.bin",
        kPlayerBookmarkDirectory,
        static_cast<unsigned long>(hash >> 32),
        static_cast<unsigned long>(hash & 0xFFFFFFFFu));
}

bool read_player_bookmark_candidate(const char *file_path, const char *track_path,
                                    PlayerBookmarks *bookmarks)
{
    FILE *file = lyra::sd::open(file_path, "rb", lyra::sd::Client::Filesystem);
    if (!file) return false;
    PlayerBookmarks loaded{};
    const bool read_ok = lyra::sd::read_exact(file, &loaded, sizeof(loaded),
                                             lyra::sd::Client::Filesystem);
    const bool close_ok = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0;
    if (!read_ok || !close_ok || loaded.magic != kPlayerBookmarkMagic ||
        loaded.version != kPlayerBookmarkVersion ||
        loaded.count > kPlayerBookmarkCount ||
        std::strncmp(loaded.path, track_path, sizeof(loaded.path)) != 0) return false;
    *bookmarks = loaded;
    return true;
}

bool load_player_bookmarks(const char *track_path, PlayerBookmarks *bookmarks)
{
    if (!track_path || !track_path[0] || !bookmarks ||
        std::strlen(track_path) >= sizeof(bookmarks->path)) return false;
    *bookmarks = {};
    std::strncpy(bookmarks->path, track_path, sizeof(bookmarks->path) - 1);
    char path[112];
    char backup[120];
    playback_bookmark_file_path(track_path, path, sizeof(path));
    std::snprintf(backup, sizeof(backup), "%s.bak", path);
    if (read_player_bookmark_candidate(path, track_path, bookmarks) ||
        read_player_bookmark_candidate(backup, track_path, bookmarks)) return true;
    *bookmarks = {};
    std::strncpy(bookmarks->path, track_path, sizeof(bookmarks->path) - 1);
    return true;
}

bool ensure_player_bookmark_directory()
{
    if (!lyra::sd::acquire(lyra::sd::Client::Filesystem)) return false;
    struct stat info{};
    bool ok = (stat("/sdcard/.lyra", &info) == 0 && S_ISDIR(info.st_mode)) ||
              mkdir("/sdcard/.lyra", 0775) == 0;
    ok = ok && ((stat(kPlayerBookmarkDirectory, &info) == 0 &&
                  S_ISDIR(info.st_mode)) ||
                 mkdir(kPlayerBookmarkDirectory, 0775) == 0);
    lyra::sd::release(lyra::sd::Client::Filesystem);
    return ok;
}

bool save_player_bookmarks(const PlayerBookmarks &bookmarks)
{
    if (!ensure_player_bookmark_directory()) return false;
    char path[112];
    char temp[120];
    char backup[120];
    playback_bookmark_file_path(bookmarks.path, path, sizeof(path));
    std::snprintf(temp, sizeof(temp), "%s.tmp", path);
    std::snprintf(backup, sizeof(backup), "%s.bak", path);

    FILE *file = lyra::sd::open(temp, "wb", lyra::sd::Client::Filesystem);
    bool ok = file && lyra::sd::write_exact(file, &bookmarks, sizeof(bookmarks),
                                             lyra::sd::Client::Filesystem);
    if (file) {
        if (lyra::sd::acquire(lyra::sd::Client::Filesystem)) {
            ok = ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
            lyra::sd::release(lyra::sd::Client::Filesystem);
        } else {
            ok = false;
        }
        ok = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0 && ok;
    }
    if (ok) {
        lyra::sd::remove(backup, lyra::sd::Client::Filesystem);
        struct stat info{};
        const bool had_original = stat(path, &info) == 0;
        if (had_original) {
            ok = lyra::sd::rename(path, backup,
                                  lyra::sd::Client::Filesystem) == 0;
        }
        if (ok) {
            ok = lyra::sd::rename(temp, path,
                                  lyra::sd::Client::Filesystem) == 0;
            if (!ok && had_original) {
                lyra::sd::rename(backup, path, lyra::sd::Client::Filesystem);
            }
        }
        if (ok) lyra::sd::remove(backup, lyra::sd::Client::Filesystem);
    }
    if (!ok) lyra::sd::remove(temp, lyra::sd::Client::Filesystem);
    return ok;
}

void show_player_bookmarks_overlay();

void set_speed_value_label(uint16_t speed_permille)
{
    if (!s_speed_value_label) return;
    char value[56];
    format_float(lyra::i18n::StringId::PlaybackSpeedValue,
                 speed_permille / 1000.0, value, sizeof(value));
    set_label_text_if_changed(s_speed_value_label, value);
}

void player_speed_slider_cb(lv_event_t *event)
{
    auto *slider = lv_event_get_current_target_obj(event);
    if (!slider) return;
    const int percent = std::clamp<int>(lv_slider_get_value(slider), 25, 200);
    const uint16_t speed_permille = static_cast<uint16_t>(percent * 10);
    if (lyra::audio::set_playback_speed(speed_permille) == ESP_OK) {
        set_speed_value_label(speed_permille);
    }
}

void show_player_speed_overlay()
{
    lv_obj_t *dialog = nullptr;
    if (!make_player_overlay(&s_speed_overlay, 240, &dialog)) return;
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::PlaybackSpeed),
                                 kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *close = make_button(dialog, 246, 7, 34, 34, kSurface, 17);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_speed_overlay(); },
                        LV_EVENT_CLICKED, nullptr);

    s_speed_value_label = make_label(dialog, "", kAccent);
    lv_obj_set_width(s_speed_value_label, 248);
    lv_obj_set_style_text_align(s_speed_value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_speed_value_label, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *slider = lv_slider_create(dialog);
    lv_obj_set_size(slider, 246, 18);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 124);
    lv_slider_set_range(slider, 25, 200);
    const uint16_t speed_permille = lyra::audio::status().playback_speed_permille;
    lv_slider_set_value(slider, std::clamp<int>(speed_permille / 10, 25, 200),
                        LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, player_speed_slider_cb, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    lv_obj_t *minimum = make_label(dialog,
        tr(lyra::i18n::StringId::PlaybackSpeedMinimum), kTextSecondary);
    lv_obj_align(minimum, LV_ALIGN_TOP_LEFT, 16, 158);
    lv_obj_t *maximum = make_label(dialog,
        tr(lyra::i18n::StringId::PlaybackSpeedMaximum), kTextSecondary);
    lv_obj_align(maximum, LV_ALIGN_TOP_RIGHT, -16, 158);
    set_speed_value_label(speed_permille);
}

void player_bookmark_jump_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    char track_path[lyra::audio::kMaxPath];
    PlayerBookmarks bookmarks{};
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!current_player_track_path(track_path, sizeof(track_path)) ||
        std::strcmp(track_path, audio_status.path) != 0 ||
        !load_player_bookmarks(track_path, &bookmarks) || index >= bookmarks.count) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::PlaybackIdle));
        return;
    }
    close_bookmarks_overlay();
    const esp_err_t result = lyra::audio::seek(bookmarks.positions_ms[index]);
    if (result != ESP_OK) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::PlaybackBookmarkJumpFailed));
    }
}

void player_bookmark_remove_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    char track_path[lyra::audio::kMaxPath];
    PlayerBookmarks bookmarks{};
    if (!current_player_track_path(track_path, sizeof(track_path)) ||
        !load_player_bookmarks(track_path, &bookmarks) || index >= bookmarks.count) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::ReaderBookmarkRemoveFailed));
        return;
    }
    for (size_t i = index + 1; i < bookmarks.count; ++i) {
        bookmarks.positions_ms[i - 1] = bookmarks.positions_ms[i];
    }
    bookmarks.positions_ms[--bookmarks.count] = 0;
    if (!save_player_bookmarks(bookmarks)) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::ReaderBookmarkRemoveFailed));
        return;
    }
    close_bookmarks_overlay();
    show_player_bookmarks_overlay();
}

void player_bookmark_add_cb(lv_event_t *)
{
    char track_path[lyra::audio::kMaxPath];
    PlayerBookmarks bookmarks{};
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!current_player_track_path(track_path, sizeof(track_path)) ||
        std::strcmp(track_path, audio_status.path) != 0 ||
        audio_status.duration_ms == 0 || audio_status.last_error != ESP_OK ||
        !load_player_bookmarks(track_path, &bookmarks) ||
        bookmarks.count >= kPlayerBookmarkCount) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::ReaderBookmarkFailed));
        return;
    }
    bookmarks.positions_ms[bookmarks.count++] = std::min(
        audio_status.position_ms, audio_status.duration_ms);
    if (!save_player_bookmarks(bookmarks)) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::ReaderBookmarkFailed));
        return;
    }
    close_bookmarks_overlay();
    show_player_bookmarks_overlay();
}

void show_player_bookmarks_overlay()
{
    char track_path[lyra::audio::kMaxPath];
    PlayerBookmarks bookmarks{};
    if (!current_player_track_path(track_path, sizeof(track_path)) ||
        !load_player_bookmarks(track_path, &bookmarks)) {
        show_notice(tr(lyra::i18n::StringId::PlayerBookmarks),
                    tr(lyra::i18n::StringId::ReaderBookmarkFailed));
        return;
    }

    lv_obj_t *dialog = nullptr;
    if (!make_player_overlay(&s_bookmarks_overlay, 400, &dialog)) return;
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::PlayerBookmarks),
                                 kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *close = make_button(dialog, 246, 7, 34, 34, kSurface, 17);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_bookmarks_overlay(); },
                        LV_EVENT_CLICKED, nullptr);

    const int list_height = lv_obj_get_height(dialog) - 56;
    lv_obj_t *list = make_box(dialog, 8, 48, 272, list_height, kSurface, 6);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (size_t i = 0; i < bookmarks.count; ++i) {
        char time[16];
        char label_text[32];
        format_playback_time(bookmarks.positions_ms[i], false, time, sizeof(time));
        std::snprintf(label_text, sizeof(label_text), "#%u   %s",
                      static_cast<unsigned>(i + 1), time);
        lv_obj_t *jump = make_button(list, 0, static_cast<int>(i) * 48,
                                     220, 44, kBackground, 5, true);
        lv_obj_t *label = make_label(jump, label_text, kTextPrimary);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_add_event_cb(jump, player_bookmark_jump_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_t *remove = make_button(list, 224, static_cast<int>(i) * 48,
                                       44, 44, kDangerSurface, 5);
        lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_CLOSE, kTextSecondary);
        lv_obj_center(remove_icon);
        lv_obj_add_event_cb(remove, player_bookmark_remove_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
    if (bookmarks.count < kPlayerBookmarkCount) {
        lv_obj_t *add = make_button(list, 0, static_cast<int>(bookmarks.count) * 48,
                                    268, 44, kAccentDark, 5, true);
        lv_obj_t *add_label = make_label(add,
            tr(lyra::i18n::StringId::AddPlaybackBookmark), kTextOnAccent);
        lv_obj_center(add_label);
        lv_obj_add_event_cb(add, player_bookmark_add_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void player_more_action_cb(lv_event_t *event)
{
    const auto action = static_cast<MoreAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    close_more_overlay();
    switch (action) {
        case MoreAction::Equalizer:
            navigate_to(View::Equalizer);
            break;
        case MoreAction::Information:
            s_track_info_tab = TrackInfoTab::Song;
            navigate_to(View::TrackInfo);
            break;
        case MoreAction::AddToPlaylist:
            show_player_playlist_picker_cb(nullptr);
            break;
        case MoreAction::PlaybackSpeed:
            show_player_speed_overlay();
            break;
        case MoreAction::ABPlayMode: {
            const lyra::audio::Status audio_status = lyra::audio::status();
            if (!audio_status.path[0] || audio_status.duration_ms == 0 ||
                audio_status.last_error != ESP_OK) {
                show_notice(tr(lyra::i18n::StringId::ABPlayMode),
                            tr(lyra::i18n::StringId::PlaybackIdle));
                break;
            }
            s_ab_active = true;
            reset_ab_points();
            std::strncpy(s_ab_path, audio_status.path, sizeof(s_ab_path) - 1);
            s_ab_path[sizeof(s_ab_path) - 1] = '\0';
            lyra::audio::prepare_next(nullptr, audio_status.playback_serial, 0, 0, 0);
            render(View::Player);
            break;
        }
        case MoreAction::Bookmarks:
            show_player_bookmarks_overlay();
            break;
    }
}

void show_player_more_overlay()
{
    lv_obj_t *dialog = nullptr;
    if (!make_player_overlay(&s_more_overlay, 340, &dialog)) return;
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::More), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *close = make_button(dialog, 246, 7, 34, 34, kSurface, 17);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_more_overlay(); },
                        LV_EVENT_CLICKED, nullptr);

    const lyra::i18n::StringId labels[] = {
        lyra::i18n::StringId::Equalizer,
        lyra::i18n::StringId::Information,
        lyra::i18n::StringId::AddToPlaylistTitle,
        lyra::i18n::StringId::PlaybackSpeed,
        lyra::i18n::StringId::ABPlayMode,
        lyra::i18n::StringId::PlayerBookmarks,
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        lv_obj_t *choice = make_button(dialog, 10, 50 + static_cast<int>(i) * 45,
                                       268, 41, kSurface, 5, true);
        lv_obj_t *label = make_label(choice, tr(labels[i]), kTextPrimary);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *arrow = make_label(choice, LV_SYMBOL_RIGHT, kAccent);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_event_cb(choice, player_more_action_cb, LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
}

void player_ab_button_cb(lv_event_t *event)
{
    const auto action = static_cast<ABAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (action == ABAction::End) {
        s_ab_active = false;
        reset_ab_points();
        render(View::Player);
        return;
    }
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!s_ab_active || std::strcmp(audio_status.path, s_ab_path) != 0 ||
        audio_status.duration_ms == 0 || audio_status.last_error != ESP_OK) return;
    if (action == ABAction::SetA) {
        s_ab_a_ms = std::min(audio_status.position_ms,
                             audio_status.duration_ms - 1);
        s_ab_has_a = true;
        s_ab_has_b = false;
        s_ab_b_ms = 0;
    } else {
        const uint32_t latest_b = audio_status.duration_ms > 250 ?
            std::min(audio_status.position_ms, audio_status.duration_ms - 250) :
            audio_status.position_ms;
        if (!s_ab_has_a || latest_b <= s_ab_a_ms) {
            show_notice(tr(lyra::i18n::StringId::ABPlayMode),
                        tr(lyra::i18n::StringId::ABPointOrder));
            return;
        }
        s_ab_b_ms = latest_b;
        s_ab_has_b = true;
    }
    update_ab_button_style();
}

void make_ab_action_button(lv_obj_t *parent, int x, int y, const char *text,
                           ABAction action, bool selected, lv_obj_t **button_out)
{
    lv_obj_t *button = make_button(parent, x, y, 44, 38,
                                   selected ? kAccentSurface : kBackground, 7);
    lv_obj_t *label = make_label(button, text, selected ? kAccent : kTextPrimary);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, player_ab_button_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    if (button_out) *button_out = button;
}

void player_more_button_cb(lv_event_t *)
{
    show_player_more_overlay();
}

} // namespace

void reset_player_action_ui()
{
    s_more_overlay = nullptr;
    s_speed_overlay = nullptr;
    s_speed_value_label = nullptr;
    s_bookmarks_overlay = nullptr;
    s_ab_a_button = nullptr;
    s_ab_b_button = nullptr;
}

bool is_player_ab_mode_active()
{
    return s_ab_active;
}

bool update_player_ab_loop(const lyra::audio::Status &audio_status)
{
    if (!s_ab_active) return false;
    if (!audio_status.path[0] || std::strcmp(audio_status.path, s_ab_path) != 0) {
        s_ab_active = false;
        reset_ab_points();
        return true;
    }
    if (s_ab_has_a && s_ab_has_b && audio_status.playing && !audio_status.paused &&
        audio_status.position_ms >= s_ab_b_ms) {
        const esp_err_t result = lyra::audio::seek(s_ab_a_ms);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "could not repeat A-B playback: %s", esp_err_to_name(result));
        }
    }
    return false;
}

void make_player_action_buttons(lv_obj_t *parent, int y, bool favorite)
{
    if (s_ab_active) {
        make_ab_action_button(parent, 76, y, "A", ABAction::SetA, s_ab_has_a,
                              &s_ab_a_button);
        make_ab_action_button(parent, 136, y, "B", ABAction::SetB, s_ab_has_b,
                              &s_ab_b_button);
        make_ab_action_button(parent, 196, y, LV_SYMBOL_CLOSE, ABAction::End,
                              false, nullptr);
        return;
    }

    s_ab_a_button = nullptr;
    s_ab_b_button = nullptr;
    lv_obj_t *queue = make_button(parent, 76, y, 44, 38, kBackground, 7);
    lv_obj_t *queue_icon = make_label(queue, LV_SYMBOL_LIST, kTextSecondary);
    lv_obj_center(queue_icon);
    add_route(queue, View::Queue);

    lv_obj_t *more = make_button(parent, 136, y, 44, 38, kBackground, 7);
    lv_obj_t *more_label = make_label(more,
        tr(lyra::i18n::StringId::MoreEllipsis), kTextSecondary);
    lv_obj_set_style_text_font(more_label, &lv_font_montserrat_18, 0);
    lv_obj_center(more_label);
    lv_obj_add_event_cb(more, player_more_button_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *heart_button = make_button(parent, 196, y, 44, 38,
                                         kBackground, 7);
    lv_obj_t *heart = make_label(heart_button,
        favorite ? kHeartFilled : kHeartOutline,
        favorite ? kAccent : kTextPrimary);
    lv_obj_center(heart);
    lv_obj_add_event_cb(heart_button, toggle_favorite_cb, LV_EVENT_CLICKED, heart);
}

} // namespace lyra::gui::internal
