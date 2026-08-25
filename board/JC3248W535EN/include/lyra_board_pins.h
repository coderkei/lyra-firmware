/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Native-facing board constants for the JC3248W535EN.
//
// This header is intentionally independent of Arduino. It is derived from the
// vendor DEMO_MP3 pincfg.h, esp_bsp.h, and the supplied IO distribution sheets.
// Validate each signal electrically during native ESP-IDF bring-up.

namespace lyra::board::jc3248w535en {

constexpr int kDisplayWidth = 320;
constexpr int kDisplayHeight = 480;
constexpr int kDisplayBitsPerPixel = 16;

// AXS15231B display over QSPI.
constexpr int kDisplayBacklightGpio = 1;
constexpr int kDisplayChipSelectGpio = 45;
constexpr int kDisplayClockGpio = 47;
constexpr int kDisplayData0Gpio = 21;
constexpr int kDisplayData1Gpio = 48;
constexpr int kDisplayData2Gpio = 40;
constexpr int kDisplayData3Gpio = 39;
constexpr int kDisplayTeGpio = 38;
constexpr int kDisplayResetGpio = -1; // Board/sample: not connected.
constexpr int kDisplayDcGpio = -1;    // QSPI panel configuration uses no DC pin.

// AXS15231B touch interface over I2C.
constexpr int kTouchI2cPort = 0;
constexpr int kTouchSclGpio = 8;
constexpr int kTouchSdaGpio = 4;
constexpr int kTouchInterruptGpio = 3; // Present in vendor pincfg.h.
constexpr int kTouchResetGpio = -1;
constexpr int kTouchInterruptInVendorBsp = -1; // Vendor port polls instead.

// MicroSD in the supplied demo's 1-bit SDMMC configuration.
constexpr int kSdData0Gpio = 13;
constexpr int kSdClockGpio = 12;
constexpr int kSdCommandGpio = 11;
constexpr int kSdSpiChipSelectGpio = 10; // Schematic/SPI-mode reference; not used by SDMMC 1-bit demo.

// On-board I2S speaker/amplifier path.
constexpr int kAudioI2sPort = 0;
constexpr int kAudioMckGpio = -1;
constexpr int kAudioBclkGpio = 42;
constexpr int kAudioLrclkGpio = 2;
constexpr int kAudioDataOutGpio = 41;

// Battery sense input. The supplied schematic shows a resistor divider; scale
// and calibration must be confirmed before exposing a percentage to users.
constexpr int kBatteryAdcGpio = 5;

constexpr int kDocumentedFlashBytes = 16 * 1024 * 1024;
constexpr int kDocumentedPsramBytes = 8 * 1024 * 1024;

} // namespace lyra::board::jc3248w535en
