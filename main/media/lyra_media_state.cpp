/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_media_internal.h"

namespace lyra::media::internal {

FILE *s_catalog;
char s_catalog_path[kMaxPath]{};
CatalogHeader s_catalog_header{};
uint32_t *s_title_order;
uint32_t *s_physical_to_logical;
PathIndex *s_path_index;
uint32_t *s_song_sort_order;
uint32_t *s_album_sort_order;
uint32_t *s_artist_sort_order;
uint32_t *s_artist_album_order;
uint32_t *s_album_for_logical;
uint32_t *s_duration_cache;
uint8_t *s_duration_ready;
size_t s_artist_album_count;
size_t s_artist_album_artist = SIZE_MAX;
uint32_t s_artist_album_generation;
uint32_t s_song_sort_generation;
uint32_t s_album_sort_generation;
uint32_t s_artist_sort_generation;
uint8_t s_song_sort_code;
uint8_t s_album_sort_code;
uint8_t s_artist_sort_code;
CachedTrack *s_track_cache;
size_t s_cache_cursor;
ArtworkRequest s_artwork_queue[kArtworkQueueSize]{};
size_t s_artwork_queue_head;
size_t s_artwork_queue_count;
bool s_artwork_task_running;
uint64_t s_artwork_active_hash;
uint16_t *s_artwork_pixels;
uint16_t s_artwork_pixel_size;
uint32_t s_artwork_key;
bool s_artwork_valid;
uint32_t s_artwork_failed_key;
bool s_artwork_failed;
Playlist s_playlists[kMaxPlaylists]{};
size_t s_track_count;
size_t s_playlist_count;
SemaphoreHandle_t s_mutex;
sdmmc_card_t *s_card;
Status s_status{};
bool s_shutdown_requested;
ArtworkDiagnostics s_artwork_diagnostics{};
SearchStatus s_search_status{};
bool s_search_cancel_requested;
bool s_duration_cancel_requested;
uint32_t s_duration_catalog_token;
bool s_sort_cancel_requested;
uint32_t s_sort_catalog_token;
SearchResult *s_search_results;
SearchCategory s_cached_search_category = SearchCategory::Songs;
char s_cached_search_query[48]{};
bool s_nvs_ready;
SortSetting s_sort_settings[] = {
    {SortField::Title, SortDirection::Ascending},
    {SortField::Title, SortDirection::Ascending},
    {SortField::Title, SortDirection::Ascending},
};


} // namespace lyra::media::internal
