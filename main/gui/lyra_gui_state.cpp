/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui_internal.h"

namespace lyra::gui::internal {

extern const AccentPalette kAccentPalettes[] = {
    {0x38BDF8, 0x075985, 0x123A52, 0x2563EB, 0x1D4ED8, 0xDBEAFE}, // Blue
    {0xC084FC, 0x7E22CE, 0x3B214F, 0x7C3AED, 0x6D28D9, 0xEDE9FE}, // Purple
    {0x2DD4BF, 0x0F766E, 0x123F42, 0x0D9488, 0x0F766E, 0xCCFBF1}, // Teal
    {0x4ADE80, 0x15803D, 0x163D2A, 0x16A34A, 0x15803D, 0xDCFCE7}, // Green
    {0xFBBF24, 0xB45309, 0x493512, 0xD97706, 0xB45309, 0xFEF3C7}, // Amber
    {0xFB923C, 0xC2410C, 0x492812, 0xEA580C, 0xC2410C, 0xFFEDD5}, // Orange
    {0xF87171, 0xB91C1C, 0x4A2026, 0xDC2626, 0xB91C1C, 0xFEE2E2}, // Red
    {0xF472B6, 0xBE185D, 0x4A1D3B, 0xDB2777, 0xBE185D, 0xFCE7F3}, // Pink
};

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
extern const lv_color_t kOverlay = lv_color_hex(0x000000);
extern const lv_color_t kTextOnAccent = lv_color_hex(0xFFFFFF);
extern const lv_color_t kTextOnOverlayPrimary = lv_color_hex(0xFFFFFF);
extern const lv_color_t kTextOnOverlaySecondary = lv_color_hex(0xE2E8F0);

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
char s_track_list_title[lyra::media::kMaxName] = "";
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
bool s_firmware_update_in_progress = false;

lyra::media::SortSection s_sort_section = lyra::media::SortSection::Songs;
char s_database_status[64] = "";
DebugTab s_debug_tab = DebugTab::Info;
char s_debug_status[96] = "";
PlaylistManageAction s_pending_playlist_manage_action = PlaylistManageAction::DeletePlaylist;
size_t s_pending_playlist = 0;
size_t s_pending_playlist_track = 0;

lv_image_dsc_t s_player_art_descriptor{};
uint8_t *s_player_art_pixels;
char s_player_art_album[lyra::media::kMaxName]{};
uint32_t s_player_art_catalog_generation;
uint32_t s_player_art_generation;
lv_image_dsc_t s_boot_image_descriptor{};
uint8_t *s_boot_image_pixels;
lv_image_dsc_t s_brand_logo_descriptor{};
uint8_t *s_brand_logo_pixels;
lv_anim_t s_marquee_animation;
lv_style_t s_marquee_style;
bool s_marquee_style_ready = false;

} // namespace lyra::gui::internal
