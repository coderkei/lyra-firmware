/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_radio.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "lyra_media.h"
#include "lyra_sd.h"

namespace lyra::radio {
namespace {

constexpr char kStationListPath[] = "/sdcard/.lyra/radio-stations-v1.tsv";
constexpr char kTemporaryPath[] = "/sdcard/.lyra/radio-stations-v1.tmp";
constexpr char kBackupPath[] = "/sdcard/.lyra/radio-stations-v1.bak";
constexpr size_t kImportLimit = 32 * 1024;

// Radio must not reserve scarce internal RAM needed by I2S and task stacks,
// including when the user only plays local music. Allocate CPU-only data in
// PSRAM on demand, and release import scratch as soon as parsing finishes.
struct RadioBufferDeleter {
    void operator()(void *buffer) const { heap_caps_free(buffer); }
};

std::unique_ptr<Station[], RadioBufferDeleter> s_stations;
size_t s_station_count = 0;
bool s_loaded = false;

bool ensure_data_directory()
{
    if (!lyra::sd::acquire(lyra::sd::Client::Filesystem)) return false;
    struct stat info{};
    bool ready = false;
    if (stat("/sdcard/.lyra", &info) == 0) {
        ready = S_ISDIR(info.st_mode);
    } else {
        ready = mkdir("/sdcard/.lyra", 0775) == 0;
    }
    lyra::sd::release(lyra::sd::Client::Filesystem);
    return ready;
}

bool path_exists(const char *path)
{
    if (!lyra::sd::acquire(lyra::sd::Client::Filesystem)) return false;
    struct stat info{};
    const bool exists = stat(path, &info) == 0;
    lyra::sd::release(lyra::sd::Client::Filesystem);
    return exists;
}

char *trim(char *text)
{
    while (*text && std::isspace(static_cast<unsigned char>(*text))) ++text;
    size_t length = std::strlen(text);
    while (length > 0 && std::isspace(static_cast<unsigned char>(text[length - 1]))) {
        text[--length] = '\0';
    }
    return text;
}

void copy_sanitized(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    size_t output = 0;
    if (source) {
        for (; *source && output + 1 < capacity; ++source) {
            if (*source == '\r' || *source == '\n' || *source == '\t') continue;
            destination[output++] = *source;
        }
    }
    destination[output] = '\0';
}

void name_from_url(const char *url, char *name, size_t capacity)
{
    const char *start = std::strstr(url, "://");
    start = start ? start + 3 : url;
    const char *end = start;
    while (*end && *end != '/' && *end != '?' && *end != '#') ++end;
    const size_t length = std::min(static_cast<size_t>(end - start), capacity - 1);
    std::memcpy(name, start, length);
    name[length] = '\0';
    if (length == 0) copy_sanitized(name, capacity, "Internet Radio");
}

bool ensure_loaded()
{
    if (s_loaded) return true;
    if (!lyra::sd::init() || !ensure_data_directory()) return false;
    if (!s_stations) {
        s_stations.reset(static_cast<Station *>(heap_caps_calloc(
            kMaximumStations, sizeof(Station), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!s_stations) return false;
    }
    s_station_count = 0;
    bool loaded_from_backup = false;
    FILE *file = lyra::sd::open(kStationListPath, "rb", lyra::sd::Client::Filesystem);
    if (!file) {
        file = lyra::sd::open(kBackupPath, "rb", lyra::sd::Client::Filesystem);
        loaded_from_backup = file != nullptr;
    }
    if (file) {
        constexpr size_t capacity = kMaximumStations *
            (kStationNameSize + kStreamUrlSize + 2);
        std::unique_ptr<char[], RadioBufferDeleter> data(static_cast<char *>(
            heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!data) {
            lyra::sd::close(file, lyra::sd::Client::Filesystem);
            return false;
        }
        size_t count = 0;
        while (count < capacity - 1) {
            const size_t received = lyra::sd::read(file, data.get() + count,
                std::min<size_t>(8192, capacity - 1 - count),
                lyra::sd::Client::Filesystem);
            if (received == 0) break;
            count += received;
        }
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        if (loaded_from_backup) {
            lyra::sd::rename(kBackupPath, kStationListPath,
                             lyra::sd::Client::Filesystem);
        } else {
            lyra::sd::remove(kBackupPath, lyra::sd::Client::Filesystem);
        }
        data[count] = '\0';
        char *line = data.get();
        while (line && *line && s_station_count < kMaximumStations) {
            char *next = std::strchr(line, '\n');
            if (next) *next++ = '\0';
            char *separator = std::strchr(line, '\t');
            if (separator) {
                *separator++ = '\0';
                char *url = trim(separator);
                if (is_http_url(url)) {
                    Station &station = s_stations[s_station_count];
                    char clean_name[kStationNameSize]{};
                    copy_sanitized(clean_name, sizeof(clean_name), trim(line));
                    copy_sanitized(station.name, sizeof(station.name), clean_name);
                    if (!station.name[0]) name_from_url(url, station.name, sizeof(station.name));
                    copy_sanitized(station.url, sizeof(station.url), url);
                    ++s_station_count;
                }
            }
            line = next;
        }
    }
    s_loaded = true;
    return true;
}

bool persist()
{
    if (!ensure_data_directory()) return false;
    FILE *file = lyra::sd::open(kTemporaryPath, "wb", lyra::sd::Client::Filesystem);
    if (!file) return false;
    bool success = true;
    for (size_t i = 0; i < s_station_count && success; ++i) {
        char line[kStationNameSize + kStreamUrlSize + 2];
        const int length = std::snprintf(line, sizeof(line), "%s\t%s\n",
                                         s_stations[i].name, s_stations[i].url);
        success = length > 0 && static_cast<size_t>(length) < sizeof(line) &&
            lyra::sd::write_exact(file, line, static_cast<size_t>(length),
                                  lyra::sd::Client::Filesystem);
    }
    if (lyra::sd::close(file, lyra::sd::Client::Filesystem) != 0) success = false;
    if (success && path_exists(kBackupPath)) {
        lyra::sd::remove(kBackupPath, lyra::sd::Client::Filesystem);
        if (path_exists(kBackupPath)) success = false;
    }
    const bool had_previous = success && path_exists(kStationListPath);
    if (had_previous && lyra::sd::rename(kStationListPath, kBackupPath,
                                         lyra::sd::Client::Filesystem) != 0) {
        success = false;
    }
    if (success && lyra::sd::rename(kTemporaryPath, kStationListPath,
                                    lyra::sd::Client::Filesystem) == 0) {
        if (had_previous) lyra::sd::remove(kBackupPath, lyra::sd::Client::Filesystem);
        return true;
    }
    if (had_previous && path_exists(kBackupPath)) {
        lyra::sd::rename(kBackupPath, kStationListPath,
                         lyra::sd::Client::Filesystem);
    }
    lyra::sd::remove(kTemporaryPath, lyra::sd::Client::Filesystem);
    return false;
}

bool append_station(const char *url, const char *name)
{
    if (!is_http_url(url) || std::strlen(url) >= kStreamUrlSize) return false;
    for (size_t i = 0; i < s_station_count; ++i) {
        if (std::strcmp(s_stations[i].url, url) == 0) return false;
    }
    if (s_station_count >= kMaximumStations) return false;
    Station &station = s_stations[s_station_count];
    copy_sanitized(station.name, sizeof(station.name), name);
    if (!station.name[0]) name_from_url(url, station.name, sizeof(station.name));
    copy_sanitized(station.url, sizeof(station.url), url);
    ++s_station_count;
    return true;
}

char *find_case_insensitive(char *text, const char *needle)
{
    if (!text || !needle || !*needle) return text;
    const size_t length = std::strlen(needle);
    for (char *candidate = text; *candidate; ++candidate) {
        size_t index = 0;
        while (index < length && candidate[index] &&
               std::tolower(static_cast<unsigned char>(candidate[index])) ==
                   std::tolower(static_cast<unsigned char>(needle[index]))) ++index;
        if (index == length) return candidate;
    }
    return nullptr;
}

bool has_extension(const char *path, const char *extension)
{
    if (!path || !extension) return false;
    const char *dot = std::strrchr(path, '.');
    if (!dot) return false;
    ++dot;
    while (*dot && *extension) {
        if (std::tolower(static_cast<unsigned char>(*dot++)) !=
            std::tolower(static_cast<unsigned char>(*extension++))) return false;
    }
    return !*dot && !*extension;
}

size_t import_m3u(char *content)
{
    size_t added = 0;
    char pending_name[kStationNameSize]{};
    char *context = nullptr;
    for (char *line = strtok_r(content, "\r\n", &context); line;
         line = strtok_r(nullptr, "\r\n", &context)) {
        line = trim(line);
        if (std::strncmp(line, "#EXTINF:", 8) == 0) {
            const char *comma = std::strchr(line + 8, ',');
            copy_sanitized(pending_name, sizeof(pending_name), comma ? comma + 1 : "");
        } else if (is_http_url(line)) {
            if (append_station(line, pending_name)) ++added;
            pending_name[0] = '\0';
        }
    }
    return added;
}

size_t import_pls(char *content, Station *entries)
{
    char *context = nullptr;
    for (char *line = strtok_r(content, "\r\n", &context); line;
         line = strtok_r(nullptr, "\r\n", &context)) {
        line = trim(line);
        if (!*line || *line == '[' || *line == ';') continue;
        char *equal = std::strchr(line, '=');
        if (!equal) continue;
        *equal++ = '\0';
        const bool is_file = ::strncasecmp(line, "File", 4) == 0;
        const bool is_title = ::strncasecmp(line, "Title", 5) == 0;
        if (!is_file && !is_title) continue;
        const char *digits = line + (is_file ? 4 : 5);
        char *end = nullptr;
        const long number = std::strtol(digits, &end, 10);
        if (end == digits || number < 1 || number > static_cast<long>(kMaximumStations)) continue;
        Station &entry = entries[number - 1];
        if (is_file) copy_sanitized(entry.url, sizeof(entry.url), trim(equal));
        else copy_sanitized(entry.name, sizeof(entry.name), trim(equal));
    }
    size_t added = 0;
    for (size_t index = 0; index < kMaximumStations; ++index) {
        const Station &entry = entries[index];
        if (entry.url[0] && append_station(entry.url, entry.name)) ++added;
    }
    return added;
}

void decode_xml_entities(char *text)
{
    char *cursor = text;
    while ((cursor = std::strstr(cursor, "&amp;")) != nullptr) {
        const size_t tail = std::strlen(cursor + 5);
        *cursor = '&';
        std::memmove(cursor + 1, cursor + 5, tail + 1);
        ++cursor;
    }
}

size_t import_asx(char *content)
{
    size_t added = 0;
    char *entry = content;
    while ((entry = find_case_insensitive(entry, "<entry")) != nullptr) {
        char *end_entry = find_case_insensitive(entry, "</entry>");
        if (!end_entry) end_entry = entry + std::strlen(entry);
        char name[kStationNameSize]{};
        char url[kStreamUrlSize]{};
        char *title = find_case_insensitive(entry, "<title>");
        if (title && title < end_entry) {
            title += 7;
            char *end_title = find_case_insensitive(title, "</title>");
            if (end_title && end_title <= end_entry) {
                const size_t length = std::min(static_cast<size_t>(end_title - title),
                                               sizeof(name) - 1);
                std::memcpy(name, title, length);
                name[length] = '\0';
                char clean_name[kStationNameSize]{};
                copy_sanitized(clean_name, sizeof(clean_name), trim(name));
                copy_sanitized(name, sizeof(name), clean_name);
            }
        }
        char *ref = find_case_insensitive(entry, "<ref");
        if (ref && ref < end_entry) {
            char *href = find_case_insensitive(ref, "href");
            if (href && href < end_entry) {
                href += 4;
                while (href < end_entry && std::isspace(static_cast<unsigned char>(*href))) ++href;
                if (href < end_entry && *href == '=') ++href;
                while (href < end_entry && (std::isspace(static_cast<unsigned char>(*href)) ||
                                             *href == '\'' || *href == '"')) ++href;
                char *url_end = href;
                while (url_end < end_entry && *url_end != '\'' && *url_end != '"' &&
                       !std::isspace(static_cast<unsigned char>(*url_end)) && *url_end != '>') {
                    ++url_end;
                }
                const size_t length = std::min(static_cast<size_t>(url_end - href),
                                               sizeof(url) - 1);
                std::memcpy(url, href, length);
                url[length] = '\0';
                decode_xml_entities(url);
                if (append_station(url, name)) ++added;
            }
        }
        entry = *end_entry ? end_entry + 8 : end_entry;
    }
    return added;
}

} // namespace

bool is_http_url(const char *url)
{
    if (!url) return false;
    const size_t length = std::strlen(url);
    if (length == 0 || length >= kStreamUrlSize) return false;
    const bool http = ::strncasecmp(url, "http://", 7) == 0;
    const bool https = ::strncasecmp(url, "https://", 8) == 0;
    if (!http && !https) return false;
    const char *authority = url + (http ? 7 : 8);
    if (!*authority || *authority == '/' || *authority == '?' || *authority == '#') {
        return false;
    }
    for (const char *cursor = url; *cursor; ++cursor) {
        if (std::isspace(static_cast<unsigned char>(*cursor))) return false;
    }
    return true;
}

bool init()
{
    return ensure_loaded();
}

size_t station_count()
{
    return ensure_loaded() ? s_station_count : 0;
}

bool station_at(size_t index, Station *station)
{
    if (!station || !ensure_loaded() || index >= s_station_count) return false;
    std::memcpy(station, &s_stations[index], sizeof(*station));
    return true;
}

esp_err_t add_station(const char *url, const char *name)
{
    if (!is_http_url(url)) return ESP_ERR_INVALID_ARG;
    if (!ensure_loaded()) return ESP_ERR_INVALID_STATE;
    const size_t old_count = s_station_count;
    if (!append_station(url, name)) return ESP_ERR_INVALID_SIZE;
    if (persist()) return ESP_OK;
    s_station_count = old_count;
    return ESP_FAIL;
}

esp_err_t remove_station(size_t index)
{
    if (!ensure_loaded() || index >= s_station_count) return ESP_ERR_INVALID_ARG;
    const Station removed = s_stations[index];
    for (size_t i = index + 1; i < s_station_count; ++i) s_stations[i - 1] = s_stations[i];
    --s_station_count;
    if (persist()) return ESP_OK;
    for (size_t i = s_station_count; i > index; --i) s_stations[i] = s_stations[i - 1];
    s_stations[index] = removed;
    ++s_station_count;
    return ESP_FAIL;
}

bool is_playlist_file(const char *path)
{
    return has_extension(path, "m3u") || has_extension(path, "m3u8") ||
        has_extension(path, "pls") || has_extension(path, "asx");
}

esp_err_t import_playlist_file(const char *path, size_t *added)
{
    if (added) *added = 0;
    if (!path || !is_playlist_file(path) || !ensure_loaded()) return ESP_ERR_INVALID_ARG;
    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Filesystem);
    if (!file) return ESP_ERR_NOT_FOUND;
    auto *content = static_cast<char *>(heap_caps_malloc(kImportLimit + 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!content) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        return ESP_ERR_NO_MEM;
    }
    size_t length = 0;
    while (length < kImportLimit) {
        const size_t received = lyra::sd::read(file, content + length,
            std::min<size_t>(8192, kImportLimit - length),
            lyra::sd::Client::Filesystem);
        if (received == 0) break;
        length += received;
    }
    const int close_result = lyra::sd::close(file, lyra::sd::Client::Filesystem);
    if (close_result != 0 || length == kImportLimit) {
        heap_caps_free(content);
        return ESP_ERR_INVALID_SIZE;
    }
    content[length] = '\0';
    const size_t old_count = s_station_count;
    size_t count = 0;
    if (has_extension(path, "pls")) {
        std::unique_ptr<Station[], RadioBufferDeleter> entries(static_cast<Station *>(
            heap_caps_calloc(kMaximumStations, sizeof(Station),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        if (!entries) {
            heap_caps_free(content);
            return ESP_ERR_NO_MEM;
        }
        count = import_pls(content, entries.get());
    } else if (has_extension(path, "asx")) count = import_asx(content);
    else count = import_m3u(content);
    heap_caps_free(content);
    if (count == 0) {
        s_station_count = old_count;
        return ESP_ERR_NOT_FOUND;
    }
    if (!persist()) {
        s_station_count = old_count;
        return ESP_FAIL;
    }
    if (added) *added = count;
    return ESP_OK;
}

size_t browse_directory(const char *path, size_t offset, BrowserEntry *entries,
                        size_t capacity, size_t *total)
{
    if (total) *total = 0;
    if (!path || !entries || capacity == 0 || !lyra::media::begin_file_access()) return 0;
    if (!lyra::sd::acquire(lyra::sd::Client::Filesystem)) {
        lyra::media::end_file_access();
        return 0;
    }
    DIR *directory = opendir(path);
    if (!directory) {
        lyra::sd::release(lyra::sd::Client::Filesystem);
        lyra::media::end_file_access();
        return 0;
    }
    size_t matched = 0;
    size_t found = 0;
    while (dirent *entry = readdir(directory)) {
        if (!entry->d_name[0] || entry->d_name[0] == '.') continue;
        char child[kStreamUrlSize];
        const int length = std::snprintf(child, sizeof(child), "%s/%s", path,
                                         entry->d_name);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(child)) continue;
        struct stat info{};
        if (stat(child, &info) != 0) continue;
        const bool is_directory = S_ISDIR(info.st_mode);
        if (!is_directory && (!S_ISREG(info.st_mode) || !is_playlist_file(entry->d_name))) {
            continue;
        }
        if (matched++ < offset || found >= capacity) continue;
        BrowserEntry &target = entries[found++];
        copy_sanitized(target.name, sizeof(target.name), entry->d_name);
        copy_sanitized(target.path, sizeof(target.path), child);
        target.directory = is_directory;
    }
    closedir(directory);
    lyra::sd::release(lyra::sd::Client::Filesystem);
    lyra::media::end_file_access();
    if (total) *total = matched;
    return found;
}

} // namespace lyra::radio
