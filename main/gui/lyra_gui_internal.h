/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lyra_audio.h"
#include "lyra_board.h"
#include "lyra_boot_test.h"
#include "lyra_clock.h"
#include "lyra_font.h"
#include "lyra_i18n.h"
#include "lvgl.h"
#include "lyra_gui.h"
#include "lyra_gui_settings.h"
#include "lyra_media.h"
#include "lyra_png.h"
#include "lyra_sd.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace lyra::gui::internal {

using lyra::i18n::format_count;
using lyra::i18n::format_float;
using lyra::i18n::format_text;
using lyra::i18n::format_text_text;
using lyra::i18n::format_text_u32;
using lyra::i18n::format_u32;
using lyra::i18n::format_u32_text;
using lyra::i18n::format_u32_u32;
using lyra::i18n::format_u64;
using lyra::i18n::tr;

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
extern const AccentPalette kAccentPalettes[8];
constexpr size_t kAccentPaletteCount = 8;

enum class View : uintptr_t {
    Menu, Player, FullscreenArt, TrackInfo, FullscreenInfoArt, Queue,
    Library, LibrarySongs, LibraryArtists, LibraryAlbums, LibraryGenres,
    LibraryYears, AlbumDetail, ArtistDetail, Folders, FolderDetail,
    Playlists, PlaylistDetail, PlaylistCreate, PlaylistAdd, Equalizer,
    EqualizerPresets, Search, Settings, SortingSettings, SortingOptions,
    PlaybackSettings, CrossfadeOptions, SleepTimerOptions, SoundSettings,
    DisplaySettings, SystemSettings, ClockSettings, ClockTimeSettings, ClockDateSettings,
    LanguageOptions, DatabaseStorage,
    About, DebugMenu,
    Licenses, TrackList,
};
enum class LibraryTab : uint8_t { Songs, Artists, Albums, Genres, Years };
enum class ArtistDetailTab : uint8_t { Songs, Albums };
enum class TrackInfoTab : uint8_t { Song, Media };
enum class DebugTab : uint8_t { Info, Debug };
enum class PlaybackScope : uint8_t {
    Single, AllSongs, Group, Playlist, SmartPlaylist, Folder, Search, SavedQueue,
};
enum class RepeatMode : uint8_t { Off, All, Song };
enum class EqualizerPreset : uint8_t {
    Custom, Flat, FullBass, FullTreble, BassAndTreble, Rock, Pop, Jazz, Classic, Count,
};
enum class PowerAction : uintptr_t { Reboot, PowerOff, FactoryReset };
enum class DatabaseAction : uintptr_t { Playlists, Artwork, All };
enum class ArtworkSetting : uintptr_t { SdCache, Size320 };
enum class PlaylistManageAction : uint8_t { DeletePlaylist, RemoveTrack };
enum class DateFormat : uint8_t { DayMonthYear, MonthDayYear, YearMonthDay };
enum class ClockInputKind : uint8_t { Time, Date };

constexpr size_t kSearchPageSizeWithNav = 4;
constexpr size_t kSearchPageSizeWithoutNav = 5;
constexpr size_t kSearchPageCapacity = kSearchPageSizeWithoutNav;

struct NavigationState {
    View view;
    LibraryTab library_tab;
    ArtistDetailTab artist_detail_tab;
    size_t list_page;
    size_t selected_playlist;
    bool selected_playlist_is_smart;
    lyra::media::SmartPlaylistKind selected_smart_playlist;
    size_t selected_group;
    lyra::media::GroupKind selected_group_kind;
    bool playlists_from_library;
    bool playlist_add_mode;
    char track_list_title[lyra::media::kMaxName];
    char folder_path[lyra::media::kMaxPath];
};
constexpr size_t kNavigationDepth = 16;

struct ArtistDetailListContext;
struct AlbumListContext;

extern bool s_dark_mode;
extern uint8_t s_accent_colour;
extern lv_color_t kBackground;
extern lv_color_t kSurface;
extern lv_color_t kSurfaceRaised;
extern lv_color_t kAccent;
extern lv_color_t kAccentDark;
extern lv_color_t kAccentSurface;
extern lv_color_t kTextPrimary;
extern lv_color_t kTextSecondary;
extern lv_color_t kTextMuted;
extern lv_color_t kDivider;
extern lv_color_t kNavSurface;
extern lv_color_t kKeyboardSurface;
extern lv_color_t kArtworkSurface;
extern lv_color_t kPlayerArtSurface;
extern lv_color_t kDangerSurface;
extern const lv_color_t kOverlay;
extern const lv_color_t kTextOnAccent;
extern const lv_color_t kTextOnOverlayPrimary;
extern const lv_color_t kTextOnOverlaySecondary;
constexpr const char *kHeartOutline = "\xE2\x99\xA1";
constexpr const char *kHeartFilled = "\xE2\x99\xA5";

extern lv_obj_t *s_screen;
extern lv_obj_t *s_search_label;
extern lv_obj_t *s_search_results;
extern lv_obj_t *s_search_keyboard;
extern lv_obj_t *s_search_keyboard_toggle;
extern lv_obj_t *s_search_progress_label;
extern lv_obj_t *s_player_progress_bar;
extern lv_obj_t *s_player_progress_touch;
extern lv_obj_t *s_player_elapsed_label;
extern lv_obj_t *s_player_duration_label;
extern lv_obj_t *s_scan_overlay;
extern lv_obj_t *s_scan_count_label;
extern lv_obj_t *s_scan_phase_label;
extern lv_obj_t *s_sort_overlay;
extern lv_obj_t *s_sort_progress_label;
extern lv_obj_t *s_playlist_picker;
extern lv_obj_t *s_volume_popup;
extern lv_obj_t *s_status_volume_label;
extern lv_obj_t *s_status_time_label;
extern lv_obj_t *s_screenshot_button;
extern bool s_screenshot_dragged;
extern lv_point_t s_screenshot_press_point;
extern lv_point_t s_screenshot_button_start;
extern uint32_t s_screenshot_sequence;
extern View s_view;
extern LibraryTab s_library_tab;
extern ArtistDetailTab s_artist_detail_tab;
extern TrackInfoTab s_track_info_tab;
extern size_t s_current_track;
extern size_t s_selected_playlist;
extern bool s_selected_playlist_is_smart;
extern lyra::media::SmartPlaylistKind s_selected_smart_playlist;
extern size_t s_selected_group;
extern lyra::media::GroupKind s_selected_group_kind;
extern size_t s_list_page;
extern size_t s_list_page_count;
extern bool s_shuffle;
extern RepeatMode s_repeat_mode;
extern uint32_t *s_shuffle_order;
extern size_t s_shuffle_count;
extern size_t s_shuffle_cursor;
extern bool s_audio_eof_seen;
extern bool s_player_progress_dragging;
extern int32_t s_player_progress_drag_value;
extern bool s_gapless;
extern bool s_replay_gain;
extern uint8_t s_crossfade_seconds;
extern uint8_t s_brightness_percent;
extern DateFormat s_date_format;
extern bool s_use_24_hour;
extern bool s_dst_enabled;
extern bool s_speaker_output_enabled;
extern EqualizerPreset s_equalizer_preset;
extern int16_t s_equalizer_custom_bands[lyra::audio::kEqualizerBandCount];
extern uint16_t s_sleep_timer_minutes;
extern int64_t s_sleep_timer_deadline_us;
extern bool s_pending_track_advance;
extern int64_t s_pending_track_advance_us;
extern int64_t s_crossfade_fade_in_started_us;
extern int64_t s_crossfade_fade_in_ends_us;
extern int64_t s_crossfade_fade_out_started_us;
extern int64_t s_crossfade_fade_out_ends_us;
extern int s_crossfade_transition_direction;
extern bool s_crossfade_pause_pending;
extern bool s_search_keyboard_visible;
extern bool s_library_keyboard_symbols;
extern bool s_playlists_from_library;
extern bool s_playlist_add_mode;
extern bool s_playlist_manage_mode;
extern bool s_show_nav;
extern bool s_language_setup_pending;
extern char s_search_query[48];
extern char s_submitted_search_query[48];
extern lyra::media::SearchCategory s_search_category;
extern lyra::media::SearchCategory s_submitted_search_category;
extern char s_search_error[48];
extern char s_playlist_name[lyra::media::kMaxName];
extern char s_track_list_title[lyra::media::kMaxName];
extern char s_folder_path[lyra::media::kMaxPath];
extern char s_playback_folder_path[lyra::media::kMaxPath];
extern char s_folder_names[32][lyra::media::kMaxName];
extern size_t s_folder_count;
extern uint32_t s_seen_catalog_generation;
extern uint32_t s_seen_artwork_generation;
extern uint32_t s_seen_duration_generation;
extern uint32_t s_seen_sorting_generation;
extern bool s_seen_scanning;
extern bool s_list_scrolling;
extern uint32_t s_seen_search_generation;
extern NavigationState s_navigation[kNavigationDepth];
extern size_t s_navigation_depth;
extern PlaybackScope s_playback_scope;
extern lyra::media::GroupKind s_playback_group_kind;
extern size_t s_playback_group;
extern size_t s_playback_playlist;
extern lyra::media::SmartPlaylistKind s_playback_smart_playlist;
extern size_t *s_saved_queue;
extern size_t s_saved_queue_count;
extern bool s_saved_queue_pending;
extern uint32_t s_saved_playback_position_ms;
extern bool s_saved_playback_position_pending;
extern bool s_has_active_queue;
extern size_t s_queue_position;
extern bool s_queue_position_valid;
extern bool s_firmware_update_in_progress;
extern ClockInputKind s_clock_input_kind;
extern char s_clock_input_digits[9];
extern size_t s_clock_input_cursor;
extern bool s_clock_input_pm;
extern lyra::media::SortSection s_sort_section;
extern char s_database_status[64];
extern DebugTab s_debug_tab;
extern char s_debug_status[96];
extern PlaylistManageAction s_pending_playlist_manage_action;
extern size_t s_pending_playlist;
extern size_t s_pending_playlist_track;

extern lv_image_dsc_t s_player_art_descriptor;
extern uint8_t *s_player_art_pixels;
extern char s_player_art_album[lyra::media::kMaxName];
extern uint32_t s_player_art_catalog_generation;
extern uint32_t s_player_art_generation;
extern lv_image_dsc_t s_boot_image_descriptor;
extern uint8_t *s_boot_image_pixels;
extern lv_image_dsc_t s_brand_logo_descriptor;
extern uint8_t *s_brand_logo_pixels;
extern lv_anim_t s_marquee_animation;
extern lv_style_t s_marquee_style;
extern bool s_marquee_style_ready;

void apply_theme_palette();
const char *equalizer_preset_name(EqualizerPreset preset);
const int16_t *equalizer_preset_bands(EqualizerPreset preset);
bool equalizer_is_custom();
void apply_equalizer_to_audio();
esp_err_t save_user_settings();
void load_user_settings();
void copy_ui_text(char *destination, size_t capacity, const char *source);
bool append_ui_text(char *destination, size_t capacity, const char *suffix);
NavigationState capture_navigation_state();
void push_navigation_state();
void restore_navigation_state(const NavigationState &state);
void navigate_to(View target);
void navigate_back(View fallback);
int content_bottom();
int content_height(int top);
int library_keyboard_height();
size_t library_page_size();
size_t search_page_size();
lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 0, bool show_border = false);
lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color);
void make_marquee(lv_obj_t *label, int width);
lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 7, bool show_border = false);
void make_equalizer_icon(lv_obj_t *parent, lv_color_t color);
void configure_cover_aware_scroll(lv_obj_t *body);
lv_obj_t *make_scroll_body(int top);
void style_root();
void route_cb(lv_event_t *event);
void add_route(lv_obj_t *object, View target);
View back_view(View view);
void nav_back_cb(lv_event_t *);
void reset_shuffle_queue();
void clear_saved_queue();
void discard_pending_saved_queue();
bool restore_saved_queue_if_pending();
bool resume_saved_track_if_pending();
size_t playback_queue_count();
bool playback_queue_track_at(size_t position, size_t *track_index);
bool playback_queue_position(size_t track_index, size_t *position);
bool build_shuffle_queue();
bool queue_track_at(size_t queue_position, size_t *track_index);
bool current_queue_position(size_t *queue_position);
void apply_replay_gain_to_current_track();
esp_err_t start_track_audio(const lyra::media::Track &track,
                            uint32_t start_position_ms = 0,
                            bool start_paused = false,
                            bool record_play = true);
void play_queue_position(size_t queue_position);
bool move_in_playback_queue(int direction, bool automatic = false);
bool can_move_in_playback_queue(int direction);
bool begin_manual_crossfade(int direction);
bool begin_crossfade_pause();
void request_manual_queue_move(int direction);
void nav_play_cb(lv_event_t *);
void nav_previous_cb(lv_event_t *);
void nav_next_cb(lv_event_t *);
bool restart_current_track();
void make_virtual_nav();
uint8_t display_volume_percent(uint8_t volume_percent);
uint8_t audio_volume_percent_from_display(uint8_t display_percent);
void update_status_volume_label(uint8_t volume_percent);
void update_status_time_label();
void close_volume_popup();
void volume_popup_slider_cb(lv_event_t *event);
void volume_popup_slider_released_cb(lv_event_t *);
void show_volume_popup_cb(lv_event_t *);
void make_status_bar();
lv_obj_t *make_header(const char *title, View back, bool show_back = false, const char *right = nullptr, lv_event_cb_t custom_back = nullptr);
void dismiss_overlay_cb(lv_event_t *event);
void show_notice(const char *title_text, const char *message_text);
lv_obj_t *make_row(lv_obj_t *parent, int y, const char *icon_text, const char *title, const char *subtitle, View target, int height = 54);
void release_boot_image();
bool load_brand_logo();
bool load_player_art(const lyra::media::Track &track);
lv_obj_t *make_artwork(lv_obj_t *parent, int x, int y, int width, int height, const lyra::media::Track &track, int radius, bool preserve_aspect = false);
void play_track_from_row(size_t track_index, bool force_single);
void track_route_cb(lv_event_t *event);
void single_track_route_cb(lv_event_t *event);
void add_track_route(lv_obj_t *row, size_t track_index, bool force_single = false);
void make_song_row(lv_obj_t *parent, int y, size_t track_index, int height = 54,
                   bool force_single = false, bool show_play_count = false);
void add_search_playlist_route(lv_obj_t *row, size_t track_index, size_t playlist_index);
void make_search_playlist_row(lv_obj_t *parent, int y, const lyra::media::SearchResult &result);
void search_album_result_cb(lv_event_t *event);
void search_artist_result_cb(lv_event_t *event);
void make_file_row(lv_obj_t *parent, int y, size_t track_index, int height = 54);
void make_album_art(lv_obj_t *parent, int x, int y, int width, int height, const lyra::media::Track &track, bool preserve_aspect = false);
void page_cb(lv_event_t *event);
void make_page_controls(lv_obj_t *parent, int y, size_t total, size_t page_size = lyra::media::kTrackPageSize);
void open_queue_cb(lv_event_t *);
bool show_now_playing();
void open_library_cb(lv_event_t *);
void open_folders_cb(lv_event_t *);
void render_menu();
void queue_track_cb(lv_event_t *event);
void make_queue_song_row(lv_obj_t *parent, int y, size_t queue_position, bool current);
void render_queue();
View library_section_view(LibraryTab section);
lyra::media::GroupKind group_kind(LibraryTab section);
void open_track_list_cb(lv_event_t *event);
void open_album_detail_cb(lv_event_t *event);
void open_library_playlists_cb(lv_event_t *);
void render_library();
void make_album_row(lv_obj_t *parent, int y, size_t group_index);
void render_library_section(LibraryTab section);
void render_track_list();
void artist_detail_tab_cb(lv_event_t *event);
void make_artist_detail_tab(lv_obj_t *parent, int x, const char *label, ArtistDetailTab tab);
void append_artist_detail_rows(lv_obj_t *body, ArtistDetailListContext *context);
void artist_detail_list_event_cb(lv_event_t *event);
void render_artist_detail();
void append_album_song_rows(lv_obj_t *body, AlbumListContext *context);
void album_list_event_cb(lv_event_t *event);
void render_album_detail();
void folder_cb(lv_event_t *event);
void folder_back_cb(lv_event_t *);
void render_folders(bool detail);
void playlist_cb(lv_event_t *event);
void open_playlist_add_cb(lv_event_t *);
void toggle_playlist_manage_cb(lv_event_t *);
void confirm_playlist_manage_action_cb(lv_event_t *event);
void show_playlist_manage_confirmation(PlaylistManageAction action, size_t playlist_index, size_t track_index = 0);
void delete_playlist_cb(lv_event_t *event);
void remove_playlist_track_cb(lv_event_t *event);
void make_playlist_manage_row(lv_obj_t *parent, int y, size_t playlist_index, const lyra::media::Playlist &playlist);
void make_playlist_track_manage_row(lv_obj_t *parent, int y, size_t track_index);
void render_playlists(bool detail);
void toggle_favorite_cb(lv_event_t *event);
void close_playlist_picker();
void add_current_track_to_playlist_cb(lv_event_t *event);
void show_player_playlist_picker_cb(lv_event_t *);
void format_playback_time(uint32_t milliseconds, bool unknown, char *output, size_t capacity);
bool text_equals_ci(const char *left, const char *right);
const char *current_track_group_name(const lyra::media::Track &track, lyra::media::GroupKind kind);
void open_current_track_group(lyra::media::GroupKind kind);
void open_current_track_group_cb(lv_event_t *event);
void track_info_tab_cb(lv_event_t *event);
lv_obj_t *make_track_info_row(lv_obj_t *parent, int y, const char *label, const char *value, bool clickable = false);
void make_track_info_tab_button(lv_obj_t *parent, int x, const char *label, TrackInfoTab tab);
void format_track_number(uint32_t number, char *output, size_t capacity);
void format_file_size(uint64_t bytes, char *output, size_t capacity);
bool append_debug_text(char *destination, size_t capacity, const char *format, ...);
void format_mac_address(const uint8_t mac[6], char *output, size_t capacity);
const char *debug_partition_type(const esp_partition_t *partition);
bool app_partition_image_size(const esp_partition_t *partition, uint32_t *image_size);
void append_debug_partition(char *info, size_t capacity, const esp_partition_t *partition, const esp_partition_t *running);
void build_debug_info(char *info, size_t capacity);
bool dump_debug_info_to_sd();
void debug_tab_cb(lv_event_t *event);
void maximum_volume_slider_cb(lv_event_t *event);
void maximum_volume_slider_released_cb(lv_event_t *);
void make_maximum_volume_control(lv_obj_t *parent, int y);
void write_bmp_u16(uint8_t *destination, uint16_t value);
void write_bmp_u32(uint8_t *destination, uint32_t value);
bool save_screenshot_bmp(const uint8_t *pixels, uint32_t stride, uint32_t width, uint32_t height, lv_color_format_t color_format);
void create_screenshot_button(int32_t x, int32_t y);
void take_screenshot_cb(lv_event_t *);
void screenshot_button_pressed_cb(lv_event_t *);
void screenshot_button_pressing_cb(lv_event_t *);
void screenshot_button_clicked_cb(lv_event_t *);
void toggle_screenshot_button_cb(lv_event_t *);
void confirm_clear_nvs_cb(lv_event_t *event);
void show_clear_nvs_confirmation();
void firmware_update_failure_async_cb(void *context);
void post_firmware_update_failure(const char *message);
bool firmware_update_file_size(FILE *file, size_t *size);
bool firmware_update_media_idle();
void firmware_update_task(void *);
void start_firmware_update();
void confirm_firmware_update_cb(lv_event_t *event);
void show_firmware_update_confirmation();
void firmware_update_cb(lv_event_t *);
void run_force_boot_test(uint8_t ota_slot);
void confirm_force_boot_test_cb(lv_event_t *event);
void show_force_boot_confirmation(uint8_t ota_slot);
void force_boot_test_cb(lv_event_t *event);
void dump_debug_info_cb(lv_event_t *);
void render_debug_menu();
void format_file_date(uint64_t modified_time, char *output, size_t capacity);
void format_track_format(const lyra::media::Track &track, char *output, size_t capacity);
const char *track_encoding(const lyra::media::Track &track);
void render_track_info();
void render_fullscreen_info_art();
void update_player_progress_touch_value(lv_obj_t *touch);
void update_player_progress_preview();
void player_progress_touch_cb(lv_event_t *event);
lv_obj_t *make_player_progress_touch(lv_obj_t *parent, int x, int y, int width, int height);
void update_player_progress();
void render_player(bool fullscreen);
void update_equalizer_gain_label(lv_obj_t *label, int16_t gain_tenths_db, lv_color_t color);
void equalizer_slider_cb(lv_event_t *event);
void equalizer_slider_released_cb(lv_event_t *);
void equalizer_preset_menu_cb(lv_event_t *);
void equalizer_preset_option_cb(lv_event_t *event);
void make_equalizer_preset_option(lv_obj_t *parent, int y, EqualizerPreset preset);
void render_equalizer_presets();
void render_equalizer();
void update_search_label();
const char *search_category_name(lyra::media::SearchCategory category);
const char *search_empty_message(lyra::media::SearchCategory category);
void populate_search_results();
void search_category_cb(lv_event_t *event);
void keyboard_cb(lv_event_t *event);
void toggle_search_keyboard_cb(lv_event_t *);
void make_keyboard_row(lv_obj_t *parent, int y, const char *keys[], size_t count, int inset = 3, int height = 38);
void make_library_keyboard(lv_obj_t *keyboard, const char *action);
void render_search();
void render_playlist_create();
void add_playlist_track_cb(lv_event_t *event);
void render_playlist_add();
void toggle_bool_cb(lv_event_t *event);
void make_setting_toggle(lv_obj_t *parent, int y, const char *title, const char *subtitle, bool *value);
void accent_colour_cb(lv_event_t *event);
void make_accent_selector(lv_obj_t *parent, int y);
void artwork_setting_cb(lv_event_t *event);
void make_artwork_setting_toggle(lv_obj_t *parent, int y, const char *title, const char *subtitle, bool value, ArtworkSetting setting);
void volume_slider_cb(lv_event_t *event);
void volume_slider_released_cb(lv_event_t *);
void make_volume_control(lv_obj_t *parent, int y);
void brightness_slider_cb(lv_event_t *event);
void brightness_slider_released_cb(lv_event_t *);
void make_brightness_control(lv_obj_t *parent, int y);
const char *sort_section_name(lyra::media::SortSection section);
const char *sort_field_name(lyra::media::SortField field);
const char *sort_direction_name(lyra::media::SortDirection direction);
void sorting_section_cb(lv_event_t *event);
void sorting_option_cb(lv_event_t *event);
void render_sorting_settings();
void make_sorting_option_row(lv_obj_t *parent, int y, lyra::media::SortField field, lyra::media::SortDirection direction, const lyra::media::SortSetting &selected);
void render_sorting_options();
void render_language_options();
void make_playback_option_row(lv_obj_t *parent, int y, const char *title, bool active, lv_event_cb_t callback, uintptr_t value);
void crossfade_option_cb(lv_event_t *event);
void sleep_timer_option_cb(lv_event_t *event);
void render_crossfade_options();
void render_sleep_timer_options();
void render_settings_menu();
void scan_library_cb(lv_event_t *);
void save_active_queue_snapshot(const lyra::audio::Status &audio_status);
void power_action_task(void *context);
void confirm_power_action_cb(lv_event_t *event);
void cancel_power_action_cb(lv_event_t *event);
void show_power_confirmation(PowerAction action);
void reboot_cb(lv_event_t *);
void power_off_cb(lv_event_t *);
void factory_reset_cb(lv_event_t *);
void confirm_database_action_cb(lv_event_t *event);
void show_database_confirmation(DatabaseAction action);
void database_action_cb(lv_event_t *event);
void render_database_storage();
void render_settings_page(View view);
void render_clock_settings();
void clock_settings_cb(lv_event_t *event);
void render_clock_time_settings();
void render_clock_date_settings();
void clock_time_settings_cb(lv_event_t *event);
void clock_date_settings_cb(lv_event_t *event);
void clock_date_format_cb(lv_event_t *event);
void clock_time_format_cb(lv_event_t *event);
void clock_dst_cb(lv_event_t *event);
void clock_input_digit_cb(lv_event_t *event);
void clock_input_cursor_cb(lv_event_t *event);
void clock_input_meridiem_cb(lv_event_t *event);
void clock_input_save_cb(lv_event_t *event);
void clock_input_cancel_cb(lv_event_t *event);
void format_clock_time(const lyra::clock::DateTime &value, char *output, size_t capacity);
void format_clock_date_time(const lyra::clock::DateTime &value, char *output, size_t capacity);
void render_about();
void render_licenses();
void render(View view);
bool automatic_track_advance_available();
void begin_crossfade_fade_in();
bool advance_after_track_end();
void update_crossfade_gain(const lyra::audio::Status &audio_status, int64_t now_us);
void crossfade_poll_cb(lv_timer_t *);
void player_progress_poll_cb(lv_timer_t *);
void show_sorting_overlay(const lyra::media::Status &status);
void update_sorting_overlay(const lyra::media::Status &status);
void catalog_poll_cb(lv_timer_t *);

} // namespace lyra::gui::internal
