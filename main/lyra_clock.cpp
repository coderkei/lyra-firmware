/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_clock.h"

#include <cerrno>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"

namespace lyra::clock {
namespace {

constexpr const char *kTag = "lyra.clock";
constexpr std::time_t kDefaultEpoch = 946684800; // 2000-01-01 00:00:00 UTC
bool s_using_default_time = false;

DateTime default_time()
{
    return {kDefaultYear, kDefaultMonth, kDefaultDay, kDefaultHour, kDefaultMinute};
}

bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month)
{
    constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month];
}

std::time_t to_epoch(const DateTime &value)
{
    std::tm local{};
    local.tm_year = value.year - 1900;
    local.tm_mon = value.month - 1;
    local.tm_mday = value.day;
    local.tm_hour = value.hour;
    local.tm_min = value.minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return std::mktime(&local);
}

} // namespace

bool is_valid(const DateTime &value)
{
    return value.year >= kDefaultYear && value.year <= 2099 &&
           value.month >= 1 && value.month <= 12 &&
           value.day >= 1 && value.day <= days_in_month(value.year, value.month) &&
           value.hour >= 0 && value.hour <= 23 &&
           value.minute >= 0 && value.minute <= 59;
}

esp_err_t set(const DateTime &value)
{
    if (!is_valid(value)) return ESP_ERR_INVALID_ARG;
    const std::time_t timestamp = to_epoch(value);
    if (timestamp < kDefaultEpoch) return ESP_ERR_INVALID_ARG;

    const timeval current{timestamp, 0};
    if (settimeofday(&current, nullptr) != 0) {
        ESP_LOGE(kTag, "could not set system clock: errno=%d", errno);
        return ESP_FAIL;
    }
    s_using_default_time = false;
    ESP_LOGI(kTag, "system clock set to %04d-%02d-%02d %02d:%02d",
             value.year, value.month, value.day, value.hour, value.minute);
    return ESP_OK;
}

esp_err_t shift_minutes(int minutes)
{
    if (minutes == 0) return ESP_OK;
    const std::time_t current = std::time(nullptr);
    if (current < kDefaultEpoch) return ESP_ERR_INVALID_STATE;
    const std::time_t shifted = current + static_cast<std::time_t>(minutes) * 60;
    if (shifted < kDefaultEpoch) return ESP_ERR_INVALID_ARG;

    const timeval value{shifted, 0};
    if (settimeofday(&value, nullptr) != 0) {
        ESP_LOGE(kTag, "could not shift system clock: errno=%d", errno);
        return ESP_FAIL;
    }
    s_using_default_time = false;
    return ESP_OK;
}

esp_err_t init()
{
    const std::time_t current = std::time(nullptr);
    if (current >= kDefaultEpoch) {
        s_using_default_time = false;
        ESP_LOGI(kTag, "using existing system/RTC clock value");
        return ESP_OK;
    }

    const timeval fallback{kDefaultEpoch, 0};
    const esp_err_t result = settimeofday(&fallback, nullptr) == 0 ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK) {
        s_using_default_time = true;
        ESP_LOGW(kTag, "no RTC time available; starting at 00:00 01/01/2000");
    } else {
        ESP_LOGE(kTag, "could not initialize fallback clock: errno=%d", errno);
    }
    return result;
}

bool is_using_default_time()
{
    return s_using_default_time;
}

DateTime now()
{
    const std::time_t timestamp = std::time(nullptr);
    std::tm local{};
    if (timestamp >= kDefaultEpoch && localtime_r(&timestamp, &local) != nullptr) {
        return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min};
    }
    return default_time();
}

} // namespace lyra::clock
