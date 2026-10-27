/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_gui_ime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "zlib.h"

namespace lyra::gui::ime {
namespace {

constexpr char kTag[] = "lyra.ime";
constexpr size_t kPreeditCapacity = 64;
constexpr size_t kMaxKeyBytes = 48;
constexpr size_t kMaxWordBytes = 64;
constexpr size_t kHeaderSize = 32;
constexpr size_t kIndexRecordSize = 65;
constexpr size_t kMaximumIndexEntries = 32768;
constexpr size_t kMaximumBlockBytes = 128 * 1024;
constexpr char kChineseDictionaryPath[] = "/sdcard/.lyra/ime/lyra_ime_zh.bin";
constexpr char kJapaneseDictionaryPath[] = "/sdcard/.lyra/ime/lyra_ime_ja.bin";

char s_preedit[kPreeditCapacity] = "";
char s_candidate_text[kMaxWordBytes + 1] = "";
int s_hangul_initial = -1;
int s_hangul_vowel = -1;
int s_hangul_final = 0;

struct DictionaryIndex {
    uint8_t first_key_length = 0;
    char first_key[kMaxKeyBytes + 1]{};
    uint32_t offset = 0;
    uint32_t compressed_size = 0;
    uint32_t raw_size = 0;
    uint32_t checksum = 0;
};

struct DictionaryState {
    Layout layout = Layout::Count;
    bool attempted = false;
    std::FILE *file = nullptr;
    DictionaryIndex *index = nullptr;
    size_t index_count = 0;
    uint8_t *compressed = nullptr;
    uint8_t *raw = nullptr;
    size_t compressed_capacity = 0;
    size_t raw_capacity = 0;
    size_t raw_size = 0;
    int cached_block = -1;
};

struct CandidateRecord {
    size_t block = 0;
    size_t values_offset = 0;
    size_t candidate_count = 0;
    size_t matched_bytes = 0;
    bool valid = false;
};

struct CandidateState {
    Layout layout = Layout::Count;
    char query[kPreeditCapacity] = "";
    CandidateRecord record{};
    bool ready = false;
    bool kana_fallback = false;
};

DictionaryState s_dictionary{};
CandidateState s_candidates{};

constexpr int64_t kT9CycleUs = 900 * 1000;
constexpr size_t kT9StrokeCapacity = 8;

// A multi-tap cycle replays from a snapshot taken before its first tap, so each
// repeat tap restores the snapshot and applies the next character of the key.
struct T9Cycle {
    bool active = false;
    Layout layout = Layout::Count;
    char key = 0;
    size_t index = 0;
    int64_t last_us = 0;
    bool upper = false;
    char *text = nullptr;
    size_t text_length = 0;
    char preedit[kPreeditCapacity] = "";
    int hangul_initial = -1;
    int hangul_vowel = -1;
    int hangul_final = 0;
    // Cheonjiin vowel strokes: I (ㅣ), D (ㆍ), E (ㅡ).
    char strokes[kT9StrokeCapacity] = "";
};

bool s_t9_mode = false;
bool s_t9_applying = false;
T9Cycle s_t9{};

uint32_t read_u32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t read_u16(const uint8_t *bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

void invalidate_candidates()
{
    s_candidates = {};
}

void release_dictionary()
{
    if (s_dictionary.file) std::fclose(s_dictionary.file);
    if (s_dictionary.index) heap_caps_free(s_dictionary.index);
    if (s_dictionary.compressed) heap_caps_free(s_dictionary.compressed);
    if (s_dictionary.raw) heap_caps_free(s_dictionary.raw);
    s_dictionary = {};
}

void *allocate_bytes(size_t size)
{
    if (!size) return nullptr;
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return memory;
}

int compare_bytes_order(const char *left, size_t left_size,
                        const char *right, size_t right_size)
{
    const size_t common = std::min(left_size, right_size);
    const int order = std::memcmp(left, right, common);
    if (order != 0) return order < 0 ? -1 : 1;
    if (left_size == right_size) return 0;
    return left_size < right_size ? -1 : 1;
}

bool load_dictionary(Layout layout)
{
    if (s_dictionary.layout == layout && s_dictionary.attempted) {
        return s_dictionary.file != nullptr;
    }
    release_dictionary();
    s_dictionary.layout = layout;
    s_dictionary.attempted = true;

    const char *path = nullptr;
    const char *expected_magic = nullptr;
    if (layout == Layout::ChinesePinyin) {
        path = kChineseDictionaryPath;
        expected_magic = "LYZHIME1";
    } else if (layout == Layout::JapaneseKana) {
        path = kJapaneseDictionaryPath;
        expected_magic = "LYJAIME1";
    } else {
        return false;
    }

    FILE *file = std::fopen(path, "rb");
    if (!file) {
        ESP_LOGW(kTag, "offline dictionary not found: %s", path);
        return false;
    }

    uint8_t header[kHeaderSize]{};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, expected_magic, 8) != 0 || read_u32(header + 8) != 1) {
        ESP_LOGW(kTag, "invalid offline dictionary header: %s", path);
        std::fclose(file);
        return false;
    }

    const size_t index_count = read_u32(header + 12);
    const size_t index_offset = read_u32(header + 16);
    const size_t data_offset = read_u32(header + 20);
    const size_t raw_capacity = read_u32(header + 24);
    const size_t expected_file_size = read_u32(header + 28);
    if (index_count == 0 || index_count > kMaximumIndexEntries ||
        index_offset != kHeaderSize || data_offset != kHeaderSize + index_count * kIndexRecordSize ||
        raw_capacity == 0 || raw_capacity > kMaximumBlockBytes ||
        std::fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGW(kTag, "invalid offline dictionary dimensions: %s", path);
        std::fclose(file);
        return false;
    }
    const long actual_file_size = std::ftell(file);
    if (actual_file_size < 0 || static_cast<size_t>(actual_file_size) != expected_file_size ||
        expected_file_size < data_offset || std::fseek(file, static_cast<long>(index_offset), SEEK_SET) != 0) {
        ESP_LOGW(kTag, "offline dictionary size mismatch: %s", path);
        std::fclose(file);
        return false;
    }

    auto *index = static_cast<DictionaryIndex *>(allocate_bytes(index_count * sizeof(DictionaryIndex)));
    if (!index) {
        ESP_LOGW(kTag, "not enough memory for offline dictionary index");
        std::fclose(file);
        return false;
    }
    std::memset(index, 0, index_count * sizeof(DictionaryIndex));

    size_t max_compressed_size = 0;
    bool valid = true;
    uint8_t encoded[kIndexRecordSize]{};
    for (size_t i = 0; i < index_count; ++i) {
        if (std::fread(encoded, 1, sizeof(encoded), file) != sizeof(encoded)) {
            valid = false;
            break;
        }
        DictionaryIndex &entry = index[i];
        entry.first_key_length = encoded[0];
        if (entry.first_key_length == 0 || entry.first_key_length > kMaxKeyBytes) {
            valid = false;
            break;
        }
        std::memcpy(entry.first_key, encoded + 1, entry.first_key_length);
        entry.first_key[entry.first_key_length] = '\0';
        entry.offset = read_u32(encoded + 49);
        entry.compressed_size = read_u32(encoded + 53);
        entry.raw_size = read_u32(encoded + 57);
        entry.checksum = read_u32(encoded + 61);
        const uint64_t end = static_cast<uint64_t>(entry.offset) + entry.compressed_size;
        if (entry.offset < data_offset || entry.compressed_size == 0 ||
            entry.raw_size == 0 || entry.raw_size > raw_capacity || end > expected_file_size ||
            (i && compare_bytes_order(index[i - 1].first_key,
                                      index[i - 1].first_key_length,
                                      entry.first_key,
                                      entry.first_key_length) >= 0)) {
            valid = false;
            break;
        }
        max_compressed_size = std::max(max_compressed_size,
                                       static_cast<size_t>(entry.compressed_size));
    }
    if (!valid) {
        heap_caps_free(index);
        std::fclose(file);
        ESP_LOGW(kTag, "corrupt offline dictionary index: %s", path);
        return false;
    }

    auto *compressed = static_cast<uint8_t *>(allocate_bytes(max_compressed_size));
    auto *raw = static_cast<uint8_t *>(allocate_bytes(raw_capacity));
    if (!compressed || !raw) {
        if (compressed) heap_caps_free(compressed);
        if (raw) heap_caps_free(raw);
        heap_caps_free(index);
        std::fclose(file);
        ESP_LOGW(kTag, "not enough memory for offline dictionary block cache");
        return false;
    }

    s_dictionary.file = file;
    s_dictionary.index = index;
    s_dictionary.index_count = index_count;
    s_dictionary.compressed = compressed;
    s_dictionary.raw = raw;
    s_dictionary.compressed_capacity = max_compressed_size;
    s_dictionary.raw_capacity = raw_capacity;
    ESP_LOGI(kTag, "loaded %s index (%u blocks)",
             layout == Layout::ChinesePinyin ? "Chinese" : "Japanese",
             static_cast<unsigned>(index_count));
    return true;
}

bool load_block(size_t block)
{
    if (block >= s_dictionary.index_count || !s_dictionary.file) return false;
    if (s_dictionary.cached_block == static_cast<int>(block)) return true;
    const DictionaryIndex &entry = s_dictionary.index[block];
    if (entry.compressed_size > s_dictionary.compressed_capacity ||
        entry.raw_size > s_dictionary.raw_capacity ||
        std::fseek(s_dictionary.file, static_cast<long>(entry.offset), SEEK_SET) != 0 ||
        std::fread(s_dictionary.compressed, 1, entry.compressed_size,
                   s_dictionary.file) != entry.compressed_size) {
        return false;
    }
    uLongf raw_size = static_cast<uLongf>(s_dictionary.raw_capacity);
    const int result = uncompress(s_dictionary.raw, &raw_size,
                                  s_dictionary.compressed,
                                  static_cast<uLong>(entry.compressed_size));
    if (result != Z_OK || raw_size != entry.raw_size ||
        static_cast<uint32_t>(crc32(0, s_dictionary.raw, static_cast<uInt>(raw_size))) !=
            entry.checksum) {
        ESP_LOGW(kTag, "offline dictionary block %u failed validation",
                 static_cast<unsigned>(block));
        return false;
    }
    s_dictionary.raw_size = static_cast<size_t>(raw_size);
    s_dictionary.cached_block = static_cast<int>(block);
    return true;
}

bool find_record(Layout layout, const char *key, size_t key_length,
                 CandidateRecord *record)
{
    if (!record || !key || key_length == 0 || key_length > kMaxKeyBytes ||
        !load_dictionary(layout)) {
        return false;
    }

    size_t low = 0;
    size_t high = s_dictionary.index_count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const DictionaryIndex &entry = s_dictionary.index[middle];
        const int order = compare_bytes_order(key, key_length,
                                              entry.first_key, entry.first_key_length);
        if (order >= 0) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return false;
    const size_t block = low - 1;
    if (!load_block(block)) return false;

    size_t offset = 0;
    while (offset + 3 <= s_dictionary.raw_size) {
        const size_t key_length_in_record = s_dictionary.raw[offset];
        const size_t count = read_u16(s_dictionary.raw + offset + 1);
        const size_t key_offset = offset + 3;
        if (key_length_in_record == 0 || key_length_in_record > kMaxKeyBytes ||
            key_offset + key_length_in_record > s_dictionary.raw_size) {
            return false;
        }
        const int order = compare_bytes_order(key, key_length,
            reinterpret_cast<const char *>(s_dictionary.raw + key_offset), key_length_in_record);
        const size_t values_offset = key_offset + key_length_in_record;
        if (order == 0) {
            record->block = block;
            record->values_offset = values_offset;
            record->candidate_count = count;
            record->matched_bytes = key_length;
            record->valid = count != 0;
            return record->valid;
        }
        if (order < 0) return false;
        offset = values_offset;
        for (size_t candidate_index = 0; candidate_index < count; ++candidate_index) {
            if (offset >= s_dictionary.raw_size) return false;
            const size_t surface_length = s_dictionary.raw[offset++];
            if (surface_length == 0 || surface_length > kMaxWordBytes ||
                offset + surface_length > s_dictionary.raw_size) {
                return false;
            }
            offset += surface_length;
        }
    }
    return false;
}

size_t previous_utf8_boundary(const char *text, size_t length)
{
    if (length == 0) return 0;
    size_t start = length - 1;
    while (start > 0 && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) --start;
    return start;
}

void prepare_candidates(Layout layout)
{
    if (s_candidates.ready && s_candidates.layout == layout &&
        std::strcmp(s_candidates.query, s_preedit) == 0) {
        return;
    }
    invalidate_candidates();
    s_candidates.layout = layout;
    std::strncpy(s_candidates.query, s_preedit, sizeof(s_candidates.query) - 1);
    s_candidates.query[sizeof(s_candidates.query) - 1] = '\0';
    s_candidates.ready = true;

    if ((layout != Layout::ChinesePinyin && layout != Layout::JapaneseKana) || !s_preedit[0]) {
        return;
    }

    size_t query_length = std::strlen(s_preedit);
    size_t probe_length = query_length;
    while (probe_length > 0) {
        CandidateRecord record{};
        if (find_record(layout, s_preedit, probe_length, &record)) {
            s_candidates.record = record;
            s_candidates.record.matched_bytes = probe_length;
            break;
        }
        if (layout == Layout::JapaneseKana) probe_length = previous_utf8_boundary(s_preedit, probe_length);
        else --probe_length;
    }

    if (layout == Layout::JapaneseKana) {
        if (!s_candidates.record.valid) s_candidates.record.matched_bytes = query_length;
        s_candidates.kana_fallback = true;
    }
}

size_t candidate_total(Layout layout)
{
    prepare_candidates(layout);
    return s_candidates.record.candidate_count + (s_candidates.kana_fallback ? 1 : 0);
}

bool copy_candidate(Layout layout, size_t index, char *output, size_t capacity)
{
    if (!output || capacity == 0) return false;
    prepare_candidates(layout);
    if (index >= candidate_total(layout)) return false;
    const CandidateRecord &record = s_candidates.record;
    if (s_candidates.kana_fallback && index == record.candidate_count) {
        const size_t length = std::min(record.matched_bytes, capacity - 1);
        std::memcpy(output, s_preedit, length);
        output[length] = '\0';
        return length != 0;
    }
    if (!record.valid || !load_block(record.block)) return false;
    size_t offset = record.values_offset;
    for (size_t current = 0; current <= index; ++current) {
        if (offset >= s_dictionary.raw_size) return false;
        const size_t surface_length = s_dictionary.raw[offset++];
        if (surface_length == 0 || surface_length > kMaxWordBytes ||
            offset + surface_length > s_dictionary.raw_size) {
            return false;
        }
        if (current == index) {
            if (surface_length >= capacity) return false;
            std::memcpy(output, s_dictionary.raw + offset, surface_length);
            output[surface_length] = '\0';
            return true;
        }
        offset += surface_length;
    }
    return false;
}

bool append(char *text, size_t capacity, const char *suffix)
{
    if (!text || capacity == 0 || !suffix) return false;
    const size_t used = std::strlen(text);
    const size_t count = std::strlen(suffix);
    if (used + count >= capacity) return false;
    std::memcpy(text + used, suffix, count + 1);
    return true;
}

void remove_last_codepoint(char *text)
{
    if (!text || !*text) return;
    size_t length = std::strlen(text) - 1;
    while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) --length;
    text[length] = '\0';
}

void remove_prefix(char *text, size_t count)
{
    if (!text || count == 0) return;
    const size_t length = std::strlen(text);
    if (count >= length) {
        text[0] = '\0';
        return;
    }
    std::memmove(text, text + count, length - count + 1);
}

size_t encode_utf8(uint32_t codepoint, char output[5])
{
    if (codepoint < 0x80) {
        output[0] = static_cast<char>(codepoint);
        output[1] = '\0';
        return 1;
    }
    if (codepoint < 0x800) {
        output[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        output[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        output[2] = '\0';
        return 2;
    }
    if (codepoint < 0x10000) {
        output[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        output[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        output[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        output[3] = '\0';
        return 3;
    }
    output[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    output[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    output[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    output[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    output[4] = '\0';
    return 4;
}

uint32_t decode_utf8(const char **cursor)
{
    const auto *text = reinterpret_cast<const uint8_t *>(*cursor);
    if (*text < 0x80) {
        if (*text) ++*cursor;
        return *text;
    }
    size_t length = 0;
    uint32_t value = 0;
    if ((*text & 0xE0) == 0xC0) { length = 2; value = *text & 0x1F; }
    else if ((*text & 0xF0) == 0xE0) { length = 3; value = *text & 0x0F; }
    else if ((*text & 0xF8) == 0xF0) { length = 4; value = *text & 0x07; }
    else { ++*cursor; return *text; }
    for (size_t index = 1; index < length; ++index) {
        if ((text[index] & 0xC0) != 0x80) { ++*cursor; return *text; }
        value = (value << 6) | (text[index] & 0x3F);
    }
    *cursor += length;
    return value;
}

bool replace_last_codepoint(char *text, size_t capacity, uint32_t replacement)
{
    if (!text || !capacity || !*text) return false;
    const size_t length = std::strlen(text);
    size_t start = length - 1;
    while (start > 0 && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) --start;
    const size_t old_bytes = length - start;
    char encoded[5];
    const size_t new_bytes = encode_utf8(replacement, encoded);
    if (length - old_bytes + new_bytes >= capacity) return false;
    std::memcpy(text + start, encoded, new_bytes + 1);
    return true;
}

void reset_hangul()
{
    s_hangul_initial = -1;
    s_hangul_vowel = -1;
    s_hangul_final = 0;
    s_preedit[0] = '\0';
}

int hangul_initial(uint32_t codepoint)
{
    switch (codepoint) {
        case 0x3131: return 0; case 0x3132: return 1; case 0x3134: return 2;
        case 0x3137: return 3; case 0x3138: return 4; case 0x3139: return 5;
        case 0x3141: return 6; case 0x3142: return 7; case 0x3143: return 8;
        case 0x3145: return 9; case 0x3146: return 10; case 0x3147: return 11;
        case 0x3148: return 12; case 0x3149: return 13; case 0x314A: return 14;
        case 0x314B: return 15; case 0x314C: return 16; case 0x314D: return 17;
        case 0x314E: return 18; default: return -1;
    }
}

int hangul_vowel(uint32_t codepoint)
{
    switch (codepoint) {
        case 0x314F: return 0; case 0x3150: return 1; case 0x3151: return 2;
        case 0x3152: return 3; case 0x3153: return 4; case 0x3154: return 5;
        case 0x3155: return 6; case 0x3156: return 7; case 0x3157: return 8;
        case 0x3158: return 9; case 0x3159: return 10; case 0x315A: return 11;
        case 0x315B: return 12; case 0x315C: return 13; case 0x315D: return 14;
        case 0x315E: return 15; case 0x315F: return 16; case 0x3160: return 17;
        case 0x3161: return 18; case 0x3162: return 19; case 0x3163: return 20;
        default: return -1;
    }
}

int hangul_final(int initial)
{
    switch (initial) {
        case 0: return 1; case 1: return 2; case 2: return 4; case 3: return 7;
        case 5: return 8; case 6: return 16; case 7: return 17; case 9: return 19;
        case 10: return 20; case 11: return 21; case 12: return 22; case 14: return 23;
        case 15: return 24; case 16: return 25; case 17: return 26; case 18: return 27;
        default: return 0;
    }
}

int initial_from_final(int final)
{
    switch (final) {
        case 1: return 0; case 2: return 1; case 4: return 2; case 7: return 3;
        case 8: return 5; case 16: return 6; case 17: return 7; case 19: return 9;
        case 20: return 10; case 21: return 11; case 22: return 12; case 23: return 14;
        case 24: return 15; case 25: return 16; case 26: return 17; case 27: return 18;
        default: return -1;
    }
}

int combine_initial(int first, int second)
{
    if (first == 0 && second == 0) return 1;
    if (first == 3 && second == 3) return 4;
    if (first == 7 && second == 7) return 8;
    if (first == 9 && second == 9) return 10;
    if (first == 12 && second == 12) return 13;
    return -1;
}

int combine_vowel(int first, int second)
{
    if (first == 8 && second == 0) return 9;
    if (first == 8 && second == 1) return 10;
    if (first == 8 && second == 20) return 11;
    if (first == 13 && second == 4) return 14;
    if (first == 13 && second == 5) return 15;
    if (first == 13 && second == 20) return 16;
    if (first == 18 && second == 20) return 19;
    if (first == 9 && second == 20) return 10;
    if (first == 14 && second == 20) return 15;
    if (first == 0 && second == 20) return 1;
    if (first == 2 && second == 20) return 3;
    if (first == 4 && second == 20) return 5;
    if (first == 6 && second == 20) return 7;
    return -1;
}

int combine_final(int first, int second)
{
    if (first == 1 && second == 19) return 3;
    if (first == 4 && second == 22) return 5;
    if (first == 4 && second == 27) return 6;
    if (first == 8 && second == 1) return 9;
    if (first == 8 && second == 16) return 10;
    if (first == 8 && second == 17) return 11;
    if (first == 8 && second == 19) return 12;
    if (first == 8 && second == 25) return 13;
    if (first == 8 && second == 26) return 14;
    if (first == 8 && second == 27) return 15;
    if (first == 17 && second == 19) return 18;
    if (first == 1 && second == 1) return 2;
    if (first == 19 && second == 19) return 20;
    return -1;
}

void split_final(int final, int *stay, int *move)
{
    *stay = 0;
    *move = final;
    switch (final) {
        case 3: *stay = 1; *move = 9; break; case 5: *stay = 4; *move = 12; break;
        case 6: *stay = 4; *move = 18; break; case 9: *stay = 8; *move = 0; break;
        case 10: *stay = 8; *move = 6; break; case 11: *stay = 8; *move = 7; break;
        case 12: *stay = 8; *move = 9; break; case 13: *stay = 8; *move = 16; break;
        case 14: *stay = 8; *move = 17; break; case 15: *stay = 8; *move = 18; break;
        case 18: *stay = 17; *move = 9; break;
    }
}

uint32_t hangul_preedit_codepoint()
{
    if (s_hangul_initial < 0) return 0;
    if (s_hangul_vowel < 0) {
        static constexpr uint32_t initials[] = {
            0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143,
            0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E,
        };
        return initials[s_hangul_initial];
    }
    return 0xAC00 + static_cast<uint32_t>((s_hangul_initial * 21 + s_hangul_vowel) * 28 +
                                          s_hangul_final);
}

void refresh_hangul_preedit()
{
    const uint32_t codepoint = hangul_preedit_codepoint();
    if (codepoint) encode_utf8(codepoint, s_preedit);
    else s_preedit[0] = '\0';
    invalidate_candidates();
}

void commit_hangul_to(char *text, size_t capacity)
{
    if (s_preedit[0]) append(text, capacity, s_preedit);
    reset_hangul();
    invalidate_candidates();
}

void start_hangul_initial(int initial)
{
    s_hangul_initial = initial;
    s_hangul_vowel = -1;
    s_hangul_final = 0;
    refresh_hangul_preedit();
}

void process_hangul(uint32_t codepoint, char *text, size_t capacity)
{
    const int vowel = hangul_vowel(codepoint);
    if (vowel >= 0) {
        if (s_hangul_initial < 0) {
            s_hangul_initial = 11;
            s_hangul_vowel = vowel;
        } else if (s_hangul_vowel < 0) {
            s_hangul_vowel = vowel;
        } else if (s_hangul_final == 0) {
            const int combined = combine_vowel(s_hangul_vowel, vowel);
            if (combined >= 0) s_hangul_vowel = combined;
            else {
                commit_hangul_to(text, capacity);
                s_hangul_initial = 11;
                s_hangul_vowel = vowel;
            }
        } else {
            int previous_final = 0;
            int moved_final = 0;
            split_final(s_hangul_final, &previous_final, &moved_final);
            int next_initial = initial_from_final(moved_final);
            if (next_initial < 0) next_initial = 11;
            s_hangul_final = previous_final;
            refresh_hangul_preedit();
            append(text, capacity, s_preedit);
            s_hangul_initial = next_initial;
            s_hangul_vowel = vowel;
            s_hangul_final = 0;
        }
        refresh_hangul_preedit();
        return;
    }

    const int initial = hangul_initial(codepoint);
    if (initial < 0) {
        commit_hangul_to(text, capacity);
        char encoded[5];
        encode_utf8(codepoint, encoded);
        append(text, capacity, encoded);
        return;
    }
    if (s_hangul_initial < 0) {
        start_hangul_initial(initial);
        return;
    }
    if (s_hangul_vowel < 0) {
        const int combined = combine_initial(s_hangul_initial, initial);
        if (combined >= 0) s_hangul_initial = combined;
        else {
            commit_hangul_to(text, capacity);
            start_hangul_initial(initial);
        }
        refresh_hangul_preedit();
        return;
    }
    const int final = hangul_final(initial);
    if (s_hangul_final == 0 && final != 0) s_hangul_final = final;
    else {
        const int combined = combine_final(s_hangul_final, final);
        if (s_hangul_final && combined >= 0) s_hangul_final = combined;
        else {
            commit_hangul_to(text, capacity);
            start_hangul_initial(initial);
        }
    }
    refresh_hangul_preedit();
}

void process_pinyin_key(const char *key, char *text, size_t capacity)
{
    if (!key || !*key) return;
    if (key[1] != '\0' || key[0] < 'a' || key[0] > 'z') {
        commit(Layout::ChinesePinyin, text, capacity);
        append(text, capacity, key);
        return;
    }
    const size_t length = std::strlen(s_preedit);
    if (length + 1 >= sizeof(s_preedit)) return;
    s_preedit[length] = key[0];
    s_preedit[length + 1] = '\0';
    invalidate_candidates();
}

uint32_t small_kana(uint32_t codepoint)
{
    switch (codepoint) {
        case 0x3042: return 0x3041; case 0x3044: return 0x3043;
        case 0x3046: return 0x3045; case 0x3048: return 0x3047;
        case 0x304A: return 0x3049; case 0x3064: return 0x3063;
        case 0x3084: return 0x3083; case 0x3086: return 0x3085;
        case 0x3088: return 0x3087; case 0x308F: return 0x308E;
        case 0x30A2: return 0x30A1; case 0x30A4: return 0x30A3;
        case 0x30A6: return 0x30A5; case 0x30A8: return 0x30A7;
        case 0x30AA: return 0x30A9; case 0x30C4: return 0x30C3;
        case 0x30E4: return 0x30E3; case 0x30E6: return 0x30E5;
        case 0x30E8: return 0x30E7; case 0x30EF: return 0x30EE;
        default: return codepoint;
    }
}

uint32_t voiced_kana(uint32_t codepoint, bool handakuten)
{
    if (!handakuten && codepoint == 0x3046) return 0x3094;
    if (!handakuten && codepoint == 0x30A6) return 0x30F4;
    const bool katakana = codepoint >= 0x30A0 && codepoint <= 0x30FF;
    const uint32_t hiragana = katakana ? codepoint - 0x60 : codepoint;
    uint32_t replacement = hiragana;
    if (hiragana >= 0x304B && hiragana <= 0x3053 && ((hiragana - 0x304B) % 2 == 0)) {
        replacement = hiragana + 1;
    } else if (hiragana >= 0x3055 && hiragana <= 0x305D && ((hiragana - 0x3055) % 2 == 0)) {
        replacement = hiragana + 1;
    } else if (hiragana == 0x305F || hiragana == 0x3061 || hiragana == 0x3064 ||
               hiragana == 0x3066 || hiragana == 0x3068) {
        replacement = hiragana + 1;
    } else if (hiragana >= 0x306F && hiragana <= 0x307B && ((hiragana - 0x306F) % 3 == 0)) {
        replacement = hiragana + (handakuten ? 2 : 1);
    }
    if (replacement == hiragana) return codepoint;
    return katakana ? replacement + 0x60 : replacement;
}

struct T9Keys {
    char key;
    const char *lower;
    const char *upper;
};

// ITU E.161 letter groups; the digit closes each alphabet cycle like a phone.
constexpr T9Keys kEnglishT9[] = {
    {'1', ".,?!'-1", ".,?!'-1"}, {'2', "abc2", "ABC2"}, {'3', "def3", "DEF3"},
    {'4', "ghi4", "GHI4"}, {'5', "jkl5", "JKL5"}, {'6', "mno6", "MNO6"},
    {'7', "pqrs7", "PQRS7"}, {'8', "tuv8", "TUV8"}, {'9', "wxyz9", "WXYZ9"},
    {'0', "0", "0"},
};

constexpr T9Keys kRussianT9[] = {
    {'1', ".,?!'-1", ".,?!'-1"}, {'2', "абвг2", "АБВГ2"}, {'3', "деёжз3", "ДЕЁЖЗ3"},
    {'4', "ийкл4", "ИЙКЛ4"}, {'5', "мноп5", "МНОП5"}, {'6', "рсту6", "РСТУ6"},
    {'7', "фхцч7", "ФХЦЧ7"}, {'8', "шщъы8", "ШЩЪЫ8"}, {'9', "ьэюя9", "ЬЭЮЯ9"},
    {'0', "0", "0"},
};

// Pinyin letters stay out of the digit so a cycle never commits the preedit.
constexpr T9Keys kPinyinT9[] = {
    {'1', ".,?!", ".,?!"}, {'2', "abc", "ABC"}, {'3', "def", "DEF"},
    {'4', "ghi", "GHI"}, {'5', "jkl", "JKL"}, {'6', "mno", "MNO"},
    {'7', "pqrs", "PQRS"}, {'8', "tuv", "TUV"}, {'9', "wxyz", "WXYZ"},
    {'0', "0", "0"},
};

// Cheonjiin consonants; keys 1-3 are the vowel strokes handled separately.
constexpr T9Keys kKoreanT9[] = {
    {'4', "ㄱㅋㄲ", nullptr}, {'5', "ㄴㄹ", nullptr}, {'6', "ㄷㅌㄸ", nullptr},
    {'7', "ㅂㅍㅃ", nullptr}, {'8', "ㅅㅎㅆ", nullptr}, {'9', "ㅈㅊㅉ", nullptr},
    {'0', "ㅇㅁ", nullptr}, {'*', ".,?!", nullptr},
};

// Keitai toggle input over the same rows as the flick keypad.
constexpr T9Keys kKanaT9[] = {
    {'1', "あいうえお", nullptr}, {'2', "かきくけこ", nullptr}, {'3', "さしすせそ", nullptr},
    {'4', "たちつてと", nullptr}, {'5', "なにぬねの", nullptr}, {'6', "はひふへほ", nullptr},
    {'7', "まみむめも", nullptr}, {'8', "やゆよ", nullptr}, {'9', "らりるれろ", nullptr},
    {'0', "わをんー", nullptr},
};

struct CheonjiinVowel {
    const char *strokes;
    const char *jamo;
};

// Every prefix of a listed stroke sequence is itself listed, so a sequence can
// grow one stroke at a time. Dot-only entries are shown but are not vowels.
constexpr CheonjiinVowel kCheonjiinVowels[] = {
    {"I", "ㅣ"}, {"E", "ㅡ"}, {"D", "ㆍ"}, {"DD", "‥"},
    {"ID", "ㅏ"}, {"IDD", "ㅑ"}, {"IDI", "ㅐ"}, {"IDDI", "ㅒ"},
    {"DI", "ㅓ"}, {"DDI", "ㅕ"}, {"DII", "ㅔ"}, {"DDII", "ㅖ"},
    {"DE", "ㅗ"}, {"DDE", "ㅛ"}, {"DEI", "ㅚ"}, {"DEID", "ㅘ"}, {"DEIDI", "ㅙ"},
    {"ED", "ㅜ"}, {"EDD", "ㅠ"}, {"EDI", "ㅟ"}, {"EDDI", "ㅝ"}, {"EDDII", "ㅞ"},
    {"EI", "ㅢ"},
};

template <size_t Count>
const char *find_t9_characters(const T9Keys (&keys)[Count], char key, bool upper)
{
    for (const T9Keys &entry : keys) {
        if (entry.key == key) return upper && entry.upper ? entry.upper : entry.lower;
    }
    return nullptr;
}

const char *t9_characters(Layout layout, char key, bool upper)
{
    switch (layout) {
        case Layout::English: return find_t9_characters(kEnglishT9, key, upper);
        case Layout::Russian: return find_t9_characters(kRussianT9, key, upper);
        case Layout::Korean: return find_t9_characters(kKoreanT9, key, upper);
        case Layout::ChinesePinyin: return find_t9_characters(kPinyinT9, key, upper);
        case Layout::JapaneseKana: return find_t9_characters(kKanaT9, key, upper);
        case Layout::Count: break;
    }
    return nullptr;
}

size_t codepoint_count(const char *text)
{
    size_t count = 0;
    for (const char *cursor = text; *cursor; ++count) decode_utf8(&cursor);
    return count;
}

bool codepoint_at(const char *text, size_t index, char output[5])
{
    const char *cursor = text;
    for (size_t current = 0; *cursor; ++current) {
        const uint32_t codepoint = decode_utf8(&cursor);
        if (current == index) {
            encode_utf8(codepoint, output);
            return true;
        }
    }
    return false;
}

const char *cheonjiin_vowel(const char *strokes)
{
    for (const CheonjiinVowel &vowel : kCheonjiinVowels) {
        if (std::strcmp(vowel.strokes, strokes) == 0) return vowel.jamo;
    }
    return nullptr;
}

bool cheonjiin_dots_pending()
{
    return s_t9.strokes[0] && std::strspn(s_t9.strokes, "D") == std::strlen(s_t9.strokes);
}

void snapshot_t9(Layout layout, char key, bool upper, char *text)
{
    s_t9 = {};
    s_t9.active = true;
    s_t9.layout = layout;
    s_t9.key = key;
    s_t9.upper = upper;
    s_t9.text = text;
    s_t9.text_length = std::strlen(text);
    std::memcpy(s_t9.preedit, s_preedit, sizeof(s_preedit));
    s_t9.hangul_initial = s_hangul_initial;
    s_t9.hangul_vowel = s_hangul_vowel;
    s_t9.hangul_final = s_hangul_final;
}

void restore_t9()
{
    if (!s_t9.active || !s_t9.text) return;
    if (std::strlen(s_t9.text) > s_t9.text_length) s_t9.text[s_t9.text_length] = '\0';
    std::memcpy(s_preedit, s_t9.preedit, sizeof(s_preedit));
    s_hangul_initial = s_t9.hangul_initial;
    s_hangul_vowel = s_t9.hangul_vowel;
    s_hangul_final = s_t9.hangul_final;
    invalidate_candidates();
}

// Any non-T9 input ends the cycle; a lone Cheonjiin dot is not a vowel, so it
// is dropped rather than committed.
void end_t9_cycle()
{
    if (s_t9_applying) return;
    if (s_t9.active && cheonjiin_dots_pending()) restore_t9();
    s_t9 = {};
}

void apply_t9_character(Layout layout, const char *character, char *text, size_t capacity)
{
    s_t9_applying = true;
    if (layout == Layout::ChinesePinyin) {
        process_pinyin_key(character, text, capacity);
    } else if (layout == Layout::Korean) {
        const char *cursor = character;
        process_hangul(decode_utf8(&cursor), text, capacity);
    } else if (layout == Layout::JapaneseKana) {
        append(s_preedit, sizeof(s_preedit), character);
        invalidate_candidates();
    } else {
        append(text, capacity, character);
    }
    s_t9_applying = false;
}

void apply_cheonjiin(char *text, size_t capacity)
{
    const char *jamo = cheonjiin_vowel(s_t9.strokes);
    if (!jamo) return;
    if (cheonjiin_dots_pending()) {
        append(s_preedit, sizeof(s_preedit), jamo);
        invalidate_candidates();
        return;
    }
    apply_t9_character(Layout::Korean, jamo, text, capacity);
}

void process_cheonjiin_stroke(char stroke, char *text, size_t capacity)
{
    char strokes[kT9StrokeCapacity] = "";
    const size_t length = std::strlen(s_t9.strokes);
    const bool extend = s_t9.active && s_t9.layout == Layout::Korean && s_t9.text == text &&
                        length && length + 1 < sizeof(strokes);
    if (extend) {
        std::memcpy(strokes, s_t9.strokes, length);
        strokes[length] = stroke;
        strokes[length + 1] = '\0';
    }
    if (extend && cheonjiin_vowel(strokes)) {
        restore_t9();
    } else {
        end_t9_cycle();
        snapshot_t9(Layout::Korean, stroke, false, text);
        strokes[0] = stroke;
        strokes[1] = '\0';
    }
    std::memcpy(s_t9.strokes, strokes, sizeof(strokes));
    apply_cheonjiin(text, capacity);
}

} // namespace

void reset()
{
    s_preedit[0] = '\0';
    s_hangul_initial = -1;
    s_hangul_vowel = -1;
    s_hangul_final = 0;
    s_t9 = {};
    invalidate_candidates();
}

void set_t9_mode(bool enabled)
{
    if (s_t9_mode == enabled) return;
    s_t9_mode = enabled;
    reset();
}

bool t9_mode()
{
    return s_t9_mode;
}

void process_t9_key(Layout layout, char key, bool upper, char *text, size_t capacity)
{
    if (!text || capacity == 0) return;
    if (layout == Layout::Korean && key >= '1' && key <= '3') {
        process_cheonjiin_stroke(key == '1' ? 'I' : key == '2' ? 'D' : 'E', text, capacity);
        return;
    }
    const int64_t now = esp_timer_get_time();
    const bool repeat = s_t9.active && s_t9.layout == layout && s_t9.key == key &&
                        s_t9.text == text && !s_t9.strokes[0] &&
                        now - s_t9.last_us <= kT9CycleUs;
    if (repeat) {
        restore_t9();
    } else {
        end_t9_cycle();
        snapshot_t9(layout, key, upper, text);
    }
    const char *characters = t9_characters(layout, key, s_t9.upper);
    const size_t count = characters ? codepoint_count(characters) : 0;
    if (count == 0) {
        s_t9 = {};
        return;
    }
    s_t9.index = repeat ? (s_t9.index + 1) % count : 0;
    s_t9.last_us = now;
    char character[5];
    if (codepoint_at(characters, s_t9.index, character)) {
        apply_t9_character(layout, character, text, capacity);
    }
}

const char *preedit()
{
    return s_preedit;
}

bool dictionary_available(Layout layout)
{
    return load_dictionary(layout);
}

size_t candidate_count(Layout layout)
{
    return candidate_total(layout);
}

const char *candidate(Layout layout, size_t index)
{
    s_candidate_text[0] = '\0';
    copy_candidate(layout, index, s_candidate_text, sizeof(s_candidate_text));
    return s_candidate_text;
}

bool select_candidate(Layout layout, size_t index, char *text, size_t capacity)
{
    end_t9_cycle();
    prepare_candidates(layout);
    if (index >= candidate_total(layout)) return false;
    char selected[kMaxWordBytes + 1]{};
    if (!copy_candidate(layout, index, selected, sizeof(selected)) ||
        !append(text, capacity, selected)) {
        return false;
    }
    remove_prefix(s_preedit, s_candidates.record.matched_bytes);
    invalidate_candidates();
    return true;
}

void process_key(Layout layout, const char *key, char *text, size_t capacity)
{
    if (!key || !*key || !text || capacity == 0) return;
    end_t9_cycle();
    if (layout == Layout::ChinesePinyin) {
        process_pinyin_key(key, text, capacity);
        return;
    }
    if (layout == Layout::JapaneseKana) {
        commit(layout, text, capacity);
        append(text, capacity, key);
        return;
    }
    if (layout == Layout::Korean) {
        const char *cursor = key;
        const uint32_t codepoint = decode_utf8(&cursor);
        if (hangul_initial(codepoint) >= 0 || hangul_vowel(codepoint) >= 0) {
            process_hangul(codepoint, text, capacity);
            return;
        }
        commit_hangul_to(text, capacity);
    }
    append(text, capacity, key);
}

void process_kana_flick(Layout layout, const char *kana, char *, size_t)
{
    if (layout != Layout::JapaneseKana || !kana) return;
    end_t9_cycle();
    append(s_preedit, sizeof(s_preedit), kana);
    invalidate_candidates();
}

void apply_kana_modifier(KanaModifier modifier, char *, size_t)
{
    end_t9_cycle();
    if (!s_preedit[0]) return;
    const char *cursor = s_preedit;
    uint32_t previous = 0;
    while (*cursor) previous = decode_utf8(&cursor);
    uint32_t replacement = previous;
    if (modifier == KanaModifier::Small) replacement = small_kana(previous);
    else replacement = voiced_kana(previous, modifier == KanaModifier::Handakuten);
    if (replacement != previous &&
        replace_last_codepoint(s_preedit, sizeof(s_preedit), replacement)) {
        invalidate_candidates();
    }
}

void commit(Layout layout, char *text, size_t capacity)
{
    end_t9_cycle();
    if (layout == Layout::Korean) {
        commit_hangul_to(text, capacity);
        return;
    }
    if (layout == Layout::ChinesePinyin) {
        size_t guard = kPreeditCapacity;
        while (s_preedit[0] && guard--) {
            if (candidate_total(layout) == 0 || !select_candidate(layout, 0, text, capacity)) {
                append(text, capacity, s_preedit);
                s_preedit[0] = '\0';
                invalidate_candidates();
                break;
            }
        }
    } else if (s_preedit[0]) {
        append(text, capacity, s_preedit);
        s_preedit[0] = '\0';
        invalidate_candidates();
    }
}

bool backspace(Layout layout, char *text, size_t capacity)
{
    const size_t strokes = std::strlen(s_t9.strokes);
    if (s_t9.active && strokes && s_t9.text == text) {
        restore_t9();
        if (strokes > 1) {
            s_t9.strokes[strokes - 1] = '\0';
            apply_cheonjiin(text, capacity);
        } else {
            s_t9 = {};
        }
        return true;
    }
    end_t9_cycle();
    if (layout == Layout::Korean && s_hangul_initial >= 0) {
        if (s_hangul_final != 0) {
            if (s_hangul_final == 2) s_hangul_final = 1;
            else if (s_hangul_final == 20) s_hangul_final = 19;
            else {
                int stay = 0;
                int move = 0;
                split_final(s_hangul_final, &stay, &move);
                if (stay != 0) s_hangul_final = stay;
                else s_hangul_final = 0;
            }
        } else if (s_hangul_vowel >= 0) {
            switch (s_hangul_vowel) {
                case 1: s_hangul_vowel = 0; break; case 3: s_hangul_vowel = 2; break;
                case 5: s_hangul_vowel = 4; break; case 7: s_hangul_vowel = 6; break;
                case 9: s_hangul_vowel = 8; break; case 10: s_hangul_vowel = 9; break;
                case 11: s_hangul_vowel = 8; break; case 14: s_hangul_vowel = 13; break;
                case 15: s_hangul_vowel = 14; break; case 16: s_hangul_vowel = 13; break;
                case 19: s_hangul_vowel = 18; break; default: s_hangul_vowel = -1; break;
            }
        } else {
            switch (s_hangul_initial) {
                case 1: s_hangul_initial = 0; break; case 4: s_hangul_initial = 3; break;
                case 8: s_hangul_initial = 7; break; case 10: s_hangul_initial = 9; break;
                case 13: s_hangul_initial = 12; break; default: reset_hangul(); break;
            }
        }
        refresh_hangul_preedit();
        return true;
    }
    if (layout == Layout::ChinesePinyin && s_preedit[0]) {
        s_preedit[std::strlen(s_preedit) - 1] = '\0';
        invalidate_candidates();
        return true;
    }
    if (layout == Layout::JapaneseKana && s_preedit[0]) {
        remove_last_codepoint(s_preedit);
        invalidate_candidates();
        return true;
    }
    if (s_preedit[0]) {
        remove_last_codepoint(s_preedit);
        invalidate_candidates();
        return true;
    }
    if (!text || !*text) return false;
    remove_last_codepoint(text);
    return true;
}

void space(Layout layout, char *text, size_t capacity)
{
    end_t9_cycle();
    const size_t count = candidate_total(layout);
    if (layout == Layout::ChinesePinyin && count > 0) {
        select_candidate(layout, 0, text, capacity);
        return;
    }
    if (layout == Layout::JapaneseKana && count > 1) {
        select_candidate(layout, 0, text, capacity);
        return;
    }
    commit(layout, text, capacity);
    append(text, capacity, " ");
}

} // namespace lyra::gui::ime
