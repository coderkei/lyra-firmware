/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <ctime>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lyra_board.h"
#include "lyra_boot_test.h"
#include "lyra_font.h"
#include "lyra_media.h"
#include "lyra_audio.h"
#include "lyra_png.h"
#include "lyra_sd.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "lyra.gui";
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 480;
constexpr int kStatusHeight = 28;
constexpr int kNavHeight = 44;
constexpr int kKeyboardHeightWithNav = 216;
constexpr int kKeyboardHeightWithoutNav = 260;
constexpr int kKeyboardKeyHeightWithNav = 46;
constexpr int kKeyboardKeyHeightWithoutNav = 50;
constexpr int kKeyboardRowGap = 3;
constexpr int kKeyboardBottomPadding = 4;
constexpr uint16_t kBootImageWidth = 300;
constexpr uint16_t kBootImageHeight = 59;
constexpr uint32_t kBootBackgroundRgb = 0x080C12;
constexpr size_t kDebugInfoCapacity = 4096;
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kGaplessKey = "gapless";
constexpr const char *kReplayGainKey = "replay_gain";
constexpr const char *kCrossfadeKey = "crossfade";
constexpr const char *kBrightnessKey = "brightness";
constexpr const char *kDarkModeKey = "dark_mode";
constexpr const char *kAccentColorKey = "accent_color";
constexpr const char *kSpeakerOutputKey = "speaker_output";
constexpr const char *kEqualizerPresetKey = "eq_preset";
constexpr const char *kEqualizerBandKeys[lyra::audio::kEqualizerBandCount] = {
    "eq_band_0", "eq_band_1", "eq_band_2", "eq_band_3", "eq_band_4",
};
constexpr uint32_t kNonGaplessTrackPauseMs = 500;

extern const uint8_t boot_png_start[] asm("_binary_boot_png_start");
extern const uint8_t boot_png_end[] asm("_binary_boot_png_end");

struct AccentPalette {
    uint32_t dark_rgb;
    uint32_t dark_pressed_rgb;
    uint32_t dark_surface_rgb;
    uint32_t light_rgb;
    uint32_t light_pressed_rgb;
    uint32_t light_surface_rgb;
};

// These swatches intentionally use familiar, high-contrast UI colours. The
// light and dark variants keep the selected accent readable in either theme.
constexpr AccentPalette kAccentPalettes[] = {
    {0x38BDF8, 0x075985, 0x123A52, 0x2563EB, 0x1D4ED8, 0xDBEAFE}, // Blue
    {0xC084FC, 0x7E22CE, 0x3B214F, 0x7C3AED, 0x6D28D9, 0xEDE9FE}, // Purple
    {0x2DD4BF, 0x0F766E, 0x123F42, 0x0D9488, 0x0F766E, 0xCCFBF1}, // Teal
    {0x4ADE80, 0x15803D, 0x163D2A, 0x16A34A, 0x15803D, 0xDCFCE7}, // Green
    {0xFBBF24, 0xB45309, 0x493512, 0xD97706, 0xB45309, 0xFEF3C7}, // Amber
    {0xFB923C, 0xC2410C, 0x492812, 0xEA580C, 0xC2410C, 0xFFEDD5}, // Orange
    {0xF87171, 0xB91C1C, 0x4A2026, 0xDC2626, 0xB91C1C, 0xFEE2E2}, // Red
    {0xF472B6, 0xBE185D, 0x4A1D3B, 0xDB2777, 0xBE185D, 0xFCE7F3}, // Pink
};
constexpr size_t kAccentPaletteCount = sizeof(kAccentPalettes) / sizeof(kAccentPalettes[0]);

bool s_dark_mode = true;
uint8_t s_accent_colour = 0;

lv_color_t kBackground = lv_color_hex(0x080C12);
lv_color_t kSurface = lv_color_hex(0x101720);
lv_color_t kSurfaceRaised = lv_color_hex(0x18212D);
lv_color_t kAccent = lv_color_hex(0x38BDF8);
lv_color_t kAccentDark = lv_color_hex(0x075985);
lv_color_t kAccentSurface = lv_color_hex(0x123A52);
lv_color_t kTextPrimary = lv_color_hex(0xF4F7FB);
lv_color_t kTextSecondary = lv_color_hex(0xCBD5E1);
lv_color_t kTextMuted = lv_color_hex(0x718096);
lv_color_t kDivider = lv_color_hex(0x25303D);
lv_color_t kNavSurface = lv_color_hex(0x0B111A);
lv_color_t kKeyboardSurface = lv_color_hex(0x05080D);
lv_color_t kArtworkSurface = lv_color_hex(0x142131);
lv_color_t kPlayerArtSurface = lv_color_hex(0x101D45);
lv_color_t kDangerSurface = lv_color_hex(0x3F1118);
const lv_color_t kOverlay = lv_color_hex(0x000000);
const lv_color_t kTextOnAccent = lv_color_hex(0xFFFFFF);
constexpr const char *kHeartOutline = "\xE2\x99\xA1"; // U+2661 WHITE HEART SUIT
constexpr const char *kHeartFilled = "\xE2\x99\xA5";  // U+2665 BLACK HEART SUIT

enum class View : uintptr_t {
    Menu,
    Player,
    FullscreenArt,
    TrackInfo,
    FullscreenInfoArt,
    Queue,
    Library,
    LibrarySongs,
    LibraryArtists,
    LibraryAlbums,
    LibraryGenres,
    LibraryYears,
    AlbumDetail,
    ArtistDetail,
    Folders,
    FolderDetail,
    Playlists,
    PlaylistDetail,
    PlaylistCreate,
    PlaylistAdd,
    Equalizer,
    EqualizerPresets,
    Search,
    Settings,
    SortingSettings,
    SortingOptions,
    PlaybackSettings,
    CrossfadeOptions,
    SleepTimerOptions,
    SoundSettings,
    DisplaySettings,
    SystemSettings,
    DatabaseStorage,
    About,
    DebugMenu,
    Licenses,
    TrackList,
};

enum class LibraryTab : uint8_t { Songs, Artists, Albums, Genres, Years };
enum class ArtistDetailTab : uint8_t { Songs, Albums };
enum class TrackInfoTab : uint8_t { Song, Media };
enum class DebugTab : uint8_t { Info, Debug };

enum class PlaybackScope : uint8_t { Single, AllSongs, Group, Playlist, Folder, Search, SavedQueue };
enum class RepeatMode : uint8_t { Off, All, Song };
enum class EqualizerPreset : uint8_t {
    Custom,
    Flat,
    FullBass,
    FullTreble,
    BassAndTreble,
    Rock,
    Pop,
    Jazz,
    Classic,
    Count,
};

constexpr size_t kSearchPageSizeWithNav = 4;
constexpr size_t kSearchPageSizeWithoutNav = 5;
constexpr size_t kSearchPageCapacity = kSearchPageSizeWithoutNav;

struct NavigationState {
    View view;
    LibraryTab library_tab;
    ArtistDetailTab artist_detail_tab;
    size_t list_page;
    size_t selected_playlist;
    size_t selected_group;
    lyra::media::GroupKind selected_group_kind;
    bool playlists_from_library;
    bool playlist_add_mode;
    char track_list_title[lyra::media::kMaxName];
    char folder_path[lyra::media::kMaxPath];
};

constexpr size_t kNavigationDepth = 16;

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_search_label = nullptr;
lv_obj_t *s_search_results = nullptr;
lv_obj_t *s_search_keyboard = nullptr;
lv_obj_t *s_search_keyboard_toggle = nullptr;
lv_obj_t *s_search_progress_label = nullptr;
lv_obj_t *s_player_progress_bar = nullptr;
lv_obj_t *s_player_progress_touch = nullptr;
lv_obj_t *s_player_elapsed_label = nullptr;
lv_obj_t *s_player_duration_label = nullptr;
lv_obj_t *s_scan_overlay = nullptr;
lv_obj_t *s_scan_count_label = nullptr;
lv_obj_t *s_scan_phase_label = nullptr;
lv_obj_t *s_sort_overlay = nullptr;
lv_obj_t *s_sort_progress_label = nullptr;
lv_obj_t *s_playlist_picker = nullptr;
lv_obj_t *s_volume_popup = nullptr;
lv_obj_t *s_status_volume_label = nullptr;
lv_obj_t *s_screenshot_button = nullptr;
bool s_screenshot_dragged = false;
lv_point_t s_screenshot_press_point{};
lv_point_t s_screenshot_button_start{};
uint32_t s_screenshot_sequence = 0;
View s_view = View::Menu;
LibraryTab s_library_tab = LibraryTab::Songs;
ArtistDetailTab s_artist_detail_tab = ArtistDetailTab::Songs;
TrackInfoTab s_track_info_tab = TrackInfoTab::Song;
size_t s_current_track = 0;
size_t s_selected_playlist = 0;
size_t s_selected_group = 0;
lyra::media::GroupKind s_selected_group_kind = lyra::media::GroupKind::Artist;
size_t s_list_page = 0;
size_t s_list_page_count = 0;
bool s_shuffle = false;
RepeatMode s_repeat_mode = RepeatMode::Off;
uint32_t *s_shuffle_order = nullptr;
size_t s_shuffle_count = 0;
size_t s_shuffle_cursor = 0;
bool s_audio_eof_seen = false;
bool s_player_progress_dragging = false;
int32_t s_player_progress_drag_value = 0;
bool s_gapless = true;
bool s_replay_gain = true;
uint8_t s_crossfade_seconds = 0;
uint8_t s_brightness_percent = 72;
bool s_speaker_output_enabled = lyra::audio::kDefaultSpeakerOutputEnabled;
EqualizerPreset s_equalizer_preset = EqualizerPreset::Custom;
int16_t s_equalizer_custom_bands[lyra::audio::kEqualizerBandCount]{};
uint16_t s_sleep_timer_minutes = 0;
int64_t s_sleep_timer_deadline_us = 0;
bool s_pending_track_advance = false;
int64_t s_pending_track_advance_us = 0;
int64_t s_crossfade_fade_in_started_us = 0;
int64_t s_crossfade_fade_in_ends_us = 0;
int64_t s_crossfade_fade_out_started_us = 0;
int64_t s_crossfade_fade_out_ends_us = 0;
int s_crossfade_transition_direction = 0;
bool s_crossfade_pause_pending = false;
bool s_search_keyboard_visible = true;
bool s_library_keyboard_symbols = false;
bool s_playlists_from_library = false;
bool s_playlist_add_mode = false;
bool s_playlist_manage_mode = false;
// The JC3248W535EN dev kit has no Lyra hardware key matrix, so keep the
// simulator-equivalent touch dock available from first boot.
bool s_show_nav = true;
char s_search_query[48] = "";
char s_submitted_search_query[48] = "";
lyra::media::SearchCategory s_search_category = lyra::media::SearchCategory::Songs;
lyra::media::SearchCategory s_submitted_search_category = lyra::media::SearchCategory::Songs;
char s_search_error[48] = "";
char s_playlist_name[lyra::media::kMaxName] = "";
char s_track_list_title[lyra::media::kMaxName] = "All Songs";
char s_folder_path[lyra::media::kMaxPath] = "/sdcard";
char s_playback_folder_path[lyra::media::kMaxPath] = "/sdcard";
char s_folder_names[32][lyra::media::kMaxName]{};
size_t s_folder_count = 0;
uint32_t s_seen_catalog_generation = 0;
uint32_t s_seen_artwork_generation = 0;
uint32_t s_seen_duration_generation = 0;
uint32_t s_seen_sorting_generation = 0;
bool s_seen_scanning = false;
bool s_list_scrolling = false;
uint32_t s_seen_search_generation = 0;
NavigationState s_navigation[kNavigationDepth]{};
size_t s_navigation_depth = 0;
PlaybackScope s_playback_scope = PlaybackScope::Single;
lyra::media::GroupKind s_playback_group_kind = lyra::media::GroupKind::Artist;
size_t s_playback_group = 0;
size_t s_playback_playlist = 0;
size_t *s_saved_queue = nullptr;
size_t s_saved_queue_count = 0;
bool s_saved_queue_pending = false;
bool s_has_active_queue = false;
size_t s_queue_position = 0;
bool s_queue_position_valid = false;

enum class PowerAction : uintptr_t { Reboot, PowerOff };
enum class DatabaseAction : uintptr_t { Playlists, Artwork, All };
enum class ArtworkSetting : uintptr_t { SdCache, Size320 };
enum class PlaylistManageAction : uint8_t { DeletePlaylist, RemoveTrack };
lyra::media::SortSection s_sort_section = lyra::media::SortSection::Songs;
char s_database_status[64] = "";
DebugTab s_debug_tab = DebugTab::Info;
char s_debug_status[96] = "";
PlaylistManageAction s_pending_playlist_manage_action = PlaylistManageAction::DeletePlaylist;
size_t s_pending_playlist = 0;
size_t s_pending_playlist_track = 0;

void apply_theme_palette()
{
    const size_t accent_index = static_cast<size_t>(s_accent_colour) < kAccentPaletteCount ?
                                s_accent_colour : 0;
    const AccentPalette &accent = kAccentPalettes[accent_index];
    if (s_dark_mode) {
        kBackground = lv_color_hex(0x080C12);
        kSurface = lv_color_hex(0x101720);
        kSurfaceRaised = lv_color_hex(0x18212D);
        kAccent = lv_color_hex(accent.dark_rgb);
        kAccentDark = lv_color_hex(accent.dark_pressed_rgb);
        kAccentSurface = lv_color_hex(accent.dark_surface_rgb);
        kTextPrimary = lv_color_hex(0xF4F7FB);
        kTextSecondary = lv_color_hex(0xCBD5E1);
        kTextMuted = lv_color_hex(0x718096);
        kDivider = lv_color_hex(0x25303D);
        kNavSurface = lv_color_hex(0x0B111A);
        kKeyboardSurface = lv_color_hex(0x05080D);
        kArtworkSurface = lv_color_hex(0x142131);
        kPlayerArtSurface = lv_color_hex(0x101D45);
        kDangerSurface = lv_color_hex(0x3F1118);
    } else {
        kBackground = lv_color_hex(0xF8FAFC);
        kSurface = lv_color_hex(0xFFFFFF);
        kSurfaceRaised = lv_color_hex(0xF1F5F9);
        kAccent = lv_color_hex(accent.light_rgb);
        kAccentDark = lv_color_hex(accent.light_pressed_rgb);
        kAccentSurface = lv_color_hex(accent.light_surface_rgb);
        kTextPrimary = lv_color_hex(0x172033);
        kTextSecondary = lv_color_hex(0x475569);
        kTextMuted = lv_color_hex(0x64748B);
        kDivider = lv_color_hex(0xCBD5E1);
        kNavSurface = lv_color_hex(0xFFFFFF);
        kKeyboardSurface = lv_color_hex(0xE2E8F0);
        kArtworkSurface = lv_color_hex(0xE2E8F0);
        kPlayerArtSurface = lv_color_hex(0xE0E7FF);
        kDangerSurface = lv_color_hex(0xFEE2E2);
    }
}

constexpr int16_t kEqualizerFlat[lyra::audio::kEqualizerBandCount] = {
    0, 0, 0, 0, 0,
};
constexpr int16_t kEqualizerFullBass[lyra::audio::kEqualizerBandCount] = {
    60, 60, 0, 0, 0,
};
constexpr int16_t kEqualizerFullTreble[lyra::audio::kEqualizerBandCount] = {
    0, 0, 0, 60, 60,
};
constexpr int16_t kEqualizerBassAndTreble[lyra::audio::kEqualizerBandCount] = {
    60, 60, 0, 60, 60,
};
constexpr int16_t kEqualizerRock[lyra::audio::kEqualizerBandCount] = {
    50, 30, 10, 40, 50,
};
constexpr int16_t kEqualizerPop[lyra::audio::kEqualizerBandCount] = {
    -10, 20, 40, 20, -10,
};
constexpr int16_t kEqualizerJazz[lyra::audio::kEqualizerBandCount] = {
    30, 10, 20, 30, 20,
};
constexpr int16_t kEqualizerClassic[lyra::audio::kEqualizerBandCount] = {
    40, 20, 0, 30, 40,
};

const char *equalizer_preset_name(EqualizerPreset preset)
{
    switch (preset) {
        case EqualizerPreset::Custom: return "Custom";
        case EqualizerPreset::Flat: return "Flat";
        case EqualizerPreset::FullBass: return "Full Bass";
        case EqualizerPreset::FullTreble: return "Full Treble";
        case EqualizerPreset::BassAndTreble: return "Bass & Treble";
        case EqualizerPreset::Rock: return "Rock";
        case EqualizerPreset::Pop: return "Pop";
        case EqualizerPreset::Jazz: return "Jazz";
        case EqualizerPreset::Classic: return "Classic";
        case EqualizerPreset::Count: break;
    }
    return "Custom";
}

const int16_t *equalizer_preset_bands(EqualizerPreset preset)
{
    switch (preset) {
        case EqualizerPreset::Flat: return kEqualizerFlat;
        case EqualizerPreset::FullBass: return kEqualizerFullBass;
        case EqualizerPreset::FullTreble: return kEqualizerFullTreble;
        case EqualizerPreset::BassAndTreble: return kEqualizerBassAndTreble;
        case EqualizerPreset::Rock: return kEqualizerRock;
        case EqualizerPreset::Pop: return kEqualizerPop;
        case EqualizerPreset::Jazz: return kEqualizerJazz;
        case EqualizerPreset::Classic: return kEqualizerClassic;
        case EqualizerPreset::Custom: return s_equalizer_custom_bands;
        case EqualizerPreset::Count: break;
    }
    return s_equalizer_custom_bands;
}

bool equalizer_is_custom()
{
    return s_equalizer_preset == EqualizerPreset::Custom;
}

void apply_equalizer_to_audio()
{
    lyra::audio::EqualizerSettings settings{};
    const int16_t *bands = equalizer_preset_bands(s_equalizer_preset);
    std::memcpy(settings.band_tenths_db, bands, sizeof(settings.band_tenths_db));
    const esp_err_t result = lyra::audio::set_equalizer(settings);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not apply %s EQ preset: %s",
                 equalizer_preset_name(s_equalizer_preset), esp_err_to_name(result));
    }
}

void save_user_settings()
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kSettingsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "settings persistence unavailable: %s", esp_err_to_name(result));
        return;
    }
    result = nvs_set_u8(handle, kGaplessKey, s_gapless ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kReplayGainKey, s_replay_gain ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kCrossfadeKey, s_crossfade_seconds);
    if (result == ESP_OK) result = nvs_set_u8(handle, kBrightnessKey, s_brightness_percent);
    if (result == ESP_OK) result = nvs_set_u8(handle, kDarkModeKey, s_dark_mode ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kAccentColorKey, s_accent_colour);
    if (result == ESP_OK) result = nvs_set_u8(handle, kSpeakerOutputKey,
                                               s_speaker_output_enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u8(handle, kEqualizerPresetKey,
                                               static_cast<uint8_t>(s_equalizer_preset));
    for (size_t band = 0; result == ESP_OK && band < lyra::audio::kEqualizerBandCount; ++band) {
        result = nvs_set_i8(handle, kEqualizerBandKeys[band],
                            static_cast<int8_t>(s_equalizer_custom_bands[band]));
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save settings: %s", esp_err_to_name(result));
    }
}

void load_user_settings()
{
    nvs_handle_t handle;
    if (nvs_open(kSettingsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0;
    if (nvs_get_u8(handle, kGaplessKey, &value) == ESP_OK && value <= 1) s_gapless = value != 0;
    if (nvs_get_u8(handle, kReplayGainKey, &value) == ESP_OK && value <= 1) s_replay_gain = value != 0;
    if (nvs_get_u8(handle, kCrossfadeKey, &value) == ESP_OK &&
        (value == 0 || value == 1 || value == 2 || value == 4 || value == 6)) {
        s_crossfade_seconds = value;
    }
    if (nvs_get_u8(handle, kBrightnessKey, &value) == ESP_OK && value >= 1 && value <= 100) {
        s_brightness_percent = value;
    }
    if (nvs_get_u8(handle, kDarkModeKey, &value) == ESP_OK && value <= 1) {
        s_dark_mode = value != 0;
    }
    if (nvs_get_u8(handle, kAccentColorKey, &value) == ESP_OK &&
        static_cast<size_t>(value) < kAccentPaletteCount) {
        s_accent_colour = value;
    }
    if (nvs_get_u8(handle, kSpeakerOutputKey, &value) == ESP_OK && value <= 1) {
        s_speaker_output_enabled = value != 0;
    }
    if (nvs_get_u8(handle, kEqualizerPresetKey, &value) == ESP_OK &&
        value < static_cast<uint8_t>(EqualizerPreset::Count)) {
        s_equalizer_preset = static_cast<EqualizerPreset>(value);
    }
    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        int8_t gain = 0;
        if (nvs_get_i8(handle, kEqualizerBandKeys[band], &gain) == ESP_OK &&
            gain >= lyra::audio::kEqualizerMinimumTenthsDb &&
            gain <= lyra::audio::kEqualizerMaximumTenthsDb) {
            s_equalizer_custom_bands[band] = gain;
        }
    }
    nvs_close(handle);
}

void copy_ui_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    const size_t length = std::min(std::strlen(source), capacity - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool append_ui_text(char *destination, size_t capacity, const char *suffix)
{
    if (!destination || capacity == 0 || !suffix) return false;
    const size_t used = std::strlen(destination);
    const size_t suffix_length = std::strlen(suffix);
    if (used + suffix_length + 1 > capacity) return false;
    std::memcpy(destination + used, suffix, suffix_length + 1);
    return true;
}

void render(View view);
void folder_back_cb(lv_event_t *event);
void cancel_power_action_cb(lv_event_t *event);
bool show_now_playing();
bool restart_current_track();
void apply_replay_gain_to_current_track();
void begin_crossfade_fade_in();

NavigationState capture_navigation_state()
{
    NavigationState state{
        s_view, s_library_tab, s_artist_detail_tab, s_list_page, s_selected_playlist, s_selected_group,
        s_selected_group_kind, s_playlists_from_library, s_playlist_add_mode, {}, {}};
    copy_ui_text(state.track_list_title, sizeof(state.track_list_title), s_track_list_title);
    copy_ui_text(state.folder_path, sizeof(state.folder_path), s_folder_path);
    return state;
}

void push_navigation_state()
{
    const NavigationState state = capture_navigation_state();
    if (s_navigation_depth == kNavigationDepth) {
        std::move(s_navigation + 1, s_navigation + kNavigationDepth, s_navigation);
        --s_navigation_depth;
    }
    s_navigation[s_navigation_depth++] = state;
}

void restore_navigation_state(const NavigationState &state)
{
    s_library_tab = state.library_tab;
    s_artist_detail_tab = state.artist_detail_tab;
    s_list_page = state.list_page;
    s_selected_playlist = state.selected_playlist;
    s_selected_group = state.selected_group;
    s_selected_group_kind = state.selected_group_kind;
    s_playlists_from_library = state.playlists_from_library;
    s_playlist_add_mode = state.playlist_add_mode;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), state.track_list_title);
    copy_ui_text(s_folder_path, sizeof(s_folder_path), state.folder_path);
    render(state.view);
}

void navigate_to(View target)
{
    if (target == s_view) return;
    const bool player_mode_transition =
        (s_view == View::Player || s_view == View::FullscreenArt) &&
        (target == View::Player || target == View::FullscreenArt);
    if (!player_mode_transition) push_navigation_state();
    s_list_page = 0;
    render(target);
}

void navigate_back(View fallback)
{
    if (s_navigation_depth) {
        restore_navigation_state(s_navigation[--s_navigation_depth]);
        return;
    }
    s_list_page = 0;
    render(fallback);
}

int content_bottom()
{
    return kScreenHeight - (s_show_nav ? kNavHeight : 0);
}

int content_height(int top)
{
    return content_bottom() - top;
}

int library_keyboard_height()
{
    return s_show_nav ? kKeyboardHeightWithNav : kKeyboardHeightWithoutNav;
}

size_t library_page_size()
{
    return s_show_nav ? lyra::media::kTrackPageSize : 6;
}

size_t search_page_size()
{
    return s_show_nav ? kSearchPageSizeWithNav : kSearchPageSizeWithoutNav;
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 0)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, color, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, lyra::font::ui(), 0);
    return label;
}

lv_anim_t s_marquee_animation;
lv_style_t s_marquee_style;
bool s_marquee_style_ready = false;

void make_marquee(lv_obj_t *label, int width)
{
    if (!label || width <= 0) return;
    if (!s_marquee_style_ready) {
        lv_anim_init(&s_marquee_animation);
        lv_anim_set_delay(&s_marquee_animation, 2000);
        // Circular mode continues in the reading direction; the repeat delay
        // gives the text a moment at its starting edge before the next loop.
        lv_anim_set_repeat_delay(&s_marquee_animation, 1500);
        lv_anim_set_repeat_count(&s_marquee_animation, LV_ANIM_REPEAT_INFINITE);
        lv_style_init(&s_marquee_style);
        lv_style_set_anim(&s_marquee_style, &s_marquee_animation);
        s_marquee_style_ready = true;
    }
    lv_obj_set_width(label, width);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_style(label, &s_marquee_style, LV_STATE_DEFAULT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
}

lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 7)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_color(button, kAccentDark, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, radius, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    return button;
}

void make_equalizer_icon(lv_obj_t *parent, lv_color_t color)
{
    constexpr int heights[] = {7, 14, 10, 17};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        lv_obj_t *bar = make_box(parent, 12 + static_cast<int>(i) * 6,
                                 27 - heights[i], 3, heights[i], color, 2);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
}

void configure_cover_aware_scroll(lv_obj_t *body)
{
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(body, [](lv_event_t *) { s_list_scrolling = true; }, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(body, [](lv_event_t *event) {
        s_list_scrolling = false;
        // Covers entering the viewport were deliberately skipped during the
        // swipe. Refresh once at rest to populate only the final visible rows.
        lv_obj_invalidate(lv_event_get_current_target_obj(event));
    }, LV_EVENT_SCROLL_END, nullptr);
}

lv_obj_t *make_scroll_body(int top)
{
    lv_obj_t *body = make_box(s_screen, 0, top, kScreenWidth, content_height(top), kBackground);
    configure_cover_aware_scroll(body);
    return body;
}

void style_root()
{
    s_player_progress_bar = nullptr;
    s_player_progress_touch = nullptr;
    s_player_elapsed_label = nullptr;
    s_player_duration_label = nullptr;
    s_audio_eof_seen = false;
    s_player_progress_dragging = false;
    s_player_progress_drag_value = 0;
    s_playlist_picker = nullptr;
    s_volume_popup = nullptr;
    s_status_volume_label = nullptr;
    lv_obj_clean(s_screen);
    lv_obj_set_style_bg_color(s_screen, kBackground, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
}

void route_cb(lv_event_t *event)
{
    const View target = static_cast<View>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (target != s_view) {
        if (target == View::Menu) s_playlist_add_mode = false;
        if (target == View::Playlists && s_view != View::PlaylistDetail &&
            s_view != View::PlaylistCreate) s_playlists_from_library = false;
        navigate_to(target);
    }
}

void add_route(lv_obj_t *object, View target)
{
    lv_obj_add_event_cb(object, route_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(target)));
}

View back_view(View view)
{
    switch (view) {
        case View::FullscreenArt: return View::Menu;
        case View::TrackInfo: return View::Player;
        case View::FullscreenInfoArt: return View::TrackInfo;
        case View::Queue: return View::Menu;
        case View::LibrarySongs:
        case View::LibraryArtists:
        case View::LibraryAlbums:
        case View::LibraryGenres:
        case View::LibraryYears: return View::Library;
        case View::AlbumDetail: return View::LibraryAlbums;
        case View::ArtistDetail: return View::LibraryArtists;
        case View::TrackList:
            return s_library_tab == LibraryTab::Artists ? View::LibraryArtists :
                   s_library_tab == LibraryTab::Genres ? View::LibraryGenres :
                   s_library_tab == LibraryTab::Years ? View::LibraryYears : View::Library;
        case View::FolderDetail: return View::Folders;
        case View::PlaylistDetail: return View::Playlists;
        case View::PlaylistCreate: return View::Playlists;
        case View::PlaylistAdd: return View::PlaylistDetail;
        case View::PlaybackSettings:
        case View::SoundSettings:
        case View::DisplaySettings:
        case View::SystemSettings:
        case View::SortingSettings: return View::Settings;
        case View::SortingOptions: return View::SortingSettings;
        case View::CrossfadeOptions:
        case View::SleepTimerOptions: return View::PlaybackSettings;
        case View::About: return View::Settings;
        case View::DebugMenu: return View::About;
        case View::Licenses: return View::Settings;
        case View::DatabaseStorage: return View::SystemSettings;
        case View::EqualizerPresets: return View::Equalizer;
        case View::Playlists: return s_playlists_from_library ? View::Library : View::Menu;
        case View::Player:
        case View::Library:
        case View::Folders:
        case View::Equalizer:
        case View::Search:
        case View::Settings:
        case View::Menu: return View::Menu;
    }
    return View::Menu;
}

void nav_back_cb(lv_event_t *)
{
    navigate_back(back_view(s_view));
}

void reset_shuffle_queue()
{
    if (s_shuffle_order) heap_caps_free(s_shuffle_order);
    s_shuffle_order = nullptr;
    s_shuffle_count = 0;
    s_shuffle_cursor = 0;
}

void clear_saved_queue()
{
    heap_caps_free(s_saved_queue);
    s_saved_queue = nullptr;
    s_saved_queue_count = 0;
}

void discard_pending_saved_queue()
{
    s_saved_queue_pending = false;
    if (s_playback_scope == PlaybackScope::SavedQueue) clear_saved_queue();
}

bool restore_saved_queue_if_pending()
{
    if (s_has_active_queue) return true;
    if (!s_saved_queue_pending) return false;
    // A failed load is still final for this boot: a new selection should not
    // keep retrying a corrupt or stale snapshot.
    s_saved_queue_pending = false;
    clear_saved_queue();
    auto *queue = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!queue) {
        queue = static_cast<size_t *>(heap_caps_malloc(
            lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!queue) {
        ESP_LOGW(kTag, "cannot allocate saved queue");
        return false;
    }
    size_t current = 0;
    const size_t count = lyra::media::load_queue_snapshot(
        queue, lyra::media::kMaxTracks, &current);
    if (count == 0) {
        heap_caps_free(queue);
        return false;
    }
    s_saved_queue = queue;
    s_saved_queue_count = count;
    s_playback_scope = PlaybackScope::SavedQueue;
    s_queue_position = std::min(current, count - 1);
    s_queue_position_valid = true;
    s_current_track = s_saved_queue[s_queue_position];
    s_shuffle = false;
    reset_shuffle_queue();
    s_audio_eof_seen = false;
    s_has_active_queue = true;
    return true;
}

size_t playback_queue_count()
{
    switch (s_playback_scope) {
        case PlaybackScope::Single: return 1;
        case PlaybackScope::AllSongs: return lyra::media::track_count();
        case PlaybackScope::Group: {
            lyra::media::Group group{};
            return lyra::media::group_at(s_playback_group_kind, s_playback_group, &group) ?
                   group.track_count : 0;
        }
        case PlaybackScope::Playlist: {
            lyra::media::Playlist playlist{};
            return lyra::media::playlist_at(s_playback_playlist, &playlist) ?
                   playlist.track_count : 0;
        }
        case PlaybackScope::Folder: {
            size_t first = 0;
            size_t total = 0;
            lyra::media::folder_tracks(s_playback_folder_path, 0, &first, 1, &total);
            return total;
        }
        case PlaybackScope::Search: {
            const lyra::media::SearchStatus search = lyra::media::search_status();
            return search.ready ? search.result_count : 0;
        }
        case PlaybackScope::SavedQueue: return s_saved_queue_count;
    }
    return 0;
}

bool playback_queue_track_at(size_t position, size_t *track_index)
{
    if (!track_index || position >= playback_queue_count()) return false;
    switch (s_playback_scope) {
        case PlaybackScope::Single:
            *track_index = s_current_track;
            return true;
        case PlaybackScope::AllSongs:
            return lyra::media::sorted_track_at(position, track_index);
        case PlaybackScope::Group:
            return lyra::media::group_tracks(s_playback_group_kind, s_playback_group,
                                              position, track_index, 1) == 1;
        case PlaybackScope::Playlist:
            return lyra::media::playlist_tracks(s_playback_playlist, position,
                                                 track_index, 1) == 1;
        case PlaybackScope::Folder:
            return lyra::media::folder_tracks(s_playback_folder_path, position,
                                               track_index, 1) == 1;
        case PlaybackScope::Search:
        {
            lyra::media::SearchResult result{};
            if (lyra::media::search_results(position, &result, 1) != 1) return false;
            *track_index = result.track_index;
            return true;
        }
        case PlaybackScope::SavedQueue:
            *track_index = s_saved_queue[position];
            return true;
    }
    return false;
}

bool playback_queue_position(size_t track_index, size_t *position)
{
    if (!position) return false;
    const size_t count = playback_queue_count();
    for (size_t i = 0; i < count; ++i) {
        size_t candidate = 0;
        if (playback_queue_track_at(i, &candidate) && candidate == track_index) {
            *position = i;
            return true;
        }
    }
    return false;
}

bool build_shuffle_queue()
{
    reset_shuffle_queue();
    if (!s_shuffle) return true;

    const size_t count = playback_queue_count();
    if (count == 0 || count > static_cast<size_t>(0xFFFFFFFFu)) return false;
    s_shuffle_order = static_cast<uint32_t *>(heap_caps_malloc(
        count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_shuffle_order) {
        s_shuffle_order = static_cast<uint32_t *>(heap_caps_malloc(
            count * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!s_shuffle_order) return false;
    s_shuffle_count = count;
    for (size_t i = 0; i < count; ++i) s_shuffle_order[i] = static_cast<uint32_t>(i);
    for (size_t i = count; i > 1; --i) {
        const size_t other = esp_random() % i;
        std::swap(s_shuffle_order[i - 1], s_shuffle_order[other]);
    }

    size_t current_position = 0;
    size_t saved_track = 0;
    const bool retained_position = s_queue_position_valid && s_queue_position < count &&
                                   playback_queue_track_at(s_queue_position, &saved_track) &&
                                   saved_track == s_current_track;
    if (retained_position) {
        current_position = s_queue_position;
    } else if (!playback_queue_position(s_current_track, &current_position)) {
        reset_shuffle_queue();
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_shuffle_order[i] == current_position) {
            std::swap(s_shuffle_order[0], s_shuffle_order[i]);
            break;
        }
    }
    s_shuffle_cursor = 0;
    s_queue_position = 0;
    s_queue_position_valid = true;
    return true;
}

bool queue_track_at(size_t queue_position, size_t *track_index)
{
    const size_t count = playback_queue_count();
    if (!track_index || queue_position >= count) return false;
    if (!s_shuffle) return playback_queue_track_at(queue_position, track_index);
    if (!s_shuffle_order || s_shuffle_count != count) {
        if (!build_shuffle_queue()) return false;
    }
    return playback_queue_track_at(s_shuffle_order[queue_position], track_index);
}

bool current_queue_position(size_t *queue_position)
{
    if (!queue_position) return false;
    const size_t count = playback_queue_count();
    if (s_queue_position_valid && s_queue_position < count) {
        size_t saved_track = 0;
        if (queue_track_at(s_queue_position, &saved_track) && saved_track == s_current_track) {
            *queue_position = s_queue_position;
            return true;
        }
    }
    if (!s_shuffle) {
        if (!playback_queue_position(s_current_track, queue_position)) return false;
        s_queue_position = *queue_position;
        s_queue_position_valid = true;
        return true;
    }
    if (!s_shuffle_order || s_shuffle_count != count) {
        if (!build_shuffle_queue()) return false;
    }
    if (s_shuffle_cursor >= count) return false;
    *queue_position = s_shuffle_cursor;
    s_queue_position = *queue_position;
    s_queue_position_valid = true;
    return true;
}

void apply_replay_gain_to_current_track()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return;
    const int16_t adjustment = s_replay_gain ? track.replay_gain_tenths_db : 0;
    const esp_err_t result = lyra::audio::set_replay_gain_adjustment(adjustment);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not apply ReplayGain for %s: %s", track.path,
                 esp_err_to_name(result));
    }
}

esp_err_t start_track_audio(const lyra::media::Track &track)
{
    s_pending_track_advance = false;
    s_pending_track_advance_us = 0;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    const esp_err_t transition_result = lyra::audio::set_transition_gain(100);
    if (transition_result != ESP_OK) return transition_result;
    const esp_err_t replay_gain_result = lyra::audio::set_replay_gain_adjustment(
        s_replay_gain ? track.replay_gain_tenths_db : 0);
    if (replay_gain_result != ESP_OK) return replay_gain_result;
    return lyra::audio::play(track.path);
}

void play_queue_position(size_t queue_position)
{
    size_t track_index = 0;
    if (!queue_track_at(queue_position, &track_index)) return;
    if (s_shuffle) s_shuffle_cursor = queue_position;
    s_queue_position = queue_position;
    s_queue_position_valid = true;
    s_current_track = track_index;
    s_has_active_queue = true;
    s_audio_eof_seen = false;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    navigate_to(View::Player);
}

bool move_in_playback_queue(int direction, bool automatic = false)
{
    if (direction == 0) return false;
    const size_t count = playback_queue_count();
    if (count == 0) return false;

    size_t candidate_position = 0;
    if (s_shuffle) {
        if (!s_shuffle_order || s_shuffle_count != count) {
            if (!build_shuffle_queue()) return false;
        }
        const int64_t next = static_cast<int64_t>(s_shuffle_cursor) + direction;
        if (next < 0 || next >= static_cast<int64_t>(count)) {
            if (s_repeat_mode != RepeatMode::All || !build_shuffle_queue()) return false;
            s_shuffle_cursor = direction < 0 ? count - 1 : (count > 1 ? 1 : 0);
            candidate_position = s_shuffle_order[s_shuffle_cursor];
        } else {
            s_shuffle_cursor = static_cast<size_t>(next);
            candidate_position = s_shuffle_order[s_shuffle_cursor];
        }
    } else {
        size_t current_position = 0;
        if (!current_queue_position(&current_position)) return false;
        const int64_t next = static_cast<int64_t>(current_position) + direction;
        if (next < 0 || next >= static_cast<int64_t>(count)) {
            if (s_repeat_mode != RepeatMode::All) return false;
            candidate_position = direction < 0 ? count - 1 : 0;
        } else {
            candidate_position = static_cast<size_t>(next);
        }
    }

    size_t candidate = 0;
    if (!playback_queue_track_at(candidate_position, &candidate)) return false;
    s_queue_position = s_shuffle ? s_shuffle_cursor : candidate_position;
    s_queue_position_valid = true;
    s_current_track = candidate;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    s_audio_eof_seen = false;
    if (!automatic || s_view == View::Player || s_view == View::FullscreenArt) {
        render(s_view == View::FullscreenArt ? View::FullscreenArt : View::Player);
    }
    return true;
}

bool can_move_in_playback_queue(int direction)
{
    if (direction == 0) return false;
    const size_t count = playback_queue_count();
    if (count == 0) return false;
    if (s_shuffle) {
        if (!s_shuffle_order || s_shuffle_count != count) {
            if (!build_shuffle_queue()) return false;
        }
        const int64_t next = static_cast<int64_t>(s_shuffle_cursor) + direction;
        return (next >= 0 && next < static_cast<int64_t>(count)) ||
               s_repeat_mode == RepeatMode::All;
    }
    size_t current_position = 0;
    if (!current_queue_position(&current_position)) return false;
    const int64_t next = static_cast<int64_t>(current_position) + direction;
    return (next >= 0 && next < static_cast<int64_t>(count)) ||
           s_repeat_mode == RepeatMode::All;
}

bool begin_manual_crossfade(int direction)
{
    if (s_crossfade_seconds == 0 || s_crossfade_transition_direction != 0 ||
        s_crossfade_pause_pending ||
        !can_move_in_playback_queue(direction)) return false;
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return false;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = esp_timer_get_time();
    s_crossfade_fade_out_ends_us = s_crossfade_fade_out_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    s_crossfade_transition_direction = direction;
    lyra::audio::set_transition_gain(100);
    return true;
}

bool begin_crossfade_pause()
{
    if (s_crossfade_seconds == 0 || s_crossfade_transition_direction != 0 ||
        s_crossfade_pause_pending) return false;
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return false;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = esp_timer_get_time();
    s_crossfade_fade_out_ends_us = s_crossfade_fade_out_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    s_crossfade_pause_pending = true;
    lyra::audio::set_transition_gain(100);
    return true;
}

void request_manual_queue_move(int direction)
{
    if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;
    if (!begin_manual_crossfade(direction)) move_in_playback_queue(direction, false);
}

void nav_play_cb(lv_event_t *)
{
    if (s_view == View::Player || s_view == View::FullscreenArt) {
        if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;
        const lyra::audio::Status audio_status = lyra::audio::status();
        if (audio_status.playing && !audio_status.paused) {
            if (!begin_crossfade_pause()) lyra::audio::toggle_pause();
        } else if (audio_status.playing && audio_status.paused) {
            if (s_crossfade_seconds == 0) {
                lyra::audio::toggle_pause();
            } else {
                lyra::audio::set_transition_gain(0);
                if (lyra::audio::toggle_pause() == ESP_OK) begin_crossfade_fade_in();
                else lyra::audio::set_transition_gain(100);
            }
        } else if (restart_current_track()) {
            begin_crossfade_fade_in();
        }
    } else {
        show_now_playing();
    }
}

void nav_previous_cb(lv_event_t *)
{
    request_manual_queue_move(-1);
}

void nav_next_cb(lv_event_t *)
{
    request_manual_queue_move(1);
}

bool restart_current_track()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return false;
    const esp_err_t audio_ret = start_track_audio(track);
    if (audio_ret != ESP_OK) {
        ESP_LOGW(kTag, "cannot restart %s: %s", track.path, esp_err_to_name(audio_ret));
        return false;
    }
    s_audio_eof_seen = false;
    return true;
}

void make_virtual_nav()
{
    lv_obj_t *dock = make_box(s_screen, 0, kScreenHeight - kNavHeight,
                              kScreenWidth, kNavHeight, kNavSurface);
    make_box(dock, 0, 0, kScreenWidth, 1, kAccentDark);
    const char *icons[] = {LV_SYMBOL_BARS, LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_LEFT};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *button = make_button(dock, 5 + i * 63, 4, 58, 36, kSurface, 6);
        lv_obj_t *icon = make_label(button, icons[i], i == 0 && s_view == View::Menu ? kAccent : kTextSecondary);
        lv_obj_center(icon);
        if (i == 0) {
            add_route(button, View::Menu);
        } else if (i == 1) {
            lv_obj_add_event_cb(button, nav_previous_cb, LV_EVENT_CLICKED, nullptr);
        } else if (i == 2) {
            lv_obj_add_event_cb(button, nav_play_cb, LV_EVENT_CLICKED, nullptr);
        } else if (i == 3) {
            lv_obj_add_event_cb(button, nav_next_cb, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_add_event_cb(button, nav_back_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
}

void update_status_volume_label(uint8_t volume_percent)
{
    if (!s_status_volume_label) return;
    char volume_text[24];
    std::snprintf(volume_text, sizeof(volume_text), "%s %u", LV_SYMBOL_VOLUME_MID,
                  static_cast<unsigned>(volume_percent));
    lv_label_set_text(s_status_volume_label, volume_text);
}

void close_volume_popup()
{
    if (!s_volume_popup) return;
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
    lv_obj_delete(s_volume_popup);
    s_volume_popup = nullptr;
}

void volume_popup_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const int value = lv_slider_get_value(slider);
    lyra::audio::set_volume(static_cast<uint8_t>(value));
    update_status_volume_label(static_cast<uint8_t>(value));
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label) {
        char text[16];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(value_label, text);
    }
}

void volume_popup_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
}

void show_volume_popup_cb(lv_event_t *)
{
    if (s_volume_popup) {
        lv_obj_move_foreground(s_volume_popup);
        return;
    }

    const lyra::audio::Status audio_status = lyra::audio::status();
    s_volume_popup = make_box(s_screen, 8, kStatusHeight + 4, 224, 104, kSurfaceRaised, 10);
    lv_obj_set_style_border_width(s_volume_popup, 1, 0);
    lv_obj_set_style_border_color(s_volume_popup, kAccentDark, 0);
    lv_obj_set_style_shadow_width(s_volume_popup, 12, 0);
    lv_obj_set_style_shadow_color(s_volume_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_volume_popup, LV_OPA_40, 0);
    lv_obj_move_foreground(s_volume_popup);

    lv_obj_t *title = make_label(s_volume_popup, "Volume", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%",
                  static_cast<unsigned>(audio_status.volume_percent));
    lv_obj_t *value = make_label(s_volume_popup, value_text, kAccent);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -52, 12);
    lv_obj_t *close = make_button(s_volume_popup, 184, 7, 32, 32, kSurface, 16);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_volume_popup(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *slider = lv_slider_create(s_volume_popup);
    lv_obj_set_pos(slider, 14, 60);
    lv_obj_set_size(slider, 196, 14);
    lv_slider_set_range(slider, 0, lyra::audio::maximum_volume_percent());
    lv_slider_set_value(slider, audio_status.volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value);
    lv_obj_add_event_cb(slider, volume_popup_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, volume_popup_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, volume_popup_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

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

lv_obj_t *make_header(const char *title, View back, bool show_back = false, const char *right = nullptr,
                      lv_event_cb_t custom_back = nullptr)
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
    lv_obj_t *close_label = make_label(close, "OK", kTextOnAccent);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
}

lv_obj_t *make_row(lv_obj_t *parent, int y, const char *icon_text, const char *title,
                   const char *subtitle, View target, int height = 54)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 6);
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

lv_image_dsc_t s_player_art_descriptor{};
uint8_t *s_player_art_pixels;
char s_player_art_album[lyra::media::kMaxName]{};
uint32_t s_player_art_catalog_generation;
uint32_t s_player_art_generation;
lv_image_dsc_t s_boot_image_descriptor{};
uint8_t *s_boot_image_pixels;
lv_image_dsc_t s_brand_logo_descriptor{};
uint8_t *s_brand_logo_pixels;

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
                       bool preserve_aspect = false)
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

void play_track_from_row(size_t track_index, bool force_single)
{
    discard_pending_saved_queue();
    s_has_active_queue = true;
    if (force_single) {
        s_playback_scope = PlaybackScope::Single;
    } else if (s_view == View::LibrarySongs) {
        s_playback_scope = PlaybackScope::AllSongs;
    } else if (s_view == View::AlbumDetail || s_view == View::TrackList ||
               (s_view == View::ArtistDetail && s_artist_detail_tab == ArtistDetailTab::Songs)) {
        s_playback_scope = PlaybackScope::Group;
        s_playback_group_kind = s_view == View::AlbumDetail ?
            lyra::media::GroupKind::Album : s_selected_group_kind;
        s_playback_group = s_selected_group;
    } else if (s_view == View::PlaylistDetail) {
        s_playback_scope = PlaybackScope::Playlist;
        s_playback_playlist = s_selected_playlist;
    } else if (s_view == View::Folders || s_view == View::FolderDetail) {
        s_playback_scope = PlaybackScope::Folder;
        copy_ui_text(s_playback_folder_path, sizeof(s_playback_folder_path), s_folder_path);
    } else if (s_view == View::Search) {
        s_playback_scope = PlaybackScope::Search;
    } else {
        s_playback_scope = PlaybackScope::Single;
    }
    s_current_track = track_index;
    reset_shuffle_queue();
    s_queue_position_valid = false;
    current_queue_position(&s_queue_position);
    s_audio_eof_seen = false;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    navigate_to(View::Player);
}

void track_route_cb(lv_event_t *event)
{
    play_track_from_row(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)), false);
}

void single_track_route_cb(lv_event_t *event)
{
    play_track_from_row(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)), true);
}

void add_playlist_track_cb(lv_event_t *event);

void add_track_route(lv_obj_t *row, size_t track_index, bool force_single = false)
{
    lv_obj_add_event_cb(row, force_single ? single_track_route_cb : track_route_cb,
                        LV_EVENT_CLICKED, reinterpret_cast<void *>(track_index));
}

void make_song_row(lv_obj_t *parent, int y, size_t track_index, int height = 54,
                   bool force_single = false)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 5);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, s_playlist_add_mode ? 244 : 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    lv_obj_t *artist = make_label(row, track.artist, kTextSecondary);
    make_marquee(artist, s_playlist_add_mode ? 244 : 282);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
    if (s_playlist_add_mode) {
        lv_obj_t *add = make_label(row, LV_SYMBOL_PLUS, kAccent);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, add_playlist_track_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(track_index));
    } else {
        add_track_route(row, track_index, force_single);
    }
}

void add_search_playlist_route(lv_obj_t *row, size_t track_index, size_t playlist_index)
{
    // kMaxTracks is the result-buffer ceiling, so this compact payload fits
    // safely on the 32-bit ESP32 event user-data field.
    const uintptr_t payload = playlist_index * lyra::media::kMaxTracks + track_index;
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
        const uintptr_t payload = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
        const size_t playlist_index = payload / lyra::media::kMaxTracks;
        const size_t track_index = payload % lyra::media::kMaxTracks;
        if (playlist_index >= lyra::media::playlist_count()) return;
        discard_pending_saved_queue();
        s_has_active_queue = true;
        s_playback_scope = PlaybackScope::Playlist;
        s_playback_playlist = playlist_index;
        s_current_track = track_index;
        reset_shuffle_queue();
        s_queue_position_valid = false;
        current_queue_position(&s_queue_position);
        s_audio_eof_seen = false;
        lyra::media::Track track{};
        if (lyra::media::track_at(s_current_track, &track)) {
            const esp_err_t audio_ret = start_track_audio(track);
            if (audio_ret != ESP_OK) {
                ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
            }
        }
        navigate_to(View::Player);
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(payload));
}

void make_search_playlist_row(lv_obj_t *parent, int y,
                              const lyra::media::SearchResult &result)
{
    lyra::media::Track track{};
    lyra::media::Playlist playlist{};
    if (!lyra::media::track_at(result.track_index, &track) ||
        !lyra::media::playlist_at(result.playlist_index, &playlist)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    lv_obj_t *source = make_label(row, playlist.name, kTextSecondary);
    make_marquee(source, 282);
    lv_obj_align(source, LV_ALIGN_LEFT_MID, 12, 11);
    add_search_playlist_route(row, result.track_index, result.playlist_index);
}

void search_album_result_cb(lv_event_t *event)
{
    const size_t group_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, group_index, &group)) return;
    s_selected_group_kind = lyra::media::GroupKind::Album;
    s_selected_group = group_index;
    s_library_tab = LibraryTab::Albums;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
    navigate_to(View::AlbumDetail);
}

void search_artist_result_cb(lv_event_t *event)
{
    const size_t group_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Artist, group_index, &group)) return;
    s_selected_group_kind = lyra::media::GroupKind::Artist;
    s_selected_group = group_index;
    s_library_tab = LibraryTab::Artists;
    s_artist_detail_tab = ArtistDetailTab::Songs;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
    navigate_to(View::ArtistDetail);
}

void make_file_row(lv_obj_t *parent, int y, size_t track_index, int height = 54)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    const char *filename = std::strrchr(track.path, '/');
    filename = filename ? filename + 1 : track.path;
    lv_obj_t *row = make_button(parent, 7, y, 306, height, kSurface, 5);
    lv_obj_t *icon = make_label(row, LV_SYMBOL_FILE, kAccent);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *name = make_label(row, filename, kTextPrimary);
    make_marquee(name, 251);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 43, -9);
    char size[24];
    if (track.size_bytes < 1024u * 1024u) std::snprintf(size, sizeof(size), "%.0f KB", track.size_bytes / 1024.0);
    else std::snprintf(size, sizeof(size), "%.2f MB", track.size_bytes / (1024.0 * 1024.0));
    lv_obj_t *size_label = make_label(row, size, kTextSecondary);
    lv_obj_align(size_label, LV_ALIGN_LEFT_MID, 43, 11);
    add_track_route(row, track_index);
}

void make_album_art(lv_obj_t *parent, int x, int y, int width, int height,
                    const lyra::media::Track &track, bool preserve_aspect = false)
{
    make_artwork(parent, x, y, width, height, track, 13, preserve_aspect);
}

void page_cb(lv_event_t *event)
{
    const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (action == 0 && s_list_page > 0) --s_list_page;
    else if (action == 1 && s_list_page + 1 < s_list_page_count) ++s_list_page;
    else if (action == 2) s_list_page = 0;
    else if (action == 3 && s_list_page_count) s_list_page = s_list_page_count - 1;
    render(s_view);
}

void make_page_controls(lv_obj_t *parent, int y, size_t total,
                        size_t page_size = lyra::media::kTrackPageSize)
{
    const size_t pages = (total + page_size - 1) / page_size;
    if (pages <= 1) return;
    s_list_page_count = pages;
    lv_obj_t *first = make_button(parent, 7, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *first_label = make_label(first, LV_SYMBOL_PREV, s_list_page ? kTextPrimary : kTextMuted);
    lv_obj_center(first_label);
    if (s_list_page) lv_obj_add_event_cb(first, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(2));

    lv_obj_t *previous = make_button(parent, 59, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *previous_label = make_label(previous, LV_SYMBOL_LEFT, s_list_page ? kTextPrimary : kTextMuted);
    lv_obj_center(previous_label);
    if (s_list_page) lv_obj_add_event_cb(previous, page_cb, LV_EVENT_CLICKED, nullptr);

    char page_text[32];
    std::snprintf(page_text, sizeof(page_text), "%u / %u",
                  static_cast<unsigned>(s_list_page + 1), static_cast<unsigned>(pages));
    lv_obj_t *page = make_label(parent, page_text, kTextSecondary);
    lv_obj_align(page, LV_ALIGN_TOP_MID, 0, y + 13);

    lv_obj_t *next = make_button(parent, 213, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *next_label = make_label(next, LV_SYMBOL_RIGHT,
                                      s_list_page + 1 < pages ? kTextPrimary : kTextMuted);
    lv_obj_center(next_label);
    if (s_list_page + 1 < pages) {
        lv_obj_add_event_cb(next, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(1));
    }
    lv_obj_t *last = make_button(parent, 265, y, 48, 42, kSurfaceRaised, 6);
    lv_obj_t *last_label = make_label(last, LV_SYMBOL_NEXT,
                                      s_list_page + 1 < pages ? kTextPrimary : kTextMuted);
    lv_obj_center(last_label);
    if (s_list_page + 1 < pages) {
        lv_obj_add_event_cb(last, page_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(3));
    }
}

void open_queue_cb(lv_event_t *)
{
    if (s_view == View::Queue) return;
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice("Queue empty", "Select a song to begin playback\nand build a queue.");
        return;
    }
    push_navigation_state();
    const size_t page_size = library_page_size();
    size_t current = 0;
    s_list_page = current_queue_position(&current) ? current / page_size : 0;
    render(View::Queue);
}

bool show_now_playing()
{
    if (!s_has_active_queue && !restore_saved_queue_if_pending()) {
        show_notice("Queue empty", "Select a song to begin playback\nbefore opening Now Playing.");
        return false;
    }
    navigate_to(View::Player);
    return true;
}

void open_library_cb(lv_event_t *)
{
    const lyra::media::Status status = lyra::media::status();
    if (!status.mounted) {
        show_notice("No MicroSD card", "Insert a MicroSD card to use\nthe music library.");
    } else if (status.track_count == 0) {
        show_notice("No scanned songs", "Use Settings > System > Scan\nmusic library first.");
    } else {
        navigate_to(View::Library);
    }
}

void open_folders_cb(lv_event_t *)
{
    if (!lyra::media::status().mounted) {
        show_notice("No MicroSD card", "Insert a MicroSD card to browse\nits folders.");
    } else {
        navigate_to(View::Folders);
    }
}

void render_menu()
{
    make_header("Menu", View::Menu);
    lv_obj_t *body = make_scroll_body(72);
    struct MenuItem { const char *icon; const char *label; View view; };
    constexpr MenuItem items[] = {
        {LV_SYMBOL_AUDIO, "Now Playing", View::Player},
        {LV_SYMBOL_LIST, "Music Library", View::Library},
        {LV_SYMBOL_DIRECTORY, "Browse Folders", View::Folders},
        {LV_SYMBOL_LIST, "Queue", View::Queue},
        {LV_SYMBOL_SETTINGS, "Equalizer", View::Equalizer},
        {LV_SYMBOL_EDIT, "Search", View::Search},
        {LV_SYMBOL_SETTINGS, "Settings", View::Settings},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        lv_obj_t *row = make_row(body, static_cast<int>(i) * 56, items[i].icon, items[i].label,
                                 nullptr, items[i].view, 51);
        if (items[i].view == View::Queue) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_queue_cb, LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Player) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, [](lv_event_t *) { show_now_playing(); },
                                LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Library) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_library_cb, LV_EVENT_CLICKED, nullptr);
        } else if (items[i].view == View::Folders) {
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row, open_folders_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
}

void queue_track_cb(lv_event_t *event)
{
    play_queue_position(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void make_queue_song_row(lv_obj_t *parent, int y, size_t queue_position, bool current)
{
    size_t track_index = 0;
    lyra::media::Track track{};
    if (!queue_track_at(queue_position, &track_index) || !lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, current ? kAccentDark : kSurface, 5);
    const int text_x = current ? 45 : 12;
    if (current) {
        lv_obj_t *playing = make_label(row, LV_SYMBOL_PLAY, kTextOnAccent);
        lv_obj_align(playing, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(playing, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *title = make_label(row, track.title, current ? kTextOnAccent : kTextPrimary);
    make_marquee(title, current ? 220 : 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, text_x, -9);
    lv_obj_t *artist = make_label(row, track.artist, current ? kTextOnAccent : kTextSecondary);
    make_marquee(artist, current ? 220 : 282);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, text_x, 11);
    lv_obj_add_event_cb(row, queue_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(queue_position));
}

void render_queue()
{
    const size_t count = s_has_active_queue ? playback_queue_count() : 0;
    char count_label[24];
    std::snprintf(count_label, sizeof(count_label), "%u TRACKS", static_cast<unsigned>(count));
    make_header("Queue", View::Menu, true, count_label);
    lv_obj_t *list = make_scroll_body(72);
    if (count == 0) {
        make_label(list, "No active queue. Select a song to begin playback.", kTextMuted);
        return;
    }

    const size_t page_size = library_page_size();
    const size_t pages = (count + page_size - 1) / page_size;
    if (s_list_page >= pages) s_list_page = pages - 1;
    size_t current = count;
    current_queue_position(&current);
    const size_t first = s_list_page * page_size;
    const size_t shown = std::min(page_size, count - first);
    for (size_t i = 0; i < shown; ++i) {
        make_queue_song_row(list, static_cast<int>(i) * 58, first + i, first + i == current);
    }
    make_page_controls(list, static_cast<int>(shown) * 58 + 4, count, page_size);
}

View library_section_view(LibraryTab section)
{
    switch (section) {
        case LibraryTab::Songs: return View::LibrarySongs;
        case LibraryTab::Artists: return View::LibraryArtists;
        case LibraryTab::Albums: return View::LibraryAlbums;
        case LibraryTab::Genres: return View::LibraryGenres;
        case LibraryTab::Years: return View::LibraryYears;
    }
    return View::Library;
}

lyra::media::GroupKind group_kind(LibraryTab section)
{
    switch (section) {
        case LibraryTab::Artists: return lyra::media::GroupKind::Artist;
        case LibraryTab::Albums: return lyra::media::GroupKind::Album;
        case LibraryTab::Genres: return lyra::media::GroupKind::Genre;
        case LibraryTab::Years: return lyra::media::GroupKind::Year;
        case LibraryTab::Songs: break;
    }
    return lyra::media::GroupKind::Artist;
}

void open_track_list_cb(lv_event_t *event)
{
    const size_t selected_group = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const lyra::media::GroupKind selected_kind = group_kind(s_library_tab);
    lyra::media::Group selected{};
    if (!lyra::media::group_at(selected_kind, selected_group, &selected)) return;
    push_navigation_state();
    s_selected_group = selected_group;
    s_selected_group_kind = selected_kind;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), selected.name);
    s_list_page = 0;
    if (s_library_tab == LibraryTab::Albums) {
        render(View::AlbumDetail);
    } else if (s_library_tab == LibraryTab::Artists) {
        s_artist_detail_tab = ArtistDetailTab::Songs;
        render(View::ArtistDetail);
    } else {
        render(View::TrackList);
    }
}

void open_album_detail_cb(lv_event_t *event)
{
    const size_t selected_group = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    lyra::media::Group selected{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, selected_group, &selected)) return;
    push_navigation_state();
    s_library_tab = LibraryTab::Albums;
    s_selected_group = selected_group;
    s_selected_group_kind = lyra::media::GroupKind::Album;
    copy_ui_text(s_track_list_title, sizeof(s_track_list_title), selected.name);
    s_list_page = 0;
    render(View::AlbumDetail);
}

void open_library_playlists_cb(lv_event_t *)
{
    push_navigation_state();
    s_playlists_from_library = true;
    s_playlist_manage_mode = false;
    s_list_page = 0;
    render(View::Playlists);
}

void render_library()
{
    const bool adding_to_playlist = s_playlist_add_mode;
    char add_title[lyra::media::kMaxName + 12] = "Add Songs";
    if (adding_to_playlist) {
        lyra::media::Playlist playlist{};
        if (lyra::media::playlist_at(s_selected_playlist, &playlist)) {
            std::snprintf(add_title, sizeof(add_title), "Add to %s", playlist.name);
        }
    }
    make_header(adding_to_playlist ? add_title : "Music Library",
                adding_to_playlist ? View::PlaylistDetail : View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    make_row(body, 0, nullptr, "Songs", "Browse every song", View::LibrarySongs, 54);
    make_row(body, 58, nullptr, "Albums", "Artwork and album summaries", View::LibraryAlbums, 54);
    make_row(body, 116, nullptr, "Artists", "Browse by artist", View::LibraryArtists, 54);
    make_row(body, 174, nullptr, "Genres", "Browse by genre", View::LibraryGenres, 54);
    make_row(body, 232, nullptr, "Year", "Browse by release year", View::LibraryYears, 54);
    if (adding_to_playlist) {
        lv_obj_t *hint = make_label(body, "Select a song to add it to this playlist.", kTextMuted);
        lv_obj_set_pos(hint, 12, 300);
        return;
    }
    lv_obj_t *playlists = make_row(body, 290, nullptr, "Playlists", "Your saved playlists",
                                   View::Playlists, 54);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, open_library_playlists_cb, LV_EVENT_CLICKED, nullptr);
}

void make_album_row(lv_obj_t *parent, int y, size_t group_index)
{
    lyra::media::Group group{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, group_index, &group)) return;
    // The catalog representative supplies the artist subtitle. Album rows are
    // intentionally text-only; artwork is reserved for overview and player.
    const size_t representative = group.representative_track;
    lyra::media::Track track{};
    if (!lyra::media::track_at(representative, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 60, kSurface, 5);
    lv_obj_t *title = make_label(row, group.name, kTextPrimary);
    make_marquee(title, 282);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -10);
    char subtitle[lyra::media::kMaxName + 24];
    std::snprintf(subtitle, sizeof(subtitle), group.track_count == 1 ? "%s / 1 song" : "%s / %u songs",
                  track.artist, static_cast<unsigned>(group.track_count));
    lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
    make_marquee(sub, 282);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_add_event_cb(row, open_album_detail_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(group_index));
}

void render_library_section(LibraryTab section)
{
    s_library_tab = section;
    const char *title = section == LibraryTab::Songs ? "Songs" :
                        section == LibraryTab::Artists ? "Artists" :
                        section == LibraryTab::Albums ? "Albums" :
                        section == LibraryTab::Genres ? "Genres" : "Year";
    make_header(title, View::Library, true);
    lv_obj_t *body = make_scroll_body(72);
    const size_t page_size = library_page_size();
    if (section == LibraryTab::Songs) {
        const size_t count = lyra::media::track_count();
        const size_t pages = (count + page_size - 1) / page_size;
        if (pages && s_list_page >= pages) s_list_page = pages - 1;
        const size_t first = s_list_page * page_size;
        const size_t shown = std::min(page_size, count - std::min(first, count));
        for (size_t i = 0; i < shown; ++i) {
            size_t track_index = 0;
            if (lyra::media::sorted_track_at(first + i, &track_index)) {
                make_song_row(body, static_cast<int>(i) * 58, track_index, 54);
            }
        }
        make_page_controls(body, static_cast<int>(shown) * 58 + 4, count, page_size);
        if (count == 0) make_label(body, "No music scanned. Use Settings > System > Scan music library.", kTextMuted);
        return;
    }

    const lyra::media::GroupKind kind = group_kind(section);
    const size_t count = lyra::media::group_count(kind);
    const size_t pages = (count + page_size - 1) / page_size;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    const size_t first = s_list_page * page_size;
    const size_t shown = std::min(page_size, count - std::min(first, count));
    for (size_t i = 0; i < shown; ++i) {
        size_t group_index = first + i;
        if (section == LibraryTab::Albums || section == LibraryTab::Artists) {
            const lyra::media::SortSection sort_section = section == LibraryTab::Albums ?
                lyra::media::SortSection::Albums : lyra::media::SortSection::Artists;
            if (!lyra::media::sorted_group_index_at(sort_section, first + i, &group_index)) continue;
        }
        if (section == LibraryTab::Albums) {
            make_album_row(body, static_cast<int>(i) * 63, group_index);
            continue;
        }
        lyra::media::Group group{};
        if (!lyra::media::group_at(kind, group_index, &group)) continue;
        char subtitle[28];
        std::snprintf(subtitle, sizeof(subtitle), group.track_count == 1 ? "1 song" : "%u songs",
                      static_cast<unsigned>(group.track_count));
        lv_obj_t *row = make_row(body, static_cast<int>(i) * 58, nullptr,
                                 group.name, subtitle, View::TrackList, 54);
        lv_obj_remove_event_cb(row, route_cb);
        lv_obj_add_event_cb(row, open_track_list_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(group_index));
    }
    const int row_height = section == LibraryTab::Albums ? 63 : 58;
    make_page_controls(body, static_cast<int>(shown) * row_height + 4, count, page_size);
}

void render_track_list()
{
    char count_label[24];
    lyra::media::Group group{};
    if (!lyra::media::group_at(s_selected_group_kind, s_selected_group, &group)) return;
    const size_t matches = group.track_count;
    std::snprintf(count_label, sizeof(count_label), "%u TRACKS", static_cast<unsigned>(matches));
    make_header(s_track_list_title, library_section_view(s_library_tab), true, count_label);
    lv_obj_t *list = make_scroll_body(72);
    const size_t pages = (matches + lyra::media::kTrackPageSize - 1) / lyra::media::kTrackPageSize;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    size_t indices[lyra::media::kTrackPageSize];
    const size_t shown = lyra::media::group_tracks(s_selected_group_kind, s_selected_group,
        s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize);
    for (size_t i = 0; i < shown; ++i) make_song_row(list, static_cast<int>(i) * 58, indices[i], 54);
    make_page_controls(list, static_cast<int>(shown) * 58 + 4, matches);
}

void artist_detail_tab_cb(lv_event_t *event)
{
    s_artist_detail_tab = static_cast<ArtistDetailTab>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_list_page = 0;
    render(View::ArtistDetail);
}

void make_artist_detail_tab(lv_obj_t *parent, int x, const char *label,
                            ArtistDetailTab tab)
{
    lv_obj_t *button = make_button(parent, x, 0, 149, 40,
                                   s_artist_detail_tab == tab ? kAccentDark : kSurface, 6);
    lv_obj_t *text = make_label(button, label,
                                s_artist_detail_tab == tab ? kTextOnAccent : kTextSecondary);
    lv_obj_center(text);
    lv_obj_add_event_cb(button, artist_detail_tab_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
}

struct ArtistDetailListContext {
    size_t artist_group;
    size_t entry_count;
    size_t loaded;
    ArtistDetailTab tab;
};

void append_artist_detail_rows(lv_obj_t *body, ArtistDetailListContext *context)
{
    if (!body || !context || context->loaded >= context->entry_count) return;
    constexpr size_t kArtistDetailBatchSize = 8;
    const size_t start = context->loaded;
    const size_t batch = std::min(kArtistDetailBatchSize, context->entry_count - start);
    if (context->tab == ArtistDetailTab::Songs) {
        size_t indices[kArtistDetailBatchSize];
        const size_t found = lyra::media::group_tracks(lyra::media::GroupKind::Artist,
            context->artist_group, start, indices, batch);
        for (size_t i = 0; i < found; ++i) {
            make_song_row(body, 48 + static_cast<int>(start + i) * 58, indices[i], 54);
        }
        context->loaded += found;
        return;
    }

    for (size_t i = 0; i < batch; ++i) {
        size_t album_group = 0;
        if (lyra::media::artist_album_at(context->artist_group, start + i, &album_group)) {
            make_album_row(body, 48 + static_cast<int>(start + i) * 63, album_group);
        }
    }
    context->loaded += batch;
}

void artist_detail_list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<ArtistDetailListContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(context);
        return;
    }
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 140) append_artist_detail_rows(body, context);
}

void render_artist_detail()
{
    lyra::media::Group artist{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Artist, s_selected_group, &artist)) return;
    make_header(artist.name, View::LibraryArtists, true);
    lv_obj_t *body = make_scroll_body(72);
    make_artist_detail_tab(body, 7, "Songs", ArtistDetailTab::Songs);
    make_artist_detail_tab(body, 164, "Albums", ArtistDetailTab::Albums);

    const size_t total = s_artist_detail_tab == ArtistDetailTab::Songs ?
        artist.track_count : lyra::media::artist_album_count(s_selected_group);
    if (total == 0) {
        make_label(body, s_artist_detail_tab == ArtistDetailTab::Songs ?
                   "This artist has no indexed songs." : "This artist has no indexed albums.",
                   kTextMuted);
        return;
    }

    auto *context = static_cast<ArtistDetailListContext *>(
        lv_malloc_zeroed(sizeof(ArtistDetailListContext)));
    if (context) {
        context->artist_group = s_selected_group;
        context->entry_count = total;
        context->tab = s_artist_detail_tab;
        append_artist_detail_rows(body, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, artist_detail_list_event_cb, LV_EVENT_DELETE, context);
    }
}

struct AlbumListContext {
    size_t group_index;
    size_t track_count;
    size_t loaded;
};

void append_album_song_rows(lv_obj_t *body, AlbumListContext *context)
{
    if (!body || !context || context->loaded >= context->track_count) return;
    constexpr size_t kAlbumBatchSize = 8;
    size_t indices[kAlbumBatchSize];
    const size_t found = lyra::media::group_tracks(lyra::media::GroupKind::Album,
        context->group_index, context->loaded, indices, kAlbumBatchSize);
    for (size_t i = 0; i < found; ++i) {
        make_song_row(body, 130 + static_cast<int>(context->loaded + i) * 58, indices[i], 54);
    }
    context->loaded += found;
}

void album_list_event_cb(lv_event_t *event)
{
    auto *context = static_cast<AlbumListContext *>(lv_event_get_user_data(event));
    if (!context) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(context);
        return;
    }
    lv_obj_t *body = lv_event_get_current_target_obj(event);
    if (lv_obj_get_scroll_bottom(body) < 140) append_album_song_rows(body, context);
}

void render_album_detail()
{
    lyra::media::Group album{};
    if (!lyra::media::group_at(lyra::media::GroupKind::Album, s_selected_group, &album)) return;
    const size_t representative = album.representative_track;
    lyra::media::Track track{};
    if (!lyra::media::track_at(representative, &track)) return;
    make_header(album.name, View::LibraryAlbums, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *summary = make_box(body, 7, 0, 306, 122, kSurface, 7);
    make_album_art(summary, 8, 8, 106, 106, track);
    lv_obj_t *name = make_label(summary, album.name, kTextPrimary);
    lv_obj_set_style_text_font(name, lyra::font::ui(), 0);
    make_marquee(name, 174);
    lv_obj_set_pos(name, 124, 12);
    lv_obj_t *artist = make_label(summary, track.artist, kTextSecondary);
    make_marquee(artist, 174);
    lv_obj_set_pos(artist, 124, 40);
    // genre can occupy kMaxName - 1 bytes and year up to 7 bytes.
    char tags[lyra::media::kMaxName + 11];
    std::snprintf(tags, sizeof(tags), "%s / %s", track.genre, track.year);
    lv_obj_t *tag_label = make_label(summary, tags, kTextMuted);
    make_marquee(tag_label, 174);
    lv_obj_set_pos(tag_label, 124, 65);
    char count[28];
    std::snprintf(count, sizeof(count), album.track_count == 1 ? "1 song" : "%u songs",
                  static_cast<unsigned>(album.track_count));
    lv_obj_t *count_label = make_label(summary, count, kAccent);
    lv_obj_set_pos(count_label, 124, 91);

    auto *context = static_cast<AlbumListContext *>(lv_malloc_zeroed(sizeof(AlbumListContext)));
    if (context) {
        context->group_index = s_selected_group;
        context->track_count = album.track_count;
        append_album_song_rows(body, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_SCROLL_END, context);
        lv_obj_add_event_cb(body, album_list_event_cb, LV_EVENT_DELETE, context);
    }
}

void folder_cb(lv_event_t *event)
{
    const size_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= s_folder_count) return;
    char child_path[sizeof(s_folder_path)];
    copy_ui_text(child_path, sizeof(child_path), s_folder_path);
    if (!append_ui_text(child_path, sizeof(child_path), "/") ||
        !append_ui_text(child_path, sizeof(child_path), s_folder_names[index])) return;
    push_navigation_state();
    copy_ui_text(s_folder_path, sizeof(s_folder_path), child_path);
    s_list_page = 0;
    render(View::FolderDetail);
}

void folder_back_cb(lv_event_t *)
{
    navigate_back(View::Folders);
}

void render_folders(bool detail)
{
    if (!detail) copy_ui_text(s_folder_path, sizeof(s_folder_path), "/sdcard");
    make_header(detail ? s_folder_path + std::strlen("/sdcard/") : "Browse Folders",
                detail ? View::Folders : View::Menu, true, detail ? "MICROSD" : nullptr,
                detail ? folder_back_cb : nullptr);
    lv_obj_t *list = make_scroll_body(72);
    size_t total_folders = 0;
    s_folder_count = lyra::media::child_folders(s_folder_path, 0, s_folder_names, 1, &total_folders);
    size_t total_tracks = 0;
    size_t ignored_indices[1];
    lyra::media::folder_tracks(s_folder_path, 0, ignored_indices, 1, &total_tracks);
    const size_t total_entries = total_folders + total_tracks;
    const size_t pages = (total_entries + lyra::media::kTrackPageSize - 1) /
                         lyra::media::kTrackPageSize;
    if (pages && s_list_page >= pages) s_list_page = pages - 1;
    const size_t first_entry = s_list_page * lyra::media::kTrackPageSize;
    const size_t last_entry = std::min(first_entry + lyra::media::kTrackPageSize, total_entries);
    size_t row = 0;
    const size_t first_folder = std::min(first_entry, total_folders);
    const size_t last_folder = std::min(last_entry, total_folders);
    const size_t folder_capacity = last_folder - first_folder;
    s_folder_count = folder_capacity == 0 ? 0 : lyra::media::child_folders(
        s_folder_path, first_folder, s_folder_names, folder_capacity, &total_folders);
    for (size_t i = 0; i < s_folder_count; ++i) {
        lv_obj_t *folder = make_row(list, static_cast<int>(row++) * 58, LV_SYMBOL_DIRECTORY,
                                    s_folder_names[i], "Folder", View::FolderDetail, 54);
        lv_obj_remove_event_cb(folder, route_cb);
        lv_obj_add_event_cb(folder, folder_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
    }
    size_t indices[lyra::media::kTrackPageSize];
    const size_t track_offset = first_entry > total_folders ? first_entry - total_folders : 0;
    const size_t track_capacity = last_entry > total_folders ?
        last_entry - std::max(first_entry, total_folders) : 0;
    const size_t tracks = track_capacity == 0 ? 0 : lyra::media::folder_tracks(
        s_folder_path, track_offset, indices, track_capacity, &total_tracks);
    for (size_t i = 0; i < tracks; ++i) make_file_row(list, static_cast<int>(row++) * 58, indices[i], 54);
    make_page_controls(list, static_cast<int>(row) * 58 + 4, total_entries);
    if (row == 0) make_label(list, "This folder contains no scanned audio.", kTextMuted);
}

void playlist_cb(lv_event_t *event)
{
    push_navigation_state();
    s_selected_playlist = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    s_list_page = 0;
    s_playlist_manage_mode = false;
    render(View::PlaylistDetail);
}

void open_playlist_add_cb(lv_event_t *)
{
    navigate_to(View::PlaylistAdd);
}

void toggle_playlist_manage_cb(lv_event_t *)
{
    s_playlist_manage_mode = !s_playlist_manage_mode;
    render(s_view);
}

void confirm_playlist_manage_action_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    const bool deleting_playlist =
        s_pending_playlist_manage_action == PlaylistManageAction::DeletePlaylist;
    const esp_err_t result = deleting_playlist ?
        lyra::media::delete_playlist(s_pending_playlist) :
        lyra::media::remove_from_playlist(s_pending_playlist, s_pending_playlist_track);
    if (overlay) lv_obj_delete(overlay);
    if (deleting_playlist) {
        s_playlist_manage_mode = result != ESP_OK;
        s_list_page = 0;
        render(View::Playlists);
    } else {
        render(View::PlaylistDetail);
    }
}

void show_playlist_manage_confirmation(PlaylistManageAction action, size_t playlist_index,
                                       size_t track_index = 0)
{
    lyra::media::Playlist playlist{};
    if (!lyra::media::playlist_at(playlist_index, &playlist)) return;
    s_pending_playlist_manage_action = action;
    s_pending_playlist = playlist_index;
    s_pending_playlist_track = track_index;
    const bool favorites = std::strcmp(playlist.name, "Favorites") == 0;
    const char *title = action == PlaylistManageAction::RemoveTrack ? "Remove song?" :
                        favorites ? "Clear Favorites?" : "Delete playlist?";
    const char *message = action == PlaylistManageAction::RemoveTrack ?
        "This song will be removed from\nthis playlist." : favorites ?
        "All favorite songs will be cleared.\nThe Favorites playlist remains available." :
        "The playlist will be removed.\nMusic files stay on the MicroSD card.";
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                                 kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 126, 280, 228, kSurfaceRaised, 12);
    lv_obj_t *heading = make_label(dialog, title, kTextPrimary);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *detail = make_label(dialog, message, kTextSecondary);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(detail, 240);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, -12);
    lv_obj_t *cancel = make_button(dialog, 14, 166, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 166, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, action == PlaylistManageAction::RemoveTrack ?
                                          "REMOVE" : favorites ? "CLEAR" : "DELETE", kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_playlist_manage_action_cb, LV_EVENT_CLICKED, overlay);
}

void delete_playlist_cb(lv_event_t *event)
{
    show_playlist_manage_confirmation(PlaylistManageAction::DeletePlaylist,
                                      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void remove_playlist_track_cb(lv_event_t *event)
{
    show_playlist_manage_confirmation(PlaylistManageAction::RemoveTrack, s_selected_playlist,
                                      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void make_playlist_manage_row(lv_obj_t *parent, int y, size_t playlist_index,
                              const lyra::media::Playlist &playlist)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 58, kSurface, 6);
    lv_obj_t *name = make_label(row, playlist.name, kTextPrimary);
    make_marquee(name, 205);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, -10);
    char subtitle[28];
    std::snprintf(subtitle, sizeof(subtitle), "%u songs", static_cast<unsigned>(playlist.track_count));
    lv_obj_t *sub = make_label(row, subtitle, kTextSecondary);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 11);
    lv_obj_t *remove = make_button(row, 246, 11, 48, 36, lv_color_hex(0x991B1B), 5);
    lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_TRASH, kTextOnAccent);
    lv_obj_center(remove_icon);
    lv_obj_add_event_cb(remove, delete_playlist_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(playlist_index));
}

void make_playlist_track_manage_row(lv_obj_t *parent, int y, size_t track_index)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(track_index, &track)) return;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54, kSurface, 5);
    lv_obj_t *title = make_label(row, track.title, kTextPrimary);
    make_marquee(title, 220);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, -9);
    lv_obj_t *artist = make_label(row, track.artist, kTextSecondary);
    make_marquee(artist, 220);
    lv_obj_align(artist, LV_ALIGN_LEFT_MID, 12, 11);
    lv_obj_t *remove = make_button(row, 246, 9, 48, 36, lv_color_hex(0x991B1B), 5);
    lv_obj_t *remove_icon = make_label(remove, LV_SYMBOL_TRASH, kTextOnAccent);
    lv_obj_center(remove_icon);
    lv_obj_add_event_cb(remove, remove_playlist_track_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(track_index));
}

void render_playlists(bool detail)
{
    // Returning to a playlist always ends the temporary library selection mode.
    s_playlist_add_mode = false;
    lyra::media::Playlist selected{};
    if (detail) lyra::media::playlist_at(s_selected_playlist, &selected);
    const char *title = detail ? selected.name : "Playlists";
    make_header(title, detail ? View::Playlists :
                (s_playlists_from_library ? View::Library : View::Menu), true, "");
    lv_obj_t *plus = make_button(s_screen, 220, 32, 44, 36, kBackground, 4);
    lv_obj_t *plus_label = make_label(plus, LV_SYMBOL_PLUS, kAccent);
    lv_obj_center(plus_label);
    if (detail) {
        lv_obj_add_event_cb(plus, open_playlist_add_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        add_route(plus, View::PlaylistCreate);
    }
    lv_obj_t *manage = make_button(s_screen, 270, 32, 44, 36, kBackground, 4);
    lv_obj_t *manage_label = make_label(manage,
                                        s_playlist_manage_mode ? LV_SYMBOL_CLOSE : LV_SYMBOL_EDIT,
                                        s_playlist_manage_mode ? kTextSecondary : kAccent);
    lv_obj_center(manage_label);
    lv_obj_add_event_cb(manage, toggle_playlist_manage_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *list = make_scroll_body(72);
    if (!detail) {
        const size_t count = lyra::media::playlist_count();
        for (size_t i = 0; i < count; ++i) {
            lyra::media::Playlist playlist{};
            if (!lyra::media::playlist_at(i, &playlist)) continue;
            char subtitle[28];
            std::snprintf(subtitle, sizeof(subtitle), "%u songs", static_cast<unsigned>(playlist.track_count));
            if (s_playlist_manage_mode) {
                make_playlist_manage_row(list, static_cast<int>(i) * 62, i, playlist);
            } else {
                lv_obj_t *row = make_row(list, static_cast<int>(i) * 62, nullptr, playlist.name,
                                         subtitle, View::PlaylistDetail, 58);
                lv_obj_remove_event_cb(row, route_cb);
                lv_obj_add_event_cb(row, playlist_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
            }
        }
        if (count == 0) make_label(list, "No playlists yet. Tap + to create one.", kTextMuted);
    } else {
        const size_t pages = (selected.track_count + lyra::media::kTrackPageSize - 1) /
                             lyra::media::kTrackPageSize;
        if (pages && s_list_page >= pages) s_list_page = pages - 1;
        size_t indices[lyra::media::kTrackPageSize];
        const size_t count = lyra::media::playlist_tracks(s_selected_playlist,
            s_list_page * lyra::media::kTrackPageSize, indices, lyra::media::kTrackPageSize);
        for (size_t i = 0; i < count; ++i) {
            if (s_playlist_manage_mode) {
                make_playlist_track_manage_row(list, static_cast<int>(i) * 58, indices[i]);
            } else {
                make_song_row(list, static_cast<int>(i) * 58, indices[i], 54);
            }
        }
        make_page_controls(list, static_cast<int>(count) * 58 + 4, selected.track_count);
        if (count == 0) make_label(list, "Playlist is empty. Tap + to add songs.", kTextMuted);
    }
}

void toggle_favorite_cb(lv_event_t *event)
{
    const bool favorite = lyra::media::is_favorite(s_current_track);
    if (lyra::media::set_favorite(s_current_track, !favorite) != ESP_OK) return;
    lv_obj_t *label = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    lv_label_set_text(label, !favorite ? kHeartFilled : kHeartOutline);
    lv_obj_set_style_text_color(label, !favorite ? kAccent : kTextPrimary, 0);
}

void close_playlist_picker()
{
    if (!s_playlist_picker) return;
    lv_obj_delete(s_playlist_picker);
    s_playlist_picker = nullptr;
}

void add_current_track_to_playlist_cb(lv_event_t *event)
{
    const size_t playlist_index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (lyra::media::add_to_playlist(playlist_index, s_current_track) == ESP_OK) {
        close_playlist_picker();
    }
}

void show_player_playlist_picker_cb(lv_event_t *)
{
    if (s_playlist_picker) return;

    const int picker_height = content_bottom();
    const int dialog_height = std::min(316, picker_height - 64);
    s_playlist_picker = make_box(s_screen, 0, 0, kScreenWidth, picker_height, kBackground);
    lv_obj_set_style_bg_opa(s_playlist_picker, LV_OPA_80, 0);
    lv_obj_add_flag(s_playlist_picker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_playlist_picker, [](lv_event_t *) { close_playlist_picker(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *dialog = make_box(s_playlist_picker, 16, (picker_height - dialog_height) / 2,
                                288, dialog_height, kSurfaceRaised, 10);
    lv_obj_t *title = make_label(dialog, "Add to playlist", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *close = make_button(dialog, 246, 7, 34, 34, kSurface, 17);
    lv_obj_t *close_label = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_playlist_picker(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = make_box(dialog, 8, 48, 272, dialog_height - 56, kSurface, 6);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    const size_t count = lyra::media::playlist_count();
    for (size_t i = 0; i < count; ++i) {
        lyra::media::Playlist playlist{};
        if (!lyra::media::playlist_at(i, &playlist)) continue;
        lv_obj_t *choice = make_button(list, 0, static_cast<int>(i) * 50, 272, 46, kBackground, 5);
        lv_obj_t *name = make_label(choice, playlist.name, kTextPrimary);
        make_marquee(name, 196);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *choice_arrow = make_label(choice, LV_SYMBOL_RIGHT, kAccent);
        lv_obj_align(choice_arrow, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_event_cb(choice, add_current_track_to_playlist_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(i));
    }
    if (count == 0) {
        lv_obj_t *empty = make_label(list, "No playlists yet. Create one first.", kTextMuted);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 12, 12);
    }
}

void format_playback_time(uint32_t milliseconds, bool unknown, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (unknown) {
        copy_ui_text(output, capacity, "--:--");
        return;
    }
    const uint32_t total_seconds = milliseconds / 1000;
    const uint32_t hours = total_seconds / 3600;
    const uint32_t minutes = (total_seconds / 60) % 60;
    const uint32_t seconds = total_seconds % 60;
    if (hours > 0) {
        std::snprintf(output, capacity, "%u:%02u:%02u",
                      static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                      static_cast<unsigned>(seconds));
    } else {
        std::snprintf(output, capacity, "%02u:%02u",
                      static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
    }
}

bool text_equals_ci(const char *left, const char *right)
{
    if (!left || !right) return false;
    while (*left && *right) {
        if (std::tolower(static_cast<unsigned char>(*left)) !=
            std::tolower(static_cast<unsigned char>(*right))) return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

const char *current_track_group_name(const lyra::media::Track &track,
                                     lyra::media::GroupKind kind)
{
    switch (kind) {
        case lyra::media::GroupKind::Artist: return track.artist;
        case lyra::media::GroupKind::Album: return track.album;
        case lyra::media::GroupKind::Genre: return track.genre;
        case lyra::media::GroupKind::Year: return track.year;
    }
    return "";
}

void open_current_track_group(lyra::media::GroupKind kind)
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return;
    const char *name = current_track_group_name(track, kind);
    if (!name || !name[0]) return;

    const size_t count = lyra::media::group_count(kind);
    for (size_t index = 0; index < count; ++index) {
        lyra::media::Group group{};
        if (!lyra::media::group_at(kind, index, &group) || std::strcmp(group.name, name) != 0) continue;
        s_selected_group_kind = kind;
        s_selected_group = index;
        copy_ui_text(s_track_list_title, sizeof(s_track_list_title), group.name);
        switch (kind) {
            case lyra::media::GroupKind::Album:
                s_library_tab = LibraryTab::Albums;
                navigate_to(View::AlbumDetail);
                return;
            case lyra::media::GroupKind::Artist:
                s_library_tab = LibraryTab::Artists;
                s_artist_detail_tab = ArtistDetailTab::Songs;
                navigate_to(View::ArtistDetail);
                return;
            case lyra::media::GroupKind::Genre:
                s_library_tab = LibraryTab::Genres;
                navigate_to(View::TrackList);
                return;
            case lyra::media::GroupKind::Year:
                s_library_tab = LibraryTab::Years;
                navigate_to(View::TrackList);
                return;
        }
    }
}

void open_current_track_group_cb(lv_event_t *event)
{
    const auto kind = static_cast<lyra::media::GroupKind>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    open_current_track_group(kind);
}

void track_info_tab_cb(lv_event_t *event)
{
    s_track_info_tab = static_cast<TrackInfoTab>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    render(View::TrackInfo);
}

lv_obj_t *make_track_info_row(lv_obj_t *parent, int y, const char *label,
                              const char *value, bool clickable = false)
{
    lv_obj_t *row = clickable ? make_button(parent, 8, y, 304, 52, kSurface, 6) :
                                make_box(parent, 8, y, 304, 52, kSurface, 6);
    if (clickable) lv_obj_set_style_bg_color(row, kSurfaceRaised, LV_STATE_PRESSED);
    lv_obj_t *label_view = make_label(row, label, kTextMuted);
    lv_obj_set_pos(label_view, 12, 6);
    lv_obj_t *value_view = make_label(row, value && value[0] ? value : "Unavailable",
                                      clickable ? kAccent : kTextPrimary);
    make_marquee(value_view, clickable ? 252 : 278);
    lv_obj_set_pos(value_view, 12, 25);
    if (clickable) {
        lv_obj_t *arrow = make_label(row, LV_SYMBOL_RIGHT, kAccent);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 8);
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

void make_track_info_tab_button(lv_obj_t *parent, int x, const char *label,
                                TrackInfoTab tab)
{
    const bool selected = s_track_info_tab == tab;
    lv_obj_t *button = make_button(parent, x, 76, 150, 36,
                                   selected ? kAccentDark : kSurface, 6);
    lv_obj_t *text = make_label(button, label, selected ? kTextOnAccent : kTextSecondary);
    lv_obj_center(text);
    lv_obj_add_event_cb(button, track_info_tab_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
}

void format_track_number(uint32_t number, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (number == 0) copy_ui_text(output, capacity, "Unavailable");
    else std::snprintf(output, capacity, "%u", static_cast<unsigned>(number));
}

void format_file_size(uint64_t bytes, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (bytes < 1024u) std::snprintf(output, capacity, "%llu B",
                                    static_cast<unsigned long long>(bytes));
    else if (bytes < 1024u * 1024u) std::snprintf(output, capacity, "%.1f KB",
                                                   bytes / 1024.0);
    else if (bytes < 1024ull * 1024ull * 1024ull) std::snprintf(output, capacity, "%.2f MB",
                                                                 bytes / (1024.0 * 1024.0));
    else std::snprintf(output, capacity, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

bool append_debug_text(char *destination, size_t capacity, const char *format, ...)
{
    if (!destination || capacity == 0 || !format) return false;
    const size_t used = std::strlen(destination);
    if (used >= capacity) return false;
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(destination + used, capacity - used, format, arguments);
    va_end(arguments);
    return written >= 0 && static_cast<size_t>(written) < capacity - used;
}

void format_mac_address(const uint8_t mac[6], char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (!mac) {
        copy_ui_text(output, capacity, "Unavailable");
        return;
    }
    std::snprintf(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *debug_partition_type(const esp_partition_t *partition)
{
    if (!partition) return "Unknown";
    if (partition->type == ESP_PARTITION_TYPE_APP) {
        switch (partition->subtype) {
            case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "app/factory";
            case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "app/ota_0";
            case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "app/ota_1";
            default: return "app";
        }
    }
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) return "data/nvs";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA) return "data/otadata";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) return "data/littlefs";
    return "data";
}

bool app_partition_image_size(const esp_partition_t *partition, uint32_t *image_size)
{
    if (!partition || !image_size || partition->type != ESP_PARTITION_TYPE_APP) return false;

    // ESP application images use a 24-byte image header and 8-byte segment
    // headers. This is deliberately a small footprint estimate for the
    // diagnostic display; the partition itself remains the authoritative size.
    constexpr uint32_t kImageHeaderSize = 24;
    constexpr uint32_t kSegmentHeaderSize = 8;
    constexpr uint32_t kImageMagic = 0xE9;
    constexpr uint8_t kMaximumSegments = 16;
    uint8_t header[kImageHeaderSize]{};
    if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK ||
        header[0] != kImageMagic || header[1] == 0 || header[1] > kMaximumSegments) {
        return false;
    }

    uint32_t offset = kImageHeaderSize;
    for (uint8_t segment_index = 0; segment_index < header[1]; ++segment_index) {
        uint8_t segment_header[kSegmentHeaderSize]{};
        if (offset > partition->size - kSegmentHeaderSize ||
            esp_partition_read(partition, offset, segment_header, sizeof(segment_header)) != ESP_OK) {
            return false;
        }
        const uint32_t segment_size = static_cast<uint32_t>(segment_header[4]) |
                                      (static_cast<uint32_t>(segment_header[5]) << 8) |
                                      (static_cast<uint32_t>(segment_header[6]) << 16) |
                                      (static_cast<uint32_t>(segment_header[7]) << 24);
        offset += kSegmentHeaderSize;
        if (segment_size == 0 || segment_size % 4u != 0 ||
            segment_size > partition->size - offset) return false;
        offset += segment_size;
    }
    if (offset >= partition->size) return false;
    // The checksum follows the segments; a simple SHA-256 digest follows it
    // when hash_appended is set in the image header.
    const uint32_t appended_hash_size = header[23] != 0 ? 32u : 0u;
    const uint32_t trailer_size = 1u + appended_hash_size;
    if (partition->size < trailer_size || offset > partition->size - trailer_size) return false;
    *image_size = offset + 1u + appended_hash_size;
    return true;
}

void append_debug_partition(char *info, size_t capacity, const esp_partition_t *partition,
                            const esp_partition_t *running)
{
    if (!partition) return;
    char total[24];
    format_file_size(partition->size, total, sizeof(total));
    const bool is_running = partition == running ||
                            (running && partition->address == running->address);
    append_debug_text(info, capacity, "%s%s @ 0x%06X\n",
                      partition->label, is_running ? " [BOOTED]" : "",
                      static_cast<unsigned>(partition->address));
    append_debug_text(info, capacity, "  Type: %s | Total: %s\n",
                      debug_partition_type(partition), total);

    if (partition->type == ESP_PARTITION_TYPE_APP) {
        uint32_t image_size = 0;
        if (app_partition_image_size(partition, &image_size) && image_size <= partition->size) {
            char used[24];
            char free_space[24];
            format_file_size(image_size, used, sizeof(used));
            format_file_size(partition->size - image_size, free_space, sizeof(free_space));
            append_debug_text(info, capacity, "  Image: %s | Free: %s\n", used, free_space);
        } else {
            append_debug_text(info, capacity, "  Image: unavailable | Free: unavailable\n");
        }
    } else if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) {
        nvs_stats_t stats{};
        if (nvs_get_stats(partition->label, &stats) == ESP_OK) {
            char free_entries[24];
            format_file_size(static_cast<uint64_t>(stats.free_entries) * 32u,
                             free_entries, sizeof(free_entries));
            append_debug_text(info, capacity, "  Free NVS entries: %u (~%s)\n",
                              static_cast<unsigned>(stats.free_entries), free_entries);
        } else {
            append_debug_text(info, capacity, "  Free: unavailable\n");
        }
    } else {
        append_debug_text(info, capacity, "  Free: unavailable (raw data partition)\n");
    }
}

void build_debug_info(char *info, size_t capacity)
{
    if (!info || capacity == 0) return;
    info[0] = '\0';

    const esp_partition_t *running = esp_ota_get_running_partition();
    append_debug_text(info, capacity, "BOOT PARTITION\n");
    if (running) {
        char size[24];
        format_file_size(running->size, size, sizeof(size));
        append_debug_text(info, capacity, "%s (%s)\nAddress: 0x%06X | Size: %s\n\n",
                          running->label, debug_partition_type(running),
                          static_cast<unsigned>(running->address), size);
    } else {
        append_debug_text(info, capacity, "Unavailable\n\n");
    }

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint8_t mac[6]{};
    char mac_text[24];
    format_mac_address(esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK ? mac : nullptr,
                       mac_text, sizeof(mac_text));
    append_debug_text(info, capacity, "HARDWARE ID\nBoard: JC3248W535EN\n"
                      "Chip: %s rev %d | Cores: %d\n",
                      "ESP32-S3", chip.revision, chip.cores);
    append_debug_text(info, capacity, "Factory MAC: %s\nIDF: %s\nCompile time: %s %s\n\n",
                      mac_text, esp_get_idf_version(), __DATE__, __TIME__);

    append_debug_text(info, capacity, "FLASH PARTITIONS\n");
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        append_debug_partition(info, capacity, partition, running);
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);

    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_used = psram_total >= psram_free ? psram_total - psram_free : 0;
    char psram_used_text[24];
    char psram_total_text[24];
    format_file_size(psram_used, psram_used_text, sizeof(psram_used_text));
    format_file_size(psram_total, psram_total_text, sizeof(psram_total_text));
    append_debug_text(info, capacity, "\nPSRAM\nUsed: %s | Total: %s\n\n",
                      psram_used_text, psram_total_text);

    const lyra::media::Status media_status = lyra::media::status();
    lyra::media::CardInfo card{};
    const bool card_available = lyra::media::card_info(&card);
    append_debug_text(info, capacity, "MICROSD\n");
    if (!media_status.mounted || !card_available) {
        append_debug_text(info, capacity, "Not mounted\n");
    } else {
        const uint64_t card_capacity = card.capacity_bytes != 0 ? card.capacity_bytes :
                                       media_status.total_bytes;
        char capacity_text[24];
        char free_text[24];
        char total_text[24];
        format_file_size(card_capacity, capacity_text, sizeof(capacity_text));
        format_file_size(media_status.free_bytes, free_text, sizeof(free_text));
        format_file_size(media_status.total_bytes, total_text, sizeof(total_text));
        char card_name[9]{};
        std::memcpy(card_name, card.name, sizeof(card.name));
        append_debug_text(info, capacity, "Capacity: %s\nFilesystem free/total: %s / %s\n",
                          capacity_text, free_text, total_text);
        append_debug_text(info, capacity,
                          "CID fields (decoded; raw 128-bit CID unavailable):\n"
                          "MID: 0x%02X (%d) | OEM ID: 0x%04X | Product: %s\n"
                          "Revision: %d | Serial: %d | Date code: %d\n",
                          static_cast<unsigned>(card.manufacturer_id) & 0xFFu,
                          card.manufacturer_id, static_cast<unsigned>(card.oem_id) & 0xFFFFu,
                          card_name, card.revision, card.serial, card.date);
    }
}

bool dump_debug_info_to_sd()
{
    char info[kDebugInfoCapacity];
    build_debug_info(info, sizeof(info));

    char path[64];
    std::snprintf(path, sizeof(path), "/sdcard/lyra-debug-info-%llu.txt",
                  static_cast<unsigned long long>(esp_timer_get_time()));
    FILE *file = lyra::sd::open(path, "wb", lyra::sd::Client::Filesystem);
    if (!file) {
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     "Could not open the MicroSD root");
        return false;
    }

    const size_t length = std::strlen(info);
    const bool written = lyra::sd::write_exact(file, info, length,
                                               lyra::sd::Client::Filesystem);
    const bool closed = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0;
    if (!written || !closed) {
        lyra::sd::remove(path, lyra::sd::Client::Filesystem);
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     "Could not write debug info to the MicroSD root");
        return false;
    }

    std::snprintf(s_debug_status, sizeof(s_debug_status),
                  "Debug info saved to %s", path);
    return true;
}

void debug_tab_cb(lv_event_t *event)
{
    s_debug_tab = static_cast<DebugTab>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    render(View::DebugMenu);
}

void maximum_volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const uint8_t value = static_cast<uint8_t>(lv_slider_get_value(slider));
    const esp_err_t result = lyra::audio::set_maximum_volume_percent(value);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not set maximum volume: %s", esp_err_to_name(result));
        return;
    }
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(value));
        lv_label_set_text(value_label, text);
    }
    update_status_volume_label(lyra::audio::status().volume_percent);
}

void maximum_volume_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_maximum_volume();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save maximum volume: %s", esp_err_to_name(result));
    }
    lyra::audio::save_volume();
}

void make_maximum_volume_control(lv_obj_t *parent, int y)
{
    const uint8_t maximum = lyra::audio::maximum_volume_percent();
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, "Max volume override", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%", static_cast<unsigned>(maximum));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 1, lyra::audio::kMaximumVolumePercent);
    lv_slider_set_value(slider, maximum, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, maximum_volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, maximum_volume_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, maximum_volume_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void write_bmp_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void write_bmp_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

bool save_screenshot_bmp(const uint8_t *pixels, uint32_t stride, uint32_t width, uint32_t height,
                         lv_color_format_t color_format)
{
    if (!pixels || width == 0 || height == 0 || stride < width * 2u ||
        (color_format != LV_COLOR_FORMAT_RGB565 &&
         color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) {
        return false;
    }

    const uint32_t row_size = (width * 3u + 3u) & ~3u;
    const uint32_t image_size = row_size * height;
    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    write_bmp_u32(header + 2, 54u + image_size);
    write_bmp_u32(header + 10, 54);
    write_bmp_u32(header + 14, 40);
    write_bmp_u32(header + 18, width);
    write_bmp_u32(header + 22, height);
    write_bmp_u16(header + 26, 1);
    write_bmp_u16(header + 28, 24);
    write_bmp_u32(header + 34, image_size);

    char path[64];
    std::snprintf(path, sizeof(path), "/sdcard/screenshot-%llu-%u.bmp",
                  static_cast<unsigned long long>(esp_timer_get_time()),
                  static_cast<unsigned>(s_screenshot_sequence++));
    FILE *file = lyra::sd::open(path, "wb", lyra::sd::Client::Filesystem);
    if (!file) return false;
    bool written = lyra::sd::write_exact(file, header, sizeof(header),
                                         lyra::sd::Client::Filesystem);
    uint8_t row[static_cast<size_t>(kScreenWidth) * 3u + 3u]{};
    for (uint32_t output_y = 0; written && output_y < height; ++output_y) {
        const uint32_t source_y = height - 1u - output_y;
        const uint8_t *source = pixels + static_cast<size_t>(source_y) * stride;
        std::memset(row, 0, row_size);
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t rgb565 = color_format == LV_COLOR_FORMAT_RGB565_SWAPPED ?
                                    static_cast<uint16_t>((source[x * 2u] << 8) | source[x * 2u + 1u]) :
                                    static_cast<uint16_t>(source[x * 2u] | (source[x * 2u + 1u] << 8));
            row[x * 3u] = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
            row[x * 3u + 1u] = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
            row[x * 3u + 2u] = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
            const uint8_t blue = row[x * 3u];
            row[x * 3u] = row[x * 3u + 2u];
            row[x * 3u + 2u] = blue;
        }
        written = lyra::sd::write_exact(file, row, row_size, lyra::sd::Client::Filesystem);
    }
    const bool closed = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0;
    if (!written || !closed) lyra::sd::remove(path, lyra::sd::Client::Filesystem);
    return written && closed;
}

void screenshot_button_pressed_cb(lv_event_t *event);
void screenshot_button_pressing_cb(lv_event_t *event);
void screenshot_button_clicked_cb(lv_event_t *event);

void create_screenshot_button(int32_t x, int32_t y)
{
    constexpr int32_t kButtonSize = 44;
    x = std::clamp<int32_t>(x, 0, kScreenWidth - kButtonSize);
    y = std::clamp<int32_t>(y, 0, kScreenHeight - kButtonSize);
    s_screenshot_button = make_button(lv_layer_top(), x, y, kButtonSize, kButtonSize,
                                      kAccentDark, 22);
    lv_obj_t *icon = make_label(s_screenshot_button, LV_SYMBOL_IMAGE, kTextOnAccent);
    lv_obj_center(icon);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_pressed_cb,
                        LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_pressing_cb,
                        LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_clicked_cb,
                        LV_EVENT_CLICKED, nullptr);
}

void take_screenshot_cb(lv_event_t *)
{
    if (!s_screenshot_button) return;
    lv_display_t *display = lv_obj_get_display(s_screen);
    if (!display) return;

    // Removing the object avoids copying stale pixels from a direct/double
    // buffered display where hiding alone may not repaint the old button
    // before the active buffer is sampled.
    const int32_t button_x = lv_obj_get_x(s_screenshot_button);
    const int32_t button_y = lv_obj_get_y(s_screenshot_button);
    lv_obj_delete(s_screenshot_button);
    s_screenshot_button = nullptr;
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(s_screen);
    lv_refr_now(display);

    // In direct double-buffer mode LVGL swaps buffers after the flush. The
    // buffer returned as active after the first refresh is therefore the
    // previously displayed buffer, which can still contain the button. A
    // second full refresh updates that buffer too before it is sampled.
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(s_screen);
    lv_refr_now(display);

    lv_draw_buf_t *active = lv_display_get_buf_active(display);
    const bool valid_buffer = active && active->data &&
                              active->header.w == kScreenWidth &&
                              active->header.h == kScreenHeight &&
                              active->header.stride >= kScreenWidth * 2u &&
                              (active->header.cf == LV_COLOR_FORMAT_RGB565 ||
                               active->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED);
    const uint32_t captured_stride = valid_buffer ? active->header.stride : 0;
    const uint32_t captured_width = valid_buffer ? active->header.w : 0;
    const uint32_t captured_height = valid_buffer ? active->header.h : 0;
    const lv_color_format_t captured_format = valid_buffer ?
        static_cast<lv_color_format_t>(active->header.cf) : LV_COLOR_FORMAT_RGB565;
    uint8_t *copy = nullptr;
    if (valid_buffer) {
        const size_t bytes = static_cast<size_t>(active->header.stride) * active->header.h;
        copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!copy) copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
        if (copy) std::memcpy(copy, active->data, bytes);
    }

    create_screenshot_button(button_x, button_y);
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(display);
    const bool saved = copy && save_screenshot_bmp(copy, captured_stride, captured_width,
                                                   captured_height, captured_format);
    heap_caps_free(copy);
    show_notice(saved ? "Screenshot saved" : "Screenshot failed",
                saved ? "Saved to the MicroSD root." :
                        "Could not capture or write the display.");
}

void screenshot_button_pressed_cb(lv_event_t *)
{
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &s_screenshot_press_point);
    s_screenshot_button_start.x = lv_obj_get_x(s_screenshot_button);
    s_screenshot_button_start.y = lv_obj_get_y(s_screenshot_button);
    s_screenshot_dragged = false;
}

void screenshot_button_pressing_cb(lv_event_t *)
{
    if (!s_screenshot_button || !lv_indev_active()) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const int32_t dx = point.x - s_screenshot_press_point.x;
    const int32_t dy = point.y - s_screenshot_press_point.y;
    if (std::abs(dx) > 3 || std::abs(dy) > 3) s_screenshot_dragged = true;
    const int32_t width = lv_obj_get_width(s_screenshot_button);
    const int32_t height = lv_obj_get_height(s_screenshot_button);
    const int32_t x = std::clamp<int32_t>(s_screenshot_button_start.x + dx, 0,
                                          kScreenWidth - width);
    const int32_t y = std::clamp<int32_t>(s_screenshot_button_start.y + dy, 0,
                                          kScreenHeight - height);
    lv_obj_set_pos(s_screenshot_button, x, y);
}

void screenshot_button_clicked_cb(lv_event_t *)
{
    if (s_screenshot_dragged) {
        s_screenshot_dragged = false;
        return;
    }
    take_screenshot_cb(nullptr);
}

void toggle_screenshot_button_cb(lv_event_t *)
{
    if (s_screenshot_button) {
        lv_obj_delete(s_screenshot_button);
        s_screenshot_button = nullptr;
    } else {
        create_screenshot_button(268, 220);
    }
    render(View::DebugMenu);
}

void confirm_clear_nvs_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    esp_err_t result = nvs_flash_erase();
    if (result == ESP_OK) result = nvs_flash_init();
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    if (result == ESP_OK) {
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     "NVS cleared; reboot to apply defaults");
    } else {
        std::snprintf(s_debug_status, sizeof(s_debug_status), "NVS clear failed: %s",
                      esp_err_to_name(result));
    }
    if (overlay) lv_obj_delete(overlay);
    render(View::DebugMenu);
}

void show_clear_nvs_confirmation()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 126, 280, 228, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, "Clear NVS?", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog,
                                   "This erases the default NVS partition.\n"
                                   "Saved Lyra settings and debug boot state\n"
                                   "will reset after reboot.",
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *cancel = make_button(dialog, 14, 164, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 164, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, "CLEAR", kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_clear_nvs_cb, LV_EVENT_CLICKED, overlay);
}

void run_force_boot_test(uint8_t ota_slot)
{
    const esp_err_t volume_result = lyra::audio::save_volume();
    if (volume_result != ESP_OK) {
        ESP_LOGW(kTag, "could not save volume before boot test: %s",
                 esp_err_to_name(volume_result));
    }
    lyra::audio::save_maximum_volume();
    lyra::audio::stop();
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool was_mounted = lyra::media::status().mounted;
    esp_err_t result = was_mounted ? lyra::media::shutdown() : ESP_OK;
    if (result == ESP_OK) result = lyra::boot_test::request_once(ota_slot);
    if (result != ESP_OK) {
        if (was_mounted) lyra::media::init();
        char message[96];
        std::snprintf(message, sizeof(message), "OTA %u boot test failed: %s",
                      static_cast<unsigned>(ota_slot), esp_err_to_name(result));
        show_notice("Boot test failed", message);
    }
}

void confirm_force_boot_test_cb(lv_event_t *event)
{
    const uint8_t ota_slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    lv_obj_t *confirm = lv_event_get_current_target_obj(event);
    lv_obj_t *dialog = confirm ? lv_obj_get_parent(confirm) : nullptr;
    lv_obj_t *overlay = dialog ? lv_obj_get_parent(dialog) : nullptr;
    if (overlay) lv_obj_delete(overlay);

    style_root();
    lv_obj_t *label = make_label(s_screen, "REBOOTING\n\nTesting the selected OTA image...",
                                 kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    lv_refr_now(lv_obj_get_display(s_screen));
    run_force_boot_test(ota_slot);
}

void show_force_boot_confirmation(uint8_t ota_slot)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 116, 280, 252, kSurfaceRaised, 12);
    char title_text[32];
    std::snprintf(title_text, sizeof(title_text), "Boot OTA %u?",
                  static_cast<unsigned>(ota_slot));
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog,
                                   "A valid firmware image was found.\n"
                                   "Lyra will reboot for a one-shot\n"
                                   "test and restore the current slot.",
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *cancel = make_button(dialog, 14, 188, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 188, 116, 48, kAccentDark, 7);
    lv_obj_t *confirm_label = make_label(confirm, "REBOOT", kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(ota_slot)));
}

void force_boot_test_cb(lv_event_t *event)
{
    const uint8_t ota_slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    const esp_err_t result = lyra::boot_test::validate(ota_slot);
    if (result != ESP_OK) {
        char message[112];
        if (result == ESP_ERR_NOT_FOUND) {
            std::snprintf(message, sizeof(message), "OTA %u is not present in the partition table.",
                          static_cast<unsigned>(ota_slot));
        } else {
            std::snprintf(message, sizeof(message),
                          "OTA %u does not contain a valid bootable firmware image.",
                          static_cast<unsigned>(ota_slot));
        }
        show_notice("OTA unavailable", message);
        return;
    }
    show_force_boot_confirmation(ota_slot);
}

void dump_debug_info_cb(lv_event_t *)
{
    dump_debug_info_to_sd();
    render(View::DebugMenu);
}

void render_debug_menu()
{
    make_header("Debug", View::About, true);
    lv_obj_t *body = make_scroll_body(72);
    for (size_t index = 0; index < 2; ++index) {
        const DebugTab tab = static_cast<DebugTab>(index);
        lv_obj_t *button = make_button(body, 7 + static_cast<int>(index) * 153, 4, 150, 36,
                                       s_debug_tab == tab ? kAccentDark : kSurfaceRaised, 6);
        lv_obj_t *label = make_label(button, tab == DebugTab::Info ? "Info" : "Debug",
                                     s_debug_tab == tab ? kTextOnAccent : kTextSecondary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, debug_tab_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
    }

    if (s_debug_tab == DebugTab::Info) {
        char info[kDebugInfoCapacity];
        build_debug_info(info, sizeof(info));
        lv_obj_t *label = make_label(body, info, kTextSecondary);
        lv_obj_set_pos(label, 14, 52);
        lv_obj_set_width(label, 292);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(label, 4, 0);
        return;
    }

    lv_obj_t *dump = make_row(body, 48, LV_SYMBOL_SAVE, "Dump info to SD card",
                              "Save the Info tab to a TXT file in the SD root",
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(dump, route_cb);
    lv_obj_add_event_cb(dump, dump_debug_info_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *screenshot = make_row(body, 114, LV_SYMBOL_IMAGE, "Screenshot button",
                                    s_screenshot_button ? "Tap to hide floating button" :
                                                          "Show movable floating button",
                                    View::DebugMenu, 62);
    lv_obj_remove_event_cb(screenshot, route_cb);
    lv_obj_add_event_cb(screenshot, toggle_screenshot_button_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ota0 = make_row(body, 180, LV_SYMBOL_REFRESH, "Force boot OTA 0",
                              "Validate first, then confirm one-shot test",
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(ota0, route_cb);
    lv_obj_add_event_cb(ota0, force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(0)));
    lv_obj_t *ota1 = make_row(body, 246, LV_SYMBOL_REFRESH, "Force boot OTA 1",
                              "Validate first, then confirm one-shot test",
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(ota1, route_cb);
    lv_obj_add_event_cb(ota1, force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    lv_obj_t *clear = make_row(body, 312, LV_SYMBOL_CLOSE, "Clear NVS",
                               "Remove saved settings (confirmation required)",
                               View::DebugMenu, 62);
    lv_obj_remove_event_cb(clear, route_cb);
    lv_obj_add_event_cb(clear, [](lv_event_t *) { show_clear_nvs_confirmation(); },
                        LV_EVENT_CLICKED, nullptr);
    make_maximum_volume_control(body, 378);
    if (s_debug_status[0]) {
        lv_obj_t *status = make_label(body, s_debug_status, kTextMuted);
        lv_obj_set_pos(status, 14, 470);
        lv_obj_set_width(status, 292);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_WRAP);
    }
}

void format_file_date(uint64_t modified_time, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (modified_time == 0) {
        copy_ui_text(output, capacity, "Unavailable");
        return;
    }
    const std::time_t timestamp = static_cast<std::time_t>(modified_time);
    std::tm local{};
    if (localtime_r(&timestamp, &local) == nullptr ||
        std::strftime(output, capacity, "%Y-%m-%d %H:%M", &local) == 0) {
        copy_ui_text(output, capacity, "Unavailable");
    }
}

void format_track_format(const lyra::media::Track &track, char *output, size_t capacity)
{
    copy_ui_text(output, capacity, track.format);
    for (char *character = output; *character; ++character) {
        *character = static_cast<char>(std::toupper(static_cast<unsigned char>(*character)));
    }
    if (!output[0]) copy_ui_text(output, capacity, "Unknown");
}

const char *track_encoding(const lyra::media::Track &track)
{
    if (text_equals_ci(track.format, "mp3")) return "MPEG Layer III";
    if (text_equals_ci(track.format, "flac")) return "FLAC lossless";
    if (text_equals_ci(track.format, "aac")) return "Advanced Audio Coding";
    if (text_equals_ci(track.format, "m4a") || text_equals_ci(track.format, "mp4")) {
        return "MPEG-4 audio";
    }
    if (text_equals_ci(track.format, "ogg")) return "Vorbis / Opus";
    if (text_equals_ci(track.format, "opus")) return "Opus";
    if (text_equals_ci(track.format, "wav") || text_equals_ci(track.format, "aiff") ||
        text_equals_ci(track.format, "aif") || text_equals_ci(track.format, "aifc")) return "PCM";
    return "Unknown";
}

void render_track_info()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        make_header("Track Info", View::Player, true);
        lv_obj_t *empty = make_label(s_screen, "No track selected.", kTextMuted);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
        return;
    }

    make_header("Track Info", View::Player, true);
    make_track_info_tab_button(s_screen, 8, "Song Info", TrackInfoTab::Song);
    make_track_info_tab_button(s_screen, 162, "Media Info", TrackInfoTab::Media);
    lv_obj_t *body = make_scroll_body(118);

    if (s_track_info_tab == TrackInfoTab::Song) {
        constexpr int kArtSize = 116;
        const int art_x = (kScreenWidth - kArtSize) / 2;
        make_album_art(body, art_x, 8, kArtSize, kArtSize, track);
        lv_obj_t *art_hit = make_button(body, art_x, 8, kArtSize, kArtSize, kBackground, 13);
        lv_obj_set_style_bg_opa(art_hit, LV_OPA_TRANSP, 0);
        add_route(art_hit, View::FullscreenInfoArt);
        lv_obj_t *hint = make_label(body, "Tap album art for full screen", kTextMuted);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 128);

        int y = 150;
        make_track_info_row(body, y, "Title", track.title); y += 56;
        lv_obj_t *album = make_track_info_row(body, y, "Album", track.album, true);
        lv_obj_add_event_cb(album, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Album)));
        y += 56;
        lv_obj_t *artist = make_track_info_row(body, y, "Artist", track.artist, true);
        lv_obj_add_event_cb(artist, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Artist)));
        y += 56;
        make_track_info_row(body, y, "Album artist", track.album_artist); y += 56;
        make_track_info_row(body, y, "Composer", track.composer); y += 56;
        lv_obj_t *genre = make_track_info_row(body, y, "Genre", track.genre, true);
        lv_obj_add_event_cb(genre, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Genre)));
        y += 56;
        char number[16];
        format_track_number(track.track_number, number, sizeof(number));
        make_track_info_row(body, y, "Track number", number); y += 56;
        format_track_number(track.disc_number, number, sizeof(number));
        make_track_info_row(body, y, "Disc number", number); y += 56;
        lv_obj_t *year = make_track_info_row(body, y, "Year", track.year, true);
        lv_obj_add_event_cb(year, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Year)));
        return;
    }

    const lyra::audio::Status audio_status = lyra::audio::status();
    const bool current_audio = std::strcmp(audio_status.path, track.path) == 0;
    const uint32_t duration_ms = current_audio && audio_status.duration_ms > 0 ?
                                 audio_status.duration_ms : track.duration_ms;
    char filename[lyra::media::kMaxPath];
    const char *slash = std::strrchr(track.path, '/');
    copy_ui_text(filename, sizeof(filename), slash ? slash + 1 : track.path);
    char duration[16];
    format_playback_time(duration_ms, duration_ms == 0, duration, sizeof(duration));
    char bitrate[24];
    if (duration_ms == 0 || track.size_bytes == 0) copy_ui_text(bitrate, sizeof(bitrate), "Unavailable");
    else std::snprintf(bitrate, sizeof(bitrate), "%llu kbps (average)",
                       static_cast<unsigned long long>((track.size_bytes * 8u + duration_ms / 2u) /
                                                       duration_ms));
    char sample_rate[24];
    if (current_audio && audio_status.sample_rate > 0) {
        std::snprintf(sample_rate, sizeof(sample_rate), "%u Hz",
                      static_cast<unsigned>(audio_status.sample_rate));
    } else copy_ui_text(sample_rate, sizeof(sample_rate), "Unavailable");
    char bits_per_sample[16];
    if (current_audio && audio_status.bits_per_sample > 0) {
        std::snprintf(bits_per_sample, sizeof(bits_per_sample), "%u-bit",
                      static_cast<unsigned>(audio_status.bits_per_sample));
    } else copy_ui_text(bits_per_sample, sizeof(bits_per_sample), "Unavailable");
    char channels[24];
    if (current_audio && audio_status.channels > 0) {
        std::snprintf(channels, sizeof(channels), "%u (%s)",
                      static_cast<unsigned>(audio_status.channels),
                      audio_status.channels == 1 ? "mono" : audio_status.channels == 2 ? "stereo" : "multichannel");
    } else copy_ui_text(channels, sizeof(channels), "Unavailable");
    char size[24];
    format_file_size(track.size_bytes, size, sizeof(size));
    char file_date[32];
    format_file_date(track.modified_time, file_date, sizeof(file_date));
    char format[16];
    format_track_format(track, format, sizeof(format));

    int y = 8;
    make_track_info_row(body, y, "Filename", filename); y += 56;
    make_track_info_row(body, y, "File path", track.path); y += 56;
    make_track_info_row(body, y, "Duration", duration); y += 56;
    make_track_info_row(body, y, "Bitrate", bitrate); y += 56;
    make_track_info_row(body, y, "Sample rate", sample_rate); y += 56;
    make_track_info_row(body, y, "Bits per sample", bits_per_sample); y += 56;
    make_track_info_row(body, y, "Format", format); y += 56;
    make_track_info_row(body, y, "Encoding", track_encoding(track)); y += 56;
    make_track_info_row(body, y, "Channels", channels); y += 56;
    make_track_info_row(body, y, "File size", size); y += 56;
    make_track_info_row(body, y, "File date info", file_date);
}

void render_fullscreen_info_art()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        render(View::TrackInfo);
        return;
    }
    make_artwork(s_screen, 0, 0, kScreenWidth, kScreenHeight, track, 0, true);
    lv_obj_t *exit_hit = make_button(s_screen, 0, 0, kScreenWidth, kScreenHeight, kBackground, 0);
    lv_obj_set_style_bg_opa(exit_hit, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(exit_hit, [](lv_event_t *) { navigate_back(View::TrackInfo); },
                        LV_EVENT_CLICKED, nullptr);
}

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
        make_header("Now Playing", View::Menu, true);
        lv_obj_t *empty = make_label(s_screen, "No track selected\n\nScan a MicroSD card, then choose a song.", kTextMuted);
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
        lv_obj_t *exit_hit = make_button(art, 0, 0, 320, body_height, kBackground, 0);
        lv_obj_set_style_bg_opa(exit_hit, LV_OPA_TRANSP, 0);
        add_route(exit_hit, View::Player);
        lv_obj_t *overlay = make_box(art, 0, body_height - 132, 320, 132, kOverlay);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
        lv_obj_t *title = make_label(overlay, track.title, kTextPrimary);
        lv_obj_set_style_text_font(title, lyra::font::ui(), 0);
        make_marquee(title, 250);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 20);
        lv_obj_t *artist = make_label(overlay, track.artist, kTextSecondary);
        make_marquee(artist, 280);
        lv_obj_align(artist, LV_ALIGN_TOP_LEFT, 10, 43);
        lv_obj_t *heart = make_label(overlay, favorite ? kHeartFilled : kHeartOutline,
                                     favorite ? kAccent : kTextPrimary);
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
        s_player_elapsed_label = make_label(overlay, "00:00", kTextSecondary);
        lv_obj_align(s_player_elapsed_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
        s_player_duration_label = make_label(overlay, "--:--", kTextSecondary);
        lv_obj_align(s_player_duration_label, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
        update_player_progress();
        return;
    }

    lv_obj_t *body = make_box(s_screen, 0, kStatusHeight, 320, body_height, kBackground);
    const int art_size = s_show_nav ? 236 : 280;
    const int art_x = (kScreenWidth - art_size) / 2;
    const int controls_y = body_height - 54;
    const int progress_y = controls_y - 19;
    make_album_art(body, art_x, 8, art_size, art_size, track);
    lv_obj_t *art_hit = make_button(body, art_x, 8, art_size, art_size, kBackground, 13);
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
    lv_obj_t *playlist_button = make_button(body, 136, controls_y + 4, 44, 38, kBackground, 7);
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
    lv_obj_add_event_cb(shuffle, [](lv_event_t *event) {
        s_shuffle = !s_shuffle;
        reset_shuffle_queue();
        if (s_shuffle && !build_shuffle_queue()) {
            s_shuffle = false;
            ESP_LOGW(kTag, "could not allocate shuffle queue");
        }
        if (!s_shuffle) s_queue_position_valid = false;
        if (s_shuffle && lyra::audio::status().eof) s_audio_eof_seen = false;
        lv_obj_set_style_text_color(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                                    s_shuffle ? kAccent : kTextSecondary, 0);
    }, LV_EVENT_CLICKED, shuffle_icon);
}

struct EqualizerSliderContext {
    size_t band;
    lv_obj_t *value_label;
};

EqualizerSliderContext s_equalizer_slider_contexts[lyra::audio::kEqualizerBandCount]{};

void update_equalizer_gain_label(lv_obj_t *label, int16_t gain_tenths_db, lv_color_t color)
{
    if (!label) return;
    char text[8];
    std::snprintf(text, sizeof(text), "%+.1f", static_cast<float>(gain_tenths_db) / 10.0f);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
}

void equalizer_slider_cb(lv_event_t *event)
{
    auto *context = static_cast<EqualizerSliderContext *>(lv_event_get_user_data(event));
    if (!context || context->band >= lyra::audio::kEqualizerBandCount || !equalizer_is_custom()) return;
    const int value = lv_slider_get_value(lv_event_get_current_target_obj(event));
    const int16_t gain = static_cast<int16_t>(std::clamp(
        value, static_cast<int>(lyra::audio::kEqualizerMinimumTenthsDb),
        static_cast<int>(lyra::audio::kEqualizerMaximumTenthsDb)));
    s_equalizer_custom_bands[context->band] = gain;
    update_equalizer_gain_label(context->value_label, gain, kAccent);
    apply_equalizer_to_audio();
}

void equalizer_slider_released_cb(lv_event_t *)
{
    // Custom bands are saved only after the gesture settles, while every
    // intermediate slider position is still applied immediately to playback.
    save_user_settings();
}

void equalizer_preset_menu_cb(lv_event_t *)
{
    navigate_to(View::EqualizerPresets);
}

void equalizer_preset_option_cb(lv_event_t *event)
{
    const auto preset = static_cast<EqualizerPreset>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (preset >= EqualizerPreset::Count) return;
    s_equalizer_preset = preset;
    apply_equalizer_to_audio();
    save_user_settings();
    navigate_back(View::Equalizer);
}

void make_equalizer_preset_option(lv_obj_t *parent, int y, EqualizerPreset preset)
{
    const bool active = s_equalizer_preset == preset;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    lv_obj_t *label = make_label(row, equalizer_preset_name(preset),
                                 active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, equalizer_preset_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(preset)));
}

void render_equalizer_presets()
{
    make_header("EQ Preset", View::Equalizer, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr EqualizerPreset presets[] = {
        EqualizerPreset::Custom,
        EqualizerPreset::Flat,
        EqualizerPreset::FullBass,
        EqualizerPreset::FullTreble,
        EqualizerPreset::BassAndTreble,
        EqualizerPreset::Rock,
        EqualizerPreset::Pop,
        EqualizerPreset::Jazz,
        EqualizerPreset::Classic,
    };
    for (size_t index = 0; index < sizeof(presets) / sizeof(presets[0]); ++index) {
        make_equalizer_preset_option(body, static_cast<int>(index) * 58, presets[index]);
    }
}

void render_equalizer()
{
    make_header("Equalizer", View::Menu, true, "ON");
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth, content_height(72), kBackground);
    lv_obj_t *preset = make_button(body, 8, 6, 304, 48, kSurface, 6);
    lv_obj_t *preset_name = make_label(preset, equalizer_preset_name(s_equalizer_preset),
                                       kTextPrimary);
    lv_obj_align(preset_name, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *arrow = make_label(preset, LV_SYMBOL_RIGHT, kTextMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(preset, equalizer_preset_menu_cb, LV_EVENT_CLICKED, nullptr);

    constexpr const char *frequencies[] = {"60", "250", "1k", "4k", "16k"};
    constexpr int kSliderX = 51;
    constexpr int kSliderStep = 52;
    constexpr int kSliderY = 78;
    constexpr int kSliderHeight = 234;
    const bool editable = equalizer_is_custom();
    const int16_t *bands = equalizer_preset_bands(s_equalizer_preset);

    // A 43 px scale gutter keeps the +6 dB / -6 dB labels entirely clear of
    // the first slider's knob and indicator.
    lv_obj_t *top = make_label(body, "+6dB", kTextSecondary);
    lv_obj_set_pos(top, 3, 67);
    lv_obj_t *bottom = make_label(body, "-6dB", kTextSecondary);
    lv_obj_set_pos(bottom, 3, 302);

    for (size_t band = 0; band < lyra::audio::kEqualizerBandCount; ++band) {
        lv_obj_t *slider = lv_slider_create(body);
        lv_obj_set_pos(slider, kSliderX + static_cast<int>(band) * kSliderStep, kSliderY);
        lv_obj_set_size(slider, 12, kSliderHeight);
        lv_slider_set_range(slider, lyra::audio::kEqualizerMinimumTenthsDb,
                            lyra::audio::kEqualizerMaximumTenthsDb);
        lv_slider_set_value(slider, bands[band], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
        lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
        lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);

        lv_obj_t *frequency = make_label(body, frequencies[band], kTextSecondary);
        lv_obj_set_width(frequency, 40);
        lv_obj_set_style_text_align(frequency, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(frequency, 37 + static_cast<int>(band) * kSliderStep, 326);
        lv_obj_t *gain = make_label(body, "", editable ? kAccent : kTextMuted);
        lv_obj_set_width(gain, 40);
        lv_obj_set_style_text_align(gain, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(gain, 37 + static_cast<int>(band) * kSliderStep, 344);
        update_equalizer_gain_label(gain, bands[band], editable ? kAccent : kTextMuted);

        if (editable) {
            s_equalizer_slider_contexts[band] = {band, gain};
            lv_obj_add_event_cb(slider, equalizer_slider_cb, LV_EVENT_VALUE_CHANGED,
                                &s_equalizer_slider_contexts[band]);
            lv_obj_add_event_cb(slider, equalizer_slider_released_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(slider, equalizer_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
        } else {
            const lv_style_selector_t disabled_indicator =
                static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
            const lv_style_selector_t disabled_knob =
                static_cast<lv_style_selector_t>(LV_PART_KNOB) |
                static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(slider, kTextMuted,
                                      disabled_indicator);
            lv_obj_set_style_bg_color(slider, kTextMuted, disabled_knob);
            lv_obj_set_style_border_color(slider, kDivider, disabled_knob);
            lv_obj_add_state(slider, LV_STATE_DISABLED);
        }
    }
}

void update_search_label()
{
    if (s_search_label == nullptr) return;
    const bool creating = s_view == View::PlaylistCreate;
    const char *text = creating ? s_playlist_name : s_search_query;
    lv_label_set_text(s_search_label, text[0] == '\0' ? (creating ? "Playlist name..." : "Search music...") : text);
    lv_obj_set_style_text_color(s_search_label, text[0] == '\0' ? kTextMuted : kTextPrimary, 0);
}

const char *search_category_name(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return "Songs";
        case lyra::media::SearchCategory::Albums: return "Albums";
        case lyra::media::SearchCategory::Artists: return "Artists";
        case lyra::media::SearchCategory::Playlists: return "Playlists";
    }
    return "Songs";
}

const char *search_empty_message(lyra::media::SearchCategory category)
{
    switch (category) {
        case lyra::media::SearchCategory::Songs: return "No matching songs.";
        case lyra::media::SearchCategory::Albums: return "No matching albums.";
        case lyra::media::SearchCategory::Artists: return "No matching artists.";
        case lyra::media::SearchCategory::Playlists: return "No playlist songs match.";
    }
    return "No matches.";
}

void populate_search_results()
{
    if (!s_search_results) return;
    lv_obj_clean(s_search_results);
    if (!s_search_query[0]) {
        make_label(s_search_results, "Type a search term, then tap SEARCH.", kTextMuted);
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
        std::snprintf(progress, sizeof(progress), "Searching %u / %u",
                      static_cast<unsigned>(status.processed), static_cast<unsigned>(status.total));
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
        std::snprintf(prompt, sizeof(prompt), "Tap SEARCH to find matching %s.",
                      search_category_name(s_search_category));
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
            std::snprintf(subtitle, sizeof(subtitle), group.track_count == 1 ? "1 song" : "%u songs",
                          static_cast<unsigned>(group.track_count));
            const View target = kind == lyra::media::GroupKind::Album ?
                                View::AlbumDetail : View::TrackList;
            lv_obj_t *row = make_row(s_search_results, y, nullptr, group.name,
                                     subtitle, target, 54);
            lv_obj_remove_event_cb(row, route_cb);
            lv_obj_add_event_cb(row,
                kind == lyra::media::GroupKind::Album ? search_album_result_cb : search_artist_result_cb,
                LV_EVENT_CLICKED, reinterpret_cast<void *>(result.group_index));
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
            copy_ui_text(s_search_error, sizeof(s_search_error), "Search is busy - try again shortly");
        }
    }
    render(View::Search);
}

void keyboard_cb(lv_event_t *event)
{
    const char *key = static_cast<const char *>(lv_event_get_user_data(event));
    char *text = s_view == View::PlaylistCreate ? s_playlist_name : s_search_query;
    const size_t capacity = s_view == View::PlaylistCreate ? sizeof(s_playlist_name) : sizeof(s_search_query);
    size_t length = std::strlen(text);
    if (std::strcmp(key, "SEARCH") == 0) {
        s_list_page = 0;
        s_search_error[0] = '\0';
        const esp_err_t result = lyra::media::start_search(s_search_category, s_search_query);
        if (result == ESP_OK) {
            copy_ui_text(s_submitted_search_query, sizeof(s_submitted_search_query), s_search_query);
            s_submitted_search_category = s_search_category;
        }
        else copy_ui_text(s_search_error, sizeof(s_search_error), "Search is busy - try again shortly");
        render(View::Search);
        return;
    }
    if (std::strcmp(key, "SAVE") == 0) {
        size_t created = 0;
        if (lyra::media::create_playlist(s_playlist_name, &created) == ESP_OK) {
            s_selected_playlist = created;
            s_playlist_name[0] = '\0';
            render(View::PlaylistDetail);
        }
        return;
    }
    if (std::strcmp(key, "123") == 0 || std::strcmp(key, "ABC") == 0) {
        s_library_keyboard_symbols = !s_library_keyboard_symbols;
        render(s_view);
        return;
    }
    if (std::strcmp(key, "<") == 0) {
        if (length > 0) text[length - 1] = '\0';
    } else if (std::strcmp(key, "SPACE") == 0) {
        if (length + 1 < capacity) {
            text[length] = ' ';
            text[length + 1] = '\0';
        }
    } else if (length + 1 < capacity) {
        text[length] = key[0];
        text[length + 1] = '\0';
    }
    update_search_label();
    // Searching 10,000 MicroSD-backed records is intentionally explicit. This
    // keeps typing responsive and turns each query into one sequential read.
}

void toggle_search_keyboard_cb(lv_event_t *)
{
    s_search_keyboard_visible = !s_search_keyboard_visible;
    if (!s_search_keyboard || !s_search_results || !s_search_keyboard_toggle) return;
    if (s_search_keyboard_visible) {
        lv_obj_remove_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results,
                          content_height(kStatusHeight) - library_keyboard_height() - 84);
        lv_label_set_text(s_search_keyboard_toggle, "HIDE");
    } else {
        lv_obj_add_flag(s_search_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_search_results, content_height(kStatusHeight) - 84);
        lv_label_set_text(s_search_keyboard_toggle, "KEYS");
    }
    lv_obj_invalidate(s_search_results);
}

void make_keyboard_row(lv_obj_t *parent, int y, const char *keys[], size_t count, int inset = 3,
                       int height = 38)
{
    int gap = 3;
    int width = (320 - inset * 2 - gap * static_cast<int>(count - 1)) / static_cast<int>(count);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *key = make_button(parent, inset + static_cast<int>(i) * (width + gap), y, width, height,
                                    kSurfaceRaised, 5);
        lv_obj_t *label = make_label(key, keys[i], kTextPrimary);
        lv_obj_center(label);
        lv_obj_add_event_cb(key, keyboard_cb, LV_EVENT_CLICKED, const_cast<char *>(keys[i]));
    }
}

void make_library_keyboard(lv_obj_t *keyboard, const char *action)
{
    const char *letters_row1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    const char *letters_row2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    const char *letters_row3[] = {"z", "x", "c", "v", "b", "n", "m", "<"};
    const char *symbols_row1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    const char *symbols_row2[] = {"@", "#", "$", "_", "&", "-", "+", "(", ")"};
    const char *symbols_row3[] = {".", ",", "?", "!", "'", "\"", "=", "<"};
    const char *row4[] = {s_library_keyboard_symbols ? "ABC" : "123", "SPACE", action};
    const char **keys_row1 = s_library_keyboard_symbols ? symbols_row1 : letters_row1;
    const char **keys_row2 = s_library_keyboard_symbols ? symbols_row2 : letters_row2;
    const char **keys_row3 = s_library_keyboard_symbols ? symbols_row3 : letters_row3;
    // Keep the keys immediately above the dock (or display edge).  Anchoring
    // the rows from the bottom prevents empty space below them when the
    // virtual navigation setting changes.
    const int key_height =
        s_show_nav ? kKeyboardKeyHeightWithNav : kKeyboardKeyHeightWithoutNav;
    const int rows_height = key_height * 4 + kKeyboardRowGap * 3;
    const int first_row_y = library_keyboard_height() - kKeyboardBottomPadding - rows_height;
    make_keyboard_row(keyboard, first_row_y, keys_row1, 10, 3, key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap), keys_row2, 9, 16,
                      key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap) * 2, keys_row3, 8, 24,
                      key_height);
    make_keyboard_row(keyboard, first_row_y + (key_height + kKeyboardRowGap) * 3, row4, 3, 3,
                      key_height);
}

void render_search()
{
    lv_obj_t *body = make_box(s_screen, 0, kStatusHeight, 320, content_height(kStatusHeight), kBackground);
    lv_obj_t *input = make_box(body, 9, 4, 233, 43, kSurfaceRaised, 7);
    lv_obj_t *magnifier = make_label(input, LV_SYMBOL_EDIT, kTextSecondary);
    lv_obj_align(magnifier, LV_ALIGN_LEFT_MID, 10, 0);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 180);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 37, 0);
    update_search_label();
    lv_obj_t *toggle = make_button(body, 248, 4, 63, 43, kSurfaceRaised, 7);
    s_search_keyboard_toggle = make_label(toggle, s_search_keyboard_visible ? "HIDE" : "KEYS", kAccent);
    lv_obj_center(s_search_keyboard_toggle);
    lv_obj_add_event_cb(toggle, toggle_search_keyboard_cb, LV_EVENT_CLICKED, nullptr);
    constexpr lyra::media::SearchCategory categories[] = {
        lyra::media::SearchCategory::Songs,
        lyra::media::SearchCategory::Albums,
        lyra::media::SearchCategory::Artists,
        lyra::media::SearchCategory::Playlists,
    };
    constexpr const char *labels[] = {"Songs", "Albums", "Artists", "Playlists"};
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); ++i) {
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
    make_library_keyboard(keyboard, "SEARCH");
    if (!s_search_keyboard_visible) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void render_playlist_create()
{
    make_header("New Playlist", View::Playlists, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, 320, content_height(72), kBackground);
    lv_obj_t *input = make_box(body, 9, 8, 302, 43, kSurfaceRaised, 7);
    s_search_label = make_label(input, "", kTextMuted);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();
    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height, kKeyboardSurface);
    make_library_keyboard(keyboard, "SAVE");
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

void toggle_bool_cb(lv_event_t *event)
{
    lv_obj_t *toggle = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    bool *value = static_cast<bool *>(lv_obj_get_user_data(toggle));
    *value = !*value;
    if (value == &s_gapless || value == &s_replay_gain) {
        save_user_settings();
        if (value == &s_replay_gain) apply_replay_gain_to_current_track();
    }
    if (value == &s_dark_mode) {
        apply_theme_palette();
        save_user_settings();
        render(s_view);
        return;
    }
    if (value == &s_show_nav) {
        // This setting changes the usable page height and dock object tree.
        render(s_view);
        return;
    }
    if (value == &s_speaker_output_enabled) {
        const esp_err_t result = lyra::audio::set_speaker_output_enabled(*value);
        if (result != ESP_OK) {
            *value = !*value;
            ESP_LOGW(kTag, "could not change on-board speaker output: %s",
                     esp_err_to_name(result));
            return;
        }
        save_user_settings();
    }

    lv_obj_set_style_bg_color(toggle, *value ? kAccent : kDivider, 0);
    lv_obj_set_x(lv_obj_get_child(toggle, 0), *value ? 21 : 3);
}

void make_setting_toggle(lv_obj_t *parent, int y, const char *title, const char *subtitle, bool *value)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 62, kSurface, 6);
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, subtitle == nullptr ? 0 : -10);
    if (subtitle != nullptr) {
        lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    }
    lv_obj_t *toggle = make_box(row, 250, 17, 42, 24, *value ? kAccent : kDivider, 12);
    make_box(toggle, *value ? 21 : 3, 3, 18, 18,
             *value ? kTextOnAccent : kTextPrimary, 9);
    lv_obj_set_user_data(toggle, value);
    lv_obj_add_event_cb(row, toggle_bool_cb, LV_EVENT_CLICKED, toggle);
}

void accent_colour_cb(lv_event_t *event)
{
    const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (index >= kAccentPaletteCount) return;
    if (s_accent_colour == index) return;
    s_accent_colour = static_cast<uint8_t>(index);
    apply_theme_palette();
    save_user_settings();
    render(s_view);
}

void make_accent_selector(lv_obj_t *parent, int y)
{
    lv_obj_t *card = make_box(parent, 7, y, 306, 110, kSurface, 6);
    lv_obj_t *title = make_label(card, "Accent colour", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 9);

    for (size_t index = 0; index < kAccentPaletteCount; ++index) {
        const int column = static_cast<int>(index % 4);
        const int row = static_cast<int>(index / 4);
        const AccentPalette &accent = kAccentPalettes[index];
        const uint32_t rgb = s_dark_mode ? accent.dark_rgb : accent.light_rgb;
        lv_obj_t *swatch = make_box(card, 14 + column * 72, 42 + row * 36, 28, 28,
                                    lv_color_hex(rgb), 14);
        lv_obj_set_style_border_width(swatch, s_accent_colour == index ? 3 : 1, 0);
        lv_obj_set_style_border_color(swatch,
                                      s_accent_colour == index ? kTextPrimary : kDivider, 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, accent_colour_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
    }
}

void artwork_setting_cb(lv_event_t *event)
{
    const ArtworkSetting setting = static_cast<ArtworkSetting>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const lyra::media::Status status = lyra::media::status();
    const esp_err_t result = setting == ArtworkSetting::SdCache ?
        lyra::media::set_artwork_sd_cache_enabled(!status.artwork_sd_cache_enabled) :
        lyra::media::set_artwork_size(
            status.artwork_size == lyra::media::kLargeArtworkSize ?
            lyra::media::kDefaultArtworkSize : lyra::media::kLargeArtworkSize);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save artwork setting: %s", esp_err_to_name(result));
    }
    render(View::SystemSettings);
}

void make_artwork_setting_toggle(lv_obj_t *parent, int y, const char *title,
                                 const char *subtitle, bool value,
                                 ArtworkSetting setting)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 62, kSurface, 6);
    lv_obj_t *title_label = make_label(row, title, kTextPrimary);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 12, -10);
    lv_obj_t *sub = make_label(row, subtitle, kTextMuted);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 12, 12);
    lv_obj_t *toggle = make_box(row, 250, 17, 42, 24, value ? kAccent : kDivider, 12);
    make_box(toggle, value ? 21 : 3, 3, 18, 18,
             value ? kTextOnAccent : kTextPrimary, 9);
    lv_obj_add_event_cb(row, artwork_setting_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(setting)));
}

void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const int value = lv_slider_get_value(slider);
    lyra::audio::set_volume(static_cast<uint8_t>(value));
    update_status_volume_label(static_cast<uint8_t>(value));
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(value_label, text);
    }
}

void volume_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
}

void make_volume_control(lv_obj_t *parent, int y)
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, "Volume", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), "%u%%",
                  static_cast<unsigned>(audio_status.volume_percent));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 0, lyra::audio::maximum_volume_percent());
    lv_slider_set_value(slider, audio_status.volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, volume_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, volume_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void brightness_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    s_brightness_percent = static_cast<uint8_t>(lv_slider_get_value(slider));
    const esp_err_t result = lyra_board_display_set_brightness(s_brightness_percent);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not set display brightness: %s", esp_err_to_name(result));
    }
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label != nullptr) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(s_brightness_percent));
        lv_label_set_text(value_label, text);
    }
}

void brightness_slider_released_cb(lv_event_t *)
{
    save_user_settings();
}

void make_brightness_control(lv_obj_t *parent, int y)
{
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, "Brightness", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%", static_cast<unsigned>(s_brightness_percent));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 1, 100);
    lv_slider_set_value(slider, s_brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, brightness_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, brightness_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

const char *sort_section_name(lyra::media::SortSection section)
{
    switch (section) {
    case lyra::media::SortSection::Songs: return "Songs";
    case lyra::media::SortSection::Albums: return "Albums";
    case lyra::media::SortSection::Artists: return "Artists";
    }
    return "Songs";
}

const char *sort_field_name(lyra::media::SortField field)
{
    switch (field) {
    case lyra::media::SortField::Title: return "Title";
    case lyra::media::SortField::Album: return "Album";
    case lyra::media::SortField::TrackNumber: return "Track Number";
    case lyra::media::SortField::Duration: return "Duration";
    case lyra::media::SortField::Artist: return "Artist";
    case lyra::media::SortField::DateModified: return "Date Modified";
    }
    return "Title";
}

const char *sort_direction_name(lyra::media::SortDirection direction)
{
    return direction == lyra::media::SortDirection::Ascending ? "Ascending" : "Descending";
}

void sorting_section_cb(lv_event_t *event)
{
    s_sort_section = static_cast<lyra::media::SortSection>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    navigate_to(View::SortingOptions);
}

void sorting_option_cb(lv_event_t *event)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    const auto field = static_cast<lyra::media::SortField>(value / 2u);
    const auto direction = static_cast<lyra::media::SortDirection>(value % 2u);
    const esp_err_t result = lyra::media::set_sort_setting(s_sort_section, field, direction);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save %s sorting setting: %s",
                 sort_section_name(s_sort_section), esp_err_to_name(result));
    }
    render(View::SortingOptions);
}

void render_sorting_settings()
{
    make_header("Sorting", View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr lyra::media::SortSection sections[] = {
        lyra::media::SortSection::Songs,
        lyra::media::SortSection::Albums,
        lyra::media::SortSection::Artists,
    };
    constexpr const char *subtitles[] = {
        "Choose the order of every song",
        "Choose the order of album rows",
        "Choose the order of artist rows",
    };
    for (size_t index = 0; index < 3; ++index) {
        lv_obj_t *row = make_row(body, static_cast<int>(index) * 58, LV_SYMBOL_LIST,
                                 sort_section_name(sections[index]), subtitles[index],
                                 View::SortingOptions, 54);
        lv_obj_remove_event_cb(row, route_cb);
        lv_obj_add_event_cb(row, sorting_section_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(sections[index])));
    }
}

void make_sorting_option_row(lv_obj_t *parent, int y, lyra::media::SortField field,
                             lyra::media::SortDirection direction,
                             const lyra::media::SortSetting &selected)
{
    const bool active = selected.field == field && selected.direction == direction;
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    char title[64];
    std::snprintf(title, sizeof(title), "%s / %s", sort_field_name(field),
                  sort_direction_name(direction));
    lv_obj_t *label = make_label(row, title, active ? kAccent : kTextPrimary);
    make_marquee(label, 232);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    const uintptr_t payload = static_cast<uintptr_t>(field) * 2u +
                              static_cast<uintptr_t>(direction);
    lv_obj_add_event_cb(row, sorting_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(payload));
}

void render_sorting_options()
{
    char title[32];
    std::snprintf(title, sizeof(title), "Sort %s", sort_section_name(s_sort_section));
    make_header(title, View::SortingSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    const lyra::media::SortSetting selected = lyra::media::sort_setting(s_sort_section);
    constexpr lyra::media::SortField fields[] = {
        lyra::media::SortField::Title,
        lyra::media::SortField::Album,
        lyra::media::SortField::TrackNumber,
        lyra::media::SortField::Duration,
        lyra::media::SortField::Artist,
        lyra::media::SortField::DateModified,
    };
    constexpr lyra::media::SortDirection directions[] = {
        lyra::media::SortDirection::Ascending,
        lyra::media::SortDirection::Descending,
    };
    size_t row = 0;
    for (const auto field : fields) {
        for (const auto direction : directions) {
            make_sorting_option_row(body, static_cast<int>(row++ * 58), field,
                                    direction, selected);
        }
    }
}

void make_playback_option_row(lv_obj_t *parent, int y, const char *title, bool active,
                              lv_event_cb_t callback, uintptr_t value)
{
    lv_obj_t *row = make_button(parent, 7, y, 306, 54,
                                active ? kAccentSurface : kSurface, 6);
    lv_obj_t *label = make_label(row, title, active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(value));
}

void crossfade_option_cb(lv_event_t *event)
{
    s_crossfade_seconds = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    lyra::audio::set_transition_gain(100);
    save_user_settings();
    navigate_back(View::PlaybackSettings);
}

void sleep_timer_option_cb(lv_event_t *event)
{
    s_sleep_timer_minutes = static_cast<uint16_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    s_sleep_timer_deadline_us = s_sleep_timer_minutes == 0 ? 0 :
        esp_timer_get_time() + static_cast<int64_t>(s_sleep_timer_minutes) * 60 * 1000 * 1000;
    navigate_back(View::PlaybackSettings);
}

void render_crossfade_options()
{
    make_header("Crossfade", View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint8_t values[] = {0, 1, 2, 4, 6};
    constexpr const char *labels[] = {"Off", "1 second", "2 seconds", "4 seconds", "6 seconds"};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_crossfade_seconds == values[index], crossfade_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_sleep_timer_options()
{
    make_header("Sleep timer", View::PlaybackSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr uint16_t values[] = {0, 15, 30, 45, 60};
    constexpr const char *labels[] = {"Off", "15 minutes", "30 minutes", "45 minutes", "60 minutes"};
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        make_playback_option_row(body, static_cast<int>(index) * 58, labels[index],
                                 s_sleep_timer_minutes == values[index], sleep_timer_option_cb,
                                 static_cast<uintptr_t>(values[index]));
    }
}

void render_settings_menu()
{
    make_header("Settings", View::Menu, true);
    lv_obj_t *body = make_scroll_body(72);
    make_row(body, 0, LV_SYMBOL_PLAY, "Playback", nullptr, View::PlaybackSettings, 54);
    make_row(body, 58, LV_SYMBOL_VOLUME_MID, "Sound", nullptr, View::SoundSettings, 54);
    make_row(body, 116, LV_SYMBOL_IMAGE, "Display", nullptr, View::DisplaySettings, 54);
    make_row(body, 174, LV_SYMBOL_LIST, "Sorting", nullptr, View::SortingSettings, 54);
    make_row(body, 232, LV_SYMBOL_SETTINGS, "System", nullptr, View::SystemSettings, 54);
    make_row(body, 290, LV_SYMBOL_WARNING, "About", nullptr, View::About, 54);
}

void scan_library_cb(lv_event_t *)
{
    if (lyra::media::start_scan() != ESP_OK) {
        render(View::SystemSettings);
        return;
    }
    s_scan_overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(s_scan_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(s_scan_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *dialog = make_box(s_scan_overlay, 24, 126, 272, 228, kSurfaceRaised, 14);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_border_color(dialog, kAccentDark, 0);
    lv_obj_t *spinner = lv_spinner_create(dialog);
    lv_obj_set_size(spinner, 62, 62);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 25);
    lv_spinner_set_anim_params(spinner, 900, 250);
    lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_t *title = make_label(dialog, "SCANNING MUSIC LIBRARY", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);
    s_scan_count_label = make_label(dialog, "0 songs found", kAccent);
    lv_obj_align(s_scan_count_label, LV_ALIGN_TOP_MID, 0, 137);
    s_scan_phase_label = make_label(dialog, "Reading MicroSD folders...", kTextSecondary);
    lv_obj_align(s_scan_phase_label, LV_ALIGN_TOP_MID, 0, 169);
}

void save_active_queue_snapshot()
{
    if (!s_has_active_queue) return;
    const size_t count = playback_queue_count();
    size_t current = 0;
    if (count == 0 || !current_queue_position(&current)) return;

    auto *tracks = static_cast<size_t *>(heap_caps_malloc(
        count * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tracks) {
        tracks = static_cast<size_t *>(heap_caps_malloc(
            count * sizeof(size_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!tracks) {
        ESP_LOGW(kTag, "could not allocate queue snapshot");
        return;
    }
    bool complete = true;
    for (size_t position = 0; position < count; ++position) {
        if (!queue_track_at(position, &tracks[position])) {
            complete = false;
            break;
        }
    }
    const esp_err_t result = complete ?
        lyra::media::save_queue_snapshot(tracks, count, current) : ESP_FAIL;
    heap_caps_free(tracks);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save queue snapshot: %s", esp_err_to_name(result));
    }
}

void power_action_task(void *context)
{
    const PowerAction action = static_cast<PowerAction>(reinterpret_cast<uintptr_t>(context));
    // Give LVGL time to present the shutdown status before filesystem work.
    vTaskDelay(pdMS_TO_TICKS(250));
    save_active_queue_snapshot();
    const esp_err_t volume_result = lyra::audio::save_volume();
    if (volume_result != ESP_OK) ESP_LOGW(kTag, "could not save volume before shutdown: %s",
                                          esp_err_to_name(volume_result));
    lyra::audio::stop();
    // The decoder owns an open MicroSD FILE while it is active. Wait for its
    // worker to release that handle before attempting the filesystem unmount.
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const esp_err_t media_result = lyra::media::shutdown();
    if (media_result != ESP_OK) {
        ESP_LOGE(kTag, "safe shutdown failed: %s", esp_err_to_name(media_result));
        // Do not restart or sleep after an unmount failure: preserving the card
        // takes priority over completing the requested power action.
        vTaskDelete(nullptr);
        return;
    }
    lyra_board_display_set_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (action == PowerAction::Reboot) {
        ESP_LOGI(kTag, "restarting after safe MicroSD unmount");
        esp_restart();
    }
    ESP_LOGI(kTag, "entering deep sleep after safe MicroSD unmount");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
}

void confirm_power_action_cb(lv_event_t *event)
{
    const PowerAction action = static_cast<PowerAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    style_root();
    lv_obj_t *label = make_label(s_screen,
                                 action == PowerAction::Reboot ? "REBOOTING\n\nSafely unmounting MicroSD..."
                                                                : "POWERING OFF\n\nSafely unmounting MicroSD...",
                                 kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    if (xTaskCreatePinnedToCore(power_action_task, "lyra_power", 4096,
                                reinterpret_cast<void *>(static_cast<uintptr_t>(action)),
                                3, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "could not create power action task");
        render(View::SystemSettings);
    }
}

void cancel_power_action_cb(lv_event_t *event)
{
    lv_obj_delete(static_cast<lv_obj_t *>(lv_event_get_user_data(event)));
}

void show_power_confirmation(PowerAction action)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                                 kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 132, 280, 216, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, action == PowerAction::Reboot ? "Reboot Lyra?" : "Power off Lyra?",
                                 kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog,
                                   action == PowerAction::Reboot
                                       ? "The MicroSD card will be safely\nunmounted before restarting."
                                       : "The MicroSD card will be safely\nunmounted before deep sleep.",
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -15);

    lv_obj_t *cancel = make_button(dialog, 14, 152, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);

    lv_obj_t *confirm = make_button(dialog, 150, 152, 116, 48,
                                    action == PowerAction::Reboot ? kAccentDark : lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, action == PowerAction::Reboot ? "REBOOT" : "POWER OFF",
                                         kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_power_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

void reboot_cb(lv_event_t *) { show_power_confirmation(PowerAction::Reboot); }
void power_off_cb(lv_event_t *) { show_power_confirmation(PowerAction::PowerOff); }

void confirm_database_action_cb(lv_event_t *event)
{
    const DatabaseAction action = static_cast<DatabaseAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    const esp_err_t result = action == DatabaseAction::Playlists ? lyra::media::clear_playlists() :
                             action == DatabaseAction::Artwork ? lyra::media::clear_artwork_cache() :
                                                                 lyra::media::clear_all_databases();
    if (result == ESP_OK) {
        copy_ui_text(s_database_status, sizeof(s_database_status),
                     action == DatabaseAction::Playlists ? "Playlists cleared" :
                     action == DatabaseAction::Artwork ? "Album art cache cleared" :
                                                          "Databases cleared - rescan required");
    } else if (result == ESP_ERR_INVALID_STATE) {
        copy_ui_text(s_database_status, sizeof(s_database_status), "Storage busy - try again shortly");
    } else {
        copy_ui_text(s_database_status, sizeof(s_database_status), "Could not clear storage");
    }
    render(View::DatabaseStorage);
}

void show_database_confirmation(DatabaseAction action)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 118, 280, 244, kSurfaceRaised, 12);
    const char *title_text = action == DatabaseAction::Playlists ? "Clear playlists?" :
                             action == DatabaseAction::Artwork ? "Clear album art cache?" :
                                                                 "Clear all databases?";
    const char *message_text = action == DatabaseAction::Playlists ?
        "Every saved playlist and favorite\nwill be removed." :
        action == DatabaseAction::Artwork ?
        "Large decoded covers will be removed.\nThey regenerate when needed." :
        "Library index, playlists, and artwork\ncache will be removed. Music files stay.\nA rescan is required.";
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, message_text, kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(message, 240);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -17);
    lv_obj_t *cancel = make_button(dialog, 14, 180, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, "CANCEL", kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 180, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, "CLEAR", kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
}

void database_action_cb(lv_event_t *event)
{
    show_database_confirmation(static_cast<DatabaseAction>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void render_database_storage()
{
    make_header("Database Storage", View::SystemSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    lv_obj_t *playlists = make_row(body, 0, LV_SYMBOL_LIST, "Clear Playlists",
                                   "Remove playlists and favorites", View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(playlists, route_cb);
    lv_obj_add_event_cb(playlists, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Playlists)));
    lv_obj_t *artwork = make_row(body, 66, LV_SYMBOL_IMAGE, "Clear Album Art Cache",
                                 "Remove large decoded covers", View::DatabaseStorage, 62);
    lv_obj_remove_event_cb(artwork, route_cb);
    lv_obj_add_event_cb(artwork, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::Artwork)));
    lv_obj_t *all = make_row(body, 132, LV_SYMBOL_WARNING, "Clear All Databases",
                             "Music library rescan required", View::DatabaseStorage, 62);
    lv_obj_set_style_bg_color(all, kDangerSurface, 0);
    lv_obj_remove_event_cb(all, route_cb);
    lv_obj_add_event_cb(all, database_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(DatabaseAction::All)));
    if (s_database_status[0]) {
        lv_obj_t *status = make_label(body, s_database_status, kAccent);
        lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 214);
    }
}

void render_settings_page(View view)
{
    const char *title = view == View::PlaybackSettings ? "Playback" :
                        view == View::SoundSettings ? "Sound" :
                        view == View::DisplaySettings ? "Display" : "System";
    make_header(title, View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    if (view == View::PlaybackSettings) {
        make_setting_toggle(body, 0, "Gapless playback", nullptr, &s_gapless);
        make_setting_toggle(body, 66, "ReplayGain", nullptr, &s_replay_gain);
        make_row(body, 132, LV_SYMBOL_LOOP, "Crossfade", nullptr, View::CrossfadeOptions, 54);
        make_row(body, 190, LV_SYMBOL_WARNING, "Sleep timer", nullptr, View::SleepTimerOptions, 54);
    } else if (view == View::SoundSettings) {
        make_volume_control(body, 0);
        make_setting_toggle(body, 92, "On-board speaker",
                            "Also play through the built-in speaker",
                            &s_speaker_output_enabled);
        make_row(body, 158, LV_SYMBOL_SETTINGS, "EQ preset",
                 equalizer_preset_name(s_equalizer_preset), View::Equalizer, 62);
    } else if (view == View::DisplaySettings) {
        make_setting_toggle(body, 0, "Dark mode",
                            s_dark_mode ? "Dark colours for the interface" :
                                          "Light colours for the interface",
                            &s_dark_mode);
        make_accent_selector(body, 66);
        make_setting_toggle(body, 182, "Virtual controls", "Show bottom navigation bar", &s_show_nav);
        make_brightness_control(body, 248);
        lv_obj_t *screen_timeout = make_row(body, 340, LV_SYMBOL_POWER, "Screen timeout", "Coming soon",
                                             View::DisplaySettings, 62);
        lv_obj_remove_event_cb(screen_timeout, route_cb);
        lv_obj_set_style_opa(screen_timeout, LV_OPA_60, 0);
    } else {
        const lyra::media::Status status = lyra::media::status();
        char sd_status[64];
        if (status.mounted) {
            std::snprintf(sd_status, sizeof(sd_status), "Mounted / %.1f GB free",
                          static_cast<double>(status.free_bytes) / (1024.0 * 1024.0 * 1024.0));
        } else {
            copy_ui_text(sd_status, sizeof(sd_status), "Not mounted (");
            append_ui_text(sd_status, sizeof(sd_status), esp_err_to_name(status.last_error));
            append_ui_text(sd_status, sizeof(sd_status), ")");
        }
        make_row(body, 0, LV_SYMBOL_SD_CARD, "MicroSD card", sd_status, View::SystemSettings, 62);
        char scan_status[48];
        if (status.scanning) {
            copy_ui_text(scan_status, sizeof(scan_status), "Scanning MicroSD...");
        } else if (status.capacity_reached) {
            copy_ui_text(scan_status, sizeof(scan_status), "10,000 tracks (library limit)");
        } else {
            std::snprintf(scan_status, sizeof(scan_status), "%u tracks indexed",
                          static_cast<unsigned>(status.track_count));
        }
        lv_obj_t *scan = make_row(body, 66, LV_SYMBOL_REFRESH, "Scan music library", scan_status,
                                  View::SystemSettings, 62);
        lv_obj_remove_event_cb(scan, route_cb);
        lv_obj_add_event_cb(scan, scan_library_cb, LV_EVENT_CLICKED, nullptr);
        make_row(body, 132, LV_SYMBOL_DRIVE, "Database Storage", "Manage playlists and library data",
                 View::DatabaseStorage, 62);
        make_artwork_setting_toggle(body, 198, "SD album art cache",
                                    "Allow oversized JPEG decoding",
                                    status.artwork_sd_cache_enabled, ArtworkSetting::SdCache);
        make_artwork_setting_toggle(body, 264, "320 x 320 album art",
                                    "Use 240 x 240 when disabled",
                                    status.artwork_size == lyra::media::kLargeArtworkSize,
                                    ArtworkSetting::Size320);
        lv_obj_t *reboot = make_row(body, 330, LV_SYMBOL_REFRESH, "Reboot", "Safely restart Lyra",
                                    View::SystemSettings, 62);
        lv_obj_remove_event_cb(reboot, route_cb);
        lv_obj_add_event_cb(reboot, reboot_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *power_off = make_row(body, 396, LV_SYMBOL_POWER, "Power off", "Safely unmount and sleep",
                                       View::SystemSettings, 62);
        lv_obj_remove_event_cb(power_off, route_cb);
        lv_obj_add_event_cb(power_off, power_off_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void render_about()
{
    make_header("About", View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    if (load_brand_logo()) {
        lv_obj_t *logo = lv_image_create(body);
        lv_image_set_src(logo, &s_brand_logo_descriptor);
        lv_obj_set_pos(logo, 10, 22);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(logo, [](lv_event_t *) {
            s_debug_tab = DebugTab::Info;
            navigate_to(View::DebugMenu);
        }, LV_EVENT_LONG_PRESSED, nullptr);
    } else {
        lv_obj_t *logo = make_label(body, "Emotivate Lyra", kTextPrimary);
        lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 38);
        lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(logo, [](lv_event_t *) {
            s_debug_tab = DebugTab::Info;
            navigate_to(View::DebugMenu);
        }, LV_EVENT_LONG_PRESSED, nullptr);
    }
    lv_obj_t *firmware_heading = make_label(body, "FIRMWARE VERSION", kTextMuted);
    lv_obj_align(firmware_heading, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *firmware_version = make_label(body, "1.0.0", kTextPrimary);
    lv_obj_align(firmware_version, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_t *hardware_heading = make_label(body, "HARDWARE ID", kTextMuted);
    lv_obj_align(hardware_heading, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_t *hardware_id = make_label(body, "JC3248W535EN", kTextPrimary);
    lv_obj_align(hardware_id, LV_ALIGN_TOP_MID, 0, 216);
    lv_obj_t *update = make_button(body, 36, 270, 248, 48, kSurface, 7);
    lv_obj_t *update_label = make_label(update, "FIRMWARE UPDATE", kTextPrimary);
    lv_obj_center(update_label);
    lv_obj_t *licenses = make_button(body, 36, 326, 248, 48, kSurface, 7);
    lv_obj_t *licenses_label = make_label(licenses, "LICENSES", kTextPrimary);
    lv_obj_center(licenses_label);
    add_route(licenses, View::Licenses);
}

void render_licenses()
{
    make_header("Licenses", View::Settings, true);
    lv_obj_t *body = make_scroll_body(72);
    constexpr const char *notices =
        "License notices for software included in this firmware.\n\n"
        "Lyra firmware\nCopyright 2026 Emotivate Lyra contributors\nApache License 2.0\n\n"
        "ESP-IDF\nCopyright Espressif Systems (Shanghai) Co., Ltd.\nApache License 2.0\n\n"
        "LVGL\nCopyright 2025 LVGL Kft\nMIT License\n\n"
        "Source Han Sans glyph data\nCopyright 2014 Adobe Systems Incorporated\n"
        "Apache License 2.0\n\n"
        "ESP Audio Codec\nCopyright 2025 Espressif Systems (Shanghai) Co., Ltd.\n"
        "Espressif Modified MIT License\n\n"
        "libjpeg-turbo\nCopyright 2009-2025 D. R. Commander\n"
        "Copyright 2015 Viktor Szathmáry\nIJG, BSD-3-Clause, and zlib licenses\n\n"
        "libpng\nCopyright 1995-2019 The PNG Reference Library Authors\n"
        "Copyright 2018-2019 Cosmin Truta\n"
        "Copyright 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson\n"
        "Copyright 1996-1997 Andreas Dilger\n"
        "Copyright 1995-1996 Guy Eric Schalnat, Group 42, Inc.\n"
        "PNG Reference Library License\n\n"
        "zlib\nCopyright 1995-2022 Jean-loup Gailly and Mark Adler\n"
        "zlib License";
    lv_obj_t *text = make_label(body, notices, kTextSecondary);
    lv_obj_set_pos(text, 14, 14);
    lv_obj_set_width(text, 292);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(text, 5, 0);
}

void render(View view)
{
    lyra::font::init();
    s_list_scrolling = false;
    s_view = view;
    s_search_label = nullptr;
    s_search_results = nullptr;
    s_search_keyboard = nullptr;
    s_search_keyboard_toggle = nullptr;
    s_search_progress_label = nullptr;
    s_scan_overlay = nullptr;
    s_scan_count_label = nullptr;
    s_scan_phase_label = nullptr;
    s_sort_overlay = nullptr;
    s_sort_progress_label = nullptr;
    style_root();
    if (view != View::FullscreenInfoArt) make_status_bar();
    switch (view) {
        case View::Menu: render_menu(); break;
        case View::Player: render_player(false); break;
        case View::FullscreenArt: render_player(true); break;
        case View::TrackInfo: render_track_info(); break;
        case View::FullscreenInfoArt: render_fullscreen_info_art(); break;
        case View::Queue: render_queue(); break;
        case View::Library: render_library(); break;
        case View::LibrarySongs: render_library_section(LibraryTab::Songs); break;
        case View::LibraryArtists: render_library_section(LibraryTab::Artists); break;
        case View::LibraryAlbums: render_library_section(LibraryTab::Albums); break;
        case View::LibraryGenres: render_library_section(LibraryTab::Genres); break;
        case View::LibraryYears: render_library_section(LibraryTab::Years); break;
        case View::AlbumDetail: render_album_detail(); break;
        case View::ArtistDetail: render_artist_detail(); break;
        case View::TrackList: render_track_list(); break;
        case View::Folders: render_folders(false); break;
        case View::FolderDetail: render_folders(true); break;
        case View::Playlists: render_playlists(false); break;
        case View::PlaylistDetail: render_playlists(true); break;
        case View::PlaylistCreate: render_playlist_create(); break;
        case View::PlaylistAdd: render_playlist_add(); break;
        case View::Equalizer: render_equalizer(); break;
        case View::EqualizerPresets: render_equalizer_presets(); break;
        case View::Search: render_search(); break;
        case View::Settings: render_settings_menu(); break;
        case View::SortingSettings: render_sorting_settings(); break;
        case View::SortingOptions: render_sorting_options(); break;
        case View::PlaybackSettings:
        case View::SoundSettings:
        case View::DisplaySettings:
        case View::SystemSettings: render_settings_page(view); break;
        case View::CrossfadeOptions: render_crossfade_options(); break;
        case View::SleepTimerOptions: render_sleep_timer_options(); break;
        case View::DatabaseStorage: render_database_storage(); break;
        case View::About: render_about(); break;
        case View::DebugMenu: render_debug_menu(); break;
        case View::Licenses: render_licenses(); break;
    }
    if (s_show_nav && view != View::FullscreenInfoArt) make_virtual_nav();
}

bool automatic_track_advance_available()
{
    if (s_repeat_mode == RepeatMode::Song || s_repeat_mode == RepeatMode::All) return true;
    const size_t count = playback_queue_count();
    if (count == 0) return false;
    if (s_shuffle) return s_shuffle_cursor + 1 < count;
    size_t position = 0;
    return current_queue_position(&position) && position + 1 < count;
}

void begin_crossfade_fade_in()
{
    if (s_crossfade_seconds == 0) return;
    s_crossfade_fade_in_started_us = esp_timer_get_time();
    s_crossfade_fade_in_ends_us = s_crossfade_fade_in_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    lyra::audio::set_transition_gain(0);
}

bool advance_after_track_end()
{
    const bool advanced = s_repeat_mode == RepeatMode::Song ? restart_current_track() :
                          move_in_playback_queue(1, true);
    if (advanced) begin_crossfade_fade_in();
    return advanced;
}

void update_crossfade_gain(const lyra::audio::Status &audio_status, int64_t now_us)
{
    if (s_crossfade_fade_in_ends_us != 0) {
        if (now_us >= s_crossfade_fade_in_ends_us) {
            s_crossfade_fade_in_started_us = 0;
            s_crossfade_fade_in_ends_us = 0;
            lyra::audio::set_transition_gain(100);
        } else {
            const int64_t duration_us = s_crossfade_fade_in_ends_us - s_crossfade_fade_in_started_us;
            const int64_t elapsed_us = now_us - s_crossfade_fade_in_started_us;
            const uint8_t gain = static_cast<uint8_t>(std::clamp<int64_t>(
                (elapsed_us * 100) / duration_us, 0, 100));
            lyra::audio::set_transition_gain(gain);
        }
        return;
    }
    if (s_crossfade_seconds == 0 || !automatic_track_advance_available() ||
        audio_status.duration_ms == 0 || audio_status.position_ms >= audio_status.duration_ms) {
        lyra::audio::set_transition_gain(100);
        return;
    }
    const uint32_t crossfade_ms = static_cast<uint32_t>(s_crossfade_seconds) * 1000;
    const uint32_t remaining_ms = audio_status.duration_ms - audio_status.position_ms;
    const uint8_t gain = remaining_ms >= crossfade_ms ? 100 : static_cast<uint8_t>(
        (static_cast<uint64_t>(remaining_ms) * 100u) / crossfade_ms);
    lyra::audio::set_transition_gain(gain);
}

void crossfade_poll_cb(lv_timer_t *)
{
    if (s_crossfade_seconds == 0 && s_crossfade_fade_in_ends_us == 0 &&
        s_crossfade_transition_direction == 0 && !s_crossfade_pause_pending) return;
    const int64_t now_us = esp_timer_get_time();
    if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) {
        if (now_us >= s_crossfade_fade_out_ends_us) {
            const int direction = s_crossfade_transition_direction;
            const bool pause_pending = s_crossfade_pause_pending;
            s_crossfade_transition_direction = 0;
            s_crossfade_pause_pending = false;
            s_crossfade_fade_out_started_us = 0;
            s_crossfade_fade_out_ends_us = 0;
            if (pause_pending) {
                lyra::audio::set_transition_gain(0);
                lyra::audio::toggle_pause();
            } else if (move_in_playback_queue(direction, false)) begin_crossfade_fade_in();
            else lyra::audio::set_transition_gain(100);
        } else {
            const int64_t duration_us = s_crossfade_fade_out_ends_us -
                s_crossfade_fade_out_started_us;
            const int64_t remaining_us = s_crossfade_fade_out_ends_us - now_us;
            const uint8_t gain = static_cast<uint8_t>(std::clamp<int64_t>(
                (remaining_us * 100) / duration_us, 0, 100));
            lyra::audio::set_transition_gain(gain);
        }
        return;
    }
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return;
    update_crossfade_gain(audio_status, now_us);
}

void player_progress_poll_cb(lv_timer_t *)
{
    const lyra::audio::Status audio_status = lyra::audio::status();
    const int64_t now_us = esp_timer_get_time();
    if (s_sleep_timer_deadline_us != 0 && now_us >= s_sleep_timer_deadline_us) {
        s_sleep_timer_deadline_us = 0;
        s_sleep_timer_minutes = 0;
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
        s_crossfade_fade_in_started_us = 0;
        s_crossfade_fade_in_ends_us = 0;
        s_crossfade_fade_out_started_us = 0;
        s_crossfade_fade_out_ends_us = 0;
        s_crossfade_transition_direction = 0;
        s_crossfade_pause_pending = false;
        lyra::audio::set_transition_gain(100);
        lyra::audio::stop();
        show_notice("Sleep timer", "Playback stopped.");
    } else if (!audio_status.eof) {
        s_audio_eof_seen = false;
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
    } else if (!s_audio_eof_seen) {
        s_audio_eof_seen = true;
        lyra::audio::set_transition_gain(100);
        if (automatic_track_advance_available()) {
            if (s_gapless) {
                advance_after_track_end();
            } else {
                s_pending_track_advance = true;
                s_pending_track_advance_us = now_us +
                    static_cast<int64_t>(kNonGaplessTrackPauseMs) * 1000;
            }
        }
    } else if (s_pending_track_advance && now_us >= s_pending_track_advance_us) {
        s_pending_track_advance = false;
        s_pending_track_advance_us = 0;
        advance_after_track_end();
    }
    update_player_progress();
}

void show_sorting_overlay(const lyra::media::Status &status)
{
    if (s_sort_overlay || !s_screen) return;
    s_sort_overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight,
                              kOverlay);
    lv_obj_set_style_bg_opa(s_sort_overlay, LV_OPA_80, 0);
    lv_obj_add_flag(s_sort_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_sort_overlay);
    lv_obj_t *dialog = make_box(s_sort_overlay, 24, 126, 272, 228,
                                kSurfaceRaised, 14);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_border_color(dialog, kAccentDark, 0);
    lv_obj_t *spinner = lv_spinner_create(dialog);
    lv_obj_set_size(spinner, 62, 62);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 25);
    lv_spinner_set_anim_params(spinner, 900, 250);
    lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_t *title = make_label(dialog, "PREPARING LIBRARY SORT", kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);
    s_sort_progress_label = make_label(dialog, "Building sort cache...", kAccent);
    lv_obj_align(s_sort_progress_label, LV_ALIGN_TOP_MID, 0, 137);
    lv_obj_t *section = make_label(dialog,
        status.sorting_section == static_cast<uint8_t>(lyra::media::SortSection::Songs) ?
            "Songs" : status.sorting_section == static_cast<uint8_t>(lyra::media::SortSection::Albums) ?
            "Albums" : "Artists", kTextSecondary);
    lv_obj_align(section, LV_ALIGN_TOP_MID, 0, 169);
}

void update_sorting_overlay(const lyra::media::Status &status)
{
    if (!s_sort_progress_label) return;
    char progress[48];
    std::snprintf(progress, sizeof(progress), "Preparing %u / %u",
                  static_cast<unsigned>(status.sorting_indexed),
                  static_cast<unsigned>(status.sorting_total));
    lv_label_set_text(s_sort_progress_label, progress);
}

void catalog_poll_cb(lv_timer_t *)
{
    const lyra::media::Status status = lyra::media::status();
    const lyra::media::SearchStatus search = lyra::media::search_status();
    const bool changed = status.catalog_generation != s_seen_catalog_generation ||
                         status.scanning != s_seen_scanning;
    const bool artwork_changed = status.artwork_generation != s_seen_artwork_generation;
    const bool duration_changed = status.duration_generation != s_seen_duration_generation;
    const bool sorting_changed = status.sorting_generation != s_seen_sorting_generation;
    s_seen_catalog_generation = status.catalog_generation;
    s_seen_scanning = status.scanning;
    s_seen_artwork_generation = status.artwork_generation;
    s_seen_duration_generation = status.duration_generation;
    s_seen_sorting_generation = status.sorting_generation;
    if (s_view == View::Search) {
        if (search.running && s_search_progress_label) {
            char progress[48];
            std::snprintf(progress, sizeof(progress), "Searching %u / %u",
                          static_cast<unsigned>(search.processed), static_cast<unsigned>(search.total));
            lv_label_set_text(s_search_progress_label, progress);
        }
        if (search.generation != s_seen_search_generation) {
            s_seen_search_generation = search.generation;
            if (!search.running) {
                if (search.result != ESP_OK) copy_ui_text(s_search_error, sizeof(s_search_error), "Search could not be completed");
                render(View::Search);
                return;
            }
        }
    } else {
        s_seen_search_generation = search.generation;
    }
    if (s_scan_overlay) {
        char count[48];
        std::snprintf(count, sizeof(count), status.scan_found == 1 ? "%u song found" : "%u songs found",
                      static_cast<unsigned>(status.scan_found));
        if (s_scan_count_label) lv_label_set_text(s_scan_count_label, count);
        if (s_scan_phase_label) {
            lv_label_set_text(s_scan_phase_label,
                              status.scan_indexing ? "Building fast library indexes..." : "Reading MicroSD folders...");
        }
        if (!status.scanning) render(View::SystemSettings);
        return;
    }
    if (status.sorting_indexing) {
        show_sorting_overlay(status);
        update_sorting_overlay(status);
        return;
    }
    if (s_sort_overlay) {
        lv_obj_delete(s_sort_overlay);
        s_sort_overlay = nullptr;
        s_sort_progress_label = nullptr;
    }
    if (changed && s_view == View::SystemSettings) render(View::SystemSettings);
    if (sorting_changed &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::LibraryArtists)) {
        render(s_view);
        return;
    }
    if (duration_changed && !status.duration_indexing &&
        (s_view == View::LibrarySongs || s_view == View::LibraryAlbums ||
         s_view == View::LibraryArtists)) {
        render(s_view);
        return;
    }
    if (artwork_changed) {
        if (s_view == View::Player || s_view == View::FullscreenArt ||
            s_view == View::TrackInfo || s_view == View::FullscreenInfoArt ||
            s_view == View::AlbumDetail) {
            render(s_view);
        }
        else if (s_screen) lv_obj_invalidate(s_screen);
    }
}

} // namespace

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
    s_dark_mode = true;
    s_accent_colour = 0;
    s_speaker_output_enabled = lyra::audio::kDefaultSpeakerOutputEnabled;
    s_equalizer_preset = EqualizerPreset::Custom;
    std::memset(s_equalizer_custom_bands, 0, sizeof(s_equalizer_custom_bands));
    s_debug_tab = DebugTab::Info;
    s_debug_status[0] = '\0';
    s_sleep_timer_minutes = 0;
    s_sleep_timer_deadline_us = 0;
    s_pending_track_advance = false;
    s_pending_track_advance_us = 0;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    load_user_settings();
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
