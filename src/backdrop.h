// Chrono Node - background themes behind the clock face, like a smartwatch.
//
// Every theme is computed (no stored images, except Photo's 80x60 copy of a
// stored photo): `row()` returns any part of a screen row, so the display can
// composite the time, date and dial over it band by band. The tables are made
// by setup() when the clock face shows and freed by end().
//
// Motion: in Night sky and Synthwave some stars twinkle and, by day, a
// shooting star crosses the sky now and then. sparkles() lists the pixels whose
// colour changed; the display replaces only those still showing the old colour,
// so nothing drawn over the background is touched.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace backdrop {

enum class Theme : uint8_t { None, Glow, Sky, Aurora, Bokeh, Waves, Synthwave, Photo };
constexpr int kThemeCount = 8;

const char* name(Theme t);                      // "glow", "sky", ...
const char* title(Theme t);                     // "Glow", "Night sky", ...
bool fromName(const char* name, Theme& out);    // also accepts "none"/"off"

// Builds the theme's tables. `bg` is the plain background, `accent`/`accent2`
// the colour style's two colours (Glow, Bokeh and Waves use them). `night`:
// dimmer, no shooting stars. `seed` varies the stars, ribbons and lights.
// `photoPath`: the JPEG for Photo. Returns false (and draws plain `bg`) when
// there's no memory or no photo.
bool setup(Theme t, uint16_t bg, uint16_t accent, uint16_t accent2, bool night, uint32_t seed,
           const char* photoPath = nullptr);
void end();  // frees the tables
bool active();
Theme current();

// Screen row y, columns x0 .. x0+w-1, into `out` byte-swapped (sprite order).
void row(int16_t y, int16_t x0, int16_t w, uint16_t* out);
uint16_t at(int16_t x, int16_t y);  // one pixel, not swapped

// Motion: advances to `now` and lists up to `max` pixels whose colour changed
// (old -> new, not swapped). Returns how many.
struct Spark {
  int16_t x, y;
  uint16_t from, to;
};
int sparkles(uint32_t now, Spark* out, int max);

}  // namespace backdrop
