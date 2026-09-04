/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace lyra::media {

// Product ceiling. Full Track records live in the MicroSD catalog; only compact
// ordering/path indexes and a small record cache remain resident in PSRAM.
constexpr size_t kMaxTracks = 10000;
constexpr size_t kMaxPlaylists = 32;
constexpr size_t kMaxPath = 256;
constexpr size_t kMaxName = 96;
constexpr size_t kTrackPageSize = 5;
constexpr uint16_t kDefaultArtworkSize = 240;
constexpr uint16_t kLargeArtworkSize = 320;
constexpr uint16_t kMaximumArtworkSize = kLargeArtworkSize;

struct Track {
    char path[kMaxPath];
    char title[kMaxName];
    char artist[kMaxName];
    char album[kMaxName];
    char album_artist[kMaxName];
    char composer[kMaxName];
    char genre[kMaxName];
    char year[8];
    // Reserved catalog fields kept for binary record compatibility. Artwork is
    // decoded on demand from the song and is never represented by SD paths.
    char artwork[80];
    // Reserved; no list-browser thumbnail is generated.
    char thumbnail[80];
    // Reserved; the display-sized RGB565 cover lives in the media RAM buffer.
    char player_art[80];
    char format[8];
    uint64_t size_bytes;
    uint32_t duration_ms;
    uint32_t track_number;
    uint32_t disc_number;
    uint64_t modified_time;
    // Standard ReplayGain track adjustment in tenths of a decibel. Zero is
    // both the neutral value and the fallback for files without the tag.
    int16_t replay_gain_tenths_db;
};

struct Playlist {
    char name[kMaxName];
    char path[kMaxPath];
    size_t track_count;
};

struct Status {
    bool mounted;
    bool scanning;
    esp_err_t last_error;
    size_t track_count;
    size_t playlist_count;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t catalog_generation;
    bool capacity_reached;
    size_t scan_found;
    bool scan_indexing;
    bool duration_indexing;
    size_t duration_indexed;
    uint32_t duration_generation;
    bool sorting_indexing;
    size_t sorting_indexed;
    size_t sorting_total;
    uint8_t sorting_section;
    uint32_t sorting_generation;
    uint32_t artwork_generation;
    bool artwork_busy;
    bool artwork_sd_cache_enabled;
    uint16_t artwork_size;
};

// A copy of the decoded card identity is returned so callers do not need to
// depend on the SDMMC driver's private card lifetime.
struct CardInfo {
    bool mounted;
    uint64_t capacity_bytes;
    int manufacturer_id;
    int oem_id;
    char name[8];
    int revision;
    int serial;
    int date;
};

enum class GroupKind : uint8_t { Artist, Album, Genre, Year };

enum class SortSection : uint8_t { Songs, Albums, Artists };
enum class SortField : uint8_t {
    Title,
    Album,
    TrackNumber,
    Duration,
    Artist,
    DateModified,
};
enum class SortDirection : uint8_t { Ascending, Descending };

struct SortSetting {
    SortField field;
    SortDirection direction;
};

struct Group {
    char name[kMaxName];
    size_t track_count;
    size_t representative_track;
};

struct ArtworkDiagnostics {
    uint32_t source_read_us;
    uint32_t decode_us;
    uint32_t decode_count;
    bool source_psram;
    bool player_pixels_internal;
};

struct SearchStatus {
    bool running;
    bool ready;
    size_t processed;
    size_t total;
    size_t result_count;
    uint32_t generation;
    esp_err_t result;
};

enum class SearchCategory : uint8_t { Songs, Albums, Artists, Playlists };

// Search results are deliberately typed so the UI can route a result to the
// correct overview/list and, for playlist matches, retain the playlist queue.
// Only the field relevant to the result category is populated.
struct SearchResult {
    uint32_t track_index;
    uint32_t group_index;
    uint32_t playlist_index;
};

esp_err_t init();
Status status();
bool card_info(CardInfo *out);
esp_err_t start_scan();
esp_err_t clear_playlists();
esp_err_t clear_artwork_cache();
esp_err_t clear_all_databases();
esp_err_t set_artwork_sd_cache_enabled(bool enabled);
esp_err_t set_artwork_size(uint16_t size);
SortSetting sort_setting(SortSection section);
esp_err_t set_sort_setting(SortSection section, SortField field,
                           SortDirection direction);
ArtworkDiagnostics artwork_diagnostics();
// Stops accepting new work, waits for an active scan to finish, and unmounts
// the card so reboot/deep-sleep cannot interrupt a filesystem transaction.
esp_err_t shutdown(uint32_t timeout_ms = 30000);

size_t track_count();
bool track_at(size_t index, Track *out);
bool sorted_track_at(size_t index, size_t *track_index);
esp_err_t start_search(SearchCategory category, const char *query);
SearchStatus search_status();
size_t search_results(size_t offset, SearchResult *results, size_t capacity,
                     size_t *total = nullptr);
enum class ArtworkSize : uint8_t { Player };
// Artwork is keyed by album identity. Ordinary decodes remain RAM-only; covers
// that require SD-backed JPEG workspace retain their final RGB565 image on SD.
// The caller owns the destination buffer while the media service owns its copy.
bool copy_artwork(const Track &track, uint16_t *pixels, size_t pixel_count);
esp_err_t request_artwork(const Track &track, ArtworkSize size);
size_t group_count(GroupKind kind);
bool group_at(GroupKind kind, size_t index, Group *out);
bool sorted_group_index_at(SortSection section, size_t index, size_t *group_index);
size_t group_tracks(GroupKind kind, size_t group_index, size_t offset,
                    size_t *track_indices, size_t capacity);
bool group_adjacent_track(GroupKind kind, size_t group_index, size_t current_track,
                          int direction, size_t *track_index);
size_t artist_album_count(size_t artist_group_index);
bool artist_album_at(size_t artist_group_index, size_t offset, size_t *album_group_index);

size_t playlist_count();
bool playlist_at(size_t index, Playlist *out);
size_t playlist_tracks(size_t playlist_index, size_t offset,
                       size_t *track_indices, size_t capacity);
bool playlist_adjacent_track(size_t playlist_index, size_t current_track,
                             int direction, size_t *track_index);
esp_err_t create_playlist(const char *name, size_t *created_index = nullptr);
esp_err_t add_to_playlist(size_t playlist_index, size_t track_index);
esp_err_t remove_from_playlist(size_t playlist_index, size_t track_index);
// Deleting Favorites clears it; other playlists are removed from the card.
esp_err_t delete_playlist(size_t playlist_index);

// The active queue is kept separately from portable user playlists. Entries
// are written as MicroSD-relative paths so a rebuilt catalog can restore the
// same order after a restart.
bool queue_snapshot_exists();
esp_err_t save_queue_snapshot(const size_t *track_indices, size_t track_count,
                              size_t current_position);
size_t load_queue_snapshot(size_t *track_indices, size_t capacity,
                           size_t *current_position);

bool is_favorite(size_t track_index);
esp_err_t set_favorite(size_t track_index, bool favorite);

// Lists direct child folders and catalog tracks beneath a mounted SD path.
// Paths use the VFS form, starting at /sdcard.
size_t child_folders(const char *path, size_t offset, char names[][kMaxName],
                     size_t capacity, size_t *total = nullptr);
size_t folder_tracks(const char *path, size_t offset,
                     size_t *track_indices, size_t capacity, size_t *total = nullptr);

} // namespace lyra::media
