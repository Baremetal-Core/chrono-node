#include "display_manager.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <string.h>

#include "config.h"
#include "fonts/font_date.h"
#include "fonts/font_face.h"
#include "fonts/font_seconds.h"
#include "fonts/font_small.h"
#include "fonts/font_temp.h"
#include "fonts/font_time.h"
#include "weather_icons.h"

namespace {

TFT_eSPI tft;
const theme::Colors* C = &theme::kDayColors;  // active UI colours (day or night)
TFT_eSprite digitSprite(&tft);   // one big digit cell, reused for every digit and the colon
TFT_eSprite secondsSprite(&tft); // both seconds digits
TFT_eSprite bannerSprite(&tft);  // scrolling banner on the bottom line
TFT_eSprite barSprite(&tft);     // the minute bar, pushed in one go (no tearing)
TFT_eSprite dateSprite(&tft);    // a strip of the date line; kept only while the colours animate
bool barOk = false;
bool dateOk = false;

constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;

// Status bar
constexpr int16_t kStatusH = 26;
constexpr int16_t kStatusBaseline = 18;

// Big HH:MM digits: fixed-width cells so the time never shifts sideways.
constexpr int16_t kDigitW = 66;
constexpr int16_t kColonW = 30;
constexpr int16_t kDigitH = 86;
constexpr int16_t kDigitBaseline = 82;
constexpr int16_t kGlyphTop = 4;  // top of the digit glyphs inside a cell (gradient start)
constexpr int16_t kTimeTop = 54;
constexpr int16_t kTimeW = 4 * kDigitW + kColonW;
constexpr int16_t kTimeX = (kScreenW - kTimeW) / 2;

// Bar row: minute progress bar with the seconds at its right end.
constexpr int16_t kRowY = kTimeTop + kDigitH + 12;  // vertical centre of the row
constexpr int16_t kSecCellW = 18;
constexpr int16_t kSecW = 2 * kSecCellW;
constexpr int16_t kSecH = 24;
constexpr int16_t kSecBaseline = 21;
constexpr int16_t kSecX = kTimeX + kTimeW - kSecW;
constexpr int16_t kBarH = 4;
constexpr int16_t kBarX = kTimeX + 4;
constexpr int16_t kBarW = kSecX - 12 - kBarX;
constexpr int16_t kMeridiemW = 30;  // "AM"/"PM" before the bar in 12-hour mode
constexpr int16_t kBarY = kRowY - kBarH / 2;

// Date and notice lines
constexpr int16_t kDateTop = kRowY + 18;
constexpr int16_t kDateH = 30;
constexpr int16_t kNoticeTop = 212;
constexpr int16_t kNoticeH = 24;

constexpr uint32_t kRollMs = 300;
constexpr uint32_t kFrameMs = 16;

const char* const kWeekdays[] = {"SUNDAY",   "MONDAY", "TUESDAY", "WEDNESDAY",
                                 "THURSDAY", "FRIDAY", "SATURDAY"};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

int16_t digitX(int index, int16_t offset) {
  return kTimeX + offset + index * kDigitW + (index >= 2 ? kColonW : 0);
}

float easeOutCubic(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float progressOf(uint32_t startMs, uint32_t durationMs, uint32_t nowMs) {
  uint32_t elapsed = nowMs - startMs;
  if (elapsed >= durationMs) return 1.0f;
  return easeOutCubic(static_cast<float>(elapsed) / durationMs);
}

inline uint16_t swap16(uint16_t c) { return (c >> 8) | (c << 8); }

// The colour field: a top and a bottom colour for every screen column. Static
// styles fill it with one palette; rainbow and per-digit styles vary it across
// the screen, and animated styles recompute it every frame.
uint16_t fieldTop[kScreenW];
uint16_t fieldBottom[kScreenW];
bool fieldUniform = true;

// Shine: a diagonal glint across the time row, at this x (kNoShine: none).
constexpr int16_t kNoShine = INT16_MIN;
constexpr int16_t kShineW = 22;          // half width of the band
constexpr uint32_t kShinePeriodMs = 8000;
constexpr uint32_t kShineSweepMs = 1400;
int16_t shineX = kNoShine;

// Glyph shapes kept between colour frames, so animated styles only re-tint
// them: the coverage (0-63) of each settled big digit and of the date line.
// Allocated only while something animates (see syncFxBuffers): the network
// task needs ~50 KB of free heap for TLS.
struct FxCache {
  uint8_t digit[4][kDigitW * kDigitH];
  uint8_t date[kScreenW * kDateH];
};
FxCache* fxCache = nullptr;
char digitCovChar[4] = {};
int32_t dateCovKey = -1;  // year * 400 + day of year
constexpr int16_t kDateStripH = 10;  // the date is tinted and pushed in strips

void saveCoverage(const uint16_t* px, uint8_t* cov, int n) {
  for (int i = 0; i < n; ++i) cov[i] = px[i] ? (((px[i] >> 8) | (px[i] << 8)) >> 5) & 0x3F : 0;
}

void restoreCoverage(uint16_t* px, const uint8_t* cov, int n) {
  for (int i = 0; i < n; ++i) {
    const uint16_t c = cov[i] << 5;  // the tint only reads the green channel
    px[i] = (c >> 8) | (c << 8);     // sprites are byte-swapped
  }
}

// Glow: the glyph coverage blurred into a halo (box blur, two passes).
constexpr int kGlowRadius = 4;
constexpr uint8_t kGlowStrength = 150;  // 0-255
uint8_t* glowA = nullptr;  // two kDigitW * kDigitH buffers, only while glow is on
uint8_t* glowB = nullptr;

// Repaint intervals: a colour frame (time row, bar and date, re-tinting kept
// glyph shapes) takes ~30 ms (~46 ms with glow), so colour styles run at
// 12 fps (8 with glow); the shine sweep goes faster.
constexpr uint32_t kColorFrameMs = 80;
constexpr uint32_t kGlowColorFrameMs = 120;  // glow adds ~16 ms a frame
constexpr uint32_t kShineFrameMs = 50;
constexpr uint32_t kPulseFrameMs = 80;

// Hue lookup (one entry per degree) for the animated styles' top and bottom
// colours: much cheaper than converting 640 colours from HSV every frame.
uint16_t hueTop[360];
uint16_t hueBottom[360];

void initHues() {
  if (hueTop[0]) return;
  for (int h = 0; h < 360; ++h) {
    hueTop[h] = theme::hsv(h, 0.7f, 1);
    hueBottom[h] = theme::hsv(h, 1, 0.95f);
  }
}

// How a sprite is coloured.
struct Paint {
  const theme::Palette* palette = nullptr;  // one vertical gradient; nullptr: the field
  int16_t screenX = 0;                      // sprite position (field lookup, shine)
  int16_t screenY = 0;
  uint8_t coverage = 255;                   // scales the glyphs (pulse)
  bool shine = false;
  bool glow = false;
};

// Blend a -> b by t/256 (t 0..256). Shifts instead of divisions: this runs
// for every glyph pixel of every animation frame.
inline uint16_t blend256(uint16_t a, uint16_t b, int t) {
  const int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int r = ar + ((((b >> 11) - ar) * t) >> 8);
  const int g = ag + (((((b >> 5) & 0x3F) - ag) * t) >> 8);
  const int bl = ab + ((((b & 0x1F) - ab) * t) >> 8);
  return (r << 11) | (g << 5) | bl;
}

void blurCoverage(const uint16_t* px, int16_t w, int16_t h) {
  for (int i = 0; i < w * h; ++i) {
    glowA[i] = px[i] ? ((swap16(px[i]) >> 5) & 0x3F) * 4 : 0;
  }
  constexpr int kInv = 65536 / (2 * kGlowRadius + 1);  // sum / n as a multiply
  for (int16_t y = 0; y < h; ++y) {  // horizontal: A -> B
    const uint8_t* in = glowA + y * w;
    uint8_t* out = glowB + y * w;
    int sum = 0;
    for (int x = -kGlowRadius; x < w + kGlowRadius; ++x) {
      if (x + kGlowRadius < w) sum += in[x + kGlowRadius];
      if (x - kGlowRadius - 1 >= 0) sum -= in[x - kGlowRadius - 1];
      if (x >= 0 && x < w) out[x] = (sum * kInv) >> 16;
    }
  }
  for (int16_t x = 0; x < w; ++x) {  // vertical: B -> A
    int sum = 0;
    for (int y = -kGlowRadius; y < h + kGlowRadius; ++y) {
      if (y + kGlowRadius < h) sum += glowB[(y + kGlowRadius) * w + x];
      if (y - kGlowRadius - 1 >= 0) sum -= glowB[(y - kGlowRadius - 1) * w + x];
      if (y >= 0 && y < h) glowA[y * w + x] = (sum * kInv) >> 16;
    }
  }
}

// Turns white-on-black glyphs in a 16-bit sprite into coloured text over the
// background, keeping the glyphs' anti-aliasing: a vertical gradient (top
// colour at row y0, bottom at y1) from the palette or the colour field, plus
// the shine and glow effects. Sprite pixels are stored byte-swapped; `w` is
// the sprite's row stride.
void tintSprite(TFT_eSprite& s, int16_t w, int16_t h, int16_t y0, int16_t y1, const Paint& p) {
  uint16_t* px = static_cast<uint16_t*>(s.getPointer());
  if (!px) return;
  const uint16_t bg = C->bg;
  const uint16_t bgSwapped = swap16(bg);
  const bool uniform = p.palette || fieldUniform;
  const theme::Palette pal = p.palette ? *p.palette : theme::Palette{fieldTop[0], fieldBottom[0]};
  const bool glow = p.glow && glowA && w * h <= kDigitW * kDigitH;
  if (glow) blurCoverage(px, w, h);
  const bool shine = p.shine && shineX != kNoShine;
  static int shineT[kShineW];  // shine strength by distance from the band's centre
  if (shine && shineT[0] == 0) {
    for (int d = 0; d < kShineW; ++d) shineT[d] = (kShineW - d) * 190 * 256 / (kShineW * 255);
  }
  const int cov = p.coverage + (p.coverage >> 7);  // 0..256
  for (int16_t y = 0; y < h; ++y) {
    const int t = y <= y0 ? 0 : y >= y1 ? 256 : (y - y0) * 256 / (y1 - y0);
    const uint16_t rowFg = uniform ? blend256(pal.top, pal.bottom, t) : 0;
    const int16_t shineAt = shineX - (p.screenY + y - kTimeTop) * 2 / 5;  // leans right going up
    uint16_t* row = px + y * w;
    const uint8_t* glowRow = glowA + y * w;
    for (int16_t x = 0; x < w; ++x) {
      const uint16_t c = row[x];
      const int g = glow ? glowRow[x] : 0;
      if (c == 0 && g == 0) {
        row[x] = bgSwapped;
        continue;
      }
      int16_t sx = p.screenX + x;
      if (sx < 0) sx = 0;
      if (sx >= kScreenW) sx = kScreenW - 1;
      uint16_t fg = uniform ? rowFg : blend256(fieldTop[sx], fieldBottom[sx], t);
      if (shine) {
        const int d = abs(sx - shineAt);
        if (d < kShineW) fg = blend256(fg, TFT_WHITE, shineT[d]);
      }
      const uint16_t base = g ? blend256(bg, fg, (g * kGlowStrength) >> 8) : bg;
      int a = c ? (swap16(c) >> 5) & 0x3F : 0;  // glyph coverage 0..63
      a = ((a + (a >> 5)) * cov) >> 8;           // 0..64, scaled by the pulse
      row[x] = swap16(blend256(base, fg, a << 2));
    }
  }
}

// Draws one text line through a temporary full-width sprite (no flicker).
template <typename Fn>
void drawLine(int16_t top, int16_t height, const uint8_t* font, Fn&& draw) {
  TFT_eSprite line(&tft);
  line.setColorDepth(16);
  if (!line.createSprite(kScreenW, height)) return;
  line.fillSprite(C->bg);
  line.loadFont(font);
  draw(line);
  line.unloadFont();
  line.pushSprite(0, top);
  line.deleteSprite();
}

void drawWifiIcon(TFT_eSprite& s, int16_t cx, int16_t baseY, uint8_t bars, uint16_t on,
                  uint16_t off) {
  // Three arcs and a dot fanning upwards; TFT_eSPI arc angles start at 6 o'clock.
  const uint8_t radii[] = {13, 9, 5};
  for (int i = 0; i < 3; ++i) {
    uint16_t c = (3 - i) <= bars ? on : off;
    s.drawSmoothArc(cx, baseY, radii[i], radii[i] - 2, 135, 225, c, C->bg, true);
  }
  s.fillSmoothCircle(cx, baseY - 1, 1, bars > 0 ? on : off, C->bg);
}

void drawGlyphString(TFT_eSprite& s, const char* text, int16_t cellX, int16_t cellW, int16_t y) {
  s.drawString(text, cellX + (cellW - s.textWidth(text)) / 2, y);
}

}  // namespace

bool DisplayManager::begin() {
  tft.init();  // also switches the backlight (TFT_BL) on
  tft.setRotation(1);
  tft.fillScreen(C->bg);

  digitSprite.setColorDepth(16);
  secondsSprite.setColorDepth(16);
  spritesOk_ = digitSprite.createSprite(kDigitW, kDigitH) != nullptr &&
               secondsSprite.createSprite(kSecW, kSecH) != nullptr;
  if (!spritesOk_) {
    Serial.println("[DISPLAY] ERROR: sprite allocation failed");
    return false;
  }
  // Glyphs are rendered white on black, then tinted (see tintSprite).
  digitSprite.loadFont(font_time);
  digitSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  digitSprite.setTextDatum(L_BASELINE);
  secondsSprite.loadFont(font_seconds);
  secondsSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  secondsSprite.setTextDatum(L_BASELINE);

  barSprite.setColorDepth(16);
  barOk = barSprite.createSprite(kBarW, kBarH) != nullptr;

  // The banner is rendered white-on-black every frame, then tinted.
  bannerSprite.setColorDepth(16);
  bannerOk_ = bannerSprite.createSprite(kScreenW, kNoticeH) != nullptr;
  if (bannerOk_) {
    bannerSprite.loadFont(font_date);
    bannerSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    bannerSprite.setTextDatum(ML_DATUM);
  } else {
    Serial.println("[DISPLAY] WARNING: banner sprite allocation failed");
  }
  return true;
}

void DisplayManager::enterMode(Mode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  tft.fillScreen(C->bg);
  for (int i = 0; i < 4; ++i) {
    digits_[i] = ' ';
    bigRoll_[i].active = false;
  }
  secDigits_[0] = secDigits_[1] = ' ';
  secAnimating_ = false;
  barPx_ = -1;
  palValid_ = false;
  lastYday_ = -1;
  statusValid_ = false;
  bannerActive_ = false;
  notice_[0] = '\0';
  lastError_[0] = '\0';
}

bool DisplayManager::effectsOn() const { return !night_ && colors_.effects != 0; }

theme::Palette DisplayManager::basePalette(const struct tm& local, uint32_t now) {
  if (night_) return theme::nightPalette();
  theme::Palette p;
  switch (colors_.style) {
    case theme::Style::Custom:
      return colors_.custom;
    case theme::Style::Rainbow:
    case theme::Style::Cycle:
    case theme::Style::PerDigit:
      // Accents (status icon, buttons, banner) follow the middle of the field.
      refreshField(now);
      return {fieldTop[kScreenW / 2], fieldBottom[kScreenW / 2]};
    default:
      if (theme::presetPalette(colors_.style, p)) return p;
      return theme::paletteForMinute(local.tm_hour * 60 + local.tm_min);
  }
}

void DisplayManager::refreshField(uint32_t now) {
  const theme::Style st = colors_.style;
  if (night_ || !theme::styleAnimated(st)) {
    for (int16_t x = 0; x < kScreenW; ++x) {
      fieldTop[x] = pal_.top;
      fieldBottom[x] = pal_.bottom;
    }
    fieldUniform = true;
    return;
  }
  initHues();
  // The animation clock runs at the chosen speed; every period below divides
  // an hour, so it wraps without a jump.
  animPhaseMs_ = (animPhaseMs_ + (now - lastPhaseMs_) * colors_.speedQuarters / 4) % 3600000UL;
  lastPhaseMs_ = now;
  const uint32_t ms = animPhaseMs_;
  if (st == theme::Style::Cycle) {
    const int h = ms * 6 / 1000 % 360;  // once around the colour wheel per minute
    for (int16_t x = 0; x < kScreenW; ++x) {
      fieldTop[x] = hueTop[h];
      fieldBottom[x] = hueBottom[(h + 40) % 360];
    }
    fieldUniform = true;
    return;
  }
  fieldUniform = false;
  const int flow = ms * 20 / 1000;  // degrees: rainbow flows one screen width every 18 s
  const int turn = ms * 12 / 1000;  // per digit: all hues turn once every 30 s
  for (int16_t x = 0; x < kScreenW; ++x) {
    int h;
    if (st == theme::Style::Rainbow) {
      h = flow + x * 300 / kScreenW;
    } else {
      // Per digit: one hue per digit cell (the colon sits between two).
      const int16_t rx = x - (kTimeX + timeOffset_);
      int cell2;  // twice the cell index, so the colon can be 3 (= 1.5)
      if (rx < 2 * kDigitW) {
        cell2 = 2 * max(0, rx / kDigitW);
      } else if (rx < 2 * kDigitW + kColonW) {
        cell2 = 3;
      } else {
        cell2 = 2 * min(3, 2 + (rx - 2 * kDigitW - kColonW) / kDigitW);
      }
      h = turn + cell2 * 40;
    }
    fieldTop[x] = hueTop[h % 360];
    fieldBottom[x] = hueBottom[(h + 25) % 360];
  }
}

void DisplayManager::setColors(const ColorSettings& colors) {
  colors_ = colors;
  syncFxBuffers();
  shineX = kNoShine;
  if (mode_ != Mode::Clock || !palValid_) {
    // Other screens use the accent colour; the clock face repaints on return.
    if (lastLocal_.tm_year) pal_ = basePalette(lastLocal_, millis());
    return;
  }
  const theme::Palette p = basePalette(lastLocal_, millis());
  pal_.top = ~p.top;  // force setPalette() to repaint everything
  setPalette(p);
}

void DisplayManager::setPalette(const theme::Palette& palette) {
  if (palValid_ && palette == pal_) return;
  const bool firstFrame = !palValid_;
  pal_ = palette;
  palValid_ = true;
  if (!theme::styleAnimated(colors_.style) || night_) refreshField(millis());
  if (firstFrame) return;  // showTime draws everything on the first frame
  // Re-tint everything that uses the gradient colours.
  for (int i = 0; i < 4; ++i) {
    if (!bigRoll_[i].active) drawBigDigit(i, 1.0f);
  }
  drawColon();
  if (!secAnimating_) drawSeconds(1.0f);
  drawBar(true, true);
  drawDate(lastLocal_);
  drawStatusBar();
}

void DisplayManager::showTime(const struct tm& local) {
  if (!spritesOk_) return;
  if (mode_ == Mode::Test && (int32_t)(millis() - testUntilMs_) < 0) return;
  const bool firstFrame = mode_ != Mode::Clock;
  enterMode(Mode::Clock);
  const uint32_t now = millis();
  if (firstFrame || local.tm_sec != lastLocal_.tm_sec) secondStartMs_ = now;
  lastLocal_ = local;

  setPalette(basePalette(local, now));

  int hour = local.tm_hour;
  if (hour12_) {
    const bool pm = hour >= 12;
    if (pm != pm_) {
      pm_ = pm;
      drawMeridiem();
    }
    hour = hour % 12 == 0 ? 12 : hour % 12;
  }
  const char next[4] = {hour12_ && hour < 10 ? ' ' : char('0' + hour / 10), char('0' + hour % 10),
                        char('0' + local.tm_min / 10), char('0' + local.tm_min % 10)};
  // A blank leading digit (12-hour, 1-9 o'clock) shifts the time left by half
  // a cell so it stays centred; the time row is repainted when that changes.
  const int16_t offset = next[0] == ' ' ? -kDigitW / 2 : 0;
  if (offset != timeOffset_) {
    timeOffset_ = offset;
    tft.fillRect(0, kTimeTop, kScreenW, kDigitH, C->bg);
    for (int i = 0; i < 4; ++i) {
      digits_[i] = ' ';
      bigRoll_[i].active = false;
    }
    drawColon();
  }
  for (int i = 0; i < 4; ++i) {
    if (next[i] == digits_[i]) continue;
    Roll& r = bigRoll_[i];
    r.from = digits_[i];
    r.to = next[i];
    r.startMs = now;
    r.active = !firstFrame;
    digits_[i] = next[i];
    if (!r.active) drawBigDigit(i, 1.0f);
  }

  const char sec[2] = {char('0' + local.tm_sec / 10), char('0' + local.tm_sec % 10)};
  bool secChanged = false;
  for (int i = 0; i < 2; ++i) {
    secRoll_[i].from = secDigits_[i];
    secRoll_[i].to = sec[i];
    if (sec[i] != secDigits_[i]) secChanged = true;
    secDigits_[i] = sec[i];
  }
  if (secChanged) {
    secRoll_[0].startMs = secRoll_[1].startMs = now;
    secAnimating_ = !firstFrame;
    if (!secAnimating_) drawSeconds(1.0f);
  }

  drawBar(firstFrame, false);

  if (local.tm_yday != lastYday_) {
    drawDate(local);
    lastYday_ = local.tm_yday;
  }
  if (firstFrame) {
    drawColon();
    drawStatusBar();
    drawNotice();
  }
}

void DisplayManager::tick() {
  if (mode_ != Mode::Clock || !spritesOk_) return;
  const uint32_t now = millis();
  if (now - lastFrameMs_ < kFrameMs) return;
  lastFrameMs_ = now;

  // Animated styles and effects repaint the time about 15 times a second.
  const bool animatedStyle = !night_ && theme::styleAnimated(colors_.style);
  int16_t shineNow = kNoShine;
  if (effectsOn() && (colors_.effects & theme::kFxShine)) {
    const uint32_t phase = now % kShinePeriodMs;
    if (phase < kShineSweepMs) {
      shineNow = kTimeX - 60 + static_cast<int32_t>(kTimeW + 120) * phase / kShineSweepMs;
    }
  }
  const bool pulse = effectsOn() && (colors_.effects & theme::kFxPulse);
  const bool shining = shineNow != kNoShine || shineX != kNoShine;  // incl. the clean-up frame
  const bool glow = effectsOn() && (colors_.effects & theme::kFxGlow);
  const uint32_t interval = shining         ? kShineFrameMs
                            : !animatedStyle ? kPulseFrameMs
                            : glow           ? kGlowColorFrameMs
                                             : kColorFrameMs;
  if ((animatedStyle || pulse || shining) && now - lastAnimMs_ >= interval) {
    lastAnimMs_ = now;
    shineX = shineNow;
    if (animatedStyle) refreshField(now);
    if (animatedStyle || shining) {
      for (int i = 0; i < 4; ++i) {
        if (!bigRoll_[i].active) drawBigDigit(i, 1.0f);
      }
      if (!secAnimating_) drawSeconds(1.0f);
    }
    drawColon();
    if (animatedStyle) {
      drawBar(false, true);
      drawDate(lastLocal_);
    }
  }
  drawBar(false, false);  // the bar fills smoothly: redrawn whenever it grows a pixel

  if (bannerActive_) drawBanner(now);

  for (int i = 0; i < 4; ++i) {
    Roll& r = bigRoll_[i];
    if (!r.active) continue;
    float p = progressOf(r.startMs, kRollMs, now);
    drawBigDigit(i, p);
    if (p >= 1.0f) r.active = false;
  }
  if (secAnimating_) {
    float p = progressOf(secRoll_[0].startMs, kRollMs, now);
    drawSeconds(p);
    if (p >= 1.0f) secAnimating_ = false;
  }
}

void DisplayManager::drawBigDigit(int index, float progress) {
  const Roll& r = bigRoll_[index];
  digitSprite.fillSprite(TFT_BLACK);
  const char to[2] = {r.active ? r.to : digits_[index], '\0'};
  if (r.active && progress < 1.0f && r.from != ' ') {
    // Old digit slides up and out while the new one rises from below.
    const int shift = static_cast<int>(progress * kDigitH);
    const char from[2] = {r.from, '\0'};
    drawGlyphString(digitSprite, from, 0, kDigitW, kDigitBaseline - shift);
    drawGlyphString(digitSprite, to, 0, kDigitW, kDigitBaseline + kDigitH - shift);
  } else if (fxCache && digitCovChar[index] == to[0]) {
    restoreCoverage(static_cast<uint16_t*>(digitSprite.getPointer()), fxCache->digit[index],
                    kDigitW * kDigitH);
  } else {
    drawGlyphString(digitSprite, to, 0, kDigitW, kDigitBaseline);
    if (fxCache) {
      saveCoverage(static_cast<uint16_t*>(digitSprite.getPointer()), fxCache->digit[index],
                   kDigitW * kDigitH);
      digitCovChar[index] = to[0];
    }
  }
  Paint paint;
  paint.screenX = digitX(index, timeOffset_);
  paint.screenY = kTimeTop;
  paint.shine = effectsOn() && (colors_.effects & theme::kFxShine);
  paint.glow = effectsOn() && (colors_.effects & theme::kFxGlow);
  tintSprite(digitSprite, kDigitW, kDigitH, kGlyphTop, kDigitBaseline, paint);
  digitSprite.pushSprite(digitX(index, timeOffset_), kTimeTop);
}

void DisplayManager::drawColon() {
  if (mode_ != Mode::Clock) return;
  digitSprite.fillSprite(TFT_BLACK);
  drawGlyphString(digitSprite, ":", 0, kColonW, kDigitBaseline - 6);
  Paint paint;
  paint.screenX = kTimeX + timeOffset_ + 2 * kDigitW;
  paint.screenY = kTimeTop;
  paint.shine = effectsOn() && (colors_.effects & theme::kFxShine);
  paint.glow = effectsOn() && (colors_.effects & theme::kFxGlow);
  if (effectsOn() && (colors_.effects & theme::kFxPulse)) {
    // Breathes between 35% and 100% every 2 s.
    const float phase = (millis() % 2000) / 2000.0f * 2 * PI;
    paint.coverage = static_cast<uint8_t>(90 + 165 * (0.5f + 0.5f * cosf(phase)));
  }
  tintSprite(digitSprite, kDigitW, kDigitH, kGlyphTop, kDigitBaseline, paint);  // w = row stride
  digitSprite.pushSprite(kTimeX + timeOffset_ + 2 * kDigitW, kTimeTop, 0, 0, kColonW, kDigitH);
}

void DisplayManager::drawSeconds(float progress) {
  if (mode_ != Mode::Clock) return;
  secondsSprite.fillSprite(TFT_BLACK);
  const int shift = static_cast<int>(progress * kSecH);
  for (int i = 0; i < 2; ++i) {
    const Roll& r = secRoll_[i];
    const int16_t cellX = i * kSecCellW;
    const char to[2] = {secDigits_[i], '\0'};
    if (progress < 1.0f && r.from != r.to && r.from != ' ') {
      const char from[2] = {r.from, '\0'};
      drawGlyphString(secondsSprite, from, cellX, kSecCellW, kSecBaseline - shift);
      drawGlyphString(secondsSprite, to, cellX, kSecCellW, kSecBaseline + kSecH - shift);
    } else {
      drawGlyphString(secondsSprite, to, cellX, kSecCellW, kSecBaseline);
    }
  }
  Paint paint;
  paint.screenX = kSecX;
  paint.screenY = kRowY - kSecH / 2 - 1;
  paint.shine = effectsOn() && (colors_.effects & theme::kFxShine);
  tintSprite(secondsSprite, kSecW, kSecH, 2, kSecBaseline, paint);
  secondsSprite.pushSprite(kSecX, kRowY - kSecH / 2 - 1);
}

// The minute bar fills smoothly: its length follows the seconds plus the time
// since the current second began. It's drawn into a sprite and pushed in one
// go, only when it grows a pixel (`recolor`: the colours moved; `full`: also
// redraw AM/PM). The bar is never cleared first: the sprite covers it
// completely, so it can't blink when the palette changes (every second in
// the animated styles).
void DisplayManager::drawBar(bool full, bool recolor) {
  if (mode_ != Mode::Clock || !barOk) return;
  const int w = barW();
  const uint32_t intoSecond = min<uint32_t>(999, millis() - secondStartMs_);
  const int px = static_cast<int>(static_cast<int64_t>(lastLocal_.tm_sec * 1000 + intoSecond) * w / 60000);
  if (barPx_ < 0) full = true;
  if (!full && !recolor && px == barPx_) return;
  if (full) {
    // Only the strip the bar doesn't cover: AM/PM in 12-hour mode.
    if (barX() > kBarX) tft.fillRect(kBarX, kBarY, barX() - kBarX, kBarH, C->bg);
    drawMeridiem();
  }
  barPx_ = px;
  const uint16_t track = swap16(theme::mix(C->bg, pal_.bottom, 48));
  uint16_t* buf = static_cast<uint16_t*>(barSprite.getPointer());
  for (int x = 0; x < w; ++x) buf[x] = x < px ? swap16(barColor(x)) : track;  // sprites are byte-swapped
  for (int y = 1; y < kBarH; ++y) memcpy(buf + y * kBarW, buf, w * sizeof(uint16_t));
  barSprite.pushSprite(barX(), kBarY, 0, 0, w, kBarH);
}

// Bar column x (0..barW): a horizontal gradient of the palette, or the
// colour field's column when it varies across the screen.
uint16_t DisplayManager::barColor(int x) const {
  if (fieldUniform) return theme::mix(fieldTop[0], fieldBottom[0], x * 255 / barW());
  const int16_t sx = constrain(barX() + x, 0, kScreenW - 1);
  return theme::mix(fieldTop[sx], fieldBottom[sx], 128);
}


// The date line in the clock's colours: the weekday at full strength and the
// rest a little dimmer (drawn in grey, so the tint gives it less coverage).
// Night mode keeps the quiet night look. Animated styles repaint it every
// colour frame from its kept shape (see syncFxBuffers).
void DisplayManager::drawDate(const struct tm& local) {
  auto layout = [&](TFT_eSprite& s, uint16_t weekdayColor, uint16_t restColor, uint16_t bg) {
    // Long weekday/month combinations (e.g. WEDNESDAY, September) don't fit:
    // shorten the month first, then the weekday.
    char weekday[12];
    char rest[40];
    int16_t w1 = 0;
    for (int level = 0; level < 3; ++level) {
      strlcpy(weekday, kWeekdays[local.tm_wday % 7], level == 2 ? 4 : sizeof(weekday));
      snprintf(rest, sizeof(rest), " \xC2\xB7 %d %.*s %d", local.tm_mday, level >= 1 ? 3 : 12,
               kMonths[local.tm_mon % 12], local.tm_year + 1900);
      w1 = s.textWidth(weekday);
      if (w1 + s.textWidth(rest) <= kScreenW - 16) break;
    }
    const int16_t x = (kScreenW - w1 - s.textWidth(rest)) / 2;
    s.setTextDatum(ML_DATUM);
    s.setTextColor(weekdayColor, bg);
    s.drawString(weekday, x, kDateH / 2);
    s.setTextColor(restColor, bg);
    s.drawString(rest, x + w1, kDateH / 2);
  };
  if (night_) {
    drawLine(kDateTop, kDateH, font_face,
             [&](TFT_eSprite& s) { layout(s, pal_.top, C->date, C->bg); });
    return;
  }
  const int16_t y0 = kDateH / 2 - 10, y1 = kDateH / 2 + 9;  // gradient rows
  const int32_t key = local.tm_year * 400 + local.tm_yday;
  if (!fxCache || !dateOk || key != dateCovKey) {
    // Lay the text out in a temporary full-height sprite.
    TFT_eSprite s(&tft);
    s.setColorDepth(16);
    if (!s.createSprite(kScreenW, kDateH)) return;
    s.loadFont(font_face);
    s.fillSprite(TFT_BLACK);
    layout(s, TFT_WHITE, theme::rgb(190, 190, 190), TFT_BLACK);
    s.unloadFont();
    if (fxCache && dateOk) {
      saveCoverage(static_cast<uint16_t*>(s.getPointer()), fxCache->date, kScreenW * kDateH);
      dateCovKey = key;
      s.deleteSprite();
    } else {
      Paint paint;
      paint.screenY = kDateTop;
      tintSprite(s, kScreenW, kDateH, y0, y1, paint);
      s.pushSprite(0, kDateTop);
      s.deleteSprite();
      return;
    }
  }
  // Animated styles: re-tint the kept shape strip by strip.
  for (int16_t sy = 0; sy < kDateH; sy += kDateStripH) {
    const int16_t h = min<int16_t>(kDateStripH, kDateH - sy);
    restoreCoverage(static_cast<uint16_t*>(dateSprite.getPointer()), fxCache->date + sy * kScreenW,
                    kScreenW * h);
    Paint paint;
    paint.screenY = kDateTop + sy;
    tintSprite(dateSprite, kScreenW, h, y0 - sy, y1 - sy, paint);
    dateSprite.pushSprite(0, kDateTop + sy, 0, 0, kScreenW, h);
  }
}

// Buffers for animation, allocated only while something animates so the
// network task keeps enough heap for TLS: glyph shapes (32 KB) and a date
// strip (6 KB) for animated styles and the shine, glow buffers (11 KB) for glow.
void DisplayManager::syncFxBuffers() {
  const bool animated = !night_ && theme::styleAnimated(colors_.style);
  const bool cache = animated || (effectsOn() && (colors_.effects & (theme::kFxShine | theme::kFxGlow)));
  const bool glow = effectsOn() && (colors_.effects & theme::kFxGlow);
  if (cache && !fxCache) {
    fxCache = static_cast<FxCache*>(malloc(sizeof(FxCache)));
    memset(digitCovChar, 0, sizeof(digitCovChar));
    dateCovKey = -1;
  } else if (!cache && fxCache) {
    free(fxCache);
    fxCache = nullptr;
  }
  if (animated && !dateOk) {
    dateSprite.setColorDepth(16);
    dateOk = dateSprite.createSprite(kScreenW, kDateStripH) != nullptr;
    dateCovKey = -1;
  } else if (!animated && dateOk) {
    dateSprite.deleteSprite();
    dateOk = false;
  }
  if (glow && !glowA) {
    glowA = static_cast<uint8_t*>(malloc(2 * kDigitW * kDigitH));
    glowB = glowA ? glowA + kDigitW * kDigitH : nullptr;
  } else if (!glow && glowA) {
    free(glowA);
    glowA = glowB = nullptr;
  }
}

void DisplayManager::setStatus(const StatusInfo& status) {
  if (statusValid_ && memcmp(&status, &status_, sizeof(StatusInfo)) == 0) return;
  status_ = status;
  statusValid_ = true;
  drawStatusBar();
}

void DisplayManager::drawStatusBar() {
  if (mode_ != Mode::Clock || !statusValid_) return;
  const StatusInfo& st = status_;
  drawLine(0, kStatusH, font_small, [&](TFT_eSprite& s) {
    uint16_t iconColor = C->dim;
    uint8_t bars = 0;
    switch (st.wifi) {
      case StatusInfo::Wifi::Connected:
        iconColor = pal_.top;
        bars = st.rssi > -60 ? 3 : st.rssi > -72 ? 2 : 1;
        break;
      case StatusInfo::Wifi::Connecting:
        iconColor = C->dim;
        break;
      case StatusInfo::Wifi::Setup:
        iconColor = C->setup;
        bars = 3;
        break;
      case StatusInfo::Wifi::Offline:
        break;
    }
    drawWifiIcon(s, 18, kStatusBaseline + 1, bars, iconColor, C->faint);

    s.setTextDatum(L_BASELINE);
    s.setTextColor(st.wifi == StatusInfo::Wifi::Setup ? C->setup : C->dim,
                   C->bg);
    s.drawString(st.wifiText, 38, kStatusBaseline);

    int16_t x = kScreenW - 10;
    s.setTextDatum(R_BASELINE);
    if (st.hasTemp) {
      char temp[12];
      snprintf(temp, sizeof(temp), "%d\xC2\xB0" "C", st.tempC);
      s.setTextColor(C->dim, C->bg);
      s.drawString(temp, x, kStatusBaseline);
      x -= s.textWidth(temp) + 14;
    }
    s.setTextColor(st.ntpWarn ? C->warn : C->dim, C->bg);
    s.drawString(st.ntpText, x, kStatusBaseline);
  });
}

void DisplayManager::setNotice(const char* text, NoticeKind kind) {
  const char* value = text ? text : "";
  if (strcmp(value, notice_) == 0 && kind == noticeKind_) return;
  strlcpy(notice_, value, sizeof(notice_));
  noticeKind_ = kind;
  drawNotice();
}

void DisplayManager::setNightMode(bool night) {
  if (night == night_) return;
  night_ = night;
  C = night ? &theme::kNightColors : &theme::kDayColors;
  syncFxBuffers();
  // Repaint everything with the new colours on the next showTime().
  mode_ = Mode::None;
}

void DisplayManager::drawNotice() {
  if (mode_ != Mode::Clock) return;
  if (notice_[0] != '\0') {
    bannerActive_ = false;  // notices take priority
  } else if (bannerActive_) {
    return;  // the banner draws this line itself
  }
  if (notice_[0] == '\0' && weather_.valid) {
    drawWeatherLine();
    return;
  }
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(noticeKind_ == NoticeKind::Error ? C->error : C->setup, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString(notice_, kScreenW / 2, kNoticeH / 2);
  });
}

bool DisplayManager::startBanner(const char* text, uint8_t pxPerSecond) {
  if (!bannerOk_ || mode_ != Mode::Clock || notice_[0] != '\0' || !text || !*text) return false;
  strlcpy(bannerText_, text, sizeof(bannerText_));
  bannerW_ = bannerSprite.textWidth(bannerText_);
  bannerSpeed_ = pxPerSecond ? pxPerSecond : 70;
  bannerStartMs_ = millis();
  bannerActive_ = true;
  return true;
}

void DisplayManager::drawBanner(uint32_t now) {
  // Enters from the right edge and leaves fully on the left.
  const int32_t offset = (int32_t)((uint64_t)(now - bannerStartMs_) * bannerSpeed_ / 1000);
  const int32_t x = kScreenW - offset;
  if (x + bannerW_ < 0) {
    bannerActive_ = false;
    drawNotice();  // back to the weather line
    return;
  }
  bannerSprite.fillSprite(TFT_BLACK);
  bannerSprite.drawString(bannerText_, x, kNoticeH / 2);
  Paint paint;
  paint.screenY = kNoticeTop;
  tintSprite(bannerSprite, kScreenW, kNoticeH, 3, kNoticeH - 4, paint);
  // Soft fade at both edges.
  uint16_t* px = static_cast<uint16_t*>(bannerSprite.getPointer());
  constexpr int kFade = 24;
  for (int16_t y = 0; y < kNoticeH; ++y) {
    uint16_t* row = px + y * kScreenW;
    for (int i = 0; i < kFade; ++i) {
      const uint8_t a = 255 - i * 255 / kFade;
      for (int16_t xx : {(int16_t)i, (int16_t)(kScreenW - 1 - i)}) {
        const uint16_t c = swap16(row[xx]);
        row[xx] = swap16(theme::mix(c, C->bg, a));
      }
    }
  }
  bannerSprite.pushSprite(0, kNoticeTop);
}

void DisplayManager::setWeather(const weather::Data* data, bool stale) {
  // Redraw only when something visible changes.
  uint32_t key = 0;
  if (data && data->valid) {
    key = (uint32_t)lroundf(data->temp * 10) * 2654435761u ^ data->code * 40503u ^
          (data->dayCount ? (data->days[0].tmax << 8 | (uint8_t)data->days[0].tmin) : 0) ^
          (data->isDay ? 0x10000u : 0) ^ (stale ? 0x20000u : 0) ^ 1u;
  }
  if (key == weatherKey_) return;
  weatherKey_ = key;
  if (data) {
    weather_ = *data;
  } else {
    weather_.valid = false;
  }
  weatherStale_ = stale;
  drawNotice();
}

void DisplayManager::drawWeatherLine() {
  const weather::Data& w = weather_;
  char temp[8];
  snprintf(temp, sizeof(temp), "%d\xC2\xB0", (int)lroundf(w.temp));
  char detail[48];
  if (w.dayCount) {
    snprintf(detail, sizeof(detail), "%s  \xC2\xB7  %d\xC2\xB0 / %d\xC2\xB0%s", weather::describe(w.code),
             w.days[0].tmax, w.days[0].tmin, weatherStale_ ? "  \xC2\xB7  old" : "");
  } else {
    snprintf(detail, sizeof(detail), "%s", weather::describe(w.code));
  }
  drawLine(kNoticeTop, kNoticeH, font_face, [&](TFT_eSprite& s) {
    const int16_t iconW = 26;
    const int16_t tempW = s.textWidth(temp);
    s.unloadFont();
    s.loadFont(font_date);
    int16_t detailW = s.textWidth(detail);
    if (iconW + 4 + tempW + 10 + detailW > kScreenW - 8) {  // long descriptions
      s.unloadFont();
      s.loadFont(font_small);
      detailW = s.textWidth(detail);
    }
    int16_t x = (kScreenW - (iconW + 4 + tempW + 10 + detailW)) / 2;

    weather_icons::draw(s, x + iconW / 2, kNoticeH / 2, 22, weather::iconFor(w.code), w.isDay,
                        night_ || weatherStale_, C->dim);
    x += iconW + 4;
    s.setTextDatum(ML_DATUM);
    s.setTextColor(weatherStale_ ? C->dim : C->date, C->bg);
    s.drawString(detail, x + tempW + 10, kNoticeH / 2);
    s.unloadFont();
    s.loadFont(font_face);
    s.setTextColor(weatherStale_ ? C->dim : pal_.top, C->bg);
    s.drawString(temp, x, kNoticeH / 2);
  });
}

void DisplayManager::endOverlayScreen() {
  if (mode_ == Mode::Weather || mode_ == Mode::Info || mode_ == Mode::Settings ||
      mode_ == Mode::Calibrate || mode_ == Mode::Photo || mode_ == Mode::Confirm ||
      mode_ == Mode::Storage || mode_ == Mode::Update) {
    mode_ = Mode::None;  // next showTime() repaints the clock
  }
}

int16_t DisplayManager::barX() const { return hour12_ ? kBarX + kMeridiemW : kBarX; }

int16_t DisplayManager::barW() const { return hour12_ ? kBarW - kMeridiemW : kBarW; }

void DisplayManager::setHour12(bool enabled) {
  if (enabled == hour12_) return;
  hour12_ = enabled;
  pm_ = lastLocal_.tm_hour >= 12;
  if (mode_ == Mode::Clock) mode_ = Mode::None;  // repaint with the new layout
}

void DisplayManager::drawMeridiem() {
  if (mode_ != Mode::Clock || !hour12_) return;
  TFT_eSprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(kMeridiemW - 4, 18)) return;
  s.fillSprite(C->bg);
  s.loadFont(font_small);
  s.setTextColor(pal_.top, C->bg);
  s.setTextDatum(ML_DATUM);
  s.drawString(pm_ ? "PM" : "AM", 0, 9);
  s.unloadFont();
  s.pushSprite(kBarX, kRowY - 9);
  s.deleteSprite();
}

void DisplayManager::drawScreenTitle(const char* title) {
  drawLine(0, 28, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(ML_DATUM);
    s.setTextColor(pal_.top, C->bg);
    s.drawString(title, 10, 15);
    s.setTextDatum(MR_DATUM);
    s.setTextColor(C->faint, C->bg);
    s.drawString("swipe \xC2\xB7 30 s to clock", kScreenW - 10, 15);
  });
}

namespace {

// A continuous rounded bar like a phone's storage bar: segments side by side,
// free space (faint) at the end. Non-empty segments get at least 2 px.
void drawSegmentBar(TFT_eSprite& s, int16_t x, int16_t y, int16_t w, int16_t h, uint32_t total,
                    const uint32_t* seg, const uint16_t* colors, int n) {
  int16_t edge[6];
  int16_t at = 0;
  uint64_t sum = 0;
  for (int k = 0; k < n && k < 6; ++k) {
    sum += seg[k];
    int16_t to = total ? min<int64_t>(w, (int64_t)(w * sum / total)) : 0;
    if (seg[k] && to < at + 2) to = min<int16_t>(w, at + 2);
    edge[k] = at = max(at, to);
  }
  for (int16_t px = 0; px < w; ++px) {
    uint16_t c = C->faint;
    for (int k = 0; k < n && k < 6; ++k) {
      if (px < edge[k]) {
        c = colors[k];
        break;
      }
    }
    const int16_t inset = (px == 0 || px == w - 1) ? 1 : 0;  // rounded ends
    s.drawFastVLine(x + px, y + inset, h - 2 * inset, c);
  }
}

// Bytes as "1.48 MB" or "68 KB".
void formatSize(char* buf, size_t len, uint32_t bytes) {
  if (bytes >= 1024 * 1024) {
    snprintf(buf, len, "%.2f MB", bytes / 1048576.0f);
  } else {
    snprintf(buf, len, "%u KB", (unsigned)((bytes + 512) / 1024));
  }
}

constexpr int16_t kInfoTop = 30, kInfoRowH = 18;
constexpr int16_t kInfoBtnY = 212, kInfoBtnH = 26;
constexpr int16_t kInfoValueX = 100;
constexpr int16_t kInfoBarW = 110, kInfoBarH = 8;
constexpr int16_t kConfirmBtnY = 168, kConfirmBtnH = 58;
}  // namespace

void DisplayManager::showInfoScreen(const InfoView& v) {
  if (mode_ != Mode::Info) {
    enterMode(Mode::Info);
    drawScreenTitle("Info");
    drawLine(kInfoBtnY, kInfoBtnH, font_small, [&](TFT_eSprite& s) {
      const char* labels[] = {"Wi-Fi", "Storage", "Update"};
      for (int b = 0; b < 3; ++b) {
        const int16_t x = 10 + b * 102;
        s.fillSmoothRoundRect(x, 0, 96, kInfoBtnH, 8, C->faint, C->bg);
        s.setTextDatum(MC_DATUM);
        s.setTextColor(C->text, C->faint);
        s.drawString(labels[b], x + 48, kInfoBtnH / 2 + 1);
      }
    });
  }
  for (int i = 0; i < v.count && i < InfoView::kMaxRows; ++i) {
    drawLine(kInfoTop + i * kInfoRowH, kInfoRowH, font_small, [&](TFT_eSprite& s) {
      s.setTextDatum(ML_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString(v.rows[i][0], 12, kInfoRowH / 2);
      int16_t x = kInfoValueX;
      const InfoBar& bar = v.bars[i];
      if (bar.total > 0) {
        const uint16_t color = pal_.top;
        drawSegmentBar(s, x, (kInfoRowH - kInfoBarH) / 2, kInfoBarW, kInfoBarH, bar.total,
                       &bar.used, &color, 1);
        x += kInfoBarW + 8;
      }
      s.setTextColor(C->text, C->bg);
      s.drawString(v.rows[i][1], x, kInfoRowH / 2);
    });
  }
}

DisplayManager::InfoButton DisplayManager::infoButtonHit(int16_t x, int16_t y) const {
  // Generous: the touch mapping can be ~15 px off before calibration.
  if (mode_ != Mode::Info || y < kInfoBtnY - 14) return InfoButton::None;
  if (x < 109) return InfoButton::Wifi;
  if (x < 211) return InfoButton::Storage;
  return InfoButton::Update;
}

void DisplayManager::showStorageScreen(const StorageView& v) {
  enterMode(Mode::Storage);
  drawScreenTitle("Storage");
  char a[24], b[24];
  auto header = [&](int16_t y, const char* left, const char* right) {
    drawLine(y, 20, font_small, [&](TFT_eSprite& s) {
      s.setTextDatum(ML_DATUM);
      s.setTextColor(C->text, C->bg);
      s.drawString(left, 12, 10);
      s.setTextDatum(MR_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString(right, kScreenW - 12, 10);
    });
  };
  auto bar = [&](int16_t y, uint32_t total, const uint32_t* seg, const uint16_t* colors, int n) {
    drawLine(y, 14, font_small, [&](TFT_eSprite& s) {
      drawSegmentBar(s, 12, 2, kScreenW - 24, 10, total, seg, colors, n);
    });
  };
  // Legend rows: colour square, name, size.
  auto row = [&](int16_t y, uint16_t color, const char* name, uint32_t bytes) {
    drawLine(y, 18, font_small, [&](TFT_eSprite& s) {
      s.fillSmoothRoundRect(14, 4, 9, 9, 2, color, C->bg);
      s.setTextDatum(ML_DATUM);
      s.setTextColor(C->text, C->bg);
      s.drawString(name, 30, 9);
      char size[16];
      formatSize(size, sizeof(size), bytes);
      s.setTextDatum(MR_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString(size, kScreenW - 12, 9);
    });
  };

  const uint32_t flashUsed = v.firmware + v.updateSlot + v.photos + v.system;
  formatSize(a, sizeof(a), v.chip);
  formatSize(b, sizeof(b), flashUsed);
  char left[32], right[32];
  snprintf(left, sizeof(left), "Flash \xC2\xB7 %s", a);
  snprintf(right, sizeof(right), "%s used", b);
  header(30, left, right);
  const uint16_t photoColor = theme::rgb(255, 95, 162);
  const uint32_t flashSeg[4] = {v.firmware, v.updateSlot, v.photos, v.system};
  const uint16_t flashColors[4] = {pal_.top, pal_.bottom, photoColor, C->dim};
  bar(50, v.chip, flashSeg, flashColors, 4);
  row(66, pal_.top, "Firmware", v.firmware);
  row(84, pal_.bottom, "Update slot (for updates)", v.updateSlot);
  row(102, photoColor, "Photos", v.photos);
  row(120, C->dim, "System (settings, Wi-Fi)", v.system);
  row(138, C->faint, "Free", v.chip > flashUsed ? v.chip - flashUsed : 0);

  formatSize(a, sizeof(a), v.heapTotal);
  formatSize(b, sizeof(b), v.heapUsed);
  snprintf(left, sizeof(left), "RAM \xC2\xB7 %s", a);
  snprintf(right, sizeof(right), "%s used", b);
  header(160, left, right);
  const uint16_t ramColor = pal_.top;
  bar(180, v.heapTotal, &v.heapUsed, &ramColor, 1);
  row(196, pal_.top, "In use", v.heapUsed);
  row(214, C->faint, "Free", v.heapTotal > v.heapUsed ? v.heapTotal - v.heapUsed : 0);
}

void DisplayManager::showUpdateScreen(const UpdateView& v) {
  if (mode_ != Mode::Update) {
    enterMode(Mode::Update);
    drawScreenTitle("Software update");
  }
  drawLine(40, 22, font_small, [&](TFT_eSprite& s) {
    char t[48];
    snprintf(t, sizeof(t), "Installed: %s", v.installed);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString(t, kScreenW / 2, 11);
  });
  drawLine(72, 30, font_date, [&](TFT_eSprite& s) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(v.actionIsInstall ? pal_.top : C->text, C->bg);
    s.drawString(v.status, kScreenW / 2, 15);
  });
  drawLine(106, 44, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    // Up to two lines, split at the first " | ".
    const char* bar = strstr(v.detail, " | ");
    if (bar) {
      char first[64];
      strlcpy(first, v.detail, min<size_t>(sizeof(first), bar - v.detail + 1));
      s.drawString(first, kScreenW / 2, 11);
      s.drawString(bar + 3, kScreenW / 2, 32);
    } else {
      s.drawString(v.detail, kScreenW / 2, 11);
    }
  });
  drawLine(kConfirmBtnY, kConfirmBtnH, font_date, [&](TFT_eSprite& s) {
    s.fillSmoothRoundRect(10, 0, 140, kConfirmBtnH, 12, C->faint, C->bg);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->faint);
    s.drawString("Back", 80, kConfirmBtnH / 2 + 1);
    if (v.action[0]) {
      const uint16_t fill = v.actionIsInstall ? pal_.top : C->faint;
      s.fillSmoothRoundRect(170, 0, 140, kConfirmBtnH, 12, fill, C->bg);
      s.setTextColor(v.actionIsInstall ? theme::rgb(0, 0, 0) : C->text, fill);
      s.drawString(v.action, 240, kConfirmBtnH / 2 + 1);
    }
  });
}

void DisplayManager::showWifiResetScreen(const char* savedSsid, const char* portalSsid) {
  enterMode(Mode::Confirm);
  drawScreenTitle("Change Wi-Fi");
  drawLine(40, 30, font_date, [&](TFT_eSprite& s) {
    char t[64];
    if (savedSsid && *savedSsid) {
      snprintf(t, sizeof(t), "Forget \"%s\"?", savedSsid);
    } else {
      strlcpy(t, "Set up Wi-Fi?", sizeof(t));
    }
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(t, kScreenW / 2, 15);
  });
  char portal[40];
  snprintf(portal, sizeof(portal), "\"%s\"", portalSsid);
  const char* lines[] = {"The clock will open its own setup Wi-Fi", portal,
                         "Join it from your phone to choose", "the new network."};
  for (int i = 0; i < 4; ++i) {
    drawLine(76 + i * 21, 21, font_small, [&](TFT_eSprite& s) {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(i == 1 ? pal_.top : C->dim, C->bg);
      s.drawString(lines[i], kScreenW / 2, 10);
    });
  }
  drawLine(kConfirmBtnY, kConfirmBtnH, font_date, [&](TFT_eSprite& s) {
    s.fillSmoothRoundRect(10, 0, 140, kConfirmBtnH, 12, C->faint, C->bg);
    s.fillSmoothRoundRect(170, 0, 140, kConfirmBtnH, 12, C->error, C->bg);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->faint);
    s.drawString("Cancel", 80, kConfirmBtnH / 2 + 1);
    s.setTextColor(theme::rgb(0, 0, 0), C->error);
    s.drawString("Forget", 240, kConfirmBtnH / 2 + 1);
  });
}

DisplayManager::ConfirmHit DisplayManager::confirmHitTest(int16_t x, int16_t y) const {
  if ((mode_ != Mode::Confirm && mode_ != Mode::Update) || y < kConfirmBtnY - 24) {
    return ConfirmHit::None;
  }
  // A gap in the middle so a touch between the buttons does nothing.
  if (x < 150) return ConfirmHit::Cancel;
  if (x >= 170) return ConfirmHit::Confirm;
  return ConfirmHit::None;
}

namespace {

constexpr int16_t kSetTop = 30;
constexpr int16_t kSetRowH = 35;
constexpr int16_t kBtnY = 4, kBtnH = 27;
constexpr int16_t kMinusX = 172, kPlusX = 274, kStepW = 42;
constexpr int16_t kToggleX = 196, kToggleW = 120;
constexpr int16_t kArrowW = 46;

void drawButton(TFT_eSprite& s, int16_t x, int16_t w, const char* label, bool on, uint16_t accent) {
  s.fillSmoothRoundRect(x, kBtnY, w, kBtnH, 8, on ? accent : C->faint, C->bg);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(on ? theme::rgb(0, 0, 0) : C->text, on ? accent : C->faint);
  s.drawString(label, x + w / 2, kBtnY + kBtnH / 2 + 1);
}

}  // namespace

void DisplayManager::showSettingsScreen(const SettingsView& v) {
  if (mode_ != Mode::Settings) {
    enterMode(Mode::Settings);
    drawScreenTitle("Settings");
  }
  const uint16_t accent = pal_.top;
  auto row = [&](int index, auto&& draw) {
    drawLine(kSetTop + index * kSetRowH, kSetRowH, font_small, [&](TFT_eSprite& s) {
      s.setTextDatum(ML_DATUM);
      draw(s);
    });
  };
  auto label = [&](TFT_eSprite& s, const char* text, const char* sub) {
    s.setTextDatum(ML_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(text, 12, sub ? 11 : kSetRowH / 2);
    if (sub) {
      s.setTextColor(C->dim, C->bg);
      s.drawString(sub, 12, 26);
    }
  };
  auto stepper = [&](TFT_eSprite& s, const char* text, uint8_t pct) {
    label(s, text, nullptr);
    drawButton(s, kMinusX, kStepW, "-", false, accent);
    drawButton(s, kPlusX, kStepW, "+", false, accent);
    char v[8];
    snprintf(v, sizeof(v), "%u%%", pct);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(v, (kMinusX + kStepW + kPlusX) / 2, kSetRowH / 2);
  };

  row(0, [&](TFT_eSprite& s) { stepper(s, "Day brightness", v.dayPct); });
  row(1, [&](TFT_eSprite& s) { stepper(s, "Night brightness", v.nightPct); });
  row(2, [&](TFT_eSprite& s) {
    label(s, "Night mode", v.nightWindow);
    drawButton(s, kToggleX, kToggleW, v.nightOn ? "On" : "Off", v.nightOn, accent);
  });
  row(3, [&](TFT_eSprite& s) {
    label(s, "Time format", nullptr);
    drawButton(s, kToggleX, kToggleW, v.hour12 ? "12-hour" : "24-hour", true, accent);
  });
  row(4, [&](TFT_eSprite& s) {
    drawButton(s, 8, kArrowW, "<", false, accent);
    drawButton(s, kScreenW - 8 - kArrowW, kArrowW, ">", false, accent);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString("Colours", kScreenW / 2, 9);
    s.setTextColor(C->text, C->bg);
    s.drawString(v.colorName, kScreenW / 2, 25);
  });
  row(5, [&](TFT_eSprite& s) {
    const bool many = v.recentCount > 1;
    drawButton(s, 8, kArrowW, "<", false, accent);
    drawButton(s, kScreenW - 8 - kArrowW, kArrowW, ">", false, accent);
    char text[48];
    if (many && v.recentIndex >= 0) {
      snprintf(text, sizeof(text), "%s  (%d/%d)", v.place, v.recentIndex + 1, v.recentCount);
    } else {
      snprintf(text, sizeof(text), "%s", v.place);
    }
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(text, kScreenW / 2, kSetRowH / 2);
  });
}

SettingsAction DisplayManager::settingsHitTest(int16_t x, int16_t y) const {
  if (mode_ != Mode::Settings || y < kSetTop) return SettingsAction::None;
  const int row = (y - kSetTop) / kSetRowH;
  // Generous hit areas: resistive touch is imprecise.
  auto inside = [&](int16_t bx, int16_t bw) { return x >= bx - 8 && x < bx + bw + 8; };
  switch (row) {
    case 0:
      if (inside(kMinusX, kStepW)) return SettingsAction::DayMinus;
      if (inside(kPlusX, kStepW)) return SettingsAction::DayPlus;
      break;
    case 1:
      if (inside(kMinusX, kStepW)) return SettingsAction::NightMinus;
      if (inside(kPlusX, kStepW)) return SettingsAction::NightPlus;
      break;
    case 2:
      if (x >= kToggleX - 20) return SettingsAction::NightToggle;
      break;
    case 3:
      if (x >= kToggleX - 20) return SettingsAction::FormatToggle;
      break;
    case 4:
      if (x < kScreenW / 3) return SettingsAction::ColorPrev;
      if (x >= kScreenW * 2 / 3) return SettingsAction::ColorNext;
      break;
    case 5:
      if (x < kScreenW / 3) return SettingsAction::LocationPrev;
      if (x >= kScreenW * 2 / 3) return SettingsAction::LocationNext;
      break;
  }
  return SettingsAction::None;
}

bool DisplayManager::showPhoto(const char* path) {
  enterMode(Mode::Photo);
  tft.fillScreen(TFT_BLACK);
  uint16_t w = 0, h = 0;
  if (TJpgDec.getFsJpgSize(&w, &h, path, LittleFS) != JDR_OK || w == 0 || h == 0) {
    Serial.printf("[PHOTO] ERROR: can't read %s\n", path);
    return false;
  }
  // tjpgd scales by 1/1, 1/2, 1/4 or 1/8: use the largest that fits.
  uint8_t scale = 1;
  while (scale < 8 && (w / scale > kScreenW || h / scale > kScreenH)) scale *= 2;
  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback([](int16_t x, int16_t y, uint16_t bw, uint16_t bh, uint16_t* bitmap) {
    if (y >= kScreenH) return false;
    tft.pushImage(x, y, bw, bh, bitmap);
    return true;
  });
  const int16_t x = (kScreenW - (int16_t)(w / scale)) / 2;
  const int16_t y = (kScreenH - (int16_t)(h / scale)) / 2;
  const uint32_t t0 = millis();
  tft.startWrite();
  const JRESULT r = TJpgDec.drawFsJpg(x, y, path, LittleFS);
  tft.endWrite();
  Serial.printf("[PHOTO] %s %ux%u 1/%u in %lu ms%s\n", path, w, h, scale,
                (unsigned long)(millis() - t0), r == JDR_OK ? "" : " (decode error)");
  return r == JDR_OK;
}

void DisplayManager::showCalibrationTarget(int16_t x, int16_t y, const char* text) {
  enterMode(Mode::Calibrate);
  tft.fillScreen(C->bg);
  tft.drawFastHLine(x - 12, y, 25, TFT_WHITE);
  tft.drawFastVLine(x, y - 12, 25, TFT_WHITE);
  tft.drawCircle(x, y, 6, theme::rgb(255, 120, 0));
  drawLine(108, 24, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(text, kScreenW / 2, 12);
  });
}

void DisplayManager::showWeatherScreen(const weather::Data& w, uint32_t ageS) {
  enterMode(Mode::Weather);
  const uint16_t accent = pal_.top;
  auto deg = [](char* buf, size_t n, int t) { snprintf(buf, n, "%d\xC2\xB0", t); };

  // Header: place and data age.
  drawLine(0, 28, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(ML_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(w.place, 10, 15);
    char age[32];
    if (ageS == UINT32_MAX) age[0] = '\0';
    else if (ageS < 120) snprintf(age, sizeof(age), "Updated just now");
    else if (ageS < 7200) snprintf(age, sizeof(age), "Updated %lu min ago", (unsigned long)(ageS / 60));
    else snprintf(age, sizeof(age), "Updated %lu h ago", (unsigned long)(ageS / 3600));
    s.setTextDatum(MR_DATUM);
    s.setTextColor(ageS > config::kWeatherStaleS ? C->warn : C->dim, C->bg);
    s.drawString(age, kScreenW - 10, 15);
  });

  // Now: big icon, temperature, condition, details.
  drawLine(28, 88, font_temp, [&](TFT_eSprite& s) {
    weather_icons::draw(s, 54, 44, 74, weather::iconFor(w.code), w.isDay);
    char t[8];
    deg(t, sizeof(t), (int)lroundf(w.temp));
    s.setTextDatum(L_BASELINE);
    s.setTextColor(accent, C->bg);
    s.drawString(t, 102, 58);
    s.unloadFont();
    s.loadFont(font_date);
    s.setTextColor(C->text, C->bg);
    s.drawString(weather::describe(w.code), 102, 84);
    s.unloadFont();
    s.loadFont(font_small);
    char line[24];
    s.setTextColor(C->dim, C->bg);
    snprintf(line, sizeof(line), "Feels %d\xC2\xB0", (int)lroundf(w.feels));
    s.drawString(line, 216, 22);
    snprintf(line, sizeof(line), "Humidity %u%%", w.humidity);
    s.drawString(line, 216, 41);
    if (w.dayCount) {
      snprintf(line, sizeof(line), "Rain %u%%", w.days[0].pop);
      s.drawString(line, 216, 60);
    }
  });

  // Next hours, every 3 h.
  drawLine(116, 58, font_small, [&](TFT_eSprite& s) {
    const int cols = 6;
    const int16_t colW = kScreenW / cols;
    for (int c = 0; c < cols; ++c) {
      const int i = 1 + c * 3;
      if (i >= w.hourCount) break;
      const weather::Hour& h = w.hours[i];
      const int16_t cx = c * colW + colW / 2;
      struct tm t;
      time_t tt = h.time;
      localtime_r(&tt, &t);
      char label[8];
      snprintf(label, sizeof(label), "%02d:00", t.tm_hour);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString(label, cx, 8);
      weather_icons::draw(s, cx, 29, 26, weather::iconFor(h.code), h.isDay);
      char tv[8];
      deg(tv, sizeof(tv), h.temp);
      s.setTextColor(C->text, C->bg);
      s.drawString(tv, cx, 50);
    }
  });

  // Next days.
  drawLine(174, 66, font_small, [&](TFT_eSprite& s) {
    s.drawFastHLine(10, 0, kScreenW - 20, C->faint);
    const int16_t colW = kScreenW / weather::kDays;
    static const char* const kShortDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int i = 0; i < w.dayCount; ++i) {
      const weather::Day& d = w.days[i];
      const int16_t cx = i * colW + colW / 2;
      struct tm t;
      time_t tt = d.time + 12 * 3600;  // midday: safe against timezone offsets
      localtime_r(&tt, &t);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(i == 0 ? accent : C->text, C->bg);
      s.drawString(i == 0 ? "Today" : kShortDays[t.tm_wday % 7], cx, 14);
      weather_icons::draw(s, cx - 13, 34, 22, weather::iconFor(d.code), true);
      char pop[8];
      snprintf(pop, sizeof(pop), "%u%%", d.pop);
      s.setTextDatum(ML_DATUM);
      s.setTextColor(d.pop >= 50 ? theme::rgb(0, 190, 255) : C->dim, C->bg);
      s.drawString(pop, cx + 2, 34);
      char hi[8], lo[8];
      deg(hi, sizeof(hi), d.tmax);
      deg(lo, sizeof(lo), d.tmin);
      const int16_t hw = s.textWidth(hi), lw = s.textWidth(lo);
      const int16_t x0 = cx - (hw + 6 + lw) / 2;
      s.setTextColor(C->text, C->bg);
      s.drawString(hi, x0, 54);
      s.setTextColor(C->dim, C->bg);
      s.drawString(lo, x0 + hw + 6, 54);
    }
  });
}

void DisplayManager::showError(const char* title, const char* detail) {
  char key[sizeof(lastError_)];
  snprintf(key, sizeof(key), "%s|%s", title, detail);
  if (mode_ == Mode::Error && strcmp(key, lastError_) == 0) return;
  enterMode(Mode::Error);
  strlcpy(lastError_, key, sizeof(lastError_));

  drawLine(88, 34, font_date, [&](TFT_eSprite& s) {
    s.setTextColor(C->error, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString(title, kScreenW / 2, 17);
  });
  drawLine(124, 24, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(C->text, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString(detail, kScreenW / 2, 12);
  });
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(C->faint, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString(config::kProjectName, kScreenW / 2, kNoticeH / 2);
  });
}

void DisplayManager::showTestPattern(uint32_t durationMs) {
  enterMode(Mode::Test);
  testUntilMs_ = millis() + durationMs;
  struct Bar {
    const char* label;
    uint16_t color;
    uint16_t text;
  };
  const Bar bars[] = {{"RED", TFT_RED, TFT_WHITE},      {"GREEN", TFT_GREEN, TFT_BLACK},
                      {"BLUE", TFT_BLUE, TFT_WHITE},    {"WHITE", TFT_WHITE, TFT_BLACK},
                      {"YELLOW", TFT_YELLOW, TFT_BLACK}};
  const int16_t barH = 34;
  for (int i = 0; i < 5; ++i) {
    drawLine(i * barH, barH, font_date, [&](TFT_eSprite& s) {
      s.fillSprite(bars[i].color);
      s.setTextColor(bars[i].text, bars[i].color);
      s.setTextDatum(MC_DATUM);
      s.drawString(bars[i].label, kScreenW / 2, barH / 2);
    });
  }
  drawLine(5 * barH, kScreenH - 5 * barH, font_date, [&](TFT_eSprite& s) {
    s.setTextColor(TFT_WHITE, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString("BLACK background", kScreenW / 2, (kScreenH - 5 * barH) / 2);
  });
}

void DisplayManager::showPaletteTest(uint32_t durationMs) {
  if (!spritesOk_) return;
  enterMode(Mode::Test);
  testUntilMs_ = millis() + durationMs;
  const int n = theme::namedPaletteCount();
  const int16_t colW = kScreenW / n;
  for (int i = 0; i < n; ++i) {
    const theme::Palette p = theme::namedPalette(i, nullptr);
    const int16_t x = i * colW;
    // Big digit sample, cropped to the column.
    digitSprite.fillSprite(TFT_BLACK);
    drawGlyphString(digitSprite, "8", 0, colW, kDigitBaseline);
    Paint paint;
    paint.palette = &p;
    tintSprite(digitSprite, kDigitW, kDigitH, kGlyphTop, kDigitBaseline, paint);
    digitSprite.pushSprite(x, 30, 0, 0, colW, kDigitH);
    // Vertical gradient swatch.
    for (int16_t y = 0; y < 70; ++y) {
      tft.drawFastHLine(x + 8, 128 + y, colW - 16, theme::mix(p.top, p.bottom, y * 255 / 69));
    }
  }
  drawLine(206, 24, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(MC_DATUM);
    for (int i = 0; i < n; ++i) {
      const char* name = nullptr;
      const theme::Palette p = theme::namedPalette(i, &name);
      s.setTextColor(p.top, C->bg);
      s.drawString(name, i * colW + colW / 2, 12);
    }
  });
}

// Screen transitions. There's no RAM for a second frame buffer, so the old
// screen is animated away by painting black into the display memory (and, for
// Slide, the ST7789's hardware scroll, which in landscape shifts the picture
// sideways: screen column x shows memory column (x + offset) % 320). The new
// screen is then drawn with the backlight off and fades in. The ST7789's
// partial display could reveal it instead, but on this panel its hidden area
// shows white, not black.
namespace {

void cmd16(uint8_t c, std::initializer_list<uint16_t> args) {
  tft.writecommand(c);
  for (uint16_t a : args) {
    tft.writedata(a >> 8);
    tft.writedata(a & 0xFF);
  }
}

void scrollOffset(int off) { cmd16(0x37, {static_cast<uint16_t>(((off % kScreenW) + kScreenW) % kScreenW)}); }

float easeInOut(float t) { return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); }

constexpr uint32_t kSweepMs = 260;

// Runs `step(p)` with p easing from 0 to 320 over `ms`.
template <typename Fn>
void animate(uint32_t ms, Fn&& step) {
  const uint32_t t0 = millis();
  for (;;) {
    const uint32_t e = millis() - t0;
    const float t = e >= ms ? 1.0f : static_cast<float>(e) / ms;
    step(static_cast<int>(easeInOut(t) * kScreenW + 0.5f));
    if (t >= 1.0f) break;
    delay(6);
  }
}

}  // namespace

void DisplayManager::transitionOut(theme::Transition t, int dir) {
  switch (t) {
    case theme::Transition::Wipe: {
      // Black sweeps in from the side the new screen comes from.
      int done = 0;
      animate(kSweepMs, [&](int p) {
        if (p <= done) return;
        if (dir > 0) tft.fillRect(kScreenW - p, 0, p - done, kScreenH, TFT_BLACK);
        else tft.fillRect(done, 0, p - done, kScreenH, TFT_BLACK);
        done = p;
      });
      break;
    }
    case theme::Transition::Slide: {
      // The picture slides away; the memory columns that wrap round to the
      // other edge are painted black before they show.
      int done = 0;
      cmd16(0x33, {0, static_cast<uint16_t>(kScreenW), 0});  // the whole panel scrolls
      animate(kSweepMs, [&](int p) {
        if (p > done) {
          if (dir > 0) tft.fillRect(done, 0, p - done, kScreenH, TFT_BLACK);
          else tft.fillRect(kScreenW - p, 0, p - done, kScreenH, TFT_BLACK);
          done = p;
        }
        scrollOffset(dir > 0 ? p : -p);
      });
      scrollOffset(0);
      break;
    }
    case theme::Transition::Blinds: {
      // Eight stripes close.
      constexpr int kStripes = 8, kStripeW = kScreenW / kStripes;
      int done = 0;
      animate(kSweepMs, [&](int p) {
        const int w = p * kStripeW / kScreenW;
        if (w <= done) return;
        for (int i = 0; i < kStripes; ++i) {
          const int x = dir > 0 ? i * kStripeW + kStripeW - w : i * kStripeW + done;
          tft.fillRect(x, 0, w - done, kScreenH, TFT_BLACK);
        }
        done = w;
      });
      break;
    }
    default:
      break;
  }
}

void DisplayManager::screenshot(Print& out) {
  // Finish running animations so the capture shows a settled frame.
  lastFrameMs_ = 0;
  for (int i = 0; i < 4; ++i) bigRoll_[i].startMs -= kRollMs;
  secRoll_[0].startMs -= kRollMs;
  tick();

  static uint16_t row[kScreenW];
  out.printf("\n[SHOT] BEGIN %d %d\n", kScreenW, kScreenH);
  for (int16_t y = 0; y < kScreenH; ++y) {
    tft.readRect(0, y, kScreenW, 1, row);
    out.write(reinterpret_cast<const uint8_t*>(row), sizeof(row));
  }
  out.printf("\n[SHOT] END\n");
}
