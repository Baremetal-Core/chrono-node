#include "ui_theme.h"

#include <math.h>
#include <stddef.h>
#include <strings.h>

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

// Saturated colours: pastels look washed out on the CYD's IPS panel. Chosen
// on the device from side-by-side candidates (TEST PALETTE).
constexpr Rgb kNightTop = {255, 70, 150}, kNightBottom = {220, 0, 200};       // hot pink -> magenta
constexpr Rgb kSunriseTop = {255, 200, 0}, kSunriseBottom = {255, 40, 80};    // gold -> red-pink
constexpr Rgb kDayTop = {255, 225, 0}, kDayBottom = {255, 90, 0};             // yellow -> orange
constexpr Rgb kSunsetTop = {255, 120, 0}, kSunsetBottom = {235, 0, 130};      // orange -> magenta
constexpr Rgb kEveningTop = {0, 220, 255}, kEveningBottom = {40, 90, 255};    // cyan -> blue

constexpr Stop kStops[] = {
    {0, kNightTop, kNightBottom},
    {5 * 60, kNightTop, kNightBottom},
    {6 * 60 + 30, kSunriseTop, kSunriseBottom},
    {9 * 60, kDayTop, kDayBottom},
    {16 * 60, kDayTop, kDayBottom},
    {18 * 60 + 30, kSunsetTop, kSunsetBottom},
    {20 * 60, kEveningTop, kEveningBottom},
    {21 * 60 + 15, kEveningTop, kEveningBottom},
    {22 * 60, kNightTop, kNightBottom},
    {24 * 60, kNightTop, kNightBottom},
};

constexpr const char* kNames[] = {"NIGHT", "DAWN", "DAY", "DUSK", "EVE"};
constexpr Rgb kNamed[][2] = {{kNightTop, kNightBottom},
                             {kSunriseTop, kSunriseBottom},
                             {kDayTop, kDayBottom},
                             {kSunsetTop, kSunsetBottom},
                             {kEveningTop, kEveningBottom}};

struct Hsv {
  float h, s, v;  // h in degrees 0-360, s and v 0-1
};

Hsv toHsv(const Rgb& c) {
  float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
  float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
  float h = 0;
  if (d > 0) {
    if (mx == r) h = 60 * fmodf((g - b) / d + 6, 6);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
  }
  return {h, mx > 0 ? d / mx : 0, mx};
}

uint16_t fromHsv(const Hsv& c) {
  float k[3] = {5, 3, 1};
  uint8_t out[3];
  for (int i = 0; i < 3; ++i) {
    float t = fmodf(k[i] + c.h / 60, 6);
    float f = c.v - c.v * c.s * fmaxf(0, fminf(fminf(t, 4 - t), 1));
    out[i] = static_cast<uint8_t>(f * 255 + 0.5f);
  }
  return rgb(out[0], out[1], out[2]);
}

// Blends in HSV along the shorter hue arc: RGB blending between distant hues
// passes through grey, which looks washed out on the panel.
uint16_t blend(const Rgb& a, const Rgb& b, int num, int den) {
  Hsv x = toHsv(a), y = toHsv(b);
  float t = static_cast<float>(num) / den;
  float dh = y.h - x.h;
  if (dh > 180) dh -= 360;
  if (dh < -180) dh += 360;
  float h = fmodf(x.h + dh * t + 360, 360);
  return fromHsv({h, x.s + (y.s - x.s) * t, x.v + (y.v - x.v) * t});
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

Palette nightPalette() { return {blend(kNightTop, kNightTop, 0, 1), blend(kNightBottom, kNightBottom, 0, 1)}; }

int namedPaletteCount() { return sizeof(kNamed) / sizeof(kNamed[0]); }

Palette namedPalette(int index, const char** name) {
  if (name) *name = kNames[index];
  return {blend(kNamed[index][0], kNamed[index][0], 0, 1), blend(kNamed[index][1], kNamed[index][1], 0, 1)};
}

uint16_t hsv(float h, float s, float v) {
  h = fmodf(h, 360);
  if (h < 0) h += 360;
  return fromHsv({h, s, v});
}

namespace {

struct StyleInfo {
  const char* key;
  const char* name;
  Rgb top, bottom;  // presets only
};

// Presets are saturated like the day-cycle palettes (pastels wash out).
constexpr StyleInfo kStyles[] = {
    {"time", "Time of day", {}, {}},
    {"rainbow", "Rainbow flow", {}, {}},
    {"cycle", "Colour cycle", {}, {}},
    {"digits", "Per digit", {}, {}},
    {"sunset", "Sunset", {255, 176, 0}, {255, 45, 111}},
    {"ocean", "Ocean", {0, 229, 255}, {41, 98, 255}},
    {"aurora", "Aurora", {61, 255, 138}, {138, 92, 255}},
    {"neon", "Neon", {0, 255, 240}, {255, 0, 200}},
    {"fire", "Fire", {255, 224, 0}, {255, 42, 0}},
    {"candy", "Candy", {255, 138, 216}, {155, 92, 255}},
    {"custom", "Custom", {}, {}},
    {"mix", "Mix", {}, {}},
};
static_assert(sizeof(kStyles) / sizeof(kStyles[0]) == static_cast<size_t>(Style::Count), "styles");

}  // namespace

const char* styleName(Style s) { return kStyles[static_cast<int>(s) % static_cast<int>(Style::Count)].name; }

const char* styleKey(Style s) { return kStyles[static_cast<int>(s) % static_cast<int>(Style::Count)].key; }

bool styleFromKey(const char* key, Style& out) {
  for (int i = 0; i < static_cast<int>(Style::Count); ++i) {
    if (strcasecmp(key, kStyles[i].key) == 0) {
      out = static_cast<Style>(i);
      return true;
    }
  }
  return false;
}

bool styleAnimated(Style s) {
  return s == Style::Rainbow || s == Style::Cycle || s == Style::PerDigit;
}

bool presetPalette(Style s, Palette& out) {
  if (s < Style::Sunset || s > Style::Candy) return false;
  const StyleInfo& i = kStyles[static_cast<int>(s)];
  out = {rgb(i.top.r, i.top.g, i.top.b), rgb(i.bottom.r, i.bottom.g, i.bottom.b)};
  return true;
}

Style mixStyleAt(int minuteOfDay, uint8_t everyMinutes) {
  static constexpr Style kTurns[] = {Style::Rainbow, Style::Sunset, Style::Cycle, Style::Ocean,
                                     Style::PerDigit, Style::Aurora, Style::Neon, Style::Fire,
                                     Style::Candy};
  constexpr int n = sizeof(kTurns) / sizeof(kTurns[0]);
  const int turn = minuteOfDay / (everyMinutes ? everyMinutes : 1);
  return kTurns[turn % n];
}

namespace {
constexpr const char* kSpeedKeys[] = {"slow", "normal", "fast", "veryfast"};
constexpr const char* kTransitionKeys[] = {"fade", "wipe", "slide", "blinds", "none"};
}  // namespace

const char* speedKey(Speed s) { return kSpeedKeys[static_cast<int>(s) % 4]; }

bool speedFromKey(const char* key, Speed& out) {
  for (int i = 0; i < 4; ++i) {
    if (strcasecmp(key, kSpeedKeys[i]) == 0) {
      out = static_cast<Speed>(i);
      return true;
    }
  }
  return false;
}

uint8_t speedQuarters(Speed s) { return 2 << (static_cast<int>(s) % 4); }

const char* transitionKey(Transition t) { return kTransitionKeys[static_cast<int>(t) % 5]; }

bool transitionFromKey(const char* key, Transition& out) {
  for (int i = 0; i < 5; ++i) {
    if (strcasecmp(key, kTransitionKeys[i]) == 0) {
      out = static_cast<Transition>(i);
      return true;
    }
  }
  return false;
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
