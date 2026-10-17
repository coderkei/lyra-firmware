/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace lyra::wifi {

constexpr size_t kMaxAccessPoints = 20;

struct AccessPoint {
    char ssid[33];
    int8_t rssi;
    bool secured;
    bool saved;
    bool connected;
};

struct PingStatus {
    bool running;
    bool complete;
    bool dns_lookup_failed;
    uint32_t transmitted;
    uint32_t received;
    uint32_t duration_ms;
    char resolved_address[40];
};

enum class PingTarget : uint8_t { Ip, Dns };

struct Status {
    bool initialized;
    bool enabled;
    bool connected;
    bool connecting;
    bool connection_failed;
    bool scanning;
    bool has_saved_network;
    bool has_connected_rssi;
    int8_t connected_rssi;
    uint32_t generation;
    char saved_ssid[33];
    char connected_ssid[33];
    char ip_address[16];
    uint8_t access_point_count;
    AccessPoint access_points[kMaxAccessPoints];
    PingStatus ip_ping;
    PingStatus dns_ping;
};

// Initializes the station interface and restores the saved Wi-Fi preference.
// A disabled or unconfigured device remains offline without blocking startup.
esp_err_t init();

Status status();
esp_err_t set_enabled(bool enabled);
esp_err_t scan();
esp_err_t connect(const char *ssid, const char *password);
esp_err_t connect_saved();
esp_err_t disconnect();
esp_err_t forget_network();
esp_err_t start_ping(PingTarget target);

} // namespace lyra::wifi
