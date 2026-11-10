#include "ui_theme.h"

#include <stddef.h>

namespace theme {
namespace {

struct Rgb {
  uint8_t r, g, b;
};

struct Stop {
  int minute;
  Rgb top;
  Rgb bottom;
};

// Saturated colours: pastels look washed out on the CYD's IPS panel.
constexpr Rgb kNightTop = {40, 110, 255}, kNightBottom = {170, 50, 255};      // blue -> violet
constexpr Rgb kSunriseTop = {255, 200, 0}, kSunriseBottom = {255, 40, 80};    // gold -> red-pink
constexpr Rgb kDayTop = {255, 225, 0}, kDayBottom = {255, 90, 0};             // yellow -> orange
constexpr Rgb kSunsetTop = {255, 120, 0}, kSunsetBottom = {235, 0, 130};      // orange -> magenta
constexpr Rgb kEveningTop = {190, 40, 255}, kEveningBottom = {40, 70, 255};   // purple -> blue

constexpr Stop kStops[] = {
    {0, kNightTop, kNightBottom},
    {5 * 60, kNightTop, kNightBottom},
    {6 * 60 + 30, kSunriseTop, kSunriseBottom},
    {9 * 60, kDayTop, kDayBottom},
    {16 * 60, kDayTop, kDayBottom},
    {18 * 60 + 30, kSunsetTop, kSunsetBottom},
    {20 * 60 + 30, kEveningTop, kEveningBottom},
    {22 * 60, kNightTop, kNightBottom},
    {24 * 60, kNightTop, kNightBottom},
};

constexpr const char* kNames[] = {"NIGHT", "DAWN", "DAY", "DUSK", "EVE"};
constexpr Rgb kNamed[][2] = {{kNightTop, kNightBottom},
                             {kSunriseTop, kSunriseBottom},
                             {kDayTop, kDayBottom},
                             {kSunsetTop, kSunsetBottom},
                             {kEveningTop, kEveningBottom}};

uint16_t blend(const Rgb& a, const Rgb& b, int num, int den) {
  auto l = [&](uint8_t x, uint8_t y) { return uint8_t(x + (int(y) - int(x)) * num / den); };
  return rgb(l(a.r, b.r), l(a.g, b.g), l(a.b, b.b));
}

}  // namespace

Palette paletteForMinute(int minuteOfDay) {
  if (minuteOfDay < 0) minuteOfDay = 0;
  if (minuteOfDay >= 24 * 60) minuteOfDay = 24 * 60 - 1;
  for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
    const Stop& a = kStops[i - 1];
    const Stop& b = kStops[i];
    if (minuteOfDay <= b.minute) {
      int num = minuteOfDay - a.minute;
      int den = b.minute - a.minute;
      return {blend(a.top, b.top, num, den), blend(a.bottom, b.bottom, num, den)};
    }
  }
  return {blend(kStops[0].top, kStops[0].top, 0, 1), blend(kStops[0].bottom, kStops[0].bottom, 0, 1)};
}

int namedPaletteCount() { return sizeof(kNamed) / sizeof(kNamed[0]); }

Palette namedPalette(int index, const char** name) {
  if (name) *name = kNames[index];
  return {blend(kNamed[index][0], kNamed[index][0], 0, 1), blend(kNamed[index][1], kNamed[index][1], 0, 1)};
}

uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = b >> 11, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (br - ar) * t / 255;
  int g = ag + (bg - ag) * t / 255;
  int bl = ab + (bb - ab) * t / 255;
  return (r << 11) | (g << 5) | bl;
}

}  // namespace theme
