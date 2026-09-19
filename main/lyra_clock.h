/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace lyra::clock {

// The JC3248W535EN has no battery-backed RTC. Keep the firmware's valid time
// range aligned with the FAT filesystem's supported dates and use this epoch
// whenever the ESP32 clock contains no usable RTC value at boot.
constexpr int kDefaultYear = 2000;
constexpr int kDefaultMonth = 1;
constexpr int kDefaultDay = 1;
constexpr int kDefaultHour = 0;
constexpr int kDefaultMinute = 0;

struct DateTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
};

// Initializes the system clock before filesystem mounting. Existing valid
// RTC/system-clock data is preserved; otherwise the clock starts at
// 00:00 01/01/2000 and continues advancing normally while powered.
esp_err_t init();

// True only when the most recent init used the no-RTC fallback epoch. A
// subsequent manual set or correction clears this state.
bool is_using_default_time();

DateTime now();
bool is_valid(const DateTime &value);

// Sets the system clock with seconds reset to zero. The new value takes effect
// immediately and is used by filesystem timestamps for subsequent writes.
esp_err_t set(const DateTime &value);

// Applies a manual daylight-saving correction to the running system clock.
// This is an explicit hour shift because the device has no timezone setting.
esp_err_t shift_minutes(int minutes);

} // namespace lyra::clock
