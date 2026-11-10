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

constexpr Stop kStops[] = {
    {0, {130, 150, 255}, {200, 120, 255}},             // night
    {5 * 60, {130, 150, 255}, {200, 120, 255}},
    {6 * 60 + 30, {255, 210, 100}, {255, 110, 120}},   // sunrise
    {9 * 60, {255, 222, 110}, {255, 140, 60}},         // day
    {16 * 60, {255, 222, 110}, {255, 140, 60}},
    {18 * 60 + 30, {255, 160, 90}, {245, 85, 160}},    // sunset
    {20 * 60 + 30, {205, 130, 255}, {120, 120, 255}},  // evening
    {22 * 60, {130, 150, 255}, {200, 120, 255}},
    {24 * 60, {130, 150, 255}, {200, 120, 255}},
};

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

uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = b >> 11, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (br - ar) * t / 255;
  int g = ag + (bg - ag) * t / 255;
  int bl = ab + (bb - ab) * t / 255;
  return (r << 11) | (g << 5) | bl;
}

}  // namespace theme
