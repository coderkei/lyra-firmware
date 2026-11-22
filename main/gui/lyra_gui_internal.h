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
#include "lyra_wifi.h"
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
// The mini player that sits at the bottom of browsing pages.
constexpr int kNavHeight = 60;
constexpr int kKeyboardHeightWithNav = 216;
constexpr int kKeyboardHeightWithoutNav = 260;
constexpr int kKeyboardKeyHeightWithNav = 46;
constexpr int kKeyboardKeyHeightWithoutNav = 50;
constexpr int kKeyboardRowGap = 3;
constexpr int kKeyboardBottomPadding = 4;
constexpr uint16_t kBootImageWidth = 216;
constexpr uint16_t kBootImageHeight = 59;
constexpr uint32_t kBootBackgroundRgb = 0x080C12;
constexpr size_t kDebugInfoCapacity = 4096;
constexpr uint32_t kNonGaplessTrackPauseMs = 500;

extern const uint8_t boot_png_start[] asm("_binary_boot_png_start");
extern const uint8_t boot_png_end[] asm("_binary_boot_png_end");
extern const uint8_t boot_light_png_start[] asm("_binary_boot_light_png_start");
extern const uint8_t boot_light_png_end[] asm("_binary_boot_light_png_end");

// One accent colour as shown on dark and on light backgrounds. Pressed and
// tinted variants are derived from it in apply_theme_palette().
struct AccentPalette {
    uint32_t dark_rgb;
    uint32_t light_rgb;
};
constexpr size_t kAccentPaletteCount = 6;
extern const AccentPalette kAccentPalettes[kAccentPaletteCount];

enum class View : uintptr_t {
    Menu, Player, TrackInfo, FullscreenInfoArt, Queue, Apps, Photos, Ebook,
    Library, LibrarySongs, LibraryArtists, LibraryAlbums, LibraryGenres,
    LibraryYears, InternetRadio, RadioAdd, RadioFiles, RadioPlaying,
    AlbumDetail, ArtistDetail, Folders, FolderDetail, FileViewer, FolderPlaylist, FileNameInput,
    Playlists, PlaylistDetail, PlaylistCreate, PlaylistAdd, Equalizer,
    EqualizerPresets, Search, Settings, SortingSettings, SortingOptions,
    PlaybackSettings, CrossfadeOptions, SleepTimerOptions, SoundSettings,
    AppearanceSettings, LibrarySettings, SystemSettings, ClockSettings,
    ClockTimeSettings, ClockDateSettings,
    LanguageOptions, DatabaseStorage, WifiSettings, WifiPassword, NetworkTests,
    About, DebugMenu, PageJump,
    Licenses, TrackList,
    SetupWelcome, SetupLanguage, SetupClock, SetupAppearance, SetupScan, SetupComplete,
};
enum class LibraryTab : uint8_t { Songs, Artists, Albums, Genres, Years };
enum class ArtistDetailTab : uint8_t { Songs, Albums };
enum class TrackInfoTab : uint8_t { Song, Media };
enum class DebugTab : uint8_t { Info, Debug };
enum class PlaybackScope : uint8_t {
    Single, AllSongs, Group, Playlist, Search, SavedQueue,
};
enum class RepeatMode : uint8_t { Off, All, Song };
enum class EqualizerPreset : uint8_t {
    Custom, Flat, FullBass, FullTreble, BassAndTreble, Rock, Pop, Jazz, Classic, Count,
};
enum class PowerAction : uintptr_t { Reboot, PowerOff, FactoryReset, UsbTransfer };
enum class DatabaseAction : uintptr_t { Playlists, Artwork, All };
enum class ArtworkSetting : uintptr_t { SdCache, Size320 };
enum class PlaylistManageAction : uint8_t { DeletePlaylist, RemoveTrack };
enum class QueueInsertMode : uint8_t { PlayNext, AddToEnd };
enum class DateFormat : uint8_t { DayMonthYear, MonthDayYear, YearMonthDay };
enum class ClockInputKind : uint8_t { Time, Date };


struct NavigationState {
    View view;
    LibraryTab library_tab;
    ArtistDetailTab artist_detail_tab;
    size_t list_page;
    int32_t scroll_y;
    size_t selected_playlist;
    bool selected_playlist_is_smart;
    lyra::media::SmartPlaylistKind selected_smart_playlist;
    size_t selected_group;
    lyra::media::GroupKind selected_group_kind;
    bool playlists_from_library;
    bool playlist_add_mode;
    char track_list_title[lyra::media::kMaxName];
    char folder_path[lyra::media::kMaxPath];
    char folder_playlist_path[lyra::media::kMaxPath];
};
constexpr size_t kNavigationDepth = 16;

struct ArtistDetailListContext;
struct AlbumListContext;

using ClockPosition = lyra::gui_settings::ClockPosition;
extern bool s_dark_mode;
extern uint8_t s_accent_colour;
// Tints the backgrounds and surfaces with the accent colour.
extern bool s_dynamic_tint;
extern ClockPosition s_clock_position;
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
extern lv_color_t kDangerSurface;
// Solid destructive fill for buttons with kTextOnAccent labels; kDangerSurface
// is the tinted row background that keeps normal text colours.
extern lv_color_t kDanger;
extern const lv_color_t kOverlay;
extern const lv_color_t kTextOnAccent;
constexpr const char *kHeartOutline = "\xE2\x99\xA1";
constexpr const char *kHeartFilled = "\xE2\x99\xA5";

extern lv_obj_t *s_screen;
extern lv_obj_t *s_scroll_body;
extern int32_t s_pending_scroll_restore_y;
// A list row to reveal on the next render (page jump, queue); SIZE_MAX if none.
extern size_t s_pending_list_position;
// Changes whenever navigation moves to other content, even within the same
// view (another playlist, folder or album). render() keeps the scroll
// position only for re-renders of unchanged content.
extern uint32_t s_navigation_serial;
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
extern lv_obj_t *s_queue_add_popup;
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
extern bool s_quick_seek;
extern uint8_t s_crossfade_seconds;
extern uint8_t s_brightness_percent;
extern DateFormat s_date_format;
extern bool s_use_24_hour;
extern bool s_dst_enabled;
extern bool s_speaker_output_enabled;
extern bool s_t9_input;
extern bool s_list_artwork;
extern bool s_automatic_time;
extern int16_t s_utc_offset_minutes;
// Passes the time settings to network time sync.
void apply_time_sync_settings();
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
extern bool s_library_keyboard_accents;
extern lyra::gui_settings::KeyboardLayout s_library_keyboard_layout;
extern bool s_playlists_from_library;
extern bool s_playlist_add_mode;
extern bool s_playlist_manage_mode;
extern bool s_playlist_create_from_queue;
extern bool s_queue_add_mode;
extern bool s_queue_edit_mode;
// Settings > Appearance > Mini player: playback controls under browsing pages.
extern bool s_show_nav;
extern bool s_setup_pending;
extern bool s_setup_scan_active;
extern char s_search_query[48];
extern char s_submitted_search_query[48];
extern lyra::media::SearchCategory s_search_category;
extern lyra::media::SearchCategory s_submitted_search_category;
extern char s_search_error[48];
extern char s_playlist_name[lyra::media::kMaxName];
extern char s_radio_url[lyra::media::kMaxPath];
extern char s_track_list_title[lyra::media::kMaxName];
extern char s_folder_path[lyra::media::kMaxPath];
extern uint32_t s_seen_catalog_generation;
extern uint32_t s_seen_artwork_generation;
extern uint32_t s_seen_duration_generation;
extern uint32_t s_seen_sorting_generation;
extern bool s_seen_scanning;
extern uint32_t s_seen_search_generation;
extern NavigationState s_navigation[kNavigationDepth];
extern size_t s_navigation_depth;
extern PlaybackScope s_playback_scope;
extern lyra::media::GroupKind s_playback_group_kind;
extern size_t s_playback_group;
extern size_t s_playback_playlist;
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
extern char s_wifi_password[64];
extern char s_wifi_selected_ssid[33];
extern bool s_wifi_password_visible;
extern PlaylistManageAction s_pending_playlist_manage_action;
extern size_t s_pending_playlist;
extern size_t s_pending_playlist_track;

extern lv_image_dsc_t s_player_art_descriptor;
extern uint8_t *s_player_art_pixels;
extern char s_player_art_path[lyra::media::kMaxPath];
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
// The accent colour of the given palette index for the current theme.
lv_color_t accent_color(size_t index);
void render_setup_welcome();
void render_setup_language();
void render_setup_clock();
void render_setup_appearance();
void render_setup_scan();
void render_setup_complete();
void stop_setup_welcome_cycle();
void setup_scan_now_cb(lv_event_t *event);
void begin_clock_input(ClockInputKind kind);
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
void restore_navigation_state(const NavigationState &state, size_t list_page);
void navigate_to(View target);
void navigate_back(View fallback);
int content_bottom();
int content_height(int top);
int library_keyboard_height();
size_t library_page_size();
lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 0, bool show_border = false);
lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color);
// Vector icons drawn on a 20 x 20 grid and scaled.
enum class Icon : uint8_t {
    Music, NowPlaying, Search, Queue, Apps, Folder, Equalizer, Settings,
    Playback, Sound, Display, Sorting, System, About, Theme, Language, Clock,
    Wifi, SdCard, Storage, Sleep, Crossfade, Radio, Album, Artist, Genre,
    Calendar, Playlist, Favorite, Recent, Added, Timer,
    Book, Photo, Network, More, Brightness,
};
// Marks icon objects so pressed-row feedback recolours them with the text.
constexpr lv_obj_flag_t kIconFlag = LV_OBJ_FLAG_USER_3;
lv_obj_t *make_icon(lv_obj_t *parent, Icon icon, lv_color_t color, int size = 20);

// ---- LyraOS design language ---------------------------------------------------
// Flat dark or light pages, rounded cards, two-tone tile badges and one
// accent colour. Every screen builds from these pieces.
constexpr int kPageInset = 10;
constexpr int kCardRadius = 14;
constexpr int kRowRadius = 12;
constexpr int kHeaderHeight = 44;
constexpr int kContentTop = kStatusHeight + kHeaderHeight;
// 18 px headings and tile names; display figures (the control-centre clock).
const lv_font_t *title_font();
const lv_font_t *display_font(bool large = false);
// A plain rounded card in the surface colour.
lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height);
// Two overlapping rounded squares in one hue with a white icon on the front
// one. size is the side of one square; the badge is size * 4 / 3 wide.
void make_tile_badge(lv_obj_t *parent, int x, int y, int size, Icon icon, uint32_t hue);
// A home-style tile: badge above a centred name. Returns the button.
lv_obj_t *make_tile(lv_obj_t *parent, int x, int y, int width, int height,
                    Icon icon, uint32_t hue, const char *label);
// A round, borderless icon button (header actions, transport).
lv_obj_t *make_icon_button(lv_obj_t *parent, int x, int y, int size, Icon icon,
                           lv_color_t color);
lv_obj_t *make_symbol_button(lv_obj_t *parent, int x, int y, int size, const char *symbol,
                             lv_color_t color);
// An on/off switch drawn inside a row; set_switch() restyles it in place.
lv_obj_t *make_switch(lv_obj_t *parent, bool checked);
void set_switch(lv_obj_t *track, bool checked);
// A card with a title and a row of pill choices. Each pill's click callback
// receives its index as user data. Returns the card height used.
int make_choice_card(lv_obj_t *parent, int y, const char *title, const char *const *labels,
                     size_t count, size_t selected, lv_event_cb_t callback);
// Small muted caption above a group of cards.
void make_section_label(lv_obj_t *parent, int y, const char *text);
// Accent indicator, round knob and a slim rail; keeps swipes off the page.
void style_slider(lv_obj_t *slider);
// A bottom sheet over the page. *slot holds the sheet until it closes.
// content_out receives the area below the title for the caller's rows.
lv_obj_t *make_sheet(lv_obj_t **slot, const char *title, int content_height,
                     lv_obj_t **content_out);
void close_sheet(lv_obj_t **slot);
// One full-width action row inside a sheet; the callback's user data is value.
lv_obj_t *make_sheet_row(lv_obj_t *content, int y, Icon icon, const char *text,
                         lv_event_cb_t callback, uintptr_t value);

// ---- Shell: mini player, control centre and gestures --------------------------
// True when the current page shows the mini player under its content.
bool mini_player_visible();
void make_mini_player();
void show_control_centre();
void close_control_centre();
// Lets a downward drag on the status bar pull the control centre open.
void control_centre_pull_events(lv_obj_t *status_bar);
// Swipe right goes back, swipe left opens Now Playing, swipe down on the
// status bar opens the control centre. Installed once on the screen.
void install_gesture_navigation();
// Keeps an object (slider, seek bar, keyboard) from starting page gestures.
void block_page_gestures(lv_obj_t *object);
// Brings the status bar above full-bleed artwork on Now Playing.
void raise_status_bar();
// The player's mode button cycles in order, repeat all, repeat one, shuffle.
void cycle_play_mode();
// Keeps a play/pause glyph in step with playback until the label is deleted.
void track_play_icon(lv_obj_t *label);
void set_label_text_if_changed(lv_obj_t *label, const char *text);
// Truncate rests as "…" and plays once when its row is long-pressed. Focus
// is for the one thing being looked at (now playing, page title): a calm
// scroll that stops after a few passes. Continuous never stops and is only
// for text-entry fields, where the end of the text must stay reachable.
// Set the label's font first: the "…" box is sized to one line of it.
enum class MarqueeMode : uint8_t { Truncate, Focus, Continuous };
void make_marquee(lv_obj_t *label, int width, MarqueeMode mode = MarqueeMode::Truncate);
// For rows whose long press already does something else.
void disable_marquee_reveal(lv_obj_t *row);
lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color, int radius = 7, bool show_border = false);
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
bool copy_queue_tracks(size_t *indices, size_t count);
bool current_queue_position(size_t *queue_position);
bool add_tracks_to_queue(const size_t *track_indices, size_t count, QueueInsertMode mode);
bool move_queue_entry(size_t queue_position, int direction);
bool remove_queue_entry(size_t queue_position);
void clear_active_queue();
void apply_replay_gain_to_current_track();
esp_err_t start_track_audio(const lyra::media::Track &track,
                            uint32_t start_position_ms = 0,
                            bool start_paused = false,
                            bool record_play = true);
void play_queue_position(size_t queue_position, bool navigate_to_player = true);
bool move_in_playback_queue(int direction, bool automatic = false);
bool can_move_in_playback_queue(int direction);
bool begin_manual_crossfade(int direction);
void prepare_automatic_successor(const lyra::audio::Status &audio_status);
void sync_audio_handoff(const lyra::audio::Status &audio_status);
bool begin_crossfade_pause();
void request_manual_queue_move(int direction);
void nav_play_cb(lv_event_t *);
// Shows Pause on the nav bar while anything is playing, Play otherwise.
void update_nav_play_icon();
void nav_previous_cb(lv_event_t *);
void nav_next_cb(lv_event_t *);
bool restart_current_track();
// Explains why a track failed (memory, missing file, format, unreadable).
void show_playback_error(esp_err_t error);
uint8_t display_volume_percent(uint8_t volume_percent);
uint8_t audio_volume_percent_from_display(uint8_t display_percent);
void update_status_volume_label(uint8_t volume_percent);
void update_status_time_label();
void make_status_bar();
constexpr size_t kWifiSignalBarCount = 4;
int wifi_signal_bar_count(int8_t rssi);
void make_wifi_signal_bars(lv_obj_t *parent, int x, int y, int signal_bars,
                           lv_color_t active_color, lv_color_t inactive_color,
                           lv_obj_t **bars_out = nullptr);
// action_width reserves room on the right for buttons the caller adds.
lv_obj_t *make_header(const char *title, View back, bool show_back = false, const char *right = nullptr,
                      lv_event_cb_t custom_back = nullptr, int action_width = 0);
void make_queue_add_mode_toggle(lv_obj_t *parent, int x, int y, int width = 40, int height = 36);
void dismiss_overlay_cb(lv_event_t *event);
void show_notice(const char *title_text, const char *message_text);
void show_toast(const char *text);
lv_obj_t *make_row(lv_obj_t *parent, int y, const char *icon_text, const char *title, const char *subtitle, View target, int height = 54);
lv_obj_t *make_row(lv_obj_t *parent, int y, Icon icon, const char *title, const char *subtitle, View target, int height = 54);
void release_boot_image();
bool load_brand_logo();
bool load_player_art(const lyra::media::Track &track);
lv_obj_t *make_artwork(lv_obj_t *parent, int x, int y, int width, int height, const lyra::media::Track &track, int radius, bool preserve_aspect = false);
lv_obj_t *make_artwork_contents(lv_obj_t *art, int width, int height, const lyra::media::Track &track, int radius, bool preserve_aspect = false);
void make_artwork_placeholder(lv_obj_t *art, int width, int height);
void play_track_from_row(size_t track_index, bool force_single);
void track_route_cb(lv_event_t *event);
void single_track_route_cb(lv_event_t *event);
void add_track_route(lv_obj_t *row, size_t track_index, bool force_single = false);
void show_queue_add_picker_for_track(size_t track_index);
void show_queue_add_picker_for_album(size_t group_index);
void show_queue_add_picker_for_folder(const char *folder_path);
void make_song_row(lv_obj_t *parent, int y, size_t track_index, int height = 54,
                   bool force_single = false, bool show_play_count = false);
uintptr_t search_playlist_payload(size_t track_index, size_t playlist_index);
void play_search_playlist_result(uintptr_t payload);
void open_search_album_result(size_t group_index);
void open_search_artist_result(size_t group_index);
void render_file_viewer();
void open_document_file(const char *path, lyra::media::FileKind kind, View back_view);
View file_viewer_back_view();
extern char s_folder_playlist_path[lyra::media::kMaxPath];
void open_folder_playlist(const char *path);
void render_folder_playlist();
bool play_file_queue(const size_t *indices, size_t count, size_t position);
void make_document_row(lv_obj_t *parent, int y, const lyra::media::FolderFile &file,
                       View back_view = View::Folders);
void make_album_art(lv_obj_t *parent, int x, int y, int width, int height, const lyra::media::Track &track, bool preserve_aspect = false);
void page_cb(lv_event_t *event);
void make_page_controls(lv_obj_t *parent, int y, size_t total, size_t page_size = lyra::media::kTrackPageSize);
void open_page_jump(size_t total, size_t page_size, size_t page_count);
// Strip marks for a sorted section: initials, years, minutes or track numbers
// to match the sort. Each entry's initial[] holds its short strip label.
bool sort_jump_locations(lyra::media::SortSection section, size_t item_count,
                         lyra::media::SortLocation *locations, size_t capacity,
                         size_t *count, bool *pending);
// Full label for a jump point (date, duration, ...) as the page-jump screen shows it.
void format_jump_label(const lyra::media::SortLocation &entry, char *output, size_t capacity);

// Recycling list for large collections. A fixed pool of row slots is rebound
// as the list scrolls, and row text is prefetched off the GUI thread, so the
// object count and per-frame work match a single page regardless of length.
struct VirtualRow {
    size_t payload;  // Track, group or queue position, chosen by the list.
    char title[lyra::media::kMaxName];
    char subtitle[lyra::media::kMaxName + 32];
};
// Runs on the prefetch task (and once on the GUI thread for the first screen).
using VirtualFetch = bool (*)(size_t position, VirtualRow *row);
struct VirtualRowWidgets {
    lv_obj_t *button;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    unsigned variant;
    lv_obj_t *number = nullptr;
    lv_obj_t *art = nullptr;  // Optional row artwork, owned by the builder.
};
// Builds the widgets on first use and rebinds them to subsequent rows.
using VirtualBuild = void (*)(lv_obj_t *slot, VirtualRowWidgets &widgets, size_t position,
                              const VirtualRow &row, int width);
using VirtualWindow = void (*)(const size_t *payloads, size_t count);
using VirtualRefresh = void (*)(VirtualRowWidgets &widgets);
struct VirtualListSpec {
    size_t count;
    int top;
    int pitch;
    int row_height;
    VirtualFetch fetch;
    VirtualBuild build;
    bool letter_strip;                       // Jump strip (A-Z, years, ...) and bubble.
    lyra::media::SortSection letter_section;
    bool jump_button;                        // Opens the page-jump screen.
    // Optional row artwork. window receives the payloads of the rows on
    // screen, top to bottom, then of the rows just ahead of the scroll; it is
    // called whenever they change. When refresh_stamp() changes, refresh runs
    // for every shown row. release runs when the list goes away.
    VirtualWindow window = nullptr;
    uint32_t (*refresh_stamp)() = nullptr;
    VirtualRefresh refresh = nullptr;
    void (*release)() = nullptr;
    // Tallest the body can grow without a re-render (for example when a
    // keyboard hides), so enough row slots exist. 0 uses the current height.
    int max_viewport = 0;
};
void make_virtual_list(lv_obj_t *body, const VirtualListSpec &spec);
// Stops the list on body before its rows are removed without deleting body.
void destroy_virtual_list(lv_obj_t *body);
// Row thumbnails (Settings > Display > Album art in lists). A row reserves
// kRowThumbnailSize pixels at its left edge; text starts at
// kRowThumbnailTextX. make_row_thumbnail returns the image to bind, or
// nullptr if no memory was available (the placeholder still shows).
constexpr int kRowThumbnailSize = lyra::media::kThumbnailSize;
constexpr int kRowThumbnailInset = 6;
constexpr int kRowThumbnailTextX = kRowThumbnailInset * 2 + kRowThumbnailSize;
lv_obj_t *make_row_thumbnail(lv_obj_t *row, int x, int y);
void bind_row_thumbnail(lv_obj_t *image, lyra::media::ThumbnailSource source, size_t index);
void request_row_thumbnails(lyra::media::ThumbnailSource source, const size_t *indices,
                            size_t count);
// Installs the thumbnail hooks when the setting is on; otherwise does nothing.
void enable_row_thumbnails(VirtualListSpec &spec, VirtualWindow window, VirtualRefresh refresh);
// Scrolling track rows. Set s_track_row_mode before make_virtual_list():
// Library plays in the current view's scope, Single plays only the tapped
// song (search), PlaylistManage shows a remove button instead.
enum class TrackRowMode : uint8_t { Library, Single, PlaylistManage };
extern TrackRowMode s_track_row_mode;
void build_track_row(lv_obj_t *slot, VirtualRowWidgets &widgets, size_t position,
                     const VirtualRow &data, int width);
void refresh_track_row_thumbnail(VirtualRowWidgets &widgets);
void request_track_row_thumbnails(const size_t *tracks, size_t count);
void rebind_virtual_row(VirtualRowWidgets &widgets, const VirtualRow &row);
void render_page_jump();
void open_queue_cb(lv_event_t *);
bool show_now_playing();
void open_library_cb(lv_event_t *);
void open_folders_cb(lv_event_t *);
// Opens a library destination by the user data of the tile that was tapped.
void open_library_destination(uintptr_t destination);
void render_menu();
// Keeps the mini player and the control centre in step with playback.
// Called from the player poll.
void update_menu_playback();
void render_apps();
void render_app_files();
void release_app_file_scan(View view);
void stop_apps_runtime_timers(View next_view);
void render_queue();
View library_section_view(LibraryTab section);
lyra::media::GroupKind group_kind(LibraryTab section);
void open_album_detail_cb(lv_event_t *event);
void open_library_playlists_cb(lv_event_t *);
// The tiles of the Music page, in their on-screen order.
enum class LibraryDestination : uintptr_t {
    AllSongs, Albums, Artists, Genres, Years, Favorites, Playlists,
    RecentlyPlayed, RecentlyAdded, MostPlayed, Folders, Count,
};
void render_library();
void make_album_row(lv_obj_t *parent, int y, size_t group_index);
void render_library_section(LibraryTab section);
void render_radio_stations();
void render_radio_add();
void render_radio_files();
void render_radio_playing();
// True for an internet radio stream address (http/https) in audio::Status::path.
bool is_radio_stream(const char *path);
// The station shown for a playing stream: the station last started, else the
// saved station with this URL, else the URL itself.
const char *radio_station_name(const char *url);
void save_radio_address();
void leave_radio_add();
void stop_radio_status_timer();
void render_track_list();
void artist_detail_tab_cb(lv_event_t *event);
void make_artist_detail_tab(lv_obj_t *parent, int x, const char *label, ArtistDetailTab tab);
void append_artist_detail_rows(lv_obj_t *body, ArtistDetailListContext *context);
void artist_detail_list_event_cb(lv_event_t *event);
void render_artist_detail();
void append_album_song_rows(lv_obj_t *body, AlbumListContext *context);
void album_list_event_cb(lv_event_t *event);
void render_album_detail();
void folder_back_cb(lv_event_t *);
void render_folders(bool detail);
void invalidate_file_listing();
char *file_name_input_text();
size_t file_name_input_capacity();
bool file_name_input_creates_folder();
void submit_file_name_input();
void render_file_name_input();
void playlist_cb(lv_event_t *event);
void open_playlist_add_cb(lv_event_t *);
void toggle_playlist_manage_cb(lv_event_t *);
void confirm_playlist_manage_action_cb(lv_event_t *event);
void show_playlist_manage_confirmation(PlaylistManageAction action, size_t playlist_index, size_t track_index = 0);
void delete_playlist_cb(lv_event_t *event);
void make_playlist_manage_row(lv_obj_t *parent, int y, size_t playlist_index, const lyra::media::Playlist &playlist);
void render_playlists(bool detail);
void toggle_favorite_cb(lv_event_t *event);
void close_playlist_picker();
void add_current_track_to_playlist_cb(lv_event_t *event);
void show_player_playlist_picker_cb(lv_event_t *);
void reset_player_action_ui();
// The Now Playing "more" sheet: queue, playlist, album, artist, details,
// equaliser, speed, A-B repeat, bookmarks and sleep timer.
void show_player_more_menu();
// While A-B repeat is on, draws its A / B / end controls and returns true.
bool make_player_ab_bar(lv_obj_t *parent, int y);
bool is_player_ab_mode_active();
bool update_player_ab_loop(const lyra::audio::Status &audio_status);
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
void render_wifi_settings();
void render_wifi_password();
void render_network_tests();
char *wifi_password_input_text();
size_t wifi_password_input_capacity();
bool wifi_password_is_visible();
void submit_wifi_password();
void format_file_date(uint64_t modified_time, char *output, size_t capacity);
void format_track_format(const lyra::media::Track &track, char *output, size_t capacity);
const char *track_encoding(const lyra::media::Track &track);
void render_track_info();
void render_fullscreen_info_art();
void update_player_progress_touch_value(lv_obj_t *touch);
void update_player_progress_preview();
void player_progress_touch_cb(lv_event_t *event);
lv_obj_t *make_player_progress_touch(lv_obj_t *parent, int x, int y, int width, int height);
void update_player_progress_seek_preview(uint32_t position_ms, uint32_t duration_ms, bool active);
void update_player_progress();
void refresh_player_artwork();
void reset_player_lyrics_state();
void render_player();
void update_equalizer_gain_label(lv_obj_t *label, int16_t gain_tenths_db, lv_color_t color);
void equalizer_slider_cb(lv_event_t *event);
void equalizer_slider_released_cb(lv_event_t *);
void equalizer_preset_option_cb(lv_event_t *event);
void make_equalizer_preset_option(lv_obj_t *parent, int y, EqualizerPreset preset);
void render_equalizer_presets();
void render_equalizer();
void update_search_label();
const char *search_category_name(lyra::media::SearchCategory category);
const char *search_empty_message(lyra::media::SearchCategory category);
void populate_search_results();
// Builds a single-line text field with a blinking caret inside input and
// returns its label, which becomes s_search_label.
lv_obj_t *make_text_field(lv_obj_t *input, int x, int width);
void search_category_cb(lv_event_t *event);
void toggle_search_keyboard_cb(lv_event_t *);
void keyboard_selector_cb(lv_event_t *);
void commit_keyboard_preedit();
void close_keyboard_picker();
void make_library_keyboard(lv_obj_t *keyboard, const char *action);
void render_search();
void render_playlist_create();
void add_playlist_track_cb(lv_event_t *event);
void add_playlist_track(size_t track_index);
void render_playlist_add();
void toggle_bool_cb(lv_event_t *event);
void make_setting_toggle(lv_obj_t *parent, int y, const char *title, const char *subtitle,
                         bool *value, bool enabled = true);
void accent_colour_cb(lv_event_t *event);
void make_accent_selector(lv_obj_t *parent, int y);
// The Dark / Light pills. Returns the card height.
int make_theme_choices(lv_obj_t *parent, int y);
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
void update_crossfade_gain(int64_t now_us);
void crossfade_poll_cb(lv_timer_t *);
void player_progress_poll_cb(lv_timer_t *);
void show_sorting_overlay(const lyra::media::Status &status);
void update_sorting_overlay(const lyra::media::Status &status);
void catalog_poll_cb(lv_timer_t *);

} // namespace lyra::gui::internal
