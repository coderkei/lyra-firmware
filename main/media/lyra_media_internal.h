/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lyra_media.h"
#include "lyra_media_artwork.h"
#include "lyra_sd.h"
#include "lyra_audio.h"
#include "lyra_board_pins.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

namespace lyra::media::internal {

using namespace lyra::board::jc3248w535en;

constexpr const char *kTag = "lyra.media";
constexpr const char *kMount = "/sdcard";
constexpr const char *kPlaylistDir = "/sdcard/Playlists";
constexpr const char *kFavoritesPath = "/sdcard/Playlists/Favorites.m3u8";
constexpr const char *kFavoritesTempPath = "/sdcard/Playlists/Favorites.tmp";
constexpr const char *kFavoritesBackupPath = "/sdcard/Playlists/Favorites.bak";
constexpr const char *kPlaylistEditTempPath = "/sdcard/Playlists/.lyra-edit.tmp";
constexpr const char *kPlaylistEditBackupPath = "/sdcard/Playlists/.lyra-edit.bak";
constexpr const char *kDataDir = "/sdcard/.lyra";
constexpr const char *kQueueSnapshotPath = "/sdcard/.lyra/queue-v1.m3u8";
constexpr const char *kQueueSnapshotTempPath = "/sdcard/.lyra/queue-v1.tmp";
constexpr const char *kQueueSnapshotBackupPath = "/sdcard/.lyra/queue-v1.bak";
constexpr const char *kPlaybackStatsPath = "/sdcard/.lyra/play-stats-v1.bin";
constexpr const char *kPlaybackStatsTempPath = "/sdcard/.lyra/play-stats-v1.tmp";
constexpr const char *kPlaybackStatsBackupPath = "/sdcard/.lyra/play-stats-v1.bak";
constexpr const char *kArtworkDir = "/sdcard/.lyra/covers";
constexpr const char *kCatalogPath = "/sdcard/.lyra/catalog-v10.bin";
constexpr const char *kCatalogTempPath = "/sdcard/.lyra/catalog-v10.tmp";
constexpr const char *kCatalogBackupPath = "/sdcard/.lyra/catalog-v10.bak";
// A scan can hold several recursive directory frames while artwork decoding
// runs on the background core. Keep the worker isolated from the UI with
// enough stack for the FAT/VFS and decoder call chains.
constexpr size_t kScanStack = 24 * 1024;
constexpr size_t kDurationStack = 12 * 1024;
constexpr size_t kSortStack = 16 * 1024;
constexpr size_t kArtworkReadChunkBytes = 8192;
constexpr size_t kMaximumEmbeddedImageBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumOggCommentPacketBytes = 2u * 1024u * 1024u;
// A card can still be completing its power-up/reset sequence when the
// application starts, especially after flashing while the socket remains
// powered. Keep the normal 20 MHz path fast, but give transient bring-up
// failures several chances to recover before declaring the card unavailable.
constexpr int kSdMountAttempts = 5;
constexpr int kSdInitialSettleMs = 250;
constexpr int kSdRetryDelayMs = 250;
constexpr int kSdFallbackFrequencyKHz = 10000;
// Only decoded covers that required libjpeg's SD backing store are persisted.
// Versioning prevents an older raw-cache layout from being accepted silently.
constexpr uint32_t kLargeArtworkCacheVersion = 1;
constexpr const char *kSettingsNamespace = "lyra";
constexpr const char *kArtworkSdCacheKey = "art_sd_cache";
constexpr const char *kArtworkSizeKey = "art_size";
constexpr const char *kSongsSortKey = "sort_songs";
constexpr const char *kAlbumsSortKey = "sort_albums";
constexpr const char *kArtistsSortKey = "sort_artists";
constexpr const char *kSongsSortCachePath = "/sdcard/.lyra/sort-songs-v1.bin";
constexpr const char *kAlbumsSortCachePath = "/sdcard/.lyra/sort-albums-v1.bin";
constexpr const char *kArtistsSortCachePath = "/sdcard/.lyra/sort-artists-v1.bin";
constexpr char kSortCacheMagic[8] = {'L', 'Y', 'R', 'A', 'S', 'O', 'R', 'T'};
constexpr uint32_t kSortCacheVersion = 1;
// Keep cover extraction and scaling off PRO_CPU, which owns realtime audio.
constexpr BaseType_t kArtworkCore = 1;

struct CatalogHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t track_count;
    uint32_t track_size;
    uint32_t records_offset;
    uint32_t title_order_offset;
    uint32_t path_index_offset;
    uint32_t artist_group_offset;
    uint32_t artist_group_count;
    uint32_t album_group_offset;
    uint32_t album_group_count;
    uint32_t genre_group_offset;
    uint32_t genre_group_count;
    uint32_t year_group_offset;
    uint32_t year_group_count;
    uint32_t checksum;
    uint32_t file_size;
    uint32_t flags;
};

struct GroupRecord {
    char name[kMaxName];
    uint32_t track_count;
    uint32_t members_offset;
    uint32_t representative_track;
};

struct PathIndex {
    uint64_t hash;
    uint32_t track_index;
    uint32_t reserved;
};

struct SortKey {
    char value[kMaxName];
    uint32_t physical_index;
};

struct SortEntry {
    uint32_t source_index;
    char primary[kMaxName];
    char secondary[kMaxName];
    uint64_t numeric;
};

struct SortCacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t catalog_checksum;
    uint32_t item_count;
    uint8_t section;
    uint8_t sort_code;
    uint16_t reserved;
};

struct GroupTrackEntry {
    uint32_t logical_index;
    uint32_t disc_number;
    uint32_t track_number;
    char title[kMaxName];
};

struct CachedTrack {
    uint32_t physical_index;
    uint32_t generation;
    Track track;
};

constexpr char kCatalogMagic[8] = {'L', 'Y', 'R', 'A', 'C', 'A', 'T', '8'};
constexpr uint32_t kCatalogVersion = 10;
constexpr uint32_t kCatalogFlagCapacityReached = 1u << 0;
constexpr size_t kTrackCacheSize = 24;
constexpr size_t kArtworkQueueSize = 16;

struct ArtworkRequest {
    Track track;
    ArtworkSize size;
};

struct PlaybackStat {
    uint64_t path_hash;
    uint32_t play_count;
    uint32_t reserved;
    uint64_t last_played;
};

struct Mp4Atom {
    uint64_t start;
    uint64_t data_start;
    uint64_t end;
    uint64_t data_size;
    uint8_t header_size;
    uint8_t type[4];
};

extern FILE *s_catalog;
extern char s_catalog_path[kMaxPath];
extern CatalogHeader s_catalog_header;
extern uint32_t *s_title_order;
extern uint32_t *s_physical_to_logical;
extern PathIndex *s_path_index;
extern uint32_t *s_song_sort_order;
extern uint32_t *s_album_sort_order;
extern uint32_t *s_artist_sort_order;
extern uint32_t *s_artist_album_order;
extern uint32_t *s_album_for_logical;
extern uint32_t *s_duration_cache;
extern uint8_t *s_duration_ready;
extern size_t s_artist_album_count;
extern size_t s_artist_album_artist;
extern uint32_t s_artist_album_generation;
extern uint32_t s_song_sort_generation;
extern uint32_t s_album_sort_generation;
extern uint32_t s_artist_sort_generation;
extern uint8_t s_song_sort_code;
extern uint8_t s_album_sort_code;
extern uint8_t s_artist_sort_code;
extern CachedTrack *s_track_cache;
extern size_t s_cache_cursor;
extern ArtworkRequest s_artwork_queue[kArtworkQueueSize];
extern size_t s_artwork_queue_head;
extern size_t s_artwork_queue_count;
extern bool s_artwork_task_running;
extern uint64_t s_artwork_active_hash;
extern uint16_t *s_artwork_pixels;
extern uint16_t s_artwork_pixel_size;
extern uint32_t s_artwork_key;
extern bool s_artwork_valid;
extern uint32_t s_artwork_failed_key;
extern bool s_artwork_failed;
extern Playlist s_playlists[kMaxPlaylists];
extern size_t s_track_count;
extern size_t s_playlist_count;
extern SemaphoreHandle_t s_mutex;
extern sdmmc_card_t *s_card;
extern Status s_status;
extern bool s_shutdown_requested;
extern ArtworkDiagnostics s_artwork_diagnostics;
extern SearchStatus s_search_status;
extern bool s_search_cancel_requested;
extern bool s_duration_cancel_requested;
extern uint32_t s_duration_catalog_token;
extern bool s_sort_cancel_requested;
extern uint32_t s_sort_catalog_token;
extern SearchResult *s_search_results;
extern SearchCategory s_cached_search_category;
extern char s_cached_search_query[48];
extern bool s_nvs_ready;
extern SortSetting s_sort_settings[3];
extern PlaybackStat *s_playback_stats;
extern size_t s_playback_stats_count;
extern uint64_t s_playback_sequence;
extern uint32_t *s_smart_playlist_orders[4];
extern size_t s_smart_playlist_order_counts[4];
extern uint32_t s_smart_playlist_order_generations[4];
extern uint64_t s_smart_playlist_order_sequences[4];

class Lock {
public:
    Lock() { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
};

void refresh_capacity();
void start_duration_indexing_if_needed();
void start_sort_cache_indexing_locked(SortSection section);

esp_err_t mount_sd_card();
esp_err_t ensure_nvs_ready();
void load_artwork_settings();
esp_err_t save_artwork_settings();
uint8_t sort_setting_code(SortSetting setting);
bool decode_sort_setting(uint8_t code, SortSetting *setting);
void load_sort_settings();
esp_err_t save_sort_setting(SortSection section);
SortSetting current_sort_setting(SortSection section);
const char *sort_cache_path(SortSection section);
bool load_sort_cache_locked(SortSection section, uint8_t sort_code, uint32_t item_count, uint32_t **order_slot);
void save_sort_cache_locked(SortSection section, SortSetting setting, const uint32_t *order, uint32_t item_count);
void copy_text(char *destination, size_t capacity, const char *source);
bool join_path(char *destination, size_t capacity, const char *parent, const char *child);
bool equals_ci(const char *left, const char *right);
bool contains_ci(const char *text, const char *query);
uint64_t hash_path(const char *path);
uint32_t checksum_update(uint32_t hash, const void *data, size_t length);
bool checksum_file_payload(FILE *file, uint32_t start, uint32_t end, uint32_t *out);
const char *extension_of(const char *name);
bool compatible_audio(const char *name);
const char *base_name(const char *path);
void parent_name(const char *path, char *out, size_t capacity);
void metadata_from_path(Track *track);
uint32_t read_be32(const uint8_t *bytes);
uint64_t read_be64(const uint8_t *bytes);
uint32_t read_syncsafe32(const uint8_t *bytes);
uint32_t read_le32(const uint8_t *bytes);
uint32_t parse_tag_number(const char *text);
int16_t parse_replay_gain_tenths_db(const char *text);
void copy_tag_text(char *destination, size_t capacity, const uint8_t *source, size_t length, uint8_t encoding = 3);
void normalize_year(char *year);
void assign_tag(Track *track, const char *key, const uint8_t *value, size_t length, uint8_t encoding = 3);
void assign_id3_user_text_tag(Track *track, const uint8_t *text, size_t length);
void read_id3_text_tags(FILE *file, Track *track, const uint8_t header[10], long tag_start = 0);
bool read_ogg_comment_packet(FILE *file, uint8_t *packet, size_t capacity, size_t *packet_length);
void parse_vorbis_comments(const uint8_t *data, size_t length, size_t cursor, Track *track);
void read_ogg_text_tags(FILE *file, Track *track);
bool read_mp4_atom(FILE *file, uint64_t position, uint64_t limit, Mp4Atom *atom);
bool mp4_type(const Mp4Atom &atom, const char (&type)[5]);
bool read_mp4_text_value(FILE *file, const Mp4Atom &item, const char *key, Track *track);
bool read_mp4_position_value(FILE *file, const Mp4Atom &item, bool disc, Track *track);
void read_mp4_ilst(FILE *file, const Mp4Atom &ilst, Track *track);
bool mp4_container(const Mp4Atom &atom);
void read_mp4_metadata_tree(FILE *file, uint64_t start, uint64_t end, Track *track, unsigned depth = 0);
bool find_mp4_moov(FILE *file, Mp4Atom *moov);
void read_mp4_text_tags(FILE *file, Track *track);
void read_flac_text_tags(FILE *file, Track *track);
const char *riff_info_key(const uint8_t *id);
void read_wav_text_tags(FILE *file, Track *track);
void read_aiff_text_tags(FILE *file, Track *track);
void read_fast_metadata(Track *track);
bool ensure_directory(const char *path);
void remove_stale_jpeg_work_files();
void process_artwork_request(const ArtworkRequest &request);
void artwork_task(void *);
void search_task(void *);
bool scan_directory(const char *path, FILE *catalog, size_t *count,
                    bool *capacity_reached, TickType_t *last_progress,
                    size_t *reused_count, size_t *rescanned_count,
                    size_t *matched_existing_count, size_t depth = 0);
size_t count_playlist_entries(const char *path);
esp_err_t ensure_favorites_file();
void load_playlists(Playlist *playlists, size_t *count);
void reload_playlists_locked();
bool publish_playlist_edit(const char *path);
bool playlist_line_to_absolute(const char *line, char *absolute, size_t capacity);
void refresh_capacity();
bool read_physical(FILE *file, const CatalogHeader &header, uint32_t physical_index, Track *out);
void apply_cached_duration_locked(uint32_t physical_index, Track *track);
void refresh_track_file_state(Track *track);
bool read_logical_track_locked(uint32_t logical_index, Track *out, bool refresh_file = true);
int compare_sort_entry(const SortEntry &left, const SortEntry &right, SortSetting setting);
void populate_track_sort_entry(const Track &track, uint32_t logical_index, SortSetting setting, SortEntry *entry);
bool prepare_song_sort_order_locked();
bool build_song_sort_order_locked();
bool prepare_group_sort_order_locked(SortSection section);
bool build_group_sort_order_locked(SortSection section);
bool read_logical_sort_snapshot(FILE *file, const CatalogHeader &header, const uint32_t *title_order, size_t track_count, const uint32_t *duration_cache, const uint8_t *duration_ready, uint32_t logical_index, Track *out);
void update_sort_task_progress(uint32_t token, size_t processed);
void sort_cache_task(void *argument);
void start_sort_cache_indexing_locked(SortSection section);
bool build_group_track_order_locked(GroupKind kind, const GroupRecord &group, GroupTrackEntry **entries_out);
bool populate_sort_keys(FILE *file, const CatalogHeader &header, SortKey *keys, const char *(*field)(const Track &));
bool append_group_index(FILE *file, CatalogHeader *header, SortKey *keys, const uint32_t *physical_to_logical, const char *(*field)(const Track &), uint32_t *group_offset, uint32_t *group_count);
bool finish_catalog(FILE *file, CatalogHeader *header, SortKey *keys, bool capacity_reached);
bool validate_catalog(FILE *file, CatalogHeader *header, bool verify_checksum = true);
void clear_runtime_catalog();
bool remove_existing_file(const char *path);
bool remove_directory_files(const char *directory_path);
esp_err_t clear_playlists_locked();
esp_err_t clear_artwork_cache_locked();
esp_err_t clear_catalog_locked();
bool load_catalog_file(const char *path, bool verify_checksum = true);
void load_cached_state();
void load_playback_stats();
esp_err_t clear_playback_stats_locked();
void clear_smart_playlist_cache_locked();
const PlaybackStat *playback_stat_locked(uint64_t path_hash);
bool save_playback_stats_locked();
esp_err_t publish_catalog();
bool duration_sort_requested_locked();
void duration_task(void *);
void start_duration_indexing_if_needed();
void scan_task(void *);

} // namespace lyra::media::internal

namespace lyra::media {
void group_location(GroupKind kind, uint32_t *offset, uint32_t *count);
bool find_track_by_path_locked(const char *path, size_t *track_index);
bool track_at_locked(size_t index, Track *out);
} // namespace lyra::media
