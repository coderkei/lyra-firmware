/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace lyra::boot_test {

// Requests a one-shot reboot into OTA slot 0 or 1. The original running
// partition is restored as soon as the test image starts.
esp_err_t request_once(uint8_t ota_slot);

// Verifies that OTA slot 0 or 1 contains a bootable application image.
// This does not alter OTA data or reboot the device.
esp_err_t validate(uint8_t ota_slot);

// Called before normal application startup so a test boot never becomes the
// persistent active partition.
void restore_pending();

} // namespace lyra::boot_test
