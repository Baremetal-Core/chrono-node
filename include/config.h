// Chrono Node - project-wide configuration constants.
#pragma once

#include <stdint.h>

namespace config {

constexpr const char* kProjectName = "Chrono Node";
constexpr const char* kFirmwareName = "chrono-node-fw";
constexpr const char* kFirmwareVersion = "0.1.0";

constexpr uint32_t kSerialBaud = 115200;

// External RTC on the CYD LEFT 4-pin connector (3.3V, GPIO27, GPIO22, GND).
// Pins are routed through the ESP32 GPIO matrix; do not rely on default I2C pins.
constexpr int kI2cSdaPin = 27;
constexpr int kI2cSclPin = 22;
constexpr uint32_t kI2cFrequencyHz = 100000;

constexpr uint8_t kDs3231Address = 0x68;

// The RTC is polled faster than 1 Hz so the display follows the DS3231 second
// boundary closely; the screen is only redrawn when the second changes.
constexpr uint32_t kRtcPollIntervalMs = 100;
constexpr uint32_t kRtcReprobeIntervalMs = 2000;

// Set to false to silence the once-per-second time log.
constexpr bool kLogEverySecond = true;

}  // namespace config
