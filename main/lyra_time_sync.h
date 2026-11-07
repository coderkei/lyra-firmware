/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace lyra::time_sync {

// Network time. The clock keeps local time directly (there is no time zone
// database), so a sync asks an NTP server for UTC and sets the clock to UTC
// plus offset_minutes, which already includes any daylight-saving hour.
// Safe to call from any task.
void configure(bool enabled, int offset_minutes);

// Starts a sync in the background when enabled. Called when Wi-Fi gets an IP
// address and after the settings change; failures are logged and retried on
// the next call.
void request();

// Changes after every successful sync. Lock-free.
uint32_t generation();

} // namespace lyra::time_sync
