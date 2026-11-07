/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_time_sync.h"

#include <atomic>
#include <ctime>
#include <sys/time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lyra_clock.h"

namespace lyra::time_sync {
namespace {

constexpr const char *kTag = "lyra.time";
// Tried in order; one answer is enough.
constexpr const char *kServers[] = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};
constexpr int kReceiveTimeoutSeconds = 3;
constexpr uint32_t kTaskStack = 4096;
// Seconds from the NTP epoch (1900) to the Unix epoch (1970).
constexpr int64_t kNtpToUnix = 2208988800LL;

std::atomic<bool> s_enabled{false};
std::atomic<int> s_offset_minutes{0};
std::atomic<bool> s_running{false};
std::atomic<uint32_t> s_generation{0};

uint32_t read_be32(const uint8_t *bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

// Asks one server for the time. Returns Unix seconds in UTC, or 0.
int64_t query(const char *server)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(server, "123", &hints, &result) != 0 || !result) {
        ESP_LOGW(kTag, "cannot resolve %s", server);
        return 0;
    }
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        freeaddrinfo(result);
        return 0;
    }
    timeval timeout{kReceiveTimeoutSeconds, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // A minimal SNTP client request: version 3, client mode.
    uint8_t packet[48]{};
    packet[0] = 0x1B;
    int64_t unix_seconds = 0;
    const int sent = sendto(sock, packet, sizeof(packet), 0, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (sent == int(sizeof(packet))) {
        const int received = recv(sock, packet, sizeof(packet), 0);
        const unsigned mode = packet[0] & 0x07;
        const unsigned stratum = packet[1];
        // Server mode, and not a "kiss of death" (stratum 0) reply.
        if (received >= int(sizeof(packet)) && mode == 4 && stratum != 0 && stratum < 16) {
            const uint32_t seconds = read_be32(packet + 40);
            // NTP era 0 ends in 2036; small values belong to era 1.
            int64_t ntp = seconds;
            if (ntp < kNtpToUnix) ntp += int64_t(1) << 32;
            unix_seconds = ntp - kNtpToUnix;
        }
    }
    close(sock);
    if (!unix_seconds) ESP_LOGW(kTag, "no usable reply from %s", server);
    return unix_seconds;
}

void sync_task(void *)
{
    int64_t utc = 0;
    for (const char *server : kServers) {
        if (!s_enabled.load()) break;
        utc = query(server);
        if (utc) break;
    }
    if (utc && s_enabled.load()) {
        const int offset = s_offset_minutes.load();
        const std::time_t local = static_cast<std::time_t>(utc + int64_t(offset) * 60);
        if (lyra::clock::set_epoch(local) == ESP_OK) {
            s_generation.fetch_add(1, std::memory_order_release);
            ESP_LOGI(kTag, "clock set from network time (UTC%+d min)", offset);
        }
    }
    s_running.store(false);
    vTaskDelete(nullptr);
}

} // namespace

void configure(bool enabled, int offset_minutes)
{
    s_offset_minutes.store(offset_minutes);
    s_enabled.store(enabled);
}

void request()
{
    if (!s_enabled.load() || s_running.exchange(true)) return;
    // Sockets and DNS run on an internal-RAM stack.
    if (xTaskCreate(sync_task, "lyra_time", kTaskStack, nullptr, 2, nullptr) != pdPASS) {
        s_running.store(false);
        ESP_LOGW(kTag, "cannot start the time sync task");
    }
}

uint32_t generation()
{
    return s_generation.load(std::memory_order_acquire);
}

} // namespace lyra::time_sync
