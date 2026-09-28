/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_media_internal.h"

#include <climits>
#include <ctime>

namespace lyra::media {
using namespace internal;

namespace {

struct JumpCacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t catalog_checksum;
    uint32_t item_count;
    uint32_t location_count;
    uint32_t payload_checksum;
    uint8_t section;
    uint8_t sort_code;
    uint16_t reserved;
};

struct JumpCacheEntry {
    uint64_t value;
    uint32_t item_position;
    char initial[8];
    uint32_t kind;
};

struct JumpKey {
    uint64_t value;
    char initial[8];
};

struct GroupJumpKey {
    uint64_t value;
    uint32_t track_count;
    uint32_t members_offset;
    char initial[8];
};

struct SortLocationSnapshot {
    CatalogHeader header;
    SortSection section;
    SortSetting setting;
    size_t item_count;
    uint32_t catalog_generation;
    uint32_t duration_generation;
    bool identity_order;
    uint32_t *order;
    uint32_t *physical_to_logical;
    uint32_t *duration_cache;
    uint8_t *duration_ready;
};

static_assert(sizeof(JumpCacheEntry) == 24,
              "Jump cache entries must keep a stable compact disk layout");

void *jump_alloc(size_t bytes)
{
    if (bytes == 0) return nullptr;
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return memory;
}

void *jump_calloc(size_t count, size_t bytes)
{
    if (count == 0 || bytes == 0) return nullptr;
    void *memory = heap_caps_calloc(count, bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_calloc(count, bytes, MALLOC_CAP_8BIT);
    return memory;
}

const char *jump_cache_path(SortSection section)
{
    return section == SortSection::Songs ? kSongsJumpCachePath :
           section == SortSection::Albums ? kAlbumsJumpCachePath :
                                            kArtistsJumpCachePath;
}

const char *jump_cache_temp_path(SortSection section)
{
    return section == SortSection::Songs ? kSongsJumpCacheTempPath :
           section == SortSection::Albums ? kAlbumsJumpCacheTempPath :
                                            kArtistsJumpCacheTempPath;
}

SortLocationKind location_kind(SortField field)
{
    switch (field) {
    case SortField::TrackNumber: return SortLocationKind::TrackNumber;
    case SortField::Duration: return SortLocationKind::Duration;
    case SortField::DateModified: return SortLocationKind::Date;
    case SortField::Title:
    case SortField::Album:
    case SortField::Artist:
        return SortLocationKind::Initial;
    }
    return SortLocationKind::Initial;
}

bool same_setting(SortSetting left, SortSetting right)
{
    return left.field == right.field && left.direction == right.direction;
}

bool group_initial_uses_name(SortSection section, SortField field)
{
    return field == SortField::Title ||
           (section == SortSection::Albums && field == SortField::Album) ||
           (section == SortSection::Artists && field == SortField::Artist);
}

bool needs_track_keys(const SortLocationSnapshot &snapshot)
{
    if (snapshot.section == SortSection::Songs) return true;
    const SortLocationKind kind = location_kind(snapshot.setting.field);
    return kind != SortLocationKind::Initial ||
           !group_initial_uses_name(snapshot.section, snapshot.setting.field);
}

size_t codepoint_length(unsigned char first)
{
    if ((first & 0x80u) == 0) return 1;
    if ((first & 0xE0u) == 0xC0u) return 2;
    if ((first & 0xF0u) == 0xE0u) return 3;
    if ((first & 0xF8u) == 0xF0u) return 4;
    return 1;
}

void make_initial(const char *text, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!text || !text[0]) {
        output[0] = '#';
        output[1] = '\0';
        return;
    }
    const unsigned char first = static_cast<unsigned char>(text[0]);
    if (first < 0x80u) {
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) {
            output[0] = static_cast<char>(std::toupper(first));
            output[1] = '\0';
        } else if (first >= '0' && first <= '9') {
            output[0] = static_cast<char>(first);
            output[1] = '\0';
        } else {
            output[0] = '#';
            output[1] = '\0';
        }
        return;
    }
    const size_t length = std::min(codepoint_length(first), capacity - 1);
    std::memcpy(output, text, length);
    output[length] = '\0';
}

uint32_t local_date_key(uint64_t timestamp)
{
    if (!timestamp) return 0;
    const std::time_t raw = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (!localtime_r(&raw, &local)) return 0;
    return static_cast<uint32_t>((local.tm_year + 1900) * 10000 +
                                 (local.tm_mon + 1) * 100 + local.tm_mday);
}

void free_snapshot(SortLocationSnapshot *snapshot)
{
    if (!snapshot) return;
    heap_caps_free(snapshot->order);
    heap_caps_free(snapshot->physical_to_logical);
    heap_caps_free(snapshot->duration_cache);
    heap_caps_free(snapshot->duration_ready);
    snapshot->order = nullptr;
    snapshot->physical_to_logical = nullptr;
    snapshot->duration_cache = nullptr;
    snapshot->duration_ready = nullptr;
}

bool snapshot_is_current_locked(const SortLocationSnapshot &snapshot)
{
    if (!s_status.mounted || !s_catalog ||
        s_status.catalog_generation != snapshot.catalog_generation ||
        s_catalog_header.checksum != snapshot.header.checksum ||
        !same_setting(current_sort_setting(snapshot.section), snapshot.setting)) {
        return false;
    }
    if (snapshot.setting.field == SortField::Duration &&
        (s_status.duration_indexing ||
         s_status.duration_generation != snapshot.duration_generation)) {
        return false;
    }
    return true;
}

bool read_catalog_records(const SortLocationSnapshot &snapshot, uint64_t offset,
                          void *destination, size_t item_size, size_t count)
{
    if (!destination || item_size == 0 || count == 0 ||
        offset + static_cast<uint64_t>(item_size) * count > snapshot.header.file_size ||
        offset > static_cast<uint64_t>(LONG_MAX)) return false;
    Lock lock;
    if (!snapshot_is_current_locked(snapshot) || s_status.scanning || !s_catalog ||
        std::fseek(s_catalog, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(destination, item_size, count, s_catalog) == count;
}

bool load_jump_cache(const SortLocationSnapshot &snapshot,
                     SortLocation *locations, size_t capacity, size_t *count)
{
    FILE *file = std::fopen(jump_cache_path(snapshot.section), "rb");
    if (!file) return false;
    bool valid = false;
    JumpCacheHeader header{};
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long file_size = std::ftell(file);
        if (file_size >= static_cast<long>(sizeof(header)) &&
            std::fseek(file, 0, SEEK_SET) == 0 &&
            std::fread(&header, sizeof(header), 1, file) == 1) {
            const uint64_t expected_size = sizeof(header) +
                static_cast<uint64_t>(header.location_count) * sizeof(JumpCacheEntry);
            valid = std::memcmp(header.magic, kJumpCacheMagic, sizeof(header.magic)) == 0 &&
                    header.version == kJumpCacheVersion &&
                    header.catalog_checksum == snapshot.header.checksum &&
                    header.item_count == snapshot.item_count &&
                    header.section == static_cast<uint8_t>(snapshot.section) &&
                    header.sort_code == sort_setting_code(snapshot.setting) &&
                    header.location_count <= snapshot.item_count &&
                    header.location_count <= capacity &&
                    expected_size == static_cast<uint64_t>(file_size);
        }
    }

    uint32_t checksum = 2166136261u;
    uint32_t previous_position = 0;
    bool has_previous = false;
    size_t loaded = 0;
    JumpCacheEntry block[64]{};
    while (valid && loaded < header.location_count) {
        const size_t amount = std::min<size_t>(64, header.location_count - loaded);
        if (std::fread(block, sizeof(JumpCacheEntry), amount, file) != amount) {
            valid = false;
            break;
        }
        checksum = checksum_update(checksum, block, amount * sizeof(JumpCacheEntry));
        for (size_t i = 0; i < amount; ++i) {
            const JumpCacheEntry &saved = block[i];
            if (saved.item_position >= snapshot.item_count ||
                (has_previous && saved.item_position <= previous_position) ||
                saved.kind != static_cast<uint32_t>(location_kind(snapshot.setting.field)) ||
                (saved.kind == static_cast<uint32_t>(SortLocationKind::Initial) &&
                 !std::memchr(saved.initial, '\0', sizeof(saved.initial)))) {
                valid = false;
                break;
            }
            locations[loaded] = {};
            locations[loaded].value = saved.value;
            locations[loaded].item_position = saved.item_position;
            std::memcpy(locations[loaded].initial, saved.initial,
                        sizeof(locations[loaded].initial));
            locations[loaded].kind = static_cast<SortLocationKind>(saved.kind);
            previous_position = saved.item_position;
            has_previous = true;
            ++loaded;
        }
    }
    if (valid && checksum != header.payload_checksum) valid = false;
    std::fclose(file);
    if (!valid) return false;
    *count = loaded;
    return true;
}

void save_jump_cache(const SortLocationSnapshot &snapshot,
                     const SortLocation *locations, size_t count)
{
    if (!ensure_directory(kDataDir)) return;
    const char *path = jump_cache_path(snapshot.section);
    const char *temporary = jump_cache_temp_path(snapshot.section);
    std::remove(temporary);
    FILE *file = std::fopen(temporary, "wb");
    if (!file) return;

    JumpCacheHeader header{};
    std::memcpy(header.magic, kJumpCacheMagic, sizeof(header.magic));
    header.version = kJumpCacheVersion;
    header.catalog_checksum = snapshot.header.checksum;
    header.item_count = static_cast<uint32_t>(snapshot.item_count);
    header.location_count = static_cast<uint32_t>(count);
    header.section = static_cast<uint8_t>(snapshot.section);
    header.sort_code = sort_setting_code(snapshot.setting);
    bool written = std::fwrite(&header, sizeof(header), 1, file) == 1;
    uint32_t checksum = 2166136261u;
    for (size_t offset = 0; written && offset < count;) {
        JumpCacheEntry block[64]{};
        const size_t amount = std::min<size_t>(64, count - offset);
        for (size_t i = 0; i < amount; ++i) {
            const SortLocation &location = locations[offset + i];
            block[i].value = location.value;
            block[i].item_position = location.item_position;
            std::memcpy(block[i].initial, location.initial, sizeof(block[i].initial));
            block[i].kind = static_cast<uint32_t>(location.kind);
        }
        checksum = checksum_update(checksum, block, amount * sizeof(JumpCacheEntry));
        written = std::fwrite(block, sizeof(JumpCacheEntry), amount, file) == amount;
        offset += amount;
    }
    header.payload_checksum = checksum;
    written = written && std::fseek(file, 0, SEEK_SET) == 0 &&
              std::fwrite(&header, sizeof(header), 1, file) == 1 &&
              std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(temporary);
        return;
    }
    std::remove(path);
    if (std::rename(temporary, path) != 0) std::remove(temporary);
}

bool prepare_snapshot_locked(SortLocationSnapshot *snapshot,
                             SortSection section, SortSetting setting,
                             uint32_t catalog_generation,
                             uint32_t catalog_checksum)
{
    if (!snapshot || !s_status.mounted || !s_catalog || s_status.sorting_indexing ||
        s_status.catalog_generation != catalog_generation ||
        s_catalog_header.checksum != catalog_checksum ||
        !same_setting(current_sort_setting(section), setting) ||
        (setting.field == SortField::Duration && s_status.duration_indexing)) {
        return false;
    }

    const bool songs = section == SortSection::Songs;
    const size_t item_count = songs ? s_track_count :
        section == SortSection::Albums ? s_catalog_header.album_group_count :
                                         s_catalog_header.artist_group_count;
    const bool identity_order = setting.field == SortField::Title &&
                                setting.direction == SortDirection::Ascending;
    if (!identity_order) {
        const bool ready = songs ? prepare_song_sort_order_locked() :
                                   prepare_group_sort_order_locked(section);
        if (!ready) {
            start_sort_cache_indexing_locked(section);
            return false;
        }
    }

    snapshot->section = section;
    snapshot->setting = setting;
    snapshot->item_count = item_count;
    snapshot->header = s_catalog_header;
    snapshot->catalog_generation = s_status.catalog_generation;
    snapshot->duration_generation = s_status.duration_generation;
    snapshot->identity_order = identity_order;
    if (needs_track_keys(*snapshot) && s_track_count) {
        snapshot->physical_to_logical = static_cast<uint32_t *>(jump_alloc(
            s_track_count * sizeof(uint32_t)));
        if (!snapshot->physical_to_logical || !s_physical_to_logical) return false;
        std::memcpy(snapshot->physical_to_logical, s_physical_to_logical,
                    s_track_count * sizeof(uint32_t));
    }
    if (!identity_order && item_count) {
        const uint32_t *source_order = songs ? s_song_sort_order :
            section == SortSection::Albums ? s_album_sort_order : s_artist_sort_order;
        snapshot->order = static_cast<uint32_t *>(jump_alloc(
            item_count * sizeof(uint32_t)));
        if (!snapshot->order || !source_order) return false;
        std::memcpy(snapshot->order, source_order, item_count * sizeof(uint32_t));
    }
    if (setting.field == SortField::Duration && s_track_count &&
        s_duration_cache && s_duration_ready) {
        snapshot->duration_cache = static_cast<uint32_t *>(jump_alloc(
            s_track_count * sizeof(uint32_t)));
        snapshot->duration_ready = static_cast<uint8_t *>(jump_alloc(s_track_count));
        if (!snapshot->duration_cache || !snapshot->duration_ready) return false;
        std::memcpy(snapshot->duration_cache, s_duration_cache,
                    s_track_count * sizeof(uint32_t));
        std::memcpy(snapshot->duration_ready, s_duration_ready, s_track_count);
    }
    return true;
}

bool fill_track_keys(SortLocationSnapshot &snapshot, JumpKey *keys)
{
    if (snapshot.header.track_count == 0) return true;
    if (!keys || !snapshot.physical_to_logical) return false;
    constexpr size_t kTrackReadBatch = 16;
    auto *track_batch = static_cast<Track *>(jump_alloc(
        kTrackReadBatch * sizeof(Track)));
    if (!track_batch) return false;
    for (uint32_t first = 0; first < snapshot.header.track_count;) {
        const size_t amount = std::min<size_t>(kTrackReadBatch,
                                               snapshot.header.track_count - first);
        const uint64_t offset = static_cast<uint64_t>(snapshot.header.records_offset) +
                                static_cast<uint64_t>(first) * sizeof(Track);
        if (!read_catalog_records(snapshot, offset, track_batch, sizeof(Track), amount)) {
            heap_caps_free(track_batch);
            return false;
        }
        for (size_t i = 0; i < amount; ++i) {
            const uint32_t physical = first + static_cast<uint32_t>(i);
            Track &track = track_batch[i];
            const uint32_t logical = snapshot.physical_to_logical[physical];
            if (logical >= snapshot.header.track_count) {
                heap_caps_free(track_batch);
                return false;
            }
            if (snapshot.duration_cache && snapshot.duration_ready &&
                snapshot.duration_ready[physical]) {
                track.duration_ms = snapshot.duration_cache[physical];
            }
            JumpKey &key = keys[logical];
            switch (snapshot.setting.field) {
            case SortField::Title:
                make_initial(track.title, key.initial, sizeof(key.initial));
                break;
            case SortField::Album:
                make_initial(track.album, key.initial, sizeof(key.initial));
                break;
            case SortField::Artist:
                make_initial(track.artist, key.initial, sizeof(key.initial));
                break;
            case SortField::TrackNumber:
                key.value = track.track_number;
                break;
            case SortField::Duration:
                key.value = track.duration_ms;
                break;
            case SortField::DateModified:
                key.value = track.modified_time;
                break;
            }
        }
        first += static_cast<uint32_t>(amount);
    }
    heap_caps_free(track_batch);
    return true;
}

bool fill_group_keys(SortLocationSnapshot &snapshot, const JumpKey *track_keys,
                     GroupJumpKey *group_keys)
{
    const bool albums = snapshot.section == SortSection::Albums;
    const uint32_t group_count = albums ? snapshot.header.album_group_count :
                                          snapshot.header.artist_group_count;
    if (group_count == 0) return true;
    const uint32_t group_offset = albums ? snapshot.header.album_group_offset :
                                           snapshot.header.artist_group_offset;
    const bool track_keys_required = !group_initial_uses_name(
        snapshot.section, snapshot.setting.field) ||
        location_kind(snapshot.setting.field) != SortLocationKind::Initial;
    if (!group_keys || (track_keys_required && !track_keys) ||
        snapshot.header.track_count == 0) return false;

    constexpr size_t kGroupReadBatch = 16;
    GroupRecord group_batch[kGroupReadBatch]{};
    for (uint32_t first = 0; first < group_count;) {
        const size_t amount = std::min<size_t>(kGroupReadBatch, group_count - first);
        const uint64_t offset = static_cast<uint64_t>(group_offset) +
                                static_cast<uint64_t>(first) * sizeof(GroupRecord);
        if (!read_catalog_records(snapshot, offset, group_batch, sizeof(GroupRecord), amount)) {
            return false;
        }
        for (size_t i = 0; i < amount; ++i) {
            const uint32_t index = first + static_cast<uint32_t>(i);
            const GroupRecord &group = group_batch[i];
            if (group.track_count > snapshot.header.track_count ||
                group.representative_track >= snapshot.header.track_count) return false;
            group_keys[index].track_count = group.track_count;
            group_keys[index].members_offset = group.members_offset;
            if (location_kind(snapshot.setting.field) != SortLocationKind::Initial) continue;

            const bool use_group_name = group_initial_uses_name(
                snapshot.section, snapshot.setting.field);
            if (use_group_name) {
                make_initial(group.name, group_keys[index].initial,
                             sizeof(group_keys[index].initial));
            } else {
                std::memcpy(group_keys[index].initial,
                            track_keys[group.representative_track].initial,
                            sizeof(group_keys[index].initial));
            }
        }
        first += static_cast<uint32_t>(amount);
    }

    if (location_kind(snapshot.setting.field) == SortLocationKind::Initial) return true;
    uint64_t member_offset = static_cast<uint64_t>(group_offset) +
                             static_cast<uint64_t>(group_count) * sizeof(GroupRecord);
    if (member_offset > snapshot.header.file_size) return false;
    for (uint32_t group_index = 0; group_index < group_count; ++group_index) {
        GroupJumpKey &key = group_keys[group_index];
        if (key.members_offset != member_offset) return false;
        uint32_t minimum_track = UINT32_MAX;
        uint64_t total_duration = 0;
        uint64_t latest_modified = 0;
        constexpr size_t kMemberReadBatch = 32;
        uint32_t logical_batch[kMemberReadBatch]{};
        for (uint32_t first = 0; first < key.track_count;) {
            const size_t amount = std::min<size_t>(kMemberReadBatch,
                                                   key.track_count - first);
            if (!read_catalog_records(snapshot, member_offset, logical_batch,
                                      sizeof(uint32_t), amount)) return false;
            for (size_t i = 0; i < amount; ++i) {
                const uint32_t logical = logical_batch[i];
                if (logical >= snapshot.header.track_count) return false;
                const uint64_t value = track_keys[logical].value;
                switch (snapshot.setting.field) {
                case SortField::TrackNumber:
                    if (value && value < minimum_track) {
                        minimum_track = static_cast<uint32_t>(value);
                    }
                    break;
                case SortField::Duration:
                    total_duration = std::min<uint64_t>(UINT64_MAX - value,
                                                        total_duration) + value;
                    break;
                case SortField::DateModified:
                    latest_modified = std::max(latest_modified, value);
                    break;
                case SortField::Title:
                case SortField::Album:
                case SortField::Artist:
                    break;
                }
                member_offset += sizeof(uint32_t);
            }
            first += static_cast<uint32_t>(amount);
        }
        switch (snapshot.setting.field) {
        case SortField::TrackNumber:
            key.value = minimum_track == UINT32_MAX ? 0 : minimum_track;
            break;
        case SortField::Duration:
            key.value = total_duration;
            break;
        case SortField::DateModified:
            key.value = latest_modified;
            break;
        case SortField::Title:
        case SortField::Album:
        case SortField::Artist:
            break;
        }
    }
    return member_offset <= snapshot.header.file_size;
}

bool key_is_new(SortLocationKind kind, const JumpKey &key,
                uint32_t *letters_seen, bool *other_seen,
                char *previous_non_ascii, size_t previous_capacity,
                bool *has_previous_non_ascii, bool *has_previous_value,
                bool *has_previous_date, uint64_t *previous_value)
{
    if (kind == SortLocationKind::Date) {
        const uint32_t date = local_date_key(key.value);
        const bool is_new = !*has_previous_date || date != *previous_value;
        *has_previous_date = true;
        *previous_value = date;
        return is_new;
    }
    if (kind == SortLocationKind::Duration) {
        const uint64_t seconds = key.value / 1000u;
        const bool is_new = !*has_previous_value || seconds != *previous_value;
        *has_previous_value = true;
        *previous_value = seconds;
        return is_new;
    }
    if (kind == SortLocationKind::TrackNumber) {
        const bool is_new = !*has_previous_value || key.value != *previous_value;
        *has_previous_value = true;
        *previous_value = key.value;
        return is_new;
    }

    const char *initial = key.initial;
    if (initial[0] >= 'A' && initial[0] <= 'Z' && initial[1] == '\0') {
        const uint32_t bit = 1u << static_cast<unsigned>(initial[0] - 'A');
        const bool is_new = (*letters_seen & bit) == 0;
        *letters_seen |= bit;
        return is_new;
    }
    if (std::strcmp(initial, "#") == 0) {
        const bool is_new = !*other_seen;
        *other_seen = true;
        return is_new;
    }
    const bool is_new = !*has_previous_non_ascii ||
                        std::strcmp(initial, previous_non_ascii) != 0;
    std::snprintf(previous_non_ascii, previous_capacity, "%s", initial);
    *has_previous_non_ascii = true;
    return is_new;
}

esp_err_t build_locations(SortLocationSnapshot &snapshot,
                          SortLocation *locations, size_t capacity,
                          size_t *location_count)
{
    const size_t tracks = snapshot.header.track_count;
    const bool groups = snapshot.section != SortSection::Songs;
    const bool need_tracks = needs_track_keys(snapshot);
    auto *track_keys = need_tracks ? static_cast<JumpKey *>(
        jump_calloc(tracks, sizeof(JumpKey))) : nullptr;
    const size_t group_count = snapshot.item_count;
    auto *group_keys = groups ? static_cast<GroupJumpKey *>(
        jump_calloc(group_count, sizeof(GroupJumpKey))) : nullptr;
    if ((need_tracks && tracks && !track_keys) ||
        (groups && group_count && !group_keys)) {
        heap_caps_free(track_keys);
        heap_caps_free(group_keys);
        return ESP_ERR_NO_MEM;
    }

    bool success = (!need_tracks || fill_track_keys(snapshot, track_keys)) &&
        (!groups || fill_group_keys(snapshot, track_keys, group_keys));
    if (!success) {
        heap_caps_free(track_keys);
        heap_caps_free(group_keys);
        return ESP_FAIL;
    }

    const SortLocationKind kind = location_kind(snapshot.setting.field);
    uint32_t letters_seen = 0;
    bool other_seen = false;
    bool has_previous_non_ascii = false;
    bool has_previous_value = false;
    bool has_previous_date = false;
    uint64_t previous_value = 0;
    char previous_non_ascii[8] = "";
    size_t found = 0;
    for (size_t position = 0; position < snapshot.item_count; ++position) {
        const size_t source_index = snapshot.identity_order ? position : snapshot.order[position];
        if (source_index >= snapshot.item_count) {
            success = false;
            break;
        }
        JumpKey key{};
        if (groups) {
            key.value = group_keys[source_index].value;
            if (kind == SortLocationKind::Initial) {
                std::memcpy(key.initial, group_keys[source_index].initial,
                            sizeof(key.initial));
            }
        } else {
            key = track_keys[source_index];
        }
        if (!key_is_new(kind, key, &letters_seen, &other_seen,
                        previous_non_ascii, sizeof(previous_non_ascii),
                        &has_previous_non_ascii, &has_previous_value,
                        &has_previous_date, &previous_value)) continue;
        if (found >= capacity) {
            success = false;
            break;
        }
        locations[found] = {};
        locations[found].value = key.value;
        locations[found].item_position = static_cast<uint32_t>(position);
        locations[found].kind = kind;
        if (kind == SortLocationKind::Initial) {
            std::memcpy(locations[found].initial, key.initial,
                        sizeof(locations[found].initial));
        }
        ++found;
    }
    heap_caps_free(track_keys);
    heap_caps_free(group_keys);
    if (!success) return ESP_FAIL;
    *location_count = found;
    return ESP_OK;
}

} // namespace

esp_err_t sort_locations(SortSection section, SortLocation *locations,
                         size_t capacity, size_t *count)
{
    if (!locations || !count || static_cast<uint8_t>(section) >= 3) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    SortLocationSnapshot cache_identity{};
    {
        Lock lock;
        if (!s_status.mounted || !s_catalog || s_status.scanning) {
            return ESP_ERR_INVALID_STATE;
        }
        cache_identity.section = section;
        cache_identity.setting = current_sort_setting(section);
        cache_identity.item_count = section == SortSection::Songs ? s_track_count :
            section == SortSection::Albums ? s_catalog_header.album_group_count :
                                             s_catalog_header.artist_group_count;
        cache_identity.header = s_catalog_header;
        cache_identity.catalog_generation = s_status.catalog_generation;
        cache_identity.duration_generation = s_status.duration_generation;
        if (capacity < cache_identity.item_count ||
            (cache_identity.setting.field == SortField::Duration &&
             s_status.duration_indexing)) return ESP_ERR_INVALID_STATE;
    }

    if (load_jump_cache(cache_identity, locations, capacity, count)) {
        Lock lock;
        if (snapshot_is_current_locked(cache_identity)) return ESP_OK;
        *count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    SortLocationSnapshot snapshot{};
    bool prepared = false;
    {
        Lock lock;
        if (!snapshot_is_current_locked(cache_identity)) return ESP_ERR_INVALID_STATE;
        prepared = prepare_snapshot_locked(&snapshot, section,
            cache_identity.setting, cache_identity.catalog_generation,
            cache_identity.header.checksum);
    }
    if (!prepared) {
        free_snapshot(&snapshot);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = build_locations(snapshot, locations, capacity, count);
    if (result == ESP_OK) {
        Lock lock;
        if (!snapshot_is_current_locked(snapshot)) result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK) save_jump_cache(snapshot, locations, *count);
    free_snapshot(&snapshot);
    if (result != ESP_OK) *count = 0;
    return result;
}

} // namespace lyra::media
