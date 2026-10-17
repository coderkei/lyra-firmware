/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_wifi.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "ping/ping_sock.h"

namespace lyra::wifi {
namespace {

constexpr char kTag[] = "lyra.wifi";
constexpr char kNvsNamespace[] = "lyra";
constexpr char kEnabledKey[] = "wifi_enabled";
constexpr char kSsidKey[] = "wifi_ssid";
constexpr char kPasswordKey[] = "wifi_password";
constexpr uint32_t kStationStartedBit = 1U << 0;
constexpr uint32_t kStationDisconnectedBit = 1U << 1;
constexpr uint32_t kConnectWaitMs = 2500;
constexpr uint8_t kMaxReconnectAttempts = 4;
constexpr uint32_t kPingTaskStack = 6144;
constexpr uint8_t kPingCount = 4;

SemaphoreHandle_t s_mutex;
EventGroupHandle_t s_events;
esp_netif_t *s_station_netif;
Status s_status{};
char s_saved_password[64]{};
bool s_driver_started;
bool s_connect_request_active;
bool s_user_disconnect;
uint8_t s_reconnect_attempts;
uint32_t s_connect_serial;
PingTarget s_active_ping_target;

void copy_c_string(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    if (!source) {
        destination[0] = '\0';
        return;
    }
    const size_t source_length = std::strlen(source);
    const size_t copy_length = source_length < capacity - 1 ?
        source_length : capacity - 1;
    if (copy_length > 0) std::memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

PingStatus &ping_status(PingTarget target)
{
    return target == PingTarget::Ip ? s_status.ip_ping : s_status.dns_ping;
}

void bump_generation_locked()
{
    ++s_status.generation;
    if (s_status.generation == 0) ++s_status.generation;
}

bool is_initialized()
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool initialized = s_status.initialized;
    xSemaphoreGive(s_mutex);
    return initialized;
}

esp_err_t persist_preferences(bool enabled, const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, kEnabledKey, enabled ? 1 : 0);
    if (result == ESP_OK && ssid) result = nvs_set_str(handle, kSsidKey, ssid);
    if (result == ESP_OK && password) result = nvs_set_str(handle, kPasswordKey, password);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

void read_preferences()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;

    uint8_t enabled = 0;
    if (nvs_get_u8(handle, kEnabledKey, &enabled) == ESP_OK && enabled <= 1) {
        s_status.enabled = enabled != 0;
    }
    size_t ssid_size = sizeof(s_status.saved_ssid);
    if (nvs_get_str(handle, kSsidKey, s_status.saved_ssid, &ssid_size) != ESP_OK) {
        s_status.saved_ssid[0] = '\0';
    }
    size_t password_size = sizeof(s_saved_password);
    if (nvs_get_str(handle, kPasswordKey, s_saved_password, &password_size) != ESP_OK) {
        s_saved_password[0] = '\0';
    }
    s_status.has_saved_network = s_status.saved_ssid[0] != '\0';
    nvs_close(handle);
}

wifi_config_t make_station_config(const char *ssid, const char *password)
{
    wifi_config_t config{};
    const size_t ssid_length = std::strlen(ssid);
    const size_t ssid_copy_length = ssid_length < sizeof(config.sta.ssid) ?
        ssid_length : sizeof(config.sta.ssid);
    if (ssid_copy_length > 0) {
        std::memcpy(config.sta.ssid, ssid, ssid_copy_length);
    }
    copy_c_string(reinterpret_cast<char *>(config.sta.password),
                  sizeof(config.sta.password), password);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    return config;
}

void set_last_error(esp_err_t error)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.connecting = false;
    s_status.connection_failed = s_status.enabled && s_status.has_saved_network;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGW(kTag, "Wi-Fi operation failed: %s", esp_err_to_name(error));
}

void scan_complete()
{
    uint16_t count = 0;
    esp_err_t result = esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = nullptr;
    uint16_t capacity = static_cast<uint16_t>(kMaxAccessPoints);
    if (result == ESP_OK && count > 0) {
        records = static_cast<wifi_ap_record_t *>(
            std::calloc(capacity, sizeof(wifi_ap_record_t)));
        if (!records) result = ESP_ERR_NO_MEM;
        else result = esp_wifi_scan_get_ap_records(&capacity, records);
    } else if (result == ESP_OK) {
        // Retrieving the records also releases the driver's scan list.
        esp_wifi_clear_ap_list();
    }
    if (result != ESP_OK) esp_wifi_clear_ap_list();

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.scanning = false;
        s_status.access_point_count = 0;
        if (result == ESP_OK && records) {
            const uint16_t available = capacity;
            for (uint16_t index = 0; index < available &&
                 s_status.access_point_count < kMaxAccessPoints; ++index) {
                const wifi_ap_record_t &record = records[index];
                if (record.ssid[0] == '\0') continue;
                AccessPoint &ap = s_status.access_points[s_status.access_point_count++];
                std::memset(&ap, 0, sizeof(ap));
                copy_c_string(ap.ssid, sizeof(ap.ssid),
                              reinterpret_cast<const char *>(record.ssid));
                ap.rssi = record.rssi;
                ap.secured = record.authmode != WIFI_AUTH_OPEN;
                ap.saved = s_status.has_saved_network &&
                    std::strcmp(ap.ssid, s_status.saved_ssid) == 0;
                ap.connected = s_status.connected &&
                    std::strcmp(ap.ssid, s_status.connected_ssid) == 0;
            }
        }
        bump_generation_locked();
        xSemaphoreGive(s_mutex);
    }
    std::free(records);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not read Wi-Fi scan results: %s", esp_err_to_name(result));
    }
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id,
                        void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            bool should_connect = false;
            bool should_scan = false;
            char saved_ssid[33]{};
            char saved_password[64]{};
            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_driver_started = true;
                should_connect = s_status.enabled && s_status.has_saved_network &&
                    !s_connect_request_active;
                should_scan = s_status.enabled && !s_status.has_saved_network;
                if (should_connect) {
                    copy_c_string(saved_ssid, sizeof(saved_ssid), s_status.saved_ssid);
                    copy_c_string(saved_password, sizeof(saved_password), s_saved_password);
                }
                s_status.connecting = should_connect || s_connect_request_active;
                bump_generation_locked();
                xSemaphoreGive(s_mutex);
            }
            if (s_events) xEventGroupSetBits(s_events, kStationStartedBit);
            if (should_connect) {
                wifi_config_t config = make_station_config(saved_ssid, saved_password);
                const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &config);
                const esp_err_t result = configured == ESP_OK ? esp_wifi_connect() : configured;
                if (result != ESP_OK) set_last_error(result);
            } else if (should_scan) {
                scan();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
            bool retry = false;
            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_status.connected = false;
                s_status.connected_ssid[0] = '\0';
                s_status.ip_address[0] = '\0';
                s_status.scanning = false;
                const bool connection_was_pending = s_status.connecting;
                retry = s_status.enabled && s_status.has_saved_network &&
                    !s_connect_request_active && !s_user_disconnect &&
                    s_reconnect_attempts < kMaxReconnectAttempts;
                s_status.connecting = retry;
                s_status.connection_failed = connection_was_pending &&
                    !s_connect_request_active && !retry &&
                    s_status.has_saved_network && !s_user_disconnect;
                if (retry) ++s_reconnect_attempts;
                else s_reconnect_attempts = 0;
                s_user_disconnect = false;
                bump_generation_locked();
                xSemaphoreGive(s_mutex);
            }
            if (s_events) xEventGroupSetBits(s_events, kStationDisconnectedBit);
            if (event) {
                ESP_LOGI(kTag, "station disconnected (reason %u)%s",
                         static_cast<unsigned>(event->reason), retry ? "; retrying" : "");
            }
            if (retry) {
                const esp_err_t result = esp_wifi_connect();
                if (result != ESP_OK) set_last_error(result);
            }
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {
            scan_complete();
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            if (s_mutex) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_driver_started = false;
                s_status.connected = false;
                s_status.connecting = false;
                s_status.connection_failed = false;
                s_status.scanning = false;
                s_status.connected_ssid[0] = '\0';
                s_status.ip_address[0] = '\0';
                bump_generation_locked();
                xSemaphoreGive(s_mutex);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);
        char connected_ssid[33]{};
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.connected = true;
            s_status.connecting = false;
            s_status.connection_failed = false;
            s_status.has_saved_network = s_status.saved_ssid[0] != '\0';
            copy_c_string(s_status.connected_ssid, sizeof(s_status.connected_ssid),
                          s_status.saved_ssid);
            copy_c_string(connected_ssid, sizeof(connected_ssid),
                          s_status.connected_ssid);
            std::snprintf(s_status.ip_address, sizeof(s_status.ip_address), IPSTR,
                          IP2STR(&event->ip_info.ip));
            s_reconnect_attempts = 0;
            bump_generation_locked();
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(kTag, "connected to %s, IP " IPSTR, connected_ssid,
                 IP2STR(&event->ip_info.ip));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.connected = false;
            s_status.ip_address[0] = '\0';
            bump_generation_locked();
            xSemaphoreGive(s_mutex);
        }
    }
}

struct ConnectRequest {
    uint32_t serial;
    char ssid[33];
    char password[64];
};

bool connect_request_is_current(uint32_t serial)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool current = s_status.enabled && s_connect_request_active &&
                         s_connect_serial == serial;
    xSemaphoreGive(s_mutex);
    return current;
}

void connect_worker(void *context)
{
    auto *request = static_cast<ConnectRequest *>(context);
    if (!request) {
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t request_serial = request->serial;
    if (!connect_request_is_current(request_serial)) {
        std::free(request);
        vTaskDelete(nullptr);
        return;
    }

    wifi_config_t config = make_station_config(request->ssid, request->password);
    bool driver_started = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    driver_started = s_driver_started;
    xSemaphoreGive(s_mutex);
    esp_err_t result = ESP_OK;
    if (!driver_started) {
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (s_events) xEventGroupClearBits(s_events, kStationStartedBit);
        if (result == ESP_OK) result = esp_wifi_start();
        if (result == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_driver_started = true;
            xSemaphoreGive(s_mutex);
            const EventBits_t bits = xEventGroupWaitBits(s_events, kStationStartedBit,
                pdFALSE, pdTRUE, pdMS_TO_TICKS(kConnectWaitMs));
            if ((bits & kStationStartedBit) == 0) result = ESP_ERR_TIMEOUT;
        }
    } else {
        bool was_active = false;
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            was_active = s_status.connected || s_status.connecting;
            xSemaphoreGive(s_mutex);
        }
        if (was_active) {
            if (s_events) xEventGroupClearBits(s_events, kStationDisconnectedBit);
            const esp_err_t disconnect_result = esp_wifi_disconnect();
            if (disconnect_result == ESP_OK && s_events) {
                xEventGroupWaitBits(s_events, kStationDisconnectedBit, pdTRUE, pdTRUE,
                                    pdMS_TO_TICKS(kConnectWaitMs));
            }
        }
        if (!connect_request_is_current(request_serial)) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            result = esp_wifi_set_config(WIFI_IF_STA, &config);
        }
    }

    if (result == ESP_OK && connect_request_is_current(request_serial)) {
        result = esp_wifi_connect();
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_STATE;
    }
    bool request_current = false;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        request_current = s_connect_serial == request_serial;
        if (request_current) {
            s_connect_request_active = false;
            s_user_disconnect = false;
            if (result != ESP_OK) {
                s_status.connecting = false;
                s_status.connection_failed = true;
                bump_generation_locked();
            }
        }
        xSemaphoreGive(s_mutex);
    }
    if (result != ESP_OK && request_current) {
        ESP_LOGW(kTag, "could not connect to %s: %s", request->ssid,
                 esp_err_to_name(result));
    }
    std::free(request);
    vTaskDelete(nullptr);
}

void set_ping_error(PingTarget target, bool dns_lookup_failed)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    PingStatus &ping = ping_status(target);
    ping.running = false;
    ping.complete = true;
    ping.dns_lookup_failed = dns_lookup_failed;
    ping.transmitted = 0;
    ping.received = 0;
    ping.duration_ms = 0;
    ping.resolved_address[0] = '\0';
    bump_generation_locked();
    xSemaphoreGive(s_mutex);
}

void ping_end_callback(esp_ping_handle_t handle, void *context)
{
    const PingTarget target = *static_cast<PingTarget *>(context);
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(handle, ESP_PING_PROF_DURATION, &duration, sizeof(duration));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    PingStatus &ping = ping_status(target);
    ping.running = false;
    ping.complete = true;
    ping.transmitted = transmitted;
    ping.received = received;
    ping.duration_ms = duration;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);
    esp_ping_delete_session(handle);
}

void ping_task(void *context)
{
    const PingTarget target = static_cast<PingTarget>(reinterpret_cast<uintptr_t>(context));
    ip_addr_t target_address{};
    char resolved_address[40]{};

    if (target == PingTarget::Ip) {
        ipaddr_aton("1.1.1.1", &target_address);
        copy_c_string(resolved_address, sizeof(resolved_address), "1.1.1.1");
    } else {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo *addresses = nullptr;
        if (getaddrinfo("example.com", nullptr, &hints, &addresses) != 0 || !addresses) {
            if (addresses) freeaddrinfo(addresses);
            set_ping_error(target, true);
            vTaskDelete(nullptr);
            return;
        }
        if (addresses->ai_family == AF_INET) {
            const auto *address = reinterpret_cast<const sockaddr_in *>(addresses->ai_addr);
            inet_addr_to_ip4addr(ip_2_ip4(&target_address), &address->sin_addr);
            inet_ntop(AF_INET, &address->sin_addr, resolved_address, sizeof(resolved_address));
        } else if (addresses->ai_family == AF_INET6) {
            const auto *address = reinterpret_cast<const sockaddr_in6 *>(addresses->ai_addr);
            inet6_addr_to_ip6addr(ip_2_ip6(&target_address), &address->sin6_addr);
            inet_ntop(AF_INET6, &address->sin6_addr, resolved_address, sizeof(resolved_address));
        } else {
            freeaddrinfo(addresses);
            set_ping_error(target, true);
            vTaskDelete(nullptr);
            return;
        }
        freeaddrinfo(addresses);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    PingStatus &ping = ping_status(target);
    copy_c_string(ping.resolved_address, sizeof(ping.resolved_address), resolved_address);
    xSemaphoreGive(s_mutex);

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target_address;
    config.count = kPingCount;
    config.timeout_ms = 1000;
    config.interval_ms = 1000;
    esp_ping_callbacks_t callbacks{};
    callbacks.cb_args = &s_active_ping_target;
    callbacks.on_ping_end = ping_end_callback;
    esp_ping_handle_t handle = nullptr;
    esp_err_t result = esp_ping_new_session(&config, &callbacks, &handle);
    if (result == ESP_OK) result = esp_ping_start(handle);
    if (result != ESP_OK) {
        if (handle) esp_ping_delete_session(handle);
        set_ping_error(target, false);
    }
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init()
{
    if (s_status.initialized) return ESP_OK;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_mutex || !s_events) return ESP_ERR_NO_MEM;

    esp_err_t result = esp_netif_init();
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    if (result != ESP_OK) return result;
    s_station_netif = esp_netif_create_default_wifi_sta();
    if (!s_station_netif) return ESP_FAIL;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&config);
    if (result != ESP_OK) return result;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) {
        result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, nullptr);
    }
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, nullptr);
    }
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                            wifi_event_handler, nullptr);
    }
    if (result != ESP_OK) return result;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    read_preferences();
    s_status.initialized = true;
    s_status.has_saved_network = s_status.saved_ssid[0] != '\0';
    bump_generation_locked();
    const bool enabled = s_status.enabled;
    xSemaphoreGive(s_mutex);

    if (s_status.has_saved_network) {
        wifi_config_t station_config = make_station_config(s_status.saved_ssid,
                                                            s_saved_password);
        result = esp_wifi_set_config(WIFI_IF_STA, &station_config);
        if (result != ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.initialized = false;
            xSemaphoreGive(s_mutex);
            return result;
        }
    }
    if (enabled) {
        result = esp_wifi_start();
        if (result == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_driver_started = true;
            xSemaphoreGive(s_mutex);
        }
        if (result != ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.initialized = false;
            xSemaphoreGive(s_mutex);
            return result;
        }
    }
    ESP_LOGI(kTag, "Wi-Fi station initialized (%s)", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

Status status()
{
    Status result{};
    if (!s_mutex) return result;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    result = s_status;
    for (size_t index = 0; index < result.access_point_count; ++index) {
        AccessPoint &access_point = result.access_points[index];
        access_point.saved = result.has_saved_network &&
            std::strcmp(access_point.ssid, result.saved_ssid) == 0;
        access_point.connected = result.connected &&
            std::strcmp(access_point.ssid, result.connected_ssid) == 0;
    }
    xSemaphoreGive(s_mutex);
    if (result.connected) {
        wifi_ap_record_t connected_ap{};
        if (esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK) {
            result.connected_rssi = connected_ap.rssi;
            result.has_connected_rssi = true;
        }
    }
    return result;
}

esp_err_t set_enabled(bool enabled)
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    const Status previous = status();
    if (previous.enabled == enabled) return ESP_OK;
    esp_err_t result = persist_preferences(enabled, nullptr, nullptr);
    if (result != ESP_OK) return result;

    if (enabled) {
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.enabled = true;
            s_status.connecting = s_status.has_saved_network;
            s_status.connection_failed = false;
            s_reconnect_attempts = 0;
            s_user_disconnect = false;
            bump_generation_locked();
            xSemaphoreGive(s_mutex);
        }
        result = esp_wifi_start();
        if (result == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_driver_started = true;
            xSemaphoreGive(s_mutex);
        }
    } else {
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.enabled = false;
            s_status.connecting = false;
            s_status.connection_failed = false;
            s_user_disconnect = true;
            s_connect_request_active = false;
            ++s_connect_serial;
            bump_generation_locked();
            xSemaphoreGive(s_mutex);
        }
        result = esp_wifi_stop();
        if (result == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_driver_started = false;
            xSemaphoreGive(s_mutex);
        }
    }
    if (result != ESP_OK) {
        persist_preferences(previous.enabled, nullptr, nullptr);
        if (s_mutex) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.enabled = previous.enabled;
            s_status.connecting = previous.connecting;
            s_status.connection_failed = previous.connection_failed;
            s_user_disconnect = false;
            bump_generation_locked();
            xSemaphoreGive(s_mutex);
        }
    }
    return result;
}

esp_err_t scan()
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool enabled = s_status.enabled;
    const bool already_scanning = s_status.scanning;
    xSemaphoreGive(s_mutex);
    if (!enabled) return ESP_ERR_INVALID_STATE;
    if (already_scanning) return ESP_OK;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.scanning = true;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);
    wifi_scan_config_t config{};
    const esp_err_t result = esp_wifi_scan_start(&config, false);
    if (result != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.scanning = false;
        bump_generation_locked();
        xSemaphoreGive(s_mutex);
    }
    return result;
}

esp_err_t connect(const char *ssid, const char *password)
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || !ssid[0]) return ESP_ERR_INVALID_ARG;
    const size_t ssid_length = std::strlen(ssid);
    const size_t password_length = std::strlen(password);
    if (ssid_length > 32 || password_length > 63) return ESP_ERR_INVALID_ARG;

    auto *request = static_cast<ConnectRequest *>(std::calloc(1, sizeof(ConnectRequest)));
    if (!request) return ESP_ERR_NO_MEM;
    copy_c_string(request->ssid, sizeof(request->ssid), ssid);
    copy_c_string(request->password, sizeof(request->password), password);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_connect_request_active) {
        xSemaphoreGive(s_mutex);
        std::free(request);
        return ESP_ERR_INVALID_STATE;
    }
    s_connect_request_active = true;
    ++s_connect_serial;
    if (s_connect_serial == 0) ++s_connect_serial;
    request->serial = s_connect_serial;
    s_status.enabled = true;
    s_status.connecting = true;
    s_status.connection_failed = false;
    s_status.has_saved_network = true;
    copy_c_string(s_status.saved_ssid, sizeof(s_status.saved_ssid), ssid);
    copy_c_string(s_saved_password, sizeof(s_saved_password), password);
    s_user_disconnect = false;
    s_reconnect_attempts = 0;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);

    esp_err_t result = persist_preferences(true, ssid, password);
    if (result != ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connect_request_active = false;
        s_status.connecting = false;
        bump_generation_locked();
        xSemaphoreGive(s_mutex);
        std::free(request);
        return result;
    }

    const BaseType_t created = xTaskCreate(connect_worker, "lyra_wifi_connect", 4096,
                                            request, 5, nullptr);
    if (created != pdPASS) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_connect_request_active = false;
        s_status.connecting = false;
        bump_generation_locked();
        xSemaphoreGive(s_mutex);
        std::free(request);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t connect_saved()
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    char ssid[33]{};
    char password[64]{};
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_c_string(ssid, sizeof(ssid), s_status.saved_ssid);
    copy_c_string(password, sizeof(password), s_saved_password);
    xSemaphoreGive(s_mutex);
    if (!ssid[0]) return ESP_ERR_NOT_FOUND;
    return connect(ssid, password);
}

esp_err_t disconnect()
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_user_disconnect = true;
    s_connect_request_active = false;
    ++s_connect_serial;
    s_status.connecting = false;
    s_status.connection_failed = false;
    bump_generation_locked();
    const bool started = s_driver_started;
    xSemaphoreGive(s_mutex);
    return started ? esp_wifi_disconnect() : ESP_OK;
}

esp_err_t forget_network()
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = persist_preferences(status().enabled, "", "");
    if (result != ESP_OK) return result;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.has_saved_network = false;
    s_status.saved_ssid[0] = '\0';
    s_saved_password[0] = '\0';
    s_user_disconnect = true;
    s_connect_request_active = false;
    ++s_connect_serial;
    s_status.connecting = false;
    s_status.connection_failed = false;
    const bool started = s_driver_started;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);
    if (started) esp_wifi_disconnect();
    return ESP_OK;
}

esp_err_t start_ping(PingTarget target)
{
    if (!is_initialized()) return ESP_ERR_INVALID_STATE;
    if (target != PingTarget::Ip && target != PingTarget::Dns) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_status.connected) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status.ip_ping.running || s_status.dns_ping.running) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    PingStatus &ping = ping_status(target);
    ping = {};
    ping.running = true;
    s_active_ping_target = target;
    bump_generation_locked();
    xSemaphoreGive(s_mutex);

    const BaseType_t created = xTaskCreate(ping_task, "lyra_ping", kPingTaskStack,
        reinterpret_cast<void *>(static_cast<uintptr_t>(target)), 4, nullptr);
    if (created != pdPASS) {
        set_ping_error(target, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

} // namespace lyra::wifi
