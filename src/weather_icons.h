// Chrono Node - weather icons drawn with anti-aliased primitives (no bitmaps).
#pragma once

#include <TFT_eSPI.h>
#include <stdint.h>

#include "weather.h"

namespace weather_icons {

// Draws `icon` centred at (cx, cy) in a box roughly `size` pixels wide.
// `bg` is the colour underneath (the moon's bite is cut with it).
// With `mono` set, everything is drawn in `monoColor` (used in night mode).
void draw(TFT_eSprite& s, int16_t cx, int16_t cy, int16_t size, weather::Icon icon, bool isDay,
          uint16_t bg, bool mono = false, uint16_t monoColor = 0);

}  // namespace weather_icons
