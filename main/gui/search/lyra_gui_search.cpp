/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "lyra_gui_ime.h"

#include <cstdlib>

namespace lyra::gui::internal {
namespace {

using KeyboardLayout = lyra::gui_settings::KeyboardLayout;

bool s_keyboard_picker_visible = false;
lyra::gui::ime::KanaModifier s_kana_modifier = lyra::gui::ime::KanaModifier::Dakuten;
bool s_kana_modifier_just_changed = false;
lv_obj_t *s_ime_candidates = nullptr;
size_t s_ime_candidate_page = 0;

char *keyboard_input_text()
{
    if (s_view == View::WifiPassword) return wifi_password_input_text();
    if (s_view == View::RadioAdd) return s_radio_url;
    if (s_view == View::FileNameInput) return file_name_input_text();
    return s_view == View::PlaylistCreate ? s_playlist_name : s_search_query;
}

size_t keyboard_input_capacity()
{
    if (s_view == View::WifiPassword) return wifi_password_input_capacity();
    if (s_view == View::RadioAdd) return sizeof(s_radio_url);
    if (s_view == View::FileNameInput) return file_name_input_capacity();
    return s_view == View::PlaylistCreate ? sizeof(s_playlist_name) : sizeof(s_search_query);
}

lyra::gui::ime::Layout ime_layout()
{
    return static_cast<lyra::gui::ime::Layout>(s_library_keyboard_layout);
}

const char *keyboard_short_name()
{
    switch (s_library_keyboard_layout) {
        case KeyboardLayout::English: return "EN";
        case KeyboardLayout::Russian: return "РУ";
        case KeyboardLayout::Korean: return "한";
        case KeyboardLayout::ChinesePinyin: return "拼";
        case KeyboardLayout::JapaneseKana: return "かな";
        case KeyboardLayout::Count: break;
    }
    return "EN";
}

const char *keyboard_name(KeyboardLayout layout)
{
    switch (layout) {
        case KeyboardLayout::English: return tr(lyra::i18n::StringId::KeyboardEnglish);
        case KeyboardLayout::Russian: return tr(lyra::i18n::StringId::KeyboardRussian);
        case KeyboardLayout::Korean: return tr(lyra::i18n::StringId::KeyboardKorean);
        case KeyboardLayout::ChinesePinyin: return tr(lyra::i18n::StringId::KeyboardChinesePinyin);
        case KeyboardLayout::JapaneseKana: return tr(lyra::i18n::StringId::KeyboardJapaneseKana);
        case KeyboardLayout::Count: break;
    }
    return tr(lyra::i18n::StringId::KeyboardEnglish);
}

lv_timer_t *s_live_search_timer = nullptr;
constexpr uint32_t kLiveSearchDelayMs = 650;

// Search as you type: once typing pauses, run the background search for the
// committed text. Results refresh in place, so the keyboard stays put.
void live_search_cb(lv_timer_t *)
{
    s_live_search_timer = nullptr; // One-shot timers delete themselves.
    if (s_view != View::Search || !s_search_query[0]) return;
    if (std::strcmp(s_search_query, s_submitted_search_query) == 0 &&
        s_search_category == s_submitted_search_category) return;
    if (lyra::media::search_status().running) {
        // Retry after the current search; start_search() rejects overlaps.
        s_live_search_timer = lv_timer_create(live_search_cb, kLiveSearchDelayMs, nullptr);
        if (s_live_search_timer) lv_timer_set_repeat_count(s_live_search_timer, 1);
        return;
    }
    if (lyra::media::start_search(s_search_category, s_search_query) != ESP_OK) return;
    copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
    s_submitted_search_category = s_search_category;
    s_search_error[0] = '\0';
    s_list_page = 0;
    populate_search_results();
}

void schedule_live_search()
{
    if (s_view != View::Search) return;
    if (s_live_search_timer) {
        lv_timer_reset(s_live_search_timer);
        return;
    }
    s_live_search_timer = lv_timer_create(live_search_cb, kLiveSearchDelayMs, nullptr);
    if (s_live_search_timer) lv_timer_set_repeat_count(s_live_search_timer, 1);
}

void update_keyboard_display()
{
    schedule_live_search();
    if (!s_search_label) return;
    const char *text = keyboard_input_text();
    if (!text[0] && !lyra::gui::ime::preedit()[0]) {
        update_search_label();
        return;
    }
    char display[lyra::media::kMaxPath];
    if (s_view == View::WifiPassword && !wifi_password_is_visible()) {
        const char *preedit = lyra::gui::ime::preedit();
        const size_t capacity = sizeof(display);
        size_t position = 0;
        const auto append_mask = [&display, capacity, &position](const char *value) {
            if (!value) return;
            for (const unsigned char *cursor =
                     reinterpret_cast<const unsigned char *>(value); *cursor; ++cursor) {
                if ((*cursor & 0xC0U) == 0x80U) continue;
                if (position + 3 >= capacity) break;
                display[position++] = '\xE2';
                display[position++] = '\x80';
                display[position++] = '\xA2';
            }
        };
        append_mask(text);
        append_mask(preedit);
        display[position] = '\0';
        lv_label_set_text(s_search_label, display);
        lv_obj_set_style_text_color(s_search_label, kTextPrimary, 0);
        return;
    }
    const size_t capacity = sizeof(display);
    const size_t length = std::min(std::strlen(text), capacity - 1);
    std::memcpy(display, text, length);
    display[length] = '\0';
    const char *preedit = lyra::gui::ime::preedit();
    const size_t preedit_length = std::strlen(preedit);
    if (length + preedit_length < capacity) {
        std::memcpy(display + length, preedit, preedit_length + 1);
    } else {
        display[capacity - 1] = '\0';
    }
    lv_label_set_text(s_search_label, display);
    lv_obj_set_style_text_color(s_search_label, kTextPrimary, 0);
}

} // namespace

void update_search_label()
{
    if (s_search_label == nullptr) return;
    const bool creating = s_view == View::PlaylistCreate;
    const bool wifi_password = s_view == View::WifiPassword;
    const bool radio_address = s_view == View::RadioAdd;
    const bool file_name = s_view == View::FileNameInput;
    const char *text = wifi_password ? wifi_password_input_text() :
                       file_name ? file_name_input_text() :
                       radio_address ? s_radio_url : creating ? s_playlist_name : s_search_query;
    const char *preedit = lyra::gui::ime::preedit();
    if (text[0] || preedit[0]) {
        update_keyboard_display();
        return;
    }
    const char *placeholder = wifi_password ? tr(lyra::i18n::StringId::EnterWifiPassword) :
        radio_address ? tr(lyra::i18n::StringId::StreamAddressHint) :
        file_name ? tr(lyra::i18n::StringId::FileNamePlaceholder) :
        creating ? tr(lyra::i18n::StringId::PlaylistNamePlaceholder) :
                   tr(lyra::i18n::StringId::SearchMusicPlaceholder);
    lv_label_set_text(s_search_label, text[0] == '\0' ? placeholder : text);
    lv_obj_set_style_text_color(s_search_label, text[0] == '\0' ? kTextMuted : kTextPrimary, 0);
}

const char *search_category_name(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return tr(lyra::i18n::StringId::Songs);
        case lyra::media::SearchCategory::Albums: return tr(lyra::i18n::StringId::Albums);
        case lyra::media::SearchCategory::Artists: return tr(lyra::i18n::StringId::Artists);
        case lyra::media::SearchCategory::Playlists: return tr(lyra::i18n::StringId::Playlists);
    }
    return tr(lyra::i18n::StringId::Songs);
}

const char *search_empty_message(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return tr(lyra::i18n::StringId::NoMatchingSongs);
        case lyra::media::SearchCategory::Albums: return tr(lyra::i18n::StringId::NoMatchingAlbums);
        case lyra::media::SearchCategory::Artists: return tr(lyra::i18n::StringId::NoMatchingArtists);
        case lyra::media::SearchCategory::Playlists: return tr(lyra::i18n::StringId::NoPlaylistSongsMatch);
    }
    return tr(lyra::i18n::StringId::NoMatches);
}

void populate_search_results()
{
    if (!s_search_results) return;
    lv_obj_clean(s_search_results);
    if (!s_search_query[0]) {
        make_label(s_search_results, tr(lyra::i18n::StringId::TypeSearchThenTap), kTextMuted);
        return;
    }
    const lyra::media::SearchStatus status = lyra::media::search_status();
    if (status.running) {
        lv_obj_t *spinner = lv_spinner_create(s_search_results);
        lv_obj_set_size(spinner, 48, 48);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 12);
        lv_spinner_set_anim_params(spinner, 800, 250);
        lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
        char progress[48];
        format_u32_u32(lyra::i18n::StringId::SearchingProgress,
                       static_cast<uint32_t>(status.processed),
                       static_cast<uint32_t>(status.total), progress, sizeof(progress));
        s_search_progress_label = make_label(s_search_results, progress, kTextSecondary);
        lv_obj_align(s_search_progress_label, LV_ALIGN_TOP_MID, 0, 72);
        return;
    }
    if (s_search_error[0]) {
        make_label(s_search_results, s_search_error, kTextMuted);
        return;
    }
    if (!status.ready || std::strcmp(s_submitted_search_query, s_search_query) != 0 ||
        s_submitted_search_category != s_search_category) {
        char prompt[64];
        format_text(lyra::i18n::StringId::TapSearchToFind,
                    search_category_name(s_search_category), prompt, sizeof(prompt));
        make_label(s_search_results, prompt, kTextMuted);
        return;
    }
    const size_t page_size = search_page_size();
    lyra::media::SearchResult results[kSearchPageCapacity]{};
    size_t total = 0;
    size_t count = lyra::media::search_results(s_list_page * page_size, results, page_size, &total);
    const size_t pages = (total + page_size - 1) / page_size;
    if (pages && s_list_page >= pages) {
        s_list_page = pages - 1;
        count = lyra::media::search_results(s_list_page * page_size, results, page_size, &total);
    }
    for (size_t i = 0; i < count; ++i) {
        const int y = static_cast<int>(i) * 58;
        const lyra::media::SearchResult &result = results[i];
        if (s_search_category == lyra::media::SearchCategory::Songs) {
            make_song_row(s_search_results, y, result.track_index, 54, true);
        } else if (s_search_category == lyra::media::SearchCategory::Playlists) {
            make_search_playlist_row(s_search_results, y, result);
        } else {
            const lyra::media::GroupKind kind =
                s_search_category == lyra::media::SearchCategory::Albums ?
                lyra::media::GroupKind::Album : lyra::media::GroupKind::Artist;
            lyra::media::Group group{};
            if (!lyra::media::group_at(kind, result.group_index, &group)) continue;
            char subtitle[32];
            format_count(lyra::i18n::StringId::SongCount,
                         static_cast<uint32_t>(group.track_count), subtitle, sizeof(subtitle));
            const View target = kind == lyra::media::GroupKind::Album ?
                                View::AlbumDetail : View::TrackList;
            lv_obj_t *row = make_row(s_search_results, y, nullptr, group.name,
                                     subtitle, target, 54);
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row,
                kind == lyra::media::GroupKind::Album ? search_album_result_cb : search_artist_result_cb,
                LV_EVENT_CLICKED, reinterpret_cast<void *>(result.group_index));
            if (kind == lyra::media::GroupKind::Album) {
                if (s_queue_add_mode) {
                    lv_obj_t *queue_button = make_button(row, 270, 12, 24, 30,
                                                          kAccentDark, 5);
                    lv_obj_t *queue_icon = make_label(queue_button, LV_SYMBOL_PLUS,
                                                      kTextOnAccent);
                    lv_obj_center(queue_icon);
                    lv_obj_add_event_cb(queue_button, [](lv_event_t *event) {
                        lv_event_stop_bubbling(event);
                        show_queue_add_picker_for_album(reinterpret_cast<uintptr_t>(
                            lv_event_get_user_data(event)));
                    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(result.group_index));
                }
            }
        }
    }
    make_page_controls(s_search_results, static_cast<int>(count) * 58 + 4, total, page_size);
    if (count == 0) make_label(s_search_results, search_empty_message(s_search_category), kTextMuted);
}

void search_category_cb(lv_event_t *event)
{
    const auto category = static_cast<lyra::media::SearchCategory>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (category == s_search_category) return;
    s_search_category = category;
    s_list_page = 0;
    s_search_error[0] = '\0';
    const lyra::media::SearchStatus status = lyra::media::search_status();
    if (s_search_query[0] && !status.running) {
        const esp_err_t result = lyra::media::start_search(s_search_category, s_search_query);
        if (result == ESP_OK) {
            copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
            s_submitted_search_category = s_search_category;
        } else {
            copy_ui_text(s_search_error, sizeof(s_search_error),
                         tr(lyra::i18n::StringId::SearchBusy));
        }
    }
    render(View::Search);
}

namespace {

enum class KeyboardCommand : uintptr_t {
    ToggleSymbols = 1,
    Backspace,
    Space,
    Submit,
    Shift,
};

enum class KeyboardShift : uint8_t {
    Off,
    OneShot,
    CapsLock,
};

struct KeyboardTextKey {
    const char *lower = nullptr;
    char upper[8]{};
    lv_obj_t *label = nullptr;
    bool shiftable = false;
    bool pressed_shifted = false;
};

struct KeyboardCaseMapping {
    const char *lower;
    const char *upper;
};

constexpr KeyboardCaseMapping kKeyboardCaseMappings[] = {
    {"à", "À"}, {"á", "Á"}, {"â", "Â"}, {"ä", "Ä"}, {"æ", "Æ"},
    {"ç", "Ç"}, {"è", "È"}, {"é", "É"}, {"ê", "Ê"}, {"ë", "Ë"},
    {"ì", "Ì"}, {"í", "Í"}, {"î", "Î"}, {"ï", "Ï"}, {"ñ", "Ñ"},
    {"ò", "Ò"}, {"ó", "Ó"}, {"ô", "Ô"}, {"ö", "Ö"}, {"œ", "Œ"},
    {"ù", "Ù"}, {"ú", "Ú"}, {"û", "Û"}, {"ü", "Ü"}, {"ý", "Ý"},
    {"ÿ", "Y"}, {"ß", "SS"}, {"ø", "Ø"}, {"å", "Å"}, {"ё", "Ё"},
};

constexpr size_t kMaxKeyboardTextKeys = 40;
constexpr uint32_t kKeyboardDoubleTapMs = 450;
KeyboardTextKey s_keyboard_text_keys[kMaxKeyboardTextKeys]{};
size_t s_keyboard_text_key_count = 0;
KeyboardShift s_keyboard_shift = KeyboardShift::Off;
uint32_t s_last_keyboard_shift_tap = 0;
bool s_keyboard_shift_tap_pending = false;
lv_obj_t *s_keyboard_shift_key = nullptr;
lv_obj_t *s_keyboard_shift_label = nullptr;
View s_keyboard_shift_view = View::Search;
bool s_keyboard_shift_view_valid = false;

struct KanaFlickKey {
    const char *variants[5]; // centre, up, right, down, left
    lv_point_t start;
    lv_obj_t *popup_keys[5]{};
    lv_obj_t *popup_labels[5]{};
};

KanaFlickKey s_kana_flick_keys[10]{};

void keyboard_action_cb(lv_event_t *event);
void keyboard_text_cb(lv_event_t *event);
void t9_key_cb(lv_event_t *event);
void kana_flick_cb(lv_event_t *event);
void ime_candidate_cb(lv_event_t *event);
void ime_candidate_page_cb(lv_event_t *event);
void kana_modifier_cb(lv_event_t *event);
void update_keyboard_shift_display();

bool uppercase_keyboard_text(const char *lower, char *upper, size_t capacity)
{
    if (!lower || !upper || capacity == 0) return false;
    for (const KeyboardCaseMapping &mapping : kKeyboardCaseMappings) {
        if (std::strcmp(lower, mapping.lower) != 0) continue;
        const size_t length = std::strlen(mapping.upper);
        if (length >= capacity) return false;
        std::memcpy(upper, mapping.upper, length + 1);
        return true;
    }

    const size_t length = std::strlen(lower);
    if (length == 1 && lower[0] >= 'a' && lower[0] <= 'z') {
        if (capacity < 2) return false;
        upper[0] = static_cast<char>(lower[0] - ('a' - 'A'));
        upper[1] = '\0';
        return true;
    }
    if (length == 2 && static_cast<uint8_t>(lower[0]) == 0xD0 &&
        static_cast<uint8_t>(lower[1]) >= 0xB0 &&
        static_cast<uint8_t>(lower[1]) <= 0xBF) {
        if (capacity < 3) return false;
        upper[0] = static_cast<char>(0xD0);
        upper[1] = static_cast<char>(static_cast<uint8_t>(lower[1]) - 0x20);
        upper[2] = '\0';
        return true;
    }
    if (length == 2 && static_cast<uint8_t>(lower[0]) == 0xD1 &&
        static_cast<uint8_t>(lower[1]) >= 0x80 &&
        static_cast<uint8_t>(lower[1]) <= 0x8F) {
        if (capacity < 3) return false;
        upper[0] = static_cast<char>(0xD0);
        upper[1] = static_cast<char>(static_cast<uint8_t>(lower[1]) - 0x20);
        upper[2] = '\0';
        return true;
    }
    return false;
}

void update_keyboard_shift_display()
{
    const bool shifted = s_keyboard_shift != KeyboardShift::Off;
    for (size_t index = 0; index < s_keyboard_text_key_count; ++index) {
        KeyboardTextKey &context = s_keyboard_text_keys[index];
        if (!context.shiftable || !context.label) continue;
        lv_label_set_text(context.label,
            (shifted || context.pressed_shifted) ? context.upper : context.lower);
    }
    if (!s_keyboard_shift_key || !s_keyboard_shift_label) return;
    const lv_color_t background = s_keyboard_shift == KeyboardShift::CapsLock ? kAccent :
                                  s_keyboard_shift == KeyboardShift::OneShot ? kAccentDark :
                                  kSurfaceRaised;
    const lv_color_t foreground = s_keyboard_shift == KeyboardShift::Off ? kTextPrimary :
                                                                             kTextOnAccent;
    lv_obj_set_style_bg_color(s_keyboard_shift_key, background, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_keyboard_shift_key, background, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keyboard_shift_label, foreground, LV_STATE_DEFAULT);
}

void reset_keyboard_shift()
{
    s_keyboard_shift = KeyboardShift::Off;
    s_keyboard_shift_tap_pending = false;
    update_keyboard_shift_display();
}

void toggle_keyboard_shift()
{
    const uint32_t now = lv_tick_get();
    if (s_keyboard_shift == KeyboardShift::CapsLock) {
        s_keyboard_shift = KeyboardShift::Off;
        s_keyboard_shift_tap_pending = false;
    } else if (s_keyboard_shift == KeyboardShift::OneShot) {
        const bool double_tap = s_keyboard_shift_tap_pending &&
            static_cast<uint32_t>(now - s_last_keyboard_shift_tap) <= kKeyboardDoubleTapMs;
        s_keyboard_shift = double_tap ? KeyboardShift::CapsLock : KeyboardShift::Off;
        s_keyboard_shift_tap_pending = false;
    } else {
        s_keyboard_shift = KeyboardShift::OneShot;
        s_last_keyboard_shift_tap = now;
        s_keyboard_shift_tap_pending = true;
    }
    update_keyboard_shift_display();
}

void make_keyboard_shift_key(lv_obj_t *parent, int x, int y, int width, int height)
{
    s_keyboard_shift_key = make_button(parent, x, y, width, height, kSurfaceRaised, 5);
    s_keyboard_shift_label = make_label(s_keyboard_shift_key, LV_SYMBOL_UP, kTextPrimary);
    lv_obj_center(s_keyboard_shift_label);
    lv_obj_add_event_cb(s_keyboard_shift_key, keyboard_action_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(KeyboardCommand::Shift)));
    update_keyboard_shift_display();
}

void make_keyboard_command_key(lv_obj_t *parent, int x, int y, int width, int height,
                               const char *label_text, KeyboardCommand command)
{
    lv_obj_t *key = make_button(parent, x, y, width, height, kSurfaceRaised, 5);
    lv_obj_t *label = make_label(key, label_text, kTextPrimary);
    lv_obj_center(label);
    lv_obj_add_event_cb(key, keyboard_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(command)));
}

void make_keyboard_selector(lv_obj_t *parent, int x, int y, int width, int height,
                            bool icon_only = false)
{
    lv_obj_t *button = make_button(parent, x, y, width, height, kSurfaceRaised, 7);
    lv_obj_t *label = make_label(button,
        icon_only ? LV_SYMBOL_KEYBOARD : keyboard_short_name(), kAccent);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, keyboard_selector_cb, LV_EVENT_CLICKED, nullptr);
}

void make_keyboard_text_key(lv_obj_t *parent, int x, int y, int width, int height,
                            const char *text)
{
    if (std::strcmp(text, "<") == 0) {
        make_keyboard_command_key(parent, x, y, width, height, LV_SYMBOL_BACKSPACE,
                                 KeyboardCommand::Backspace);
        return;
    }
    if (s_keyboard_text_key_count >= kMaxKeyboardTextKeys) return;
    lv_obj_t *key = make_button(parent, x, y, width, height, kSurfaceRaised, 5);
    KeyboardTextKey &context = s_keyboard_text_keys[s_keyboard_text_key_count++];
    context = {};
    context.lower = text;
    context.shiftable = uppercase_keyboard_text(text, context.upper, sizeof(context.upper));
    context.label = make_label(key, text, kTextPrimary);
    lv_obj_center(context.label);
    lv_obj_add_event_cb(key, keyboard_text_cb, LV_EVENT_PRESSED, &context);
    lv_obj_add_event_cb(key, keyboard_text_cb, LV_EVENT_LONG_PRESSED_REPEAT, &context);
    lv_obj_add_event_cb(key, keyboard_text_cb, LV_EVENT_RELEASED, &context);
    lv_obj_add_event_cb(key, keyboard_text_cb, LV_EVENT_PRESS_LOST, &context);
}

void make_keyboard_text_row(lv_obj_t *parent, int y, const char *keys[], size_t count,
                            int inset, int height)
{
    const int gap = 3;
    const int width = (320 - inset * 2 - gap * static_cast<int>(count - 1)) /
                      static_cast<int>(count);
    for (size_t index = 0; index < count; ++index) {
        make_keyboard_text_key(parent, inset + static_cast<int>(index) * (width + gap), y,
                               width, height, keys[index]);
    }
}

void make_keyboard_shift_text_row(lv_obj_t *parent, int y, const char *keys[], size_t count,
                                  int height)
{
    const int gap = 3;
    const size_t columns = count + 1;
    const int width = (320 - 6 - gap * static_cast<int>(columns - 1)) /
                      static_cast<int>(columns);
    make_keyboard_shift_key(parent, 3, y, width, height);
    for (size_t index = 0; index < count; ++index) {
        make_keyboard_text_key(parent, 3 + static_cast<int>(index + 1) * (width + gap), y,
                               width, height, keys[index]);
    }
}

void make_keyboard_command_row(lv_obj_t *parent, int y, const char *labels[],
                               const KeyboardCommand commands[], size_t count, int height)
{
    const int gap = 3;
    const int inset = 3;
    const int width = (320 - inset * 2 - gap * static_cast<int>(count - 1)) /
                      static_cast<int>(count);
    for (size_t index = 0; index < count; ++index) {
        make_keyboard_command_key(parent, inset + static_cast<int>(index) * (width + gap), y,
                                  width, height, labels[index], commands[index]);
    }
}

void refresh_ime_candidates()
{
    if (!s_ime_candidates) return;
    lv_obj_clean(s_ime_candidates);
    const auto layout = ime_layout();
    if (!lyra::gui::ime::dictionary_available(layout)) {
        lv_obj_t *label = make_label(s_ime_candidates,
            tr(lyra::i18n::StringId::ImeDictionaryInstallHint), kTextMuted);
        lv_obj_center(label);
        return;
    }
    const size_t count = lyra::gui::ime::candidate_count(layout);
    if (count == 0) {
        if (lyra::gui::ime::preedit()[0]) {
            lv_obj_t *label = make_label(s_ime_candidates, lyra::gui::ime::preedit(), kTextMuted);
            lv_obj_center(label);
        }
        return;
    }
    constexpr size_t page_size = 5;
    constexpr int gap = 2;
    constexpr int inset = 2;
    const size_t page_count = (count + page_size - 1) / page_size;
    if (s_ime_candidate_page >= page_count) s_ime_candidate_page = page_count - 1;
    const size_t start = s_ime_candidate_page * page_size;
    const size_t visible = std::min(page_size, count - start);
    const bool paged = page_count > 1;
    const int arrow_width = paged ? 24 : 0;
    const int available = 320 - inset * 2 - arrow_width * 2 - gap *
        static_cast<int>(visible - 1) - (paged ? gap * 2 : 0);
    const int width = available / static_cast<int>(visible);
    int x = inset;
    if (paged) {
        lv_obj_t *previous = make_button(s_ime_candidates, x, 0, arrow_width, 30,
                                         kSurfaceRaised, 4);
        lv_obj_t *previous_label = make_label(previous, LV_SYMBOL_LEFT, kTextPrimary);
        lv_obj_center(previous_label);
        lv_obj_add_event_cb(previous, ime_candidate_page_cb, LV_EVENT_CLICKED, nullptr);
        x += arrow_width + gap;
    }
    for (size_t visible_index = 0; visible_index < visible; ++visible_index) {
        const size_t index = start + visible_index;
        lv_obj_t *button = make_button(s_ime_candidates,
            x + static_cast<int>(visible_index) * (width + gap), 0, width, 30,
            kSurfaceRaised, 4);
        lv_obj_t *label = make_label(button, lyra::gui::ime::candidate(layout, index), kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, ime_candidate_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(index + 1)));
    }
    if (paged) {
        const int right_x = 320 - inset - arrow_width;
        lv_obj_t *next = make_button(s_ime_candidates, right_x, 0, arrow_width, 30,
                                     kSurfaceRaised, 4);
        lv_obj_t *next_label = make_label(next, LV_SYMBOL_RIGHT, kTextPrimary);
        lv_obj_center(next_label);
        lv_obj_add_event_cb(next, ime_candidate_page_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(1)));
    }
}

void make_keyboard_picker(lv_obj_t *keyboard)
{
    static constexpr KeyboardLayout layouts[] = {
        KeyboardLayout::English, KeyboardLayout::Russian, KeyboardLayout::Korean,
        KeyboardLayout::ChinesePinyin, KeyboardLayout::JapaneseKana,
    };
    const int gap = 2;
    const int row_height = (library_keyboard_height() - 8 - gap * 4) / 5;
    for (size_t index = 0; index < sizeof(layouts) / sizeof(layouts[0]); ++index) {
        const bool selected = layouts[index] == s_library_keyboard_layout;
        lv_obj_t *button = make_button(keyboard, 5, 4 + static_cast<int>(index) * (row_height + gap),
            310, row_height, selected ? kAccentDark : kSurfaceRaised, 5);
        lv_obj_t *label = make_label(button, keyboard_name(layouts[index]),
                                     selected ? kTextOnAccent : kTextPrimary);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_add_event_cb(button, [](lv_event_t *event) {
            const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            if (encoded == 0 || encoded > static_cast<uintptr_t>(KeyboardLayout::Count)) return;
            commit_keyboard_preedit();
            s_library_keyboard_layout = static_cast<KeyboardLayout>(encoded - 1);
            s_library_keyboard_symbols = false;
            s_library_keyboard_accents = false;
            s_kana_modifier = lyra::gui::ime::KanaModifier::Dakuten;
            s_kana_modifier_just_changed = false;
            s_ime_candidate_page = 0;
            lyra::gui::ime::reset();
            s_keyboard_picker_visible = false;
            if (s_view != View::RadioAdd) save_user_settings();
            render(s_view);
        }, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(layouts[index]) + 1));
    }
}

void make_kana_keyboard(lv_obj_t *keyboard, const char *action)
{
    static constexpr const char *variants[10][5] = {
        {"あ", "う", "え", "お", "い"}, {"か", "く", "け", "こ", "き"},
        {"さ", "す", "せ", "そ", "し"}, {"た", "つ", "て", "と", "ち"},
        {"な", "ぬ", "ね", "の", "に"}, {"は", "ふ", "へ", "ほ", "ひ"},
        {"ま", "む", "め", "も", "み"}, {"や", "ゆ", "", "よ", ""},
        {"ら", "る", "れ", "ろ", "り"}, {"わ", "ん", "ー", "、", "を"},
    };
    const int gap = 2;
    s_ime_candidates = make_box(keyboard, 0, 2, 320, 30, kKeyboardSurface);
    refresh_ime_candidates();
    const int first_y = 34;
    const int key_height = (library_keyboard_height() - first_y -
                            kKeyboardBottomPadding - gap * 4) / 5;
    const int key_width = (320 - 6 - gap * 2) / 3;
    for (size_t index = 0; index < 10; ++index) {
        KanaFlickKey &context = s_kana_flick_keys[index];
        for (size_t variant = 0; variant < 5; ++variant) {
            context.variants[variant] = variants[index][variant];
            context.popup_keys[variant] = nullptr;
            context.popup_labels[variant] = nullptr;
        }
        const int row = static_cast<int>(index / 3);
        const int column = static_cast<int>(index % 3);
        lv_obj_t *key = make_button(keyboard, 3 + column * (key_width + gap),
            first_y + row * (key_height + gap), key_width, key_height, kSurfaceRaised, 5);
        lv_obj_t *label = make_label(key, variants[index][0], kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(key, kana_flick_cb, LV_EVENT_PRESSED, &context);
        lv_obj_add_event_cb(key, kana_flick_cb, LV_EVENT_PRESSING, &context);
        lv_obj_add_event_cb(key, kana_flick_cb, LV_EVENT_RELEASED, &context);
        lv_obj_add_event_cb(key, kana_flick_cb, LV_EVENT_PRESS_LOST, &context);
    }
    const int row4_y = first_y + 3 * (key_height + gap);
    lv_obj_t *modifier = make_button(keyboard, 3 + key_width + gap, row4_y,
                                     key_width, key_height, kSurfaceRaised, 5);
    const char *modifier_text = s_kana_modifier == lyra::gui::ime::KanaModifier::Dakuten ?
        "゛" : s_kana_modifier == lyra::gui::ime::KanaModifier::Handakuten ? "゜" : "小";
    lv_obj_t *modifier_label = make_label(modifier, modifier_text, kTextPrimary);
    lv_obj_center(modifier_label);
    lv_obj_add_event_cb(modifier, kana_modifier_cb, LV_EVENT_PRESSED, modifier_label);
    lv_obj_add_event_cb(modifier, kana_modifier_cb, LV_EVENT_CLICKED, modifier_label);
    lv_obj_add_event_cb(modifier, kana_modifier_cb, LV_EVENT_LONG_PRESSED, modifier_label);
    make_keyboard_command_key(keyboard, 3 + (key_width + gap) * 2, row4_y, key_width, key_height,
        LV_SYMBOL_BACKSPACE, KeyboardCommand::Backspace);
    const char *labels[] = {
        s_library_keyboard_symbols ? tr(lyra::i18n::StringId::LettersKey) :
                                    tr(lyra::i18n::StringId::NumbersKey),
        tr(lyra::i18n::StringId::SpaceKey), action,
    };
    const KeyboardCommand commands[] = {
        KeyboardCommand::ToggleSymbols, KeyboardCommand::Space, KeyboardCommand::Submit,
    };
    make_keyboard_command_row(keyboard, first_y + 4 * (key_height + gap), labels, commands, 3,
                              key_height);
}

const char *keyboard_toggle_label()
{
    if (s_library_keyboard_layout == KeyboardLayout::English && s_view != View::RadioAdd) {
        return s_library_keyboard_accents ? tr(lyra::i18n::StringId::NumbersKey) :
            (s_library_keyboard_symbols ? tr(lyra::i18n::StringId::LettersKey) : "é");
    }
    return s_library_keyboard_symbols ? tr(lyra::i18n::StringId::LettersKey) :
                                        tr(lyra::i18n::StringId::NumbersKey);
}

struct T9KeyLabel {
    char key;
    const char *digit;
    const char *letters;
};

// Keypad order is 1-9, *, 0; the twelfth key is always backspace.
constexpr size_t kT9LabelCount = 11;
constexpr T9KeyLabel kLatinT9Labels[kT9LabelCount] = {
    {'1', "1", ".,?!"}, {'2', "2", "abc"}, {'3', "3", "def"},
    {'4', "4", "ghi"}, {'5', "5", "jkl"}, {'6', "6", "mno"},
    {'7', "7", "pqrs"}, {'8', "8", "tuv"}, {'9', "9", "wxyz"},
    {'*', nullptr, nullptr}, {'0', "0", nullptr},
};
constexpr T9KeyLabel kRussianT9Labels[kT9LabelCount] = {
    {'1', "1", ".,?!"}, {'2', "2", "абвг"}, {'3', "3", "деёжз"},
    {'4', "4", "ийкл"}, {'5', "5", "мноп"}, {'6', "6", "рсту"},
    {'7', "7", "фхцч"}, {'8', "8", "шщъы"}, {'9', "9", "ьэюя"},
    {'*', nullptr, nullptr}, {'0', "0", nullptr},
};
constexpr T9KeyLabel kKoreanT9Labels[kT9LabelCount] = {
    {'1', nullptr, "ㅣ"}, {'2', nullptr, "ㆍ"}, {'3', nullptr, "ㅡ"},
    {'4', nullptr, "ㄱㅋ"}, {'5', nullptr, "ㄴㄹ"}, {'6', nullptr, "ㄷㅌ"},
    {'7', nullptr, "ㅂㅍ"}, {'8', nullptr, "ㅅㅎ"}, {'9', nullptr, "ㅈㅊ"},
    {'*', nullptr, ".,?!"}, {'0', nullptr, "ㅇㅁ"},
};

void make_t9_key(lv_obj_t *parent, int x, int y, int width, int height,
                 const T9KeyLabel &label)
{
    lv_obj_t *key = make_button(parent, x, y, width, height, kSurfaceRaised, 5);
    if (label.letters) {
        lv_obj_t *letters = make_label(key, label.letters, kTextPrimary);
        lv_obj_center(letters);
        if (label.digit) {
            lv_obj_t *digit = make_label(key, label.digit, kTextMuted);
            lv_obj_align(digit, LV_ALIGN_LEFT_MID, 8, 0);
        }
    } else {
        lv_obj_t *digit = make_label(key, label.digit, kTextPrimary);
        lv_obj_center(digit);
    }
    lv_obj_add_event_cb(key, t9_key_cb, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(label.key)));
}

void make_t9_keyboard(lv_obj_t *keyboard, const char *action)
{
    const KeyboardLayout layout = s_library_keyboard_layout;
    const T9KeyLabel *labels = layout == KeyboardLayout::Russian ? kRussianT9Labels :
                               layout == KeyboardLayout::Korean ? kKoreanT9Labels :
                                                                  kLatinT9Labels;
    // Korean puts punctuation on *, so only the alphabets get a shift key.
    const bool has_shift = layout != KeyboardLayout::Korean;
    const int gap = 2;
    int first_y = 4;
    if (layout == KeyboardLayout::ChinesePinyin) {
        s_ime_candidates = make_box(keyboard, 0, 2, 320, 30, kKeyboardSurface);
        refresh_ime_candidates();
        first_y = 34;
    }
    const int key_height = (library_keyboard_height() - first_y -
                            kKeyboardBottomPadding - gap * 4) / 5;
    const int key_width = (320 - 6 - gap * 2) / 3;
    for (size_t index = 0; index <= kT9LabelCount; ++index) {
        const int x = 3 + static_cast<int>(index % 3) * (key_width + gap);
        const int y = first_y + static_cast<int>(index / 3) * (key_height + gap);
        if (index == kT9LabelCount) {
            make_keyboard_command_key(keyboard, x, y, key_width, key_height,
                                      LV_SYMBOL_BACKSPACE, KeyboardCommand::Backspace);
        } else if (labels[index].key == '*' && has_shift) {
            make_keyboard_shift_key(keyboard, x, y, key_width, key_height);
        } else {
            make_t9_key(keyboard, x, y, key_width, key_height, labels[index]);
        }
    }
    const char *command_labels[] = {
        keyboard_toggle_label(), tr(lyra::i18n::StringId::SpaceKey), action,
    };
    const KeyboardCommand commands[] = {
        KeyboardCommand::ToggleSymbols, KeyboardCommand::Space, KeyboardCommand::Submit,
    };
    make_keyboard_command_row(keyboard, first_y + 4 * (key_height + gap), command_labels,
                              commands, 3, key_height);
}

void make_library_keyboard_impl(lv_obj_t *keyboard, const char *action)
{
    lyra::gui::ime::set_t9_mode(s_t9_input);
    if (!s_keyboard_shift_view_valid || s_keyboard_shift_view != s_view) {
        s_keyboard_shift = KeyboardShift::Off;
        s_keyboard_shift_tap_pending = false;
        s_keyboard_shift_view = s_view;
        s_keyboard_shift_view_valid = true;
    }
    s_keyboard_text_key_count = 0;
    s_keyboard_shift_key = nullptr;
    s_keyboard_shift_label = nullptr;
    s_ime_candidates = nullptr;
    if (s_keyboard_picker_visible) {
        make_keyboard_picker(keyboard);
        return;
    }
    const char *letters_row1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    const char *letters_row2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    const char *letters_row3[] = {"z", "x", "c", "v", "b", "n", "m", "<"};
    const char *symbols_row1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    const char *symbols_row2[] = {"@", "#", "$", "_", "&", "-", "+", ":", "/"};
    const char *symbols_row3[] = {".", ",", "?", "=", "'", "\"", "%", "<"};
    const char *accent_row1[] = {"à", "á", "â", "ä", "æ", "ç", "è", "é", "ê", "ë"};
    const char *accent_row2[] = {"ì", "í", "î", "ï", "ñ", "ò", "ó", "ô", "ö", "œ"};
    const char *accent_row3[] = {"ù", "ú", "û", "ü", "ý", "ÿ", "ß", "ø", "å", "<"};
    const char *russian_row1[] = {"ё", "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х"};
    const char *russian_row2[] = {"ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э"};
    const char *russian_row3[] = {"я", "ч", "с", "м", "и", "т", "ь", "б", "ю", "ъ", "<"};
    const char *korean_row1[] = {"ㅂ", "ㅈ", "ㄷ", "ㄱ", "ㅅ", "ㅛ", "ㅕ", "ㅑ", "ㅐ", "ㅔ"};
    const char *korean_row2[] = {"ㅁ", "ㄴ", "ㅇ", "ㄹ", "ㅎ", "ㅗ", "ㅓ", "ㅏ", "ㅣ"};
    const char *korean_row3[] = {"ㅋ", "ㅌ", "ㅊ", "ㅍ", "ㅠ", "ㅜ", "ㅡ", "<"};
    const char **row1 = letters_row1;
    const char **row2 = letters_row2;
    const char **row3 = letters_row3;
    size_t count1 = 10;
    size_t count2 = 9;
    size_t count3 = 8;
    int key_height = s_show_nav ? kKeyboardKeyHeightWithNav : kKeyboardKeyHeightWithoutNav;
    int first_y = 0;
    int row_gap = kKeyboardRowGap;

    if (s_library_keyboard_layout == KeyboardLayout::JapaneseKana) {
        if (s_library_keyboard_symbols) {
            const int symbol_height = s_show_nav ? kKeyboardKeyHeightWithNav :
                                                  kKeyboardKeyHeightWithoutNav;
            const int symbol_y = library_keyboard_height() - kKeyboardBottomPadding -
                (symbol_height * 4 + kKeyboardRowGap * 3);
            make_keyboard_text_row(keyboard, symbol_y, symbols_row1, 10, 3, symbol_height);
            make_keyboard_text_row(keyboard, symbol_y + symbol_height + kKeyboardRowGap,
                                   symbols_row2, 9, 16, symbol_height);
            make_keyboard_text_row(keyboard, symbol_y + (symbol_height + kKeyboardRowGap) * 2,
                                   symbols_row3, 8, 24, symbol_height);
            const char *labels[] = {tr(lyra::i18n::StringId::LettersKey),
                                    tr(lyra::i18n::StringId::SpaceKey), action};
            const KeyboardCommand commands[] = {KeyboardCommand::ToggleSymbols,
                KeyboardCommand::Space, KeyboardCommand::Submit};
            make_keyboard_command_row(keyboard,
                symbol_y + (symbol_height + kKeyboardRowGap) * 3,
                labels, commands, 3, symbol_height);
            return;
        }
        make_kana_keyboard(keyboard, action);
        return;
    }

    if (s_t9_input && !s_library_keyboard_symbols && !s_library_keyboard_accents) {
        make_t9_keyboard(keyboard, action);
        return;
    }

    if (s_library_keyboard_layout == KeyboardLayout::Russian && !s_library_keyboard_symbols) {
        row1 = russian_row1; row2 = russian_row2; row3 = russian_row3;
        count1 = 12; count2 = 11; count3 = 11;
    } else if (s_library_keyboard_layout == KeyboardLayout::Korean && !s_library_keyboard_symbols) {
        row1 = korean_row1; row2 = korean_row2; row3 = korean_row3;
    } else if (s_library_keyboard_layout == KeyboardLayout::English && s_library_keyboard_accents) {
        row1 = accent_row1; row2 = accent_row2; row3 = accent_row3;
        count2 = 10; count3 = 10;
    } else if (s_library_keyboard_symbols) {
        row1 = symbols_row1; row2 = symbols_row2; row3 = symbols_row3;
    }

    if (s_library_keyboard_layout == KeyboardLayout::ChinesePinyin) {
        constexpr int candidate_height = 34;
        key_height = s_show_nav ? 39 : 49;
        row_gap = 3;
        first_y = candidate_height + 4;
        s_ime_candidates = make_box(keyboard, 0, 2, 320, 30, kKeyboardSurface);
        refresh_ime_candidates();
    } else {
        const int rows_height = key_height * 4 + row_gap * 3;
        first_y = library_keyboard_height() - kKeyboardBottomPadding - rows_height;
    }

    make_keyboard_text_row(keyboard, first_y, row1, count1, count1 == 12 ? 2 : 3, key_height);
    make_keyboard_text_row(keyboard, first_y + key_height + row_gap, row2, count2, 16, key_height);
    const int third_row_y = first_y + (key_height + row_gap) * 2;
    const bool has_shift = !s_library_keyboard_symbols &&
        (s_library_keyboard_layout == KeyboardLayout::English ||
         s_library_keyboard_layout == KeyboardLayout::Russian ||
         s_library_keyboard_layout == KeyboardLayout::ChinesePinyin);
    if (has_shift) {
        make_keyboard_shift_text_row(keyboard, third_row_y, row3, count3, key_height);
    } else {
        make_keyboard_text_row(keyboard, third_row_y, row3, count3,
                               count3 >= 10 ? 3 : 24, key_height);
    }
    update_keyboard_shift_display();

    const char *action_labels[] = {
        keyboard_toggle_label(), tr(lyra::i18n::StringId::SpaceKey), action,
    };
    const KeyboardCommand action_commands[] = {
        KeyboardCommand::ToggleSymbols, KeyboardCommand::Space, KeyboardCommand::Submit,
    };
    make_keyboard_command_row(keyboard, first_y + (key_height + row_gap) * 3,
                              action_labels, action_commands, 3, key_height);
}

void keyboard_text_cb(lv_event_t *event)
{
    auto *context = static_cast<KeyboardTextKey *>(lv_event_get_user_data(event));
    if (!context) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        context->pressed_shifted = false;
        update_keyboard_shift_display();
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        context->pressed_shifted = context->shiftable &&
                                   s_keyboard_shift != KeyboardShift::Off;
    }
    const char *key = context->pressed_shifted ? context->upper : context->lower;
    lyra::gui::ime::process_key(ime_layout(), key, keyboard_input_text(), keyboard_input_capacity());
    if (code == LV_EVENT_PRESSED && context->pressed_shifted &&
        s_keyboard_shift == KeyboardShift::OneShot) {
        s_keyboard_shift = KeyboardShift::Off;
        s_keyboard_shift_tap_pending = false;
    }
    update_keyboard_display();
    s_ime_candidate_page = 0;
    refresh_ime_candidates();
    update_keyboard_shift_display();
}

void t9_key_cb(lv_event_t *event)
{
    const char key = static_cast<char>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const bool upper = s_keyboard_shift != KeyboardShift::Off;
    lyra::gui::ime::process_t9_key(ime_layout(), key, upper, keyboard_input_text(),
                                   keyboard_input_capacity());
    // Only the letter keys consume a one-shot shift.
    if (key >= '2' && key <= '9' && s_keyboard_shift == KeyboardShift::OneShot) {
        s_keyboard_shift = KeyboardShift::Off;
        s_keyboard_shift_tap_pending = false;
    }
    update_keyboard_display();
    s_ime_candidate_page = 0;
    refresh_ime_candidates();
    update_keyboard_shift_display();
}

void hide_kana_flick_picker(KanaFlickKey *context)
{
    if (!context) return;
    for (size_t index = 0; index < 5; ++index) {
        if (context->popup_keys[index]) lv_obj_delete(context->popup_keys[index]);
        context->popup_keys[index] = nullptr;
        context->popup_labels[index] = nullptr;
    }
}

size_t kana_flick_variant(int dx, int dy)
{
    if (std::abs(dx) <= 18 && std::abs(dy) <= 18) return 0;
    if (std::abs(dx) > std::abs(dy)) return dx > 0 ? 2 : 4;
    return dy > 0 ? 3 : 1;
}

void show_kana_flick_picker(KanaFlickKey *context, lv_obj_t *key)
{
    if (!context || !key || !s_screen) return;
    hide_kana_flick_picker(context);

    constexpr int cell_width = 44;
    constexpr int cell_height = 42;
    constexpr int gap = 2;
    constexpr int picker_width = cell_width * 3 + gap * 2;
    constexpr int picker_height = cell_height * 3 + gap * 2;
    constexpr int step_x = cell_width + gap;
    constexpr int step_y = cell_height + gap;
    constexpr int cell_x[] = {step_x, step_x, step_x * 2, step_x, 0};
    constexpr int cell_y[] = {step_y, 0, step_y, step_y * 2, step_y};

    lv_area_t key_area{};
    lv_area_t screen_area{};
    lv_obj_get_coords(key, &key_area);
    lv_obj_get_coords(s_screen, &screen_area);
    const int center_x = (key_area.x1 + key_area.x2 + 1) / 2 - screen_area.x1;
    const int center_y = (key_area.y1 + key_area.y2 + 1) / 2 - screen_area.y1;
    const int screen_width = lv_obj_get_width(s_screen);
    const int screen_height = lv_obj_get_height(s_screen);
    const int x = std::max(0, std::min(screen_width - picker_width,
                                       center_x - picker_width / 2));
    const int y = std::max(0, std::min(screen_height - picker_height,
                                       center_y - picker_height / 2));

    for (size_t variant = 0; variant < 5; ++variant) {
        if (!context->variants[variant] || !context->variants[variant][0]) continue;
        lv_obj_t *button = make_button(s_screen, x + cell_x[variant], y + cell_y[variant],
            cell_width, cell_height, variant == 0 ? kAccentDark : kSurfaceRaised, 5, true);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *label = make_label(button, context->variants[variant],
            variant == 0 ? kTextOnAccent : kTextPrimary);
        lv_obj_center(label);
        context->popup_keys[variant] = button;
        context->popup_labels[variant] = label;
    }
}

void update_kana_flick_picker(KanaFlickKey *context, size_t selected)
{
    if (!context) return;
    for (size_t variant = 0; variant < 5; ++variant) {
        if (!context->popup_keys[variant] || !context->popup_labels[variant]) continue;
        const bool active = variant == selected;
        lv_obj_set_style_bg_color(context->popup_keys[variant],
            active ? kAccentDark : kSurfaceRaised, LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(context->popup_labels[variant],
            active ? kTextOnAccent : kTextPrimary, LV_STATE_DEFAULT);
    }
}

void keyboard_action_cb(lv_event_t *event)
{
    const auto command = static_cast<KeyboardCommand>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    char *text = keyboard_input_text();
    const size_t capacity = keyboard_input_capacity();
    switch (command) {
        case KeyboardCommand::Shift:
            toggle_keyboard_shift();
            return;
        case KeyboardCommand::ToggleSymbols:
            reset_keyboard_shift();
            if (s_view == View::RadioAdd) {
                s_library_keyboard_symbols = !s_library_keyboard_symbols;
                s_library_keyboard_accents = false;
            } else if (s_library_keyboard_layout == KeyboardLayout::English) {
                if (s_library_keyboard_accents) {
                    s_library_keyboard_accents = false;
                    s_library_keyboard_symbols = true;
                } else if (s_library_keyboard_symbols) {
                    s_library_keyboard_symbols = false;
                } else {
                    s_library_keyboard_accents = true;
                }
            } else if (s_library_keyboard_accents) {
                s_library_keyboard_accents = false;
                s_library_keyboard_symbols = false;
            } else {
                s_library_keyboard_symbols = !s_library_keyboard_symbols;
            }
            render(s_view);
            return;
        case KeyboardCommand::Backspace:
            lyra::gui::ime::backspace(ime_layout(), text, capacity);
            break;
        case KeyboardCommand::Space:
            lyra::gui::ime::space(ime_layout(), text, capacity);
            break;
        case KeyboardCommand::Submit:
            commit_keyboard_preedit();
            if (s_view == View::WifiPassword) {
                submit_wifi_password();
                return;
            }
            if (s_view == View::FileNameInput) {
                submit_file_name_input();
                return;
            }
            if (s_view == View::PlaylistCreate) {
                const bool from_queue = s_playlist_create_from_queue;
                size_t created = 0;
                if (lyra::media::create_playlist(s_playlist_name, &created) == ESP_OK) {
                    bool complete = true;
                    if (from_queue) {
                        const size_t queue_count = s_has_active_queue ? playback_queue_count() : 0;
                        for (size_t position = 0; position < queue_count; ++position) {
                            size_t track_index = 0;
                            if (!queue_track_at(position, &track_index) ||
                                lyra::media::add_to_playlist(created, track_index) != ESP_OK) {
                                complete = false;
                                break;
                            }
                        }
                    }
                    if (!complete) {
                        lyra::media::delete_playlist(created);
                        show_notice(tr(lyra::i18n::StringId::Queue),
                                    tr(lyra::i18n::StringId::QueueSaveFailed));
                        return;
                    }
                    s_selected_playlist = created;
                    s_selected_playlist_is_smart = false;
                    s_playlist_name[0] = '\0';
                    s_playlist_create_from_queue = false;
                    render(View::PlaylistDetail);
                } else if (from_queue) {
                    show_notice(tr(lyra::i18n::StringId::Queue),
                                tr(lyra::i18n::StringId::QueueSaveFailed));
                }
                return;
            }
            if (s_view == View::RadioAdd) {
                save_radio_address();
                return;
            }
            s_list_page = 0;
            s_search_error[0] = '\0';
            if (lyra::media::start_search(s_search_category, s_search_query) == ESP_OK) {
                copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
                s_submitted_search_category = s_search_category;
            } else {
                copy_ui_text(s_search_error, sizeof(s_search_error),
                             tr(lyra::i18n::StringId::SearchBusy));
            }
            render(View::Search);
            return;
    }
    update_keyboard_display();
    s_ime_candidate_page = 0;
    refresh_ime_candidates();
}

void kana_flick_cb(lv_event_t *event)
{
    auto *context = static_cast<KanaFlickKey *>(lv_event_get_user_data(event));
    if (!context) return;
    lv_point_t point{};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &point);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        context->start = point;
        show_kana_flick_picker(context,
            static_cast<lv_obj_t *>(lv_event_get_target(event)));
        update_kana_flick_picker(context, 0);
        return;
    }
    int dx = point.x - context->start.x;
    int dy = point.y - context->start.y;
    if (code == LV_EVENT_PRESSING) {
        update_kana_flick_picker(context, kana_flick_variant(dx, dy));
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        hide_kana_flick_picker(context);
        return;
    }
    if (code != LV_EVENT_RELEASED) return;

    const size_t variant = kana_flick_variant(dx, dy);
    const char *kana = context->variants[variant];
    hide_kana_flick_picker(context);
    if (kana && *kana) {
        // In T9 mode a plain tap toggles through the row; flicks still work.
        const size_t key_index = static_cast<size_t>(context - s_kana_flick_keys);
        if (s_t9_input && variant == 0) {
            lyra::gui::ime::process_t9_key(ime_layout(),
                key_index == 9 ? '0' : static_cast<char>('1' + key_index), false,
                keyboard_input_text(), keyboard_input_capacity());
        } else {
            lyra::gui::ime::process_kana_flick(
                ime_layout(), kana, keyboard_input_text(), keyboard_input_capacity());
        }
        update_keyboard_display();
        s_ime_candidate_page = 0;
        refresh_ime_candidates();
    }
}

void ime_candidate_cb(lv_event_t *event)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encoded == 0) return;
    lyra::gui::ime::select_candidate(ime_layout(), static_cast<size_t>(encoded - 1),
                                     keyboard_input_text(), keyboard_input_capacity());
    s_ime_candidate_page = 0;
    render(s_view);
}

void ime_candidate_page_cb(lv_event_t *event)
{
    const uintptr_t direction = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const size_t count = lyra::gui::ime::candidate_count(ime_layout());
    const size_t page_count = (count + 4) / 5;
    if (page_count < 2) return;
    if (direction == 0) {
        s_ime_candidate_page = s_ime_candidate_page == 0 ?
            page_count - 1 : s_ime_candidate_page - 1;
    } else {
        s_ime_candidate_page = (s_ime_candidate_page + 1) % page_count;
    }
    refresh_ime_candidates();
}

void kana_modifier_cb(lv_event_t *event)
{
    auto *label = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (!label) return;
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        s_kana_modifier_just_changed = false;
        return;
    }
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        const uint8_t next = (static_cast<uint8_t>(s_kana_modifier) + 1) % 3;
        s_kana_modifier = static_cast<lyra::gui::ime::KanaModifier>(next);
        s_kana_modifier_just_changed = true;
        const char *modifier_text = s_kana_modifier == lyra::gui::ime::KanaModifier::Dakuten ?
            "゛" : s_kana_modifier == lyra::gui::ime::KanaModifier::Handakuten ? "゜" : "小";
        lv_label_set_text(label, modifier_text);
        return;
    }
    if (s_kana_modifier_just_changed) {
        s_kana_modifier_just_changed = false;
        return;
    }
    lyra::gui::ime::apply_kana_modifier(s_kana_modifier,
        keyboard_input_text(), keyboard_input_capacity());
    update_keyboard_display();
    s_ime_candidate_page = 0;
    refresh_ime_candidates();
}

} // namespace

void commit_keyboard_preedit()
{
    reset_keyboard_shift();
    if (s_view != View::Search && s_view != View::PlaylistCreate &&
        s_view != View::RadioAdd && s_view != View::FileNameInput) return;
    lyra::gui::ime::commit(ime_layout(), keyboard_input_text(), keyboard_input_capacity());
    update_search_label();
}

void close_keyboard_picker()
{
    s_keyboard_picker_visible = false;
    s_ime_candidates = nullptr;
}

void keyboard_selector_cb(lv_event_t *)
{
    s_keyboard_picker_visible = !s_keyboard_picker_visible;
    if (s_keyboard_picker_visible && s_view == View::Search) s_search_keyboard_visible = true;
    render(s_view);
}

void make_library_keyboard(lv_obj_t *keyboard, const char *action)
{
    make_library_keyboard_impl(keyboard, action);
}

void toggle_search_keyboard_cb(lv_event_t *)
{
    s_search_keyboard_visible = !s_search_keyboard_visible;
    if (!s_search_keyboard || !s_search_results || !s_search_keyboard_toggle) return;
    lv_label_set_text(s_search_keyboard_toggle,
                      s_search_keyboard_visible ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
    if (s_search_keyboard_visible) {
        lv_obj_remove_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results,
                          content_height(kStatusHeight) - library_keyboard_height() - 84);
    } else {
        lv_obj_add_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results, content_height(kStatusHeight) - 84);
    }
    lv_obj_invalidate(s_search_results);
}

void render_search()
{
    lv_obj_t *body = make_box(s_screen, 0, kStatusHeight, 320, content_height(kStatusHeight), kBackground);
    constexpr int input_width = 170;
    lv_obj_t *input = make_box(body, 9, 4, input_width, 43, kSurfaceRaised, 7);
    lv_obj_t *magnifier = make_label(input, LV_SYMBOL_EDIT, kTextSecondary);
    lv_obj_align(magnifier, LV_ALIGN_LEFT_MID, 10, 0);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, input_width - 44, MarqueeMode::Continuous);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 37, 0);
    update_search_label();
    make_keyboard_selector(body, 183, 4, 36, 43, true);
    make_queue_add_mode_toggle(body, 223, 7);
    lv_obj_t *toggle = make_button(body, 267, 4, 44, 43, kSurfaceRaised, 7);
    s_search_keyboard_toggle = make_label(toggle,
        s_search_keyboard_visible ? LV_SYMBOL_DOWN : LV_SYMBOL_UP, kAccent);
    lv_obj_center(s_search_keyboard_toggle);
    lv_obj_add_event_cb(toggle, toggle_search_keyboard_cb, LV_EVENT_CLICKED, nullptr);
    constexpr lyra::media::SearchCategory categories[] = {
        lyra::media::SearchCategory::Songs,
        lyra::media::SearchCategory::Albums,
        lyra::media::SearchCategory::Artists,
        lyra::media::SearchCategory::Playlists,
    };
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i) {
        const char *labels[] = {tr(lyra::i18n::StringId::Songs),
                                tr(lyra::i18n::StringId::Albums),
                                tr(lyra::i18n::StringId::Artists),
                                tr(lyra::i18n::StringId::Playlists)};
        lv_obj_t *tab = make_button(body, 7 + static_cast<int>(i) * 77, 52, 72, 28,
                                    categories[i] == s_search_category ? kAccentDark : kSurfaceRaised, 5);
        lv_obj_t *tab_label = make_label(tab, labels[i],
                                         categories[i] == s_search_category ? kTextOnAccent : kTextSecondary);
        lv_obj_center(tab_label);
        lv_obj_add_event_cb(tab, search_category_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(categories[i])));
    }
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(kStatusHeight) - keyboard_height;
    const int results_height = s_search_keyboard_visible ? keyboard_y - 84 :
        content_height(kStatusHeight) - 84;
    s_search_results = make_box(body, 0, 84, 320, results_height, kBackground);
    configure_cover_aware_scroll(s_search_results);
    populate_search_results();
    s_search_keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    lv_obj_t *keyboard = s_search_keyboard;
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SearchKey));
    if (!s_search_keyboard_visible) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void render_playlist_create()
{
    make_header(s_playlist_create_from_queue ?
                tr(lyra::i18n::StringId::SaveQueueAsPlaylist) :
                tr(lyra::i18n::StringId::NewPlaylist), View::Playlists, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, 320, content_height(72), kBackground);
    lv_obj_t *input = make_box(body, 9, 8, 240, 43, kSurfaceRaised, 7);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 214, MarqueeMode::Continuous);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();
    make_keyboard_selector(body, 255, 8, 56, 43);
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SaveKey));
}

void render_file_name_input()
{
    make_header(tr(file_name_input_creates_folder() ? lyra::i18n::StringId::FileNewFolder :
                                                      lyra::i18n::StringId::FileRename),
                View::Folders, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, 320, content_height(72), kBackground);
    lv_obj_t *input = make_box(body, 9, 8, 240, 43, kSurfaceRaised, 7);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 214, MarqueeMode::Continuous);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();
    make_keyboard_selector(body, 255, 8, 56, 43);
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::SaveKey));
}

void add_playlist_track_cb(lv_event_t *event)
{
    const size_t track_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (lyra::media::add_to_playlist(s_selected_playlist, track_index) == ESP_OK) {
        while (s_navigation_depth) {
            const NavigationState state = s_navigation[--s_navigation_depth];
            if (state.view == View::PlaylistDetail) {
                restore_navigation_state(state);
                return;
            }
        }
        render(View::PlaylistDetail);
    }
}

void render_playlist_add()
{
    s_playlist_add_mode = true;
    render_library();
}

} // namespace lyra::gui::internal
