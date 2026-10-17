/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include "esp_err.h"

namespace lyra::radio {

constexpr size_t kMaximumStations = 32;
constexpr size_t kStationNameSize = 64;
constexpr size_t kStreamUrlSize = 256;

struct Station {
    char name[kStationNameSize]{};
    char url[kStreamUrlSize]{};
};

struct BrowserEntry {
    char name[kStationNameSize]{};
    char path[kStreamUrlSize]{};
    bool directory = false;
};

bool init();
size_t station_count();
bool station_at(size_t index, Station *station);
bool is_http_url(const char *url);
esp_err_t add_station(const char *url, const char *name = nullptr);
esp_err_t remove_station(size_t index);
esp_err_t import_playlist_file(const char *path, size_t *added = nullptr);
bool is_playlist_file(const char *path);
size_t browse_directory(const char *path, size_t offset, BrowserEntry *entries,
                        size_t capacity, size_t *total = nullptr);

} // namespace lyra::radio
