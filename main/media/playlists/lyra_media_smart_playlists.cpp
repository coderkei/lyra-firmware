/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

namespace lyra::media::internal {
namespace {

constexpr char kPlaybackStatsMagic[8] = {'L', 'Y', 'R', 'A', 'P', 'L', 'A', 'Y'};
constexpr uint32_t kPlaybackStatsVersion = 1;

struct PlaybackStatsHeader {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint32_t record_count;
    uint32_t reserved;
    uint64_t sequence;
};

struct SmartTrackEntry {
    uint32_t track_index;
    uint64_t primary;
    uint64_t secondary;
};

PlaybackStat *allocate_playback_stats()
{
    auto *stats = static_cast<PlaybackStat *>(heap_caps_calloc(
        kMaxTracks, sizeof(PlaybackStat), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stats) {
        stats = static_cast<PlaybackStat *>(heap_caps_calloc(
            kMaxTracks, sizeof(PlaybackStat), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return stats;
}

bool remove_optional_file(const char *path)
{
    if (std::remove(path) == 0 || errno == ENOENT) return true;
    return false;
}

bool read_playback_stats_file(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;

    PlaybackStatsHeader header{};
    const bool header_ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
                           std::memcmp(header.magic, kPlaybackStatsMagic,
                                       sizeof(header.magic)) == 0 &&
                           header.version == kPlaybackStatsVersion &&
                           header.record_size == sizeof(PlaybackStat) &&
                           header.record_count <= kMaxTracks;
    if (!header_ok) {
        std::fclose(file);
        return false;
    }

    if (header.record_count != 0 && std::fread(
            s_playback_stats, sizeof(PlaybackStat), header.record_count, file) !=
            header.record_count) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    s_playback_stats_count = header.record_count;
    s_playback_sequence = header.sequence;
    std::sort(s_playback_stats, s_playback_stats + s_playback_stats_count,
              [](const PlaybackStat &left, const PlaybackStat &right) {
                  return left.path_hash < right.path_hash;
              });

    // Keep one record per path if an older interrupted write contained a
    // duplicate. The newest/highest values are the least surprising recovery.
    size_t unique_count = 0;
    for (size_t index = 0; index < s_playback_stats_count; ++index) {
        PlaybackStat &source = s_playback_stats[index];
        if (unique_count != 0 &&
            s_playback_stats[unique_count - 1].path_hash == source.path_hash) {
            PlaybackStat &destination = s_playback_stats[unique_count - 1];
            destination.play_count = std::max(destination.play_count, source.play_count);
            destination.last_played = std::max(destination.last_played, source.last_played);
            continue;
        }
        s_playback_stats[unique_count++] = source;
    }
    s_playback_stats_count = unique_count;
    for (size_t index = 0; index < s_playback_stats_count; ++index) {
        s_playback_sequence = std::max(s_playback_sequence,
                                       s_playback_stats[index].last_played);
    }
    return true;
}

PlaybackStat *mutable_playback_stat_locked(uint64_t path_hash)
{
    if (!s_playback_stats) return nullptr;
    PlaybackStat probe{path_hash, 0, 0, 0};
    auto *begin = s_playback_stats;
    auto *end = s_playback_stats + s_playback_stats_count;
    auto *found = std::lower_bound(begin, end, probe,
                                   [](const PlaybackStat &left, const PlaybackStat &right) {
                                       return left.path_hash < right.path_hash;
                                   });
    return found != end && found->path_hash == path_hash ? found : nullptr;
}

bool append_playback_stat_locked(uint64_t path_hash, PlaybackStat **out)
{
    if (!out || !s_playback_stats || s_playback_stats_count >= kMaxTracks) return false;
    PlaybackStat probe{path_hash, 0, 0, 0};
    auto *begin = s_playback_stats;
    auto *end = s_playback_stats + s_playback_stats_count;
    auto *found = std::lower_bound(begin, end, probe,
                                   [](const PlaybackStat &left, const PlaybackStat &right) {
                                       return left.path_hash < right.path_hash;
                                   });
    if (found != end && found->path_hash == path_hash) {
        *out = found;
        return true;
    }
    std::move_backward(found, end, end + 1);
    *found = {path_hash, 0, 0, 0};
    ++s_playback_stats_count;
    *out = found;
    return true;
}

bool write_playback_stats_locked()
{
    if (!s_playback_stats || !ensure_directory(kDataDir)) return false;
    remove_optional_file(kPlaybackStatsTempPath);
    FILE *file = std::fopen(kPlaybackStatsTempPath, "wb");
    if (!file) return false;

    PlaybackStatsHeader header{};
    std::memcpy(header.magic, kPlaybackStatsMagic, sizeof(header.magic));
    header.version = kPlaybackStatsVersion;
    header.record_size = sizeof(PlaybackStat);
    header.record_count = static_cast<uint32_t>(s_playback_stats_count);
    header.sequence = s_playback_sequence;
    const bool written = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                         (s_playback_stats_count == 0 ||
                          std::fwrite(s_playback_stats, sizeof(PlaybackStat),
                                      s_playback_stats_count, file) ==
                              s_playback_stats_count) &&
                         std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        remove_optional_file(kPlaybackStatsTempPath);
        return false;
    }

    remove_optional_file(kPlaybackStatsBackupPath);
    struct stat existing{};
    if (stat(kPlaybackStatsPath, &existing) == 0 &&
        std::rename(kPlaybackStatsPath, kPlaybackStatsBackupPath) != 0) {
        remove_optional_file(kPlaybackStatsTempPath);
        return false;
    }
    if (std::rename(kPlaybackStatsTempPath, kPlaybackStatsPath) != 0) {
        std::rename(kPlaybackStatsBackupPath, kPlaybackStatsPath);
        remove_optional_file(kPlaybackStatsTempPath);
        return false;
    }
    remove_optional_file(kPlaybackStatsBackupPath);
    return true;
}

bool include_smart_track(SmartPlaylistKind kind, const PlaybackStat *stat)
{
    switch (kind) {
    case SmartPlaylistKind::RecentlyAdded: return true;
    case SmartPlaylistKind::RecentlyPlayed:
    case SmartPlaylistKind::MostPlayed: return stat && stat->play_count != 0;
    case SmartPlaylistKind::NeverPlayed: return !stat || stat->play_count == 0;
    case SmartPlaylistKind::Count: break;
    }
    return false;
}

} // namespace

const PlaybackStat *playback_stat_locked(uint64_t path_hash)
{
    return mutable_playback_stat_locked(path_hash);
}

bool save_playback_stats_locked()
{
    return write_playback_stats_locked();
}

void clear_smart_playlist_cache_locked()
{
    for (size_t index = 0; index < static_cast<size_t>(SmartPlaylistKind::Count); ++index) {
        heap_caps_free(s_smart_playlist_orders[index]);
        s_smart_playlist_orders[index] = nullptr;
        s_smart_playlist_order_counts[index] = 0;
        s_smart_playlist_order_generations[index] = 0;
        s_smart_playlist_order_sequences[index] = 0;
    }
}

void load_playback_stats()
{
    clear_smart_playlist_cache_locked();
    heap_caps_free(s_playback_stats);
    s_playback_stats = allocate_playback_stats();
    s_playback_stats_count = 0;
    s_playback_sequence = 0;
    if (!s_playback_stats) {
        ESP_LOGW(kTag, "could not allocate playback statistics");
        return;
    }
    if (!read_playback_stats_file(kPlaybackStatsPath) &&
        read_playback_stats_file(kPlaybackStatsBackupPath)) {
        ESP_LOGW(kTag, "restored playback statistics from backup");
    }
}

esp_err_t clear_playback_stats_locked()
{
    const bool removed = remove_optional_file(kPlaybackStatsPath) &&
                         remove_optional_file(kPlaybackStatsTempPath) &&
                         remove_optional_file(kPlaybackStatsBackupPath);
    heap_caps_free(s_playback_stats);
    s_playback_stats = nullptr;
    s_playback_stats_count = 0;
    s_playback_sequence = 0;
    clear_smart_playlist_cache_locked();
    return removed ? ESP_OK : ESP_FAIL;
}

} // namespace lyra::media::internal

namespace lyra::media {
using namespace internal;

size_t smart_playlist_count()
{
    return static_cast<size_t>(SmartPlaylistKind::Count);
}

bool smart_playlist_at(size_t index, SmartPlaylist *out)
{
    if (!out || index >= smart_playlist_count()) return false;
    const auto kind = static_cast<SmartPlaylistKind>(index);
    out->kind = kind;
    out->track_count = smart_playlist_track_count(kind);
    return true;
}

size_t smart_playlist_track_count(SmartPlaylistKind kind)
{
    if (static_cast<uint8_t>(kind) >= static_cast<uint8_t>(SmartPlaylistKind::Count)) {
        return 0;
    }
    Lock lock;
    if (!s_catalog || !s_path_index) return 0;
    if (kind == SmartPlaylistKind::RecentlyAdded) return s_track_count;

    size_t count = 0;
    for (size_t index = 0; index < s_track_count; ++index) {
        const PlaybackStat *stat = playback_stat_locked(s_path_index[index].hash);
        if (include_smart_track(kind, stat)) ++count;
    }
    return count;
}

size_t smart_playlist_tracks(SmartPlaylistKind kind, size_t offset,
                             size_t *track_indices, size_t capacity)
{
    if (static_cast<uint8_t>(kind) >= static_cast<uint8_t>(SmartPlaylistKind::Count) ||
        !track_indices || capacity == 0) {
        return 0;
    }
    Lock lock;
    if (!s_catalog || !s_path_index || !s_physical_to_logical || s_track_count == 0) return 0;

    const size_t kind_index = static_cast<size_t>(kind);
    const bool cache_valid = s_smart_playlist_orders[kind_index] &&
                             s_smart_playlist_order_generations[kind_index] ==
                                 s_status.catalog_generation &&
                             s_smart_playlist_order_sequences[kind_index] ==
                                 s_playback_sequence;
    if (cache_valid) {
        const size_t total = s_smart_playlist_order_counts[kind_index];
        const size_t shown = offset < total ? std::min(capacity, total - offset) : 0;
        if (shown != 0) {
            std::memcpy(track_indices, s_smart_playlist_orders[kind_index] + offset,
                        shown * sizeof(uint32_t));
        }
        return shown;
    }

    auto *entries = static_cast<SmartTrackEntry *>(heap_caps_malloc(
        s_track_count * sizeof(SmartTrackEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries && s_track_count != 0) {
        entries = static_cast<SmartTrackEntry *>(heap_caps_malloc(
            s_track_count * sizeof(SmartTrackEntry), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!entries && s_track_count != 0) return 0;

    size_t entry_count = 0;
    if (kind == SmartPlaylistKind::RecentlyAdded) {
        for (size_t physical = 0; physical < s_track_count; ++physical) {
            Track track{};
            if (!read_physical(s_catalog, s_catalog_header, static_cast<uint32_t>(physical),
                               &track)) continue;
            const uint32_t logical = s_physical_to_logical[physical];
            entries[entry_count++] = {logical, track.modified_time,
                                      static_cast<uint64_t>(logical)};
        }
    } else {
        for (size_t path_index = 0; path_index < s_track_count; ++path_index) {
            const PathIndex &path = s_path_index[path_index];
            const PlaybackStat *stat = playback_stat_locked(path.hash);
            if (!include_smart_track(kind, stat)) continue;
            const uint64_t primary = kind == SmartPlaylistKind::MostPlayed ?
                static_cast<uint64_t>(stat->play_count) :
                kind == SmartPlaylistKind::RecentlyPlayed ? stat->last_played : 0;
            const uint64_t secondary = kind == SmartPlaylistKind::MostPlayed ?
                stat->last_played : static_cast<uint64_t>(path.track_index);
            entries[entry_count++] = {path.track_index, primary, secondary};
        }
    }

    std::sort(entries, entries + entry_count,
              [kind](const SmartTrackEntry &left, const SmartTrackEntry &right) {
                  if (left.primary != right.primary) return left.primary > right.primary;
                  if (left.secondary != right.secondary) return left.secondary > right.secondary;
                  return left.track_index < right.track_index;
              });
    auto *order = static_cast<uint32_t *>(heap_caps_malloc(
        entry_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!order && entry_count != 0) {
        order = static_cast<uint32_t *>(heap_caps_malloc(
            entry_count * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    const bool cached = order && entry_count != 0;
    if (cached) {
        for (size_t index = 0; index < entry_count; ++index) {
            order[index] = entries[index].track_index;
        }
        heap_caps_free(s_smart_playlist_orders[kind_index]);
        s_smart_playlist_orders[kind_index] = order;
        s_smart_playlist_order_counts[kind_index] = entry_count;
        s_smart_playlist_order_generations[kind_index] = s_status.catalog_generation;
        s_smart_playlist_order_sequences[kind_index] = s_playback_sequence;
    } else {
        heap_caps_free(s_smart_playlist_orders[kind_index]);
        s_smart_playlist_orders[kind_index] = nullptr;
        s_smart_playlist_order_counts[kind_index] = 0;
        s_smart_playlist_order_generations[kind_index] = 0;
        s_smart_playlist_order_sequences[kind_index] = 0;
    }
    const size_t shown = offset < entry_count ?
        std::min(capacity, entry_count - offset) : 0;
    if (cached) {
        if (shown != 0) {
            std::memcpy(track_indices, order + offset, shown * sizeof(uint32_t));
        }
    } else {
        for (size_t index = 0; index < shown; ++index) {
            track_indices[index] = entries[offset + index].track_index;
        }
    }
    if (!cached) heap_caps_free(order);
    heap_caps_free(entries);
    return shown;
}

uint32_t track_play_count(size_t track_index)
{
    Lock lock;
    Track track{};
    if (!track_at_locked(track_index, &track)) return 0;
    const PlaybackStat *stat = playback_stat_locked(hash_path(track.path));
    return stat ? stat->play_count : 0;
}

esp_err_t record_track_play(size_t track_index)
{
    Lock lock;
    if (!s_status.mounted || s_shutdown_requested || !s_catalog) {
        return ESP_ERR_INVALID_STATE;
    }
    Track track{};
    if (!track_at_locked(track_index, &track)) return ESP_ERR_NOT_FOUND;
    if (!s_playback_stats) {
        s_playback_stats = allocate_playback_stats();
        if (!s_playback_stats) return ESP_ERR_NO_MEM;
    }

    PlaybackStat *stat = mutable_playback_stat_locked(hash_path(track.path));
    if (!stat && !append_playback_stat_locked(hash_path(track.path), &stat)) {
        return ESP_ERR_NO_MEM;
    }
    if (s_playback_sequence == UINT64_MAX) s_playback_sequence = 0;
    ++s_playback_sequence;
    if (stat->play_count != UINT32_MAX) ++stat->play_count;
    stat->last_played = s_playback_sequence;
    return save_playback_stats_locked() ? ESP_OK : ESP_FAIL;
}

} // namespace lyra::media
