#include "ui_theme.h"

#include <stddef.h>

namespace theme {
namespace {

struct Stop {
  int minute;
  uint8_t r, g, b;
};

constexpr Stop kStops[] = {
    {0, 96, 120, 255},      // night: indigo
    {5 * 60, 96, 120, 255},
    {6 * 60 + 30, 255, 128, 96},   // sunrise: coral
    {9 * 60, 255, 184, 72},        // day: amber
    {16 * 60, 255, 184, 72},
    {18 * 60 + 30, 255, 104, 120}, // sunset: rose
    {20 * 60 + 30, 160, 110, 255}, // evening: violet
    {22 * 60, 96, 120, 255},
    {24 * 60, 96, 120, 255},
};

uint8_t lerp(uint8_t a, uint8_t b, int num, int den) { return a + (int(b) - int(a)) * num / den; }

}  // namespace

uint16_t accentForMinute(int minuteOfDay) {
  if (minuteOfDay < 0) minuteOfDay = 0;
  if (minuteOfDay >= 24 * 60) minuteOfDay = 24 * 60 - 1;
  for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
    const Stop& a = kStops[i - 1];
    const Stop& b = kStops[i];
    if (minuteOfDay <= b.minute) {
      int num = minuteOfDay - a.minute;
      int den = b.minute - a.minute;
      return rgb(lerp(a.r, b.r, num, den), lerp(a.g, b.g, num, den), lerp(a.b, b.b, num, den));
    }
  }
  return rgb(kStops[0].r, kStops[0].g, kStops[0].b);
}

uint16_t dim(uint16_t color, uint8_t level) {
  uint32_t r = ((color >> 11) & 0x1F) * level / 255;
  uint32_t g = ((color >> 5) & 0x3F) * level / 255;
  uint32_t b = (color & 0x1F) * level / 255;
  return (r << 11) | (g << 5) | b;
}

}  // namespace theme
