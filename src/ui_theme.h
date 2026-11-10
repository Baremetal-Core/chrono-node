// Chrono Node - colour palette and the time-of-day gradient colours.
#pragma once

#include <stdint.h>

namespace theme {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t kBackground = rgb(6, 8, 22);  // deep navy
constexpr uint16_t kText = rgb(210, 212, 225);
constexpr uint16_t kDate = rgb(190, 192, 210);
constexpr uint16_t kTextDim = rgb(130, 134, 160);
constexpr uint16_t kTextFaint = rgb(48, 52, 76);
constexpr uint16_t kWarn = rgb(255, 170, 40);
constexpr uint16_t kError = rgb(255, 84, 96);
constexpr uint16_t kSetup = rgb(80, 210, 255);

// Two colours for vertical/horizontal gradients on the time digits.
struct Palette {
  uint16_t top;
  uint16_t bottom;
  bool operator==(const Palette& o) const { return top == o.top && bottom == o.bottom; }
  bool operator!=(const Palette& o) const { return !(*this == o); }
};

// Palette for a minute of the local day (0-1439), blended between stops:
// night blue->violet, sunrise gold->red-pink, day yellow->orange,
// sunset orange->magenta, evening purple->blue.
Palette paletteForMinute(int minuteOfDay);

// The five key palettes (night, sunrise, day, sunset, evening), for previews.
int namedPaletteCount();
Palette namedPalette(int index, const char** name);

// Linear blend between two colours; t in 0..255.
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);

}  // namespace theme
