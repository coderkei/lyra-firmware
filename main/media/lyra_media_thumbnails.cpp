/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

// Album-list thumbnails.
//
// Decoding a cover means opening the song, walking its tags and reading the
// whole compressed picture, often hundreds of kilobytes, before the decoder
// can throw most of it away. That cost is what made per-row artwork too slow,
// so it is paid once per album: the 48x48 RGB565 result is appended to a
// packed SD cache. The cache's small index lives in PSRAM, so a later visit
// needs only one open and one 4.5 KB read. A PSRAM LRU holds the most
// recently shown thumbnails for instant redisplay while scrolling back.
//
// Cache files (removed by Clear artwork cache):
//   covers/thumbs-v1-48.idx     IndexEntry records, appended
//   covers/thumbs-v1-48.rgb565  kThumbnailPixels RGB565 values per slot
// The pixel slot is written before its index entry, so an interrupted write
// leaves at most an unreferenced slot. A later entry for a key supersedes
// earlier ones, and the files restart when the index fills.

#include "lyra_media_internal.h"

#include <atomic>

namespace lyra::media {
using namespace internal;

namespace {

constexpr const char *kThumbTag = "lyra.media.thumbs";
constexpr const char *kIndexPath = "/sdcard/.lyra/covers/thumbs-v1-48.idx";
constexpr const char *kPixelsPath = "/sdcard/.lyra/covers/thumbs-v1-48.rgb565";
constexpr size_t kThumbnailBytes = kThumbnailPixels * sizeof(uint16_t);
constexpr size_t kMaxIndexEntries = 4096;
constexpr uint32_t kNoSlot = UINT32_MAX;
// Two screens of rows plus look-ahead; about 150 KB of PSRAM while a list is open.
constexpr size_t kRamEntries = 32;
constexpr size_t kMaxWanted = 32;
constexpr uint32_t kWorkerStack = 24 * 1024;
constexpr TickType_t kPlayerArtworkBackoff = pdMS_TO_TICKS(40);

struct IndexEntry {
    uint32_t key;    // artwork::key() of the album's representative track.
    uint32_t stamp;  // That track's size and modification time.
    uint32_t slot;   // Pixel slot, or kNoSlot when the album has no artwork.
};
static_assert(sizeof(IndexEntry) == 12, "thumbnail index layout is persisted");

struct RamEntry {
    bool used;
    uint32_t catalog_generation;
    size_t group;
    ThumbnailState state;
    uint32_t last_used;
};

// ---- RAM state, guarded by s_ram_mutex. Never held during SD or decode work.
SemaphoreHandle_t s_ram_mutex;
RamEntry s_ram[kRamEntries];
uint16_t *s_ram_pixels;  // kRamEntries * kThumbnailPixels, PSRAM.
uint32_t s_ram_clock;
size_t s_wanted[kMaxWanted];
size_t s_wanted_count;
size_t s_wanted_cursor;
uint32_t s_wanted_generation;
TaskHandle_t s_worker;
std::atomic<uint32_t> s_ready_generation{0};

// ---- SD cache state, guarded by s_disk_mutex. Lock order: media, disk, RAM.
SemaphoreHandle_t s_disk_mutex;
IndexEntry *s_index;  // PSRAM.
size_t s_index_count;
uint32_t s_pixel_slots;
bool s_index_loaded;
// The index describes the card that was mounted for this catalog generation.
uint32_t s_index_generation;

struct Mutex {
    explicit Mutex(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Mutex() { xSemaphoreGive(mutex_); }
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
private:
    SemaphoreHandle_t mutex_;
};

bool ensure_mutexes()
{
    // Created by init_thumbnails() during media start-up.
    return s_ram_mutex && s_disk_mutex;
}

uint32_t track_stamp(const Track &track)
{
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](const void *data, size_t length) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    };
    mix(&track.size_bytes, sizeof(track.size_bytes));
    mix(&track.modified_time, sizeof(track.modified_time));
    return hash;
}

// ---- RAM cache ---------------------------------------------------------------

RamEntry *find_ram_locked(uint32_t generation, size_t group)
{
    for (RamEntry &entry : s_ram) {
        if (entry.used && entry.catalog_generation == generation && entry.group == group) return &entry;
    }
    return nullptr;
}

bool wanted_locked(size_t group)
{
    for (size_t i = 0; i < s_wanted_count; ++i) {
        if (s_wanted[i] == group) return true;
    }
    return false;
}

void store_ram(uint32_t generation, size_t group, ThumbnailState state, const uint16_t *pixels)
{
    Mutex lock(s_ram_mutex);
    if (!s_ram_pixels) return;  // Released while this one was loading.
    RamEntry *entry = find_ram_locked(generation, group);
    if (!entry) {
        // Evict the least recently used entry, sparing ones still on screen.
        RamEntry *victim = nullptr;
        for (RamEntry &candidate : s_ram) {
            if (!candidate.used) { victim = &candidate; break; }
            const bool spare = candidate.catalog_generation == s_wanted_generation &&
                               wanted_locked(candidate.group);
            if (spare) continue;
            if (!victim || candidate.last_used < victim->last_used) victim = &candidate;
        }
        if (!victim) victim = &s_ram[0];
        entry = victim;
    }
    const size_t index = static_cast<size_t>(entry - s_ram);
    *entry = {true, generation, group, state, ++s_ram_clock};
    if (state == ThumbnailState::Ready && pixels) {
        std::memcpy(s_ram_pixels + index * kThumbnailPixels, pixels, kThumbnailBytes);
    }
    s_ready_generation.fetch_add(1, std::memory_order_release);
}

// Returns the next wanted group without a RAM entry, in priority order.
bool next_wanted(size_t *group, uint32_t *generation)
{
    Mutex lock(s_ram_mutex);
    while (s_wanted_cursor < s_wanted_count) {
        const size_t candidate = s_wanted[s_wanted_cursor];
        if (find_ram_locked(s_wanted_generation, candidate)) {
            ++s_wanted_cursor;
            continue;
        }
        *group = candidate;
        *generation = s_wanted_generation;
        return true;
    }
    return false;
}

void skip_wanted(size_t group, uint32_t generation)
{
    Mutex lock(s_ram_mutex);
    if (generation == s_wanted_generation && s_wanted_cursor < s_wanted_count &&
        s_wanted[s_wanted_cursor] == group) {
        ++s_wanted_cursor;
    }
}

// ---- SD cache ----------------------------------------------------------------

void reset_disk_locked(bool remove_files)
{
    s_index_count = 0;
    s_pixel_slots = 0;
    s_index_loaded = remove_files;  // Files are gone, so the empty index is current.
    if (remove_files) {
        lyra::sd::remove(kIndexPath, lyra::sd::Client::Artwork);
        lyra::sd::remove(kPixelsPath, lyra::sd::Client::Artwork);
    }
}

bool load_index_locked(uint32_t generation)
{
    if (s_index_loaded && s_index_generation != generation) {
        // A rescan or another card: reread rather than trust stale slots.
        s_index_loaded = false;
    }
    s_index_generation = generation;
    if (s_index_loaded) return s_index != nullptr;
    if (!s_index) {
        s_index = static_cast<IndexEntry *>(heap_caps_malloc(
            kMaxIndexEntries * sizeof(IndexEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_index) return false;
    }
    s_index_count = 0;
    s_pixel_slots = 0;
    s_index_loaded = true;

    struct stat info{};
    if (stat(kPixelsPath, &info) == 0 && S_ISREG(info.st_mode)) {
        s_pixel_slots = static_cast<uint32_t>(info.st_size / kThumbnailBytes);
    }
    FILE *file = lyra::sd::open(kIndexPath, "rb", lyra::sd::Client::Artwork);
    if (!file) return true;
    const size_t read = lyra::sd::read(file, s_index, kMaxIndexEntries * sizeof(IndexEntry),
                                       lyra::sd::Client::Artwork);
    lyra::sd::close(file, lyra::sd::Client::Artwork);
    // Drop a torn trailing record and entries whose pixels never landed.
    const size_t records = read / sizeof(IndexEntry);
    for (size_t i = 0; i < records; ++i) {
        const IndexEntry &entry = s_index[i];
        if (entry.slot != kNoSlot && entry.slot >= s_pixel_slots) continue;
        s_index[s_index_count++] = entry;
    }
    ESP_LOGI(kThumbTag, "thumbnail cache: %u entries, %u images",
             static_cast<unsigned>(s_index_count), static_cast<unsigned>(s_pixel_slots));
    return true;
}

const IndexEntry *find_index_locked(uint32_t key, uint32_t stamp)
{
    for (size_t i = s_index_count; i > 0; --i) {
        const IndexEntry &entry = s_index[i - 1];
        if (entry.key == key) return entry.stamp == stamp ? &entry : nullptr;
    }
    return nullptr;
}

// Looks the album up in the SD cache. Returns false when it is not cached.
bool read_cached(uint32_t generation, uint32_t key, uint32_t stamp, uint16_t *pixels,
                 ThumbnailState *state)
{
    Mutex lock(s_disk_mutex);
    if (!load_index_locked(generation)) return false;
    const IndexEntry *entry = find_index_locked(key, stamp);
    if (!entry) return false;
    if (entry->slot == kNoSlot) {
        *state = ThumbnailState::None;
        return true;
    }
    FILE *file = lyra::sd::open(kPixelsPath, "rb", lyra::sd::Client::Artwork);
    const bool read = file &&
        lyra::sd::seek(file, static_cast<long>(entry->slot) * static_cast<long>(kThumbnailBytes),
                       SEEK_SET, lyra::sd::Client::Artwork) == 0 &&
        lyra::sd::read_exact(file, pixels, kThumbnailBytes, lyra::sd::Client::Artwork);
    if (file) lyra::sd::close(file, lyra::sd::Client::Artwork);
    if (!read) return false;
    *state = ThumbnailState::Ready;
    return true;
}

bool append_file(const char *path, const void *data, size_t length)
{
    FILE *file = lyra::sd::open(path, "ab", lyra::sd::Client::Artwork);
    if (!file) return false;
    const bool written = lyra::sd::write_exact(file, data, length, lyra::sd::Client::Artwork);
    const bool closed = lyra::sd::close(file, lyra::sd::Client::Artwork) == 0;
    return written && closed;
}

void write_cached(uint32_t generation, uint32_t key, uint32_t stamp, const uint16_t *pixels)
{
    Mutex lock(s_disk_mutex);
    if (!load_index_locked(generation)) return;
    if (!ensure_directory(kDataDir) || !ensure_directory(kArtworkDir)) return;
    if (s_index_count >= kMaxIndexEntries) {
        ESP_LOGI(kThumbTag, "thumbnail cache full; starting a new one");
        reset_disk_locked(true);
    }
    IndexEntry entry{key, stamp, kNoSlot};
    if (pixels) {
        entry.slot = s_pixel_slots;
        if (!append_file(kPixelsPath, pixels, kThumbnailBytes)) {
            // A partial slot would misalign every later one.
            reset_disk_locked(true);
            return;
        }
        ++s_pixel_slots;
    }
    if (!append_file(kIndexPath, &entry, sizeof(entry))) {
        reset_disk_locked(true);
        return;
    }
    s_index[s_index_count++] = entry;
}

// ---- Worker ------------------------------------------------------------------

enum class LoadResult : uint8_t { Done, Retry, Abandon };

LoadResult load_thumbnail(size_t group_index, uint32_t generation, uint16_t *scratch)
{
    const Status media = status();
    if (!media.mounted || media.scanning || media.catalog_generation != generation) {
        return LoadResult::Abandon;
    }
    Group group{};
    Track track{};
    if (!group_at(GroupKind::Album, group_index, &group) ||
        !track_at(group.representative_track, &track) || !track.path[0]) {
        store_ram(generation, group_index, ThumbnailState::None, nullptr);
        return LoadResult::Done;
    }
    const uint32_t key = artwork::key(track);
    const uint32_t stamp = track_stamp(track);
    if (!begin_file_access()) return LoadResult::Abandon;

    ThumbnailState state = ThumbnailState::None;
    if (read_cached(generation, key, stamp, scratch, &state)) {
        end_file_access();
        store_ram(generation, group_index, state, scratch);
        return LoadResult::Done;
    }
    // The cover the player is waiting for goes first.
    if (status().artwork_busy) {
        end_file_access();
        return LoadResult::Retry;
    }
    const int64_t started = esp_timer_get_time();
    ArtworkDiagnostics diagnostics{};
    uint16_t *pixels = nullptr;
    // SD workspace lets very large progressive JPEGs decode; it is slow but
    // happens once per album, after which the cached thumbnail is used.
    const bool decoded = artwork::extract_and_decode(track, &pixels, kThumbnailSize,
                                                     &diagnostics, true, nullptr);
    write_cached(generation, key, stamp, decoded ? pixels : nullptr);
    end_file_access();
    ESP_LOGD(kThumbTag, "album %u thumbnail %s in %u ms", static_cast<unsigned>(group_index),
             decoded ? "decoded" : "unavailable",
             static_cast<unsigned>((esp_timer_get_time() - started) / 1000));
    store_ram(generation, group_index, decoded ? ThumbnailState::Ready : ThumbnailState::None,
              pixels);
    heap_caps_free(pixels);
    return LoadResult::Done;
}

void thumbnail_task(void *)
{
    auto *scratch = static_cast<uint16_t *>(heap_caps_malloc(
        kThumbnailBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t group = 0;
        uint32_t generation = 0;
        while (scratch && next_wanted(&group, &generation)) {
            const LoadResult result = load_thumbnail(group, generation, scratch);
            if (result == LoadResult::Retry) {
                vTaskDelay(kPlayerArtworkBackoff);
            } else if (result == LoadResult::Abandon) {
                // Nothing can load now; a new request will restart the walk.
                skip_wanted(group, generation);
                Mutex lock(s_ram_mutex);
                s_wanted_count = 0;
                s_wanted_cursor = 0;
            }
        }
    }
}

} // namespace

ThumbnailState album_thumbnail(size_t album_group, uint16_t *pixels)
{
    if (!pixels || !ensure_mutexes()) return ThumbnailState::Pending;
    Mutex lock(s_ram_mutex);
    RamEntry *entry = find_ram_locked(s_wanted_generation, album_group);
    if (!entry || !s_ram_pixels) return ThumbnailState::Pending;
    entry->last_used = ++s_ram_clock;
    if (entry->state == ThumbnailState::Ready) {
        const size_t index = static_cast<size_t>(entry - s_ram);
        std::memcpy(pixels, s_ram_pixels + index * kThumbnailPixels, kThumbnailBytes);
    }
    return entry->state;
}

void request_album_thumbnails(const size_t *album_groups, size_t count)
{
    if (!ensure_mutexes()) return;
    const Status media = status();
    if (!media.mounted || media.scanning) return;
    count = album_groups ? std::min(count, kMaxWanted) : 0;
    bool changed = false;
    {
        Mutex lock(s_ram_mutex);
        if (!s_ram_pixels) {
            s_ram_pixels = static_cast<uint16_t *>(heap_caps_malloc(
                kRamEntries * kThumbnailBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!s_ram_pixels) return;
            for (RamEntry &entry : s_ram) entry = {};
        }
        changed = media.catalog_generation != s_wanted_generation || count != s_wanted_count ||
                  !std::equal(album_groups, album_groups + count, s_wanted);
        if (changed) {
            std::copy(album_groups, album_groups + count, s_wanted);
            s_wanted_count = count;
            s_wanted_cursor = 0;
            s_wanted_generation = media.catalog_generation;
        }
    }
    if (!changed || !count) return;
    if (!s_worker &&
        xTaskCreatePinnedToCoreWithCaps(thumbnail_task, "lyra_thumbs", kWorkerStack, nullptr, 1,
                                        &s_worker, kArtworkCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_worker = nullptr;
        ESP_LOGW(kThumbTag, "could not start the thumbnail task");
        return;
    }
    xTaskNotifyGive(s_worker);
}

uint32_t thumbnail_generation()
{
    return s_ready_generation.load(std::memory_order_acquire);
}

void release_album_thumbnails()
{
    if (!ensure_mutexes()) return;
    uint16_t *pixels = nullptr;
    {
        Mutex lock(s_ram_mutex);
        s_wanted_count = 0;
        s_wanted_cursor = 0;
        pixels = s_ram_pixels;
        s_ram_pixels = nullptr;
        for (RamEntry &entry : s_ram) entry = {};
    }
    heap_caps_free(pixels);
}

namespace internal {

void init_thumbnails()
{
    static StaticSemaphore_t ram_storage;
    static StaticSemaphore_t disk_storage;
    if (!s_ram_mutex) s_ram_mutex = xSemaphoreCreateMutexStatic(&ram_storage);
    if (!s_disk_mutex) s_disk_mutex = xSemaphoreCreateMutexStatic(&disk_storage);
}

void invalidate_thumbnails()
{
    if (!ensure_mutexes()) return;
    {
        Mutex lock(s_disk_mutex);
        reset_disk_locked(true);
    }
    Mutex lock(s_ram_mutex);
    for (RamEntry &entry : s_ram) entry = {};
    s_wanted_cursor = 0;
    s_ready_generation.fetch_add(1, std::memory_order_release);
}

} // namespace internal

} // namespace lyra::media
