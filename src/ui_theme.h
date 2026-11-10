// Chrono Node - colour palette and the time-of-day gradient colours.
#pragma once

#include <stdint.h>

namespace theme {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// UI colours other than the time gradient. Night mode swaps in a dim pink
// set on pure black so the screen doesn't light up a dark room.
struct Colors {
  uint16_t bg;
  uint16_t text;
  uint16_t date;
  uint16_t dim;
  uint16_t faint;
  uint16_t warn;
  uint16_t error;
  uint16_t setup;
};

constexpr Colors kDayColors = {
    rgb(6, 8, 22),       // bg: deep navy
    rgb(210, 212, 225),  // text
    rgb(190, 192, 210),  // date
    rgb(130, 134, 160),  // dim
    rgb(48, 52, 76),     // faint
    rgb(255, 170, 40),   // warn
    rgb(255, 84, 96),    // error
    rgb(80, 210, 255),   // setup
};

constexpr Colors kNightColors = {
    rgb(0, 0, 0),        // bg: black
    rgb(190, 50, 140),   // text: dim pink, matching the night palette
    rgb(170, 44, 125),   // date
    rgb(120, 30, 90),    // dim
    rgb(44, 10, 34),     // faint
    rgb(230, 110, 0),    // warn
    rgb(255, 40, 60),    // error
    rgb(220, 80, 200),   // setup
};

// Two colours for vertical/horizontal gradients on the time digits.
struct Palette {
  uint16_t top;
  uint16_t bottom;
  bool operator==(const Palette& o) const { return top == o.top && bottom == o.bottom; }
  bool operator!=(const Palette& o) const { return !(*this == o); }
};

// Palette for a minute of the local day (0-1439), blended between stops:
// night hot pink->magenta, sunrise gold->red-pink, day yellow->orange,
// sunset orange->magenta, evening cyan->blue (blended in HSV).
Palette paletteForMinute(int minuteOfDay);

// Palette used while night mode is active.
Palette nightPalette();

// The five key palettes (night, sunrise, day, sunset, evening), for previews.
int namedPaletteCount();
Palette namedPalette(int index, const char** name);

// Linear blend between two colours; t in 0..255.
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);

}  // namespace theme
