// Chrono Node - colour palette and the time-of-day accent.
#pragma once

#include <stdint.h>

namespace theme {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t kBackground = rgb(0, 0, 0);
constexpr uint16_t kDigits = rgb(240, 240, 245);
constexpr uint16_t kText = rgb(200, 200, 208);
constexpr uint16_t kDate = rgb(170, 170, 180);
constexpr uint16_t kTextDim = rgb(120, 120, 132);
constexpr uint16_t kTextFaint = rgb(56, 56, 64);
constexpr uint16_t kWarn = rgb(255, 170, 40);
constexpr uint16_t kError = rgb(255, 80, 80);
constexpr uint16_t kSetup = rgb(80, 200, 255);

// Accent for a minute of the local day (0-1439): indigo at night, coral at
// sunrise, warm amber through the day, rose and violet in the evening.
uint16_t accentForMinute(int minuteOfDay);

// Scales a colour's brightness by level/255.
uint16_t dim(uint16_t color, uint8_t level);

}  // namespace theme
