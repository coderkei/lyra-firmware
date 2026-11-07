/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

namespace lyra::usb_transfer {

// True when the firmware was built with TinyUSB mass-storage support
// (CONFIG_TINYUSB_MSC_ENABLED).
bool available();

// Shows the MicroSD card to a computer as a USB drive. The ESP32-S3 has one
// USB PHY, so this replaces the USB serial/JTAG port (logs, monitor and
// flashing) until restart. Call only after lyra::media::shutdown() unmounted
// the card: the computer must be its only user.
esp_err_t start();

// Detaches the drive and restarts, which brings the serial/JTAG port back.
// Nothing about this mode is persisted, so every boot starts normally.
[[noreturn]] void finish_and_restart();

} // namespace lyra::usb_transfer
