#include "display_manager.h"

#include "mem_lease.h"
#include "moon.h"
#include <math.h>

#include <Arduino.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <string.h>

#include "backdrop.h"
#include "config.h"
#include "fonts/font_date.h"
#include "fonts/font_face.h"
#include "fonts/font_seconds.h"
#include "fonts/font_small.h"
#include "fonts/font_tiny.h"
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
uint16_t barBg[4 * 320];  // the theme under the minute bar (kBarH rows), swapped
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
// The date line starts just under the seconds and leaves room above the capitals
// for stacked Vietnamese accents (Ỗ, Ổ, Ằ reach 27 px above the baseline).
constexpr int16_t kDateTop = kRowY + 14;
constexpr int16_t kDateH = 34;
constexpr int16_t kDateBaseline = 28;
constexpr int16_t kPhraseTop = kStatusH;  // the phrase line, between the status bar and the time
constexpr int16_t kPhraseH = kTimeTop - kStatusH - 2;
constexpr uint32_t kPhraseMinMs = 4000;   // each part shows at least this long,
constexpr uint32_t kPhraseCharMs = 70;    // plus this per character
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

void faceRow(int16_t y, int16_t x0, int16_t w, uint16_t* out);  // below
void fillBackdrop(TFT_eSprite& s, int16_t sx, int16_t sy);
extern uint16_t faceRowBuf[320];

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
int32_t dateCovKey = -1;  // (year * 400 + day of year) * 4 + date view
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

// Analog face. The dial's static parts (ring, ticks, numerals) are rendered
// once into a mask, one byte per pixel: coverage (high 4 bits) and which ink
// (low bits: 0 minute ticks, 1 hour ticks and numerals, 2 the ring). Frames
// rebuild only the rectangle the hands moved through, from the mask, in a
// band sprite. Both are allocated only while the analog face shows (see
// syncFxBuffers), like the animation buffers.
constexpr int16_t kDialD = 208;  // the dial's square, at the screen's top left
constexpr int16_t kDialX = 2, kDialY = 0;
constexpr int16_t kDialC = kDialD / 2;  // centre, dial coordinates
constexpr int16_t kDialR = 100;
constexpr int16_t kDialBandH = 32;
constexpr int16_t kColX = kDialX + kDialD + 4;  // the date column
constexpr int16_t kColW = kScreenW - kColX;
constexpr uint32_t kAnalogFrameMs = 50;  // 20 fps for the sweeping second hand
// The mask in row chunks: after HTTPS the heap rarely has one 43 KB block
// free again, but always has eight 5.4 KB ones.
constexpr int16_t kDialChunkRows = 26, kDialChunks = kDialD / kDialChunkRows;
static_assert(kDialChunks * kDialChunkRows == kDialD, "whole chunks");
uint8_t* dialChunks[kDialChunks] = {};
bool dialMask = false;  // all chunks allocated
bool dialMaskBuilt = false;
uint32_t dialRetryMs = 0;  // the last try to allocate the dial's memory
bool dialMissLogged = false;
inline uint8_t* dialRow(int16_t y) {
  return dialChunks[y / kDialChunkRows] + (y % kDialChunkRows) * kDialD;
}
void freeDialMask() {
  for (uint8_t*& c : dialChunks) {
    free(c);
    c = nullptr;
  }
  dialMask = false;
}
bool allocDialMask() {
  for (uint8_t*& c : dialChunks) {
    c = static_cast<uint8_t*>(malloc(kDialChunkRows * kDialD));
    if (!c) {
      freeDialMask();
      return false;
    }
  }
  dialMask = true;
  return true;
}
TFT_eSprite dialBand(&tft);
bool dialBandOk = false;

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
  const uint16_t* under = nullptr;  // the background under the sprite (swapped, stride w); nullptr: computed
};

// The theme under the banner's line, kept while a banner scrolls: it's
// redrawn every frame (16 ms), too often to compute the theme each time.
uint16_t* bannerBg = nullptr;
void freeBannerBg() {
  free(bannerBg);
  bannerBg = nullptr;
}

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
  const bool themedBg = backdrop::active();
  for (int16_t y = 0; y < h; ++y) {
    const int t = y <= y0 ? 0 : y >= y1 ? 256 : (y - y0) * 256 / (y1 - y0);
    const uint16_t rowFg = uniform ? blend256(pal.top, pal.bottom, t) : 0;
    if (themedBg && !p.under) faceRow(p.screenY + y, p.screenX, min<int16_t>(w, kScreenW), faceRowBuf);
    const uint16_t* bgRow = p.under ? p.under + y * w : faceRowBuf;
    const int16_t shineAt = shineX - (p.screenY + y - kTimeTop) * 2 / 5;  // leans right going up
    uint16_t* row = px + y * w;
    const uint8_t* glowRow = glowA + y * w;
    for (int16_t x = 0; x < w; ++x) {
      const uint16_t c = row[x];
      const int g = glow ? glowRow[x] : 0;
      const uint16_t under = themedBg ? swap16(bgRow[x]) : bg;
      if (c == 0 && g == 0) {
        row[x] = themedBg ? bgRow[x] : bgSwapped;
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
      const uint16_t base = g ? blend256(under, fg, (g * kGlowStrength) >> 8) : under;
      int a = c ? (swap16(c) >> 5) & 0x3F : 0;  // glyph coverage 0..63
      a = ((a + (a >> 5)) * cov) >> 8;           // 0..64, scaled by the pulse
      row[x] = swap16(blend256(base, fg, a << 2));
    }
  }
}

// ---- Background themes on the clock face (see backdrop.h) ----

// The analog face darkens the theme inside its dial, for the ticks and hands.
bool dimDial = false;
constexpr int16_t kDialAreaX = 2, kDialAreaC = 104, kDialAreaR = 99;  // = kDialX, kDialC, kDialR - 1

// A row of the clock face's background (byte-swapped, sprite order).
void faceRow(int16_t y, int16_t x0, int16_t w, uint16_t* out) {
  backdrop::row(y, x0, w, out);
  if (!dimDial) return;
  const int dy = y - kDialAreaC;
  if (dy <= -kDialAreaR || dy >= kDialAreaR) return;
  const int hw = static_cast<int>(sqrtf(static_cast<float>(kDialAreaR * kDialAreaR - dy * dy)));
  const int xa = max<int>(x0, kDialAreaX + kDialAreaC - hw), xb = min<int>(x0 + w, kDialAreaX + kDialAreaC + hw);
  for (int x = xa; x < xb; ++x) {
    out[x - x0] = swap16(blend256(swap16(out[x - x0]), C->bg, 150));
  }
}

// Text and anti-aliased shapes on the clock face: over a theme they blend with
// what's under them (TFT_eSPI reads the pixel), else with the plain background.
inline bool themed() { return backdrop::active(); }
inline uint32_t aaBg() { return themed() ? 0x00FFFFFFu : C->bg; }
inline void ink(TFT_eSprite& s, uint16_t fg) {
  if (themed()) {
    s.setTextColor(fg, fg);  // fg == bg: a sprite blends glyph edges with its own pixels
  } else {
    s.setTextColor(fg, C->bg);
  }
}
uint16_t faceRowBuf[320];  // scratch: one row of the background

// An anti-aliased ring segment (radii ir..r around cx, cy, within `half`
// degrees either side of straight up; 180: a whole ring) blended with the
// pixels under it. TFT_eSPI's arcs can't read the background (they blend
// toward the literal colour 0x00FFFFFF, white), so over a theme this is used.
void blendArc(TFT_eSPI& g, float cx, float cy, float r, float ir, float half, uint16_t color) {
  const float halfRad = half * DEG_TO_RAD;
  const int y0 = static_cast<int>(floorf(cy - r - 1)), y1 = static_cast<int>(ceilf(cy + r + 1));
  const float rOut2 = (r + 1) * (r + 1), rIn2 = ir > 1 ? (ir - 1) * (ir - 1) : 0;
  for (int y = y0; y <= y1; ++y) {
    const float dy = y - cy;
    if (half < 90 && dy > 0.5f) break;  // an upward fan: nothing below the centre
    const float span = sqrtf(max(0.0f, rOut2 - dy * dy));
    for (int x = static_cast<int>(floorf(cx - span)); x <= static_cast<int>(ceilf(cx + span)); ++x) {
      const float dx = x - cx, d2 = dx * dx + dy * dy;
      if (d2 > rOut2 || d2 < rIn2) continue;
      const float d = sqrtf(d2);
      float cov = constrain(0.5f + min(r - d, d - ir), 0.0f, 1.0f);
      if (half < 180 && cov > 0) {
        const float a = fabsf(atan2f(dx, -dy));  // from straight up
        cov *= constrain(0.5f + (halfRad - a) * d, 0.0f, 1.0f);
      }
      if (cov <= 0.02f) continue;
      g.drawPixel(x, y, theme::mix(g.readPixel(x, y), color, static_cast<uint8_t>(cov * 255)));
    }
  }
}

// Room loadFont() needs for its glyph tables; TFT_eSPI doesn't check its mallocs.
constexpr uint32_t kFontHeapBytes = 6 * 1024;

// Draws one text line through a temporary full-width sprite (no flicker).
// Returns false (drawing nothing) when the heap can't spare the sprite.
template <typename Fn>
bool drawLine(int16_t top, int16_t height, const uint8_t* font, Fn&& draw) {
  TFT_eSprite line(&tft);
  line.setColorDepth(16);
  if (!line.createSprite(kScreenW, height)) return false;
  if (ESP.getMaxAllocHeap() < kFontHeapBytes) {
    line.deleteSprite();
    return false;
  }
  fillBackdrop(line, 0, top);
  line.loadFont(font);
  draw(line);
  line.unloadFont();
  line.pushSprite(0, top);
  line.deleteSprite();
  return true;
}

// Like drawLine, for tall blocks: drawn in bands of at most kBandH rows so a
// screen never needs one big sprite (an 88-row line is 56 KB) while HTTPS
// runs. `draw(s, y0)` draws the whole block shifted up by y0; the sprite clips.
constexpr int16_t kBandH = 30;
template <typename Fn>
bool drawBlockAt(int16_t x, int16_t top, int16_t width, int16_t height, const uint8_t* font,
                 Fn&& draw) {
  const int16_t bands = (height + kBandH - 1) / kBandH;
  const int16_t bandH = (height + bands - 1) / bands;
  TFT_eSprite band(&tft);
  band.setColorDepth(16);
  if (!band.createSprite(width, bandH)) return false;
  for (int16_t y0 = 0; y0 < height; y0 += bandH) {
    if (ESP.getMaxAllocHeap() < kFontHeapBytes) return false;  // the sprite frees itself
    fillBackdrop(band, x, top + y0);
    band.loadFont(font);
    draw(band, y0);
    band.unloadFont();
    band.pushSprite(x, top + y0, 0, 0, width, min<int16_t>(bandH, height - y0));
  }
  band.deleteSprite();
  return true;
}

template <typename Fn>
bool drawBlock(int16_t top, int16_t height, const uint8_t* font, Fn&& draw) {
  return drawBlockAt(0, top, kScreenW, height, font, draw);
}

// Fills a sprite whose top-left is at screen (sx, sy) with the background
// theme (or the plain background when there's none).
void fillBackdrop(TFT_eSprite& s, int16_t sx, int16_t sy) {
  if (!backdrop::active()) {
    s.fillSprite(C->bg);
    return;
  }
  uint16_t* px = static_cast<uint16_t*>(s.getPointer());
  const int16_t w = s.width(), h = s.height();
  for (int16_t y = 0; y < h; ++y) faceRow(sy + y, sx, w, px + y * w);
}

// Paints the background theme straight to a screen rectangle, in bands.
void paintBackdrop(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!backdrop::active()) {
    tft.fillRect(x, y, w, h, C->bg);
    return;
  }
  const int16_t bandH = min<int16_t>(h, kBandH);
  TFT_eSprite band(&tft);
  band.setColorDepth(16);
  if (!band.createSprite(w, bandH)) {
    tft.fillRect(x, y, w, h, C->bg);
    return;
  }
  for (int16_t y0 = 0; y0 < h; y0 += bandH) {
    fillBackdrop(band, x, y + y0);
    band.pushSprite(x, y + y0, 0, 0, w, min<int16_t>(bandH, h - y0));
  }
  band.deleteSprite();
}

void drawWifiIcon(TFT_eSprite& s, int16_t cx, int16_t baseY, uint8_t bars, uint16_t on,
                  uint16_t off) {
  // Three arcs and a dot fanning upwards; TFT_eSPI arc angles start at 6 o'clock.
  const uint8_t radii[] = {13, 9, 5};
  for (int i = 0; i < 3; ++i) {
    uint16_t c = (3 - i) <= bars ? on : off;
    if (themed()) {
      blendArc(s, cx, baseY, radii[i], radii[i] - 2, 45, c);  // (round ends: close enough)
    } else {
      s.drawSmoothArc(cx, baseY, radii[i], radii[i] - 2, 135, 225, c, C->bg, true);
    }
  }
  s.fillSmoothCircle(cx, baseY - 1, 1, bars > 0 ? on : off, aaBg());
}

// US AQI category colours (EPA), the dark ones lifted to read on the background.
uint16_t aqiColor(int aqi) {
  if (aqi <= 50) return theme::rgb(0, 228, 0);
  if (aqi <= 100) return theme::rgb(255, 230, 0);
  if (aqi <= 150) return theme::rgb(255, 126, 0);
  if (aqi <= 200) return theme::rgb(255, 40, 40);
  if (aqi <= 300) return theme::rgb(190, 110, 255);
  return theme::rgb(220, 40, 90);
}

// The moon at phase `angle` (0 new, 90 first quarter, 180 full, 270 last
// quarter), lit from the right while waxing, with smooth edges. The dark part
// keeps a faint "earthshine" so the disc stays visible.
void drawMoon(TFT_eSprite& s, int16_t cx, int16_t cy, float r, float angle, uint16_t lit,
              uint16_t bg, bool over = false) {  // over: blend with the sprite's pixels
  const uint16_t dark = theme::mix(bg, lit, 50);
  const float t = cosf(angle * DEG_TO_RAD);
  const bool waxing = angle < 180;
  const int16_t n = static_cast<int16_t>(r) + 1;
  for (int16_t dy = -n; dy <= n; ++dy) {
    const float w = sqrtf(max(r * r - dy * dy, 0.0f));
    const float term = w * t;  // the terminator's x on this row
    for (int16_t dx = -n; dx <= n; ++dx) {
      const float disc = constrain(r + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
      if (disc <= 0) continue;
      const float side = waxing ? dx - term : -dx - term;
      const uint16_t c = theme::mix(dark, lit, static_cast<uint8_t>(constrain(side + 0.5f, 0.0f, 1.0f) * 255));
      const uint16_t under = over ? s.readPixel(cx + dx, cy + dy) : bg;
      s.drawPixel(cx + dx, cy + dy, theme::mix(under, c, static_cast<uint8_t>(disc * 255)));
    }
  }
}

uint16_t moonColor(bool night) {
  const uint16_t c = theme::rgb(255, 236, 170);
  return night ? theme::mix(theme::kNightColors.bg, c, 150) : c;
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
  title_ = nullptr;
  dialShownKey_ = -1;  // the screen is cleared below
  if (photoCorner_ && mode != Mode::Photo) {
    free(photoCorner_);
    photoCorner_ = nullptr;
  }
  syncFxBuffers();
  barBgKey_ = -1;
  freeBannerBg();
  if (mode == Mode::Clock && backdrop_ != 0) {
    // The background theme, made for this face's colours and the night.
    const theme::Palette p = lastLocal_.tm_year ? basePalette(lastLocal_, millis()) : pal_;
    dimDial = analog_;
    if (!backdrop::setup(static_cast<backdrop::Theme>(backdrop_), C->bg, p.top, p.bottom, night_, backdropSeed_,
                         backdropPhoto_)) {
      Serial.printf("[THEME] %s unavailable (memory or photo)\n", backdrop::title(static_cast<backdrop::Theme>(backdrop_)));
    }
    paintBackdrop(0, 0, kScreenW, kScreenH);
  } else {
    backdrop::end();
    tft.fillScreen(C->bg);
  }
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
      // The field holds still while the network borrows the animation memory.
      if (!fxLeased_) refreshField(now);
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
  const bool restyle = backdrop_ != 0 && colors.style != colors_.style && mode_ == Mode::Clock;
  colors_ = colors;
  if (restyle) mode_ = Mode::None;  // the theme takes the new colours: redraw the face
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
  if (analog_) {
    // The dial's ring and hands use the palette; the column only when it
    // changes by the minute (animated styles keep its first colours).
    analogFull_ = true;
    if (!firstFrame && (night_ || !theme::styleAnimated(colors_.style))) drawAnalogColumn();
    return;
  }
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
  if (analog_) {
    const bool first = mode_ != Mode::Clock;
    enterMode(Mode::Clock);
    const uint32_t now = millis();
    if (first || local.tm_sec != lastLocal_.tm_sec) secondStartMs_ = now;
    lastLocal_ = local;
    setPalette(basePalette(local, now));
    if (first) {
      analogFull_ = true;
      drawAnalogColumn();
      drawNotice();
    }
    return;
  }
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
    paintBackdrop(0, kTimeTop, kScreenW, kDigitH);
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

  const int view = dateViewAt(local);
  if (local.tm_yday != lastYday_ || view != dateView_) {
    dateView_ = view;
    drawDate(local);
    lastYday_ = local.tm_yday;
  }
  if (firstFrame) {
    drawColon();
    drawStatusBar();
    drawPhraseLine();
    drawNotice();
  }
}

void DisplayManager::tick() {
  if (mem_lease::requested() != fxLeased_) {
    // The network borrows the animation memory: hold the frame meanwhile and
    // repaint the time with the effects when the memory is back.
    syncFxBuffers();
    lastPhaseMs_ = millis();  // colours carry on from where they held
    if (analog_) {
      analogFull_ = true;
    } else if (!fxLeased_ && mode_ == Mode::Clock && palValid_) {
      for (int i = 0; i < 4; ++i) {
        if (!bigRoll_[i].active) drawBigDigit(i, 1.0f);
      }
      if (!secAnimating_) drawSeconds(1.0f);
      drawColon();
      drawDate(lastLocal_);
    }
  }
  if (mode_ == Mode::Home && homeAnim_) tickHome(millis());
  if (mode_ != Mode::Clock || !spritesOk_) return;
  const uint32_t now = millis();
  if (now - lastFrameMs_ < kFrameMs) return;
  lastFrameMs_ = now;
  tickBackdrop(now);

  if (analog_) {
    if (!fxLeased_ && (!dialMask || !dialBandOk) && now - dialRetryMs >= 1000) syncFxBuffers();
    if (!fxLeased_ && dialMask && dialBandOk) {
      if (now - lastAnalogMs_ >= kAnalogFrameMs) {
        lastAnalogMs_ = now;
        analogFrame();
      }
    } else if (lastLocal_.tm_hour * 60 + lastLocal_.tm_min != dialShownKey_) {
      drawDialPlain();  // the dial isn't (fully) on the screen: never leave it blank
    }
    if (bannerActive_) drawBanner(now);
    return;
  }

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
  if ((animatedStyle || pulse || shining) && !fxLeased_ && now - lastAnimMs_ >= interval) {
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
  drawBar(false, false);  // the bar flows: redrawn whenever it grows 1/16 px

  if (bannerActive_) drawBanner(now);

  // The next phrase, or the next part of a long one.
  if (phraseCount_ && !fxLeased_ && (int32_t)(now - phraseNextMs_) >= 0) {
    if (phraseCount_ == 1 && phraseParts_ == 1) {
      phraseNextMs_ = now + kPhraseMinMs;  // nothing else to show
    } else {
      if (++phrasePart_ >= phraseParts_) {
        phrasePart_ = 0;
        phraseIdx_ = (phraseIdx_ + 1) % phraseCount_;
      }
      drawPhraseLine();
    }
  }

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
  // The length in 1/16 px: the end pixel is filled in part, so the bar flows
  // (~64 updates a second) instead of growing a pixel every 1/4 s.
  const int len = static_cast<int>(static_cast<int64_t>(lastLocal_.tm_sec * 1000 + intoSecond) * w * 16 / 60000);
  const int px = len >> 4;
  const uint8_t part = (len & 15) * 17;  // 0..255
  if (barPx_ < 0) full = true;
  if (!full && !recolor && len == barPx_) return;
  if (full) {
    // Only the strip the bar doesn't cover: AM/PM in 12-hour mode.
    if (barX() > kBarX) paintBackdrop(kBarX, kBarY, barX() - kBarX, kBarH);
    drawMeridiem();
  }
  barPx_ = len;
  uint16_t* buf = static_cast<uint16_t*>(barSprite.getPointer());
  if (backdrop::active()) {
    // Over a theme the track is a tint of what's under it (kept: the bar is
    // redrawn ~55 times a second).
    if (full || barBgKey_ != barX()) {
      for (int y = 0; y < kBarH; ++y) faceRow(kBarY + y, barX(), w, barBg + y * kBarW);
      barBgKey_ = barX();
    }
    for (int y = 0; y < kBarH; ++y) {
      const uint16_t* under = barBg + y * kBarW;
      uint16_t* out = buf + y * kBarW;
      for (int x = 0; x < w; ++x) {
        const uint16_t trackRaw = theme::mix(swap16(under[x]), pal_.bottom, 70);
        out[x] = x < px ? swap16(barColor(x))
                 : x == px && part ? swap16(theme::mix(trackRaw, barColor(x), part))
                                   : swap16(trackRaw);
      }
    }
  } else {
    const uint16_t trackRaw = theme::mix(C->bg, pal_.bottom, 48);
    const uint16_t track = swap16(trackRaw);
    for (int x = 0; x < w; ++x) {  // sprites are byte-swapped
      buf[x] = x < px ? swap16(barColor(x)) : x == px && part ? swap16(theme::mix(trackRaw, barColor(x), part)) : track;
    }
    for (int y = 1; y < kBarH; ++y) memcpy(buf + y * kBarW, buf, w * sizeof(uint16_t));
  }
  barSprite.pushSprite(barX(), kBarY, 0, 0, w, kBarH);
}

// Bar column x (0..barW): a horizontal gradient of the palette, or the
// colour field's column when it varies across the screen.
uint16_t DisplayManager::barColor(int x) const {
  if (fieldUniform) return theme::mix(fieldTop[0], fieldBottom[0], x * 255 / barW());
  const int16_t sx = constrain(barX() + x, 0, kScreenW - 1);
  return theme::mix(fieldTop[sx], fieldBottom[sx], 128);
}


void DisplayManager::setDateAlts(const DateAlt* alts, int count, bool even) {
  count = constrain(count, 0, kMaxDateAlts);
  if (count == dateAltCount_ && even == dateAltEven_ &&
      memcmp(alts, dateAlts_, sizeof(DateAlt) * count) == 0) {
    return;
  }
  memcpy(dateAlts_, alts, sizeof(DateAlt) * count);
  dateAltCount_ = count;
  dateAltEven_ = even;
  dateView_ = -1;  // redraw on the next showTime()
  dateCovKey = -1;
}

// Which text the date line shows now, in 5 s slots: solar, solar, alt 1,
// alt 2..., or two slots each when even.
int DisplayManager::dateViewAt(const struct tm& local) const {
  if (dateAltCount_ == 0) return 0;
  const int slot = (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) / 5;
  if (dateAltEven_) return slot % (2 * (dateAltCount_ + 1)) / 2;
  const int i = slot % (dateAltCount_ + 2);
  return i < 2 ? 0 : i - 1;
}

// The date line in the clock's colours, or one of its alternative texts (the
// lunar date). Night mode keeps the quiet night look. Animated styles repaint
// it every colour frame from its kept shape (see syncFxBuffers).
void DisplayManager::drawDate(const struct tm& local) {
  auto layout = [&](TFT_eSprite& s, uint16_t weekdayColor, uint16_t restColor, uint16_t bg) {
    // Long weekday/month combinations (e.g. WEDNESDAY, September) don't fit:
    // shorten the month first, then the weekday.
    char weekday[sizeof(DateAlt::head)];
    char rest[48];
    int16_t w1 = 0;
    if (dateView_ > 0) {
      const DateAlt& a = dateAlts_[dateView_ - 1];
      strlcpy(weekday, a.head, sizeof(weekday));
      w1 = s.textWidth(weekday);
      const bool fits = w1 + s.textWidth(a.rest) <= kScreenW - 16;
      strlcpy(rest, fits ? a.rest : a.shortRest, sizeof(rest));
    }
    for (int level = 0; level < 3 && dateView_ <= 0; ++level) {
      strlcpy(weekday, kWeekdays[local.tm_wday % 7], level == 2 ? 4 : sizeof(weekday));
      snprintf(rest, sizeof(rest), " \xC2\xB7 %d %.*s %d", local.tm_mday, level >= 1 ? 3 : 12,
               kMonths[local.tm_mon % 12], local.tm_year + 1900);
      w1 = s.textWidth(weekday);
      if (w1 + s.textWidth(rest) <= kScreenW - 16) break;
    }
    const int16_t x = (kScreenW - w1 - s.textWidth(rest)) / 2;
    s.setTextDatum(L_BASELINE);
    if (bg == C->bg) ink(s, weekdayColor); else s.setTextColor(weekdayColor, bg);
    s.drawString(weekday, x, kDateBaseline);
    if (bg == C->bg) ink(s, restColor); else s.setTextColor(restColor, bg);
    s.drawString(rest, x + w1, kDateBaseline);
  };
  if (night_) {
    drawLine(kDateTop, kDateH, font_face,
             [&](TFT_eSprite& s) { layout(s, pal_.top, C->date, C->bg); });
    return;
  }
  const int16_t y0 = kDateBaseline - 19, y1 = kDateBaseline;  // gradient rows: the capitals
  const int32_t key = (local.tm_year * 400 + local.tm_yday) * 4 + max(dateView_, 0);
  if (!fxCache || !dateOk || key != dateCovKey) {
    // Lay the text out in a temporary full-height sprite.
    TFT_eSprite s(&tft);
    s.setColorDepth(16);
    if (!s.createSprite(kScreenW, kDateH)) return;
    if (ESP.getMaxAllocHeap() < kFontHeapBytes) return;  // the sprite frees itself
    s.loadFont(font_face);
    s.fillSprite(TFT_BLACK);
    layout(s, TFT_WHITE, TFT_WHITE, TFT_BLACK);
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
// Other screens free them: their tall lines need the room.
void DisplayManager::syncFxBuffers() {
  // Withdraw "released" before looking for a lease: a lease taken meanwhile
  // then waits for the next sync instead of overlapping our allocations.
  mem_lease::setReleased(false);
  fxLeased_ = mem_lease::requested();
  if (fxLeased_) freeBannerBg();  // (the banner then computes the theme each frame)
  const bool face = mode_ == Mode::Clock && !fxLeased_;
  const bool clock = face && !analog_;  // the digital face's buffers
  const bool dial = face && analog_;
  const bool fx = clock && effectsOn();
  const bool animated = clock && !night_ && theme::styleAnimated(colors_.style);
  const bool cache = animated || (fx && (colors_.effects & (theme::kFxShine | theme::kFxGlow)));
  const bool glow = fx && (colors_.effects & theme::kFxGlow);
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
  if (dial && !dialMask) {
    allocDialMask();
    dialMaskBuilt = false;
  } else if (!dial && dialMask) {
    freeDialMask();
  }
  if (dial && !dialBandOk) {
    dialBand.setColorDepth(16);
    dialBandOk = dialBand.createSprite(kDialD, kDialBandH) != nullptr;
  } else if (!dial && dialBandOk) {
    dialBand.deleteSprite();
    dialBandOk = false;
  }
  if (dial && dialMask && dialBandOk) dialMissLogged = false;
  if (dial && (!dialMask || !dialBandOk)) {
    // tick() tries again every second.
    dialRetryMs = millis();
    if (!dialMissLogged) Serial.printf("[DISPLAY] No memory for the analog dial yet (free %u, largest block %u)\n",
                  heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    dialMissLogged = true;
  }
  if (!dial) {
    analogFull_ = true;  // the dial is drawn whole when it's back
    handsValid_ = false;
  }
  mem_lease::setReleased(!fxCache && !dateOk && !glowA && !dialMask && !dialBandOk);
}

void DisplayManager::setStatus(const StatusInfo& status) {
  if (statusValid_ && memcmp(&status, &status_, sizeof(StatusInfo)) == 0) return;
  status_ = status;
  statusValid_ = true;
  drawStatusBar();
}

void DisplayManager::drawStatusBar() {
  if (mode_ != Mode::Clock || !statusValid_ || analog_) return;
  const StatusInfo& st = status_;
  statusValid_ = drawLine(0, kStatusH, font_small, [&](TFT_eSprite& s) {
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
    ink(s, st.wifi == StatusInfo::Wifi::Setup ? C->setup : C->dim);
    s.drawString(st.wifiText, 38, kStatusBaseline);

    int16_t x = kScreenW - 10;
    s.setTextDatum(R_BASELINE);
    if (st.hasTemp) {
      char temp[12];
      snprintf(temp, sizeof(temp), "%d\xC2\xB0" "C", st.tempC);
      ink(s, C->dim);
      s.drawString(temp, x, kStatusBaseline);
      x -= s.textWidth(temp) + 14;
    }
    if (st.aqi >= 0) {
      // A dot in the category colour; the number too once it's unhealthy for some.
      char aqi[12];
      snprintf(aqi, sizeof(aqi), "AQI %d", st.aqi);
      uint16_t c = aqiColor(st.aqi);
      if (night_) c = theme::mix(C->bg, c, 140);
      ink(s, st.aqi > 100 ? c : C->dim);
      s.drawString(aqi, x, kStatusBaseline);
      x -= s.textWidth(aqi) + 9;
      s.fillSmoothCircle(x, kStatusBaseline - 5, 4, c, aaBg());
      x -= 4 + 14;
    }
    ink(s, st.ntpWarn ? C->warn : C->dim);
    s.drawString(st.ntpText, x, kStatusBaseline);
    if (st.moonAngle >= 0) {
      x -= s.textWidth(st.ntpText) + 16;
      drawMoon(s, x, kStatusBaseline - 5, 6.5f, st.moonAngle, moonColor(night_), C->bg, themed());
    }
  });
}

namespace {

// Renders the dial's ticks, numerals and ring into dialMask, band by band:
// each ink is drawn in its own pure colour on black, so a pixel's strongest
// channel says which ink it is and how much it covers.
void buildDialMask() {
  constexpr uint16_t kInkMinute = TFT_BLUE, kInkHour = TFT_GREEN, kInkRing = TFT_RED;
  dialBand.loadFont(font_date);
  for (int16_t y0 = 0; y0 < kDialD; y0 += kDialBandH) {
    TFT_eSprite& s = dialBand;
    s.fillSprite(TFT_BLACK);
    const int16_t cy = kDialC - y0;
    s.fillSmoothCircle(kDialC, cy, kDialR, kInkRing, TFT_BLACK);
    s.fillSmoothCircle(kDialC, cy, kDialR - 2, TFT_BLACK, kInkRing);
    for (int i = 0; i < 60; ++i) {
      const float a = i * PI / 30, sx = sinf(a), cx = cosf(a);
      const bool hour = i % 5 == 0;
      const float r0 = kDialR - (hour ? 15 : 9), r1 = kDialR - 5;
      const float w = hour ? 1.7f : 0.7f;
      s.drawWedgeLine(kDialC + r0 * sx, cy - r0 * cx, kDialC + r1 * sx, cy - r1 * cx, w, w,
                      hour ? kInkHour : kInkMinute, TFT_BLACK);
    }
    s.setTextDatum(MC_DATUM);
    s.setTextColor(kInkHour, TFT_BLACK);
    for (int h = 1; h <= 12; ++h) {
      const float a = h * PI / 6;
      char n[4];
      snprintf(n, sizeof(n), "%d", h);
      s.drawString(n, kDialC + (kDialR - 30) * sinf(a), cy - (kDialR - 30) * cosf(a) + 1);
    }
    const uint16_t* px = static_cast<uint16_t*>(s.getPointer());
    for (int16_t r = 0; r < kDialBandH && y0 + r < kDialD; ++r) {
      for (int16_t c = 0; c < kDialD; ++c) {
        const uint16_t v = swap16(px[r * kDialD + c]);
        const int red = (v >> 11) * 255 / 31, green = ((v >> 5) & 0x3F) * 255 / 63,
                  blue = (v & 0x1F) * 255 / 31;
        const int top = max(red, max(green, blue));
        const uint8_t ink = top == red ? 2 : top == green ? 1 : 0;
        dialRow(y0 + r)[c] = static_cast<uint8_t>(((top * 15 + 127) / 255) << 4 | ink);
      }
    }
  }
  dialBand.unloadFont();
  dialMaskBuilt = true;
}

}  // namespace

void DisplayManager::setPhrases(const char* const* texts, int count) {
  count = constrain(count, 0, 2);
  bool same = count == phraseCount_;
  for (int i = 0; i < count && same; ++i) same = texts[i] == phrases_[i];
  if (same) return;
  for (int i = 0; i < count; ++i) phrases_[i] = texts[i];
  phraseCount_ = count;
  phraseIdx_ = phrasePart_ = 0;
  drawPhraseLine();
}

// The phrase line above the digits: the current phrase, or the current part
// of one too long for the line (split at spaces, with "…" where it continues).
void DisplayManager::drawPhraseLine() {
  if (mode_ != Mode::Clock || analog_) return;
  drawLine(kPhraseTop, kPhraseH, font_small, [&](TFT_eSprite& s) {
    if (!phraseCount_) return;
    const char* text = phrases_[phraseIdx_ % phraseCount_];
    constexpr int kMaxParts = 6;
    const char* starts[kMaxParts];
    int lens[kMaxParts];
    int parts = 0;
    const int16_t maxW = kScreenW - 40;  // room for the quote marks and "…"
    char buf[160];
    // Greedy split at spaces into parts no wider than `limit`.
    auto split = [&](int16_t limit) {
      parts = 0;
      const char* p = text;
      while (*p && parts < kMaxParts) {
        const char* start = p;
        const char* fit = nullptr;
        for (const char* q = p;;) {
          const char* space = strchr(q, ' ');
          const char* end = space ? space : q + strlen(q);
          const int len = min<int>(end - start, sizeof(buf) - 1);
          memcpy(buf, start, len);
          buf[len] = '\0';
          if (fit && s.textWidth(buf) > limit) break;
          fit = end;
          if (!space) break;
          q = space + 1;
        }
        starts[parts] = start;
        lens[parts++] = fit - start;
        p = fit;
        while (*p == ' ') ++p;
      }
    };
    split(maxW);
    if (parts > 1) {
      // Balance the parts: the narrowest limit that still needs no more of them.
      const int n = parts;
      for (int16_t limit = s.textWidth(text) / n; limit < maxW; limit += 6) {
        split(limit);
        if (parts <= n) break;
      }
      if (parts > n) split(maxW);
    }
    phraseParts_ = max(parts, 1);
    const int part = min(phrasePart_, phraseParts_ - 1);
    const bool last = part == phraseParts_ - 1;
    snprintf(buf, sizeof(buf), "%s%.*s%s", part == 0 ? "\xE2\x80\x9C" : "\xE2\x80\xA6", lens[part],
             starts[part], last ? "\xE2\x80\x9D" : "\xE2\x80\xA6");
    s.setTextDatum(MC_DATUM);
    ink(s, C->dim);
    s.drawString(buf, kScreenW / 2, kPhraseH / 2 + 1);
    int chars = 0;
    for (const char* c = buf; *c; ++c) chars += (*c & 0xC0) != 0x80;  // UTF-8 code points
    phraseNextMs_ = millis() + kPhraseMinMs + chars * kPhraseCharMs;
  });
}

void DisplayManager::setAnalog(bool analog) {
  if (analog == analog_) return;
  analog_ = analog;
  if (mode_ == Mode::Clock) mode_ = Mode::None;  // the next showTime() draws the whole face
}

void DisplayManager::setAnalogInfo(const AnalogInfo& info) {
  if (analogInfoValid_ && memcmp(&info, &analogInfo_, sizeof(AnalogInfo)) == 0) return;
  analogInfo_ = info;
  analogInfoValid_ = true;
  drawAnalogColumn();
}

void DisplayManager::drawDialPlain() {
  dialShownKey_ = lastLocal_.tm_hour * 60 + lastLocal_.tm_min;
  const int16_t cx = kDialX + kDialC, cy = kDialY + kDialC;
  // Over a theme: the background first, then the ring as an outline (the
  // shapes read the screen to blend their edges).
  paintBackdrop(kDialX, kDialY, kDialD, kDialD);
  if (themed()) {
    blendArc(tft, cx, cy, kDialR, kDialR - 2, 180, pal_.top);
  } else {
    tft.fillSmoothCircle(cx, cy, kDialR, pal_.top, C->bg);
    tft.fillSmoothCircle(cx, cy, kDialR - 2, C->bg, pal_.top);
  }
  for (int i = 0; i < 60; ++i) {
    const float a = i * PI / 30, sx = sinf(a), cs = cosf(a);
    const bool hour = i % 5 == 0;
    const float r0 = kDialR - (hour ? 15 : 9), r1 = kDialR - 5, w = hour ? 1.7f : 0.7f;
    tft.drawWedgeLine(cx + r0 * sx, cy - r0 * cs, cx + r1 * sx, cy - r1 * cs, w, w, hour ? C->text : C->dim, aaBg());
  }
  if (ESP.getMaxAllocHeap() >= kFontHeapBytes) {  // the numerals when the font fits
    tft.loadFont(font_date);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C->text, C->bg);
    for (int h = 1; h <= 12; ++h) {
      const float a = h * PI / 6;
      char n[4];
      snprintf(n, sizeof(n), "%d", h);
      const int16_t nx = cx + (kDialR - 30) * sinf(a), ny = cy - (kDialR - 30) * cosf(a) + 1;
      if (themed()) tft.setTextColor(C->text, theme::mix(backdrop::at(nx, ny), C->bg, 150));
      tft.drawString(n, nx, ny);
    }
    tft.unloadFont();
  }
  const float minute = lastLocal_.tm_min + lastLocal_.tm_sec / 60.0f;
  const float hour = lastLocal_.tm_hour % 12 + minute / 60;
  auto hand = [&](float turns, float len, float tail, float ar, float br) {
    const float a = turns * 2 * PI, sx = sinf(a), cs = cosf(a);
    tft.drawWedgeLine(cx - tail * sx, cy + tail * cs, cx + len * sx, cy - len * cs, ar, br, C->text, aaBg());
  };
  hand(hour / 12, 52, 10, 4.2f, 2.4f);
  hand(minute / 60, 80, 12, 3.2f, 1.6f);
  tft.fillSmoothCircle(cx, cy, 5, pal_.top, aaBg());
  tft.fillSmoothCircle(cx, cy, 2, C->bg, pal_.top);
  analogFull_ = true;  // the full dial, with the second hand, once the buffers are back
}

// One frame of the analog face: the hands where they are now (the second hand
// sweeps), redrawn only in the rectangle they moved through.
void DisplayManager::analogFrame() {
  if (mode_ != Mode::Clock || !dialMask || !dialBandOk) return;
  if (!dialMaskBuilt) {
    buildDialMask();
    analogFull_ = true;
  }
  const float frac = constrain((millis() - secondStartMs_) / 1000.0f, 0.0f, 0.999f);
  const float sec = lastLocal_.tm_sec + frac;
  const float minute = lastLocal_.tm_min + sec / 60;
  const float hour = lastLocal_.tm_hour % 12 + minute / 60;
  auto hand = [](float turns, float len, float tail, float ar, float br) {
    const float a = turns * 2 * PI, sx = sinf(a), cx = cosf(a);
    return Hand{kDialC - tail * sx, kDialC + tail * cx, kDialC + len * sx, kDialC - len * cx, ar, br};
  };
  const Hand next[3] = {hand(hour / 12, 52, 10, 4.2f, 2.4f), hand(minute / 60, 80, 12, 3.2f, 1.6f),
                        hand(sec / 60, 88, 20, 1.4f, 0.9f)};
  dialShownKey_ = lastLocal_.tm_hour * 60 + lastLocal_.tm_min;
  if (analogFull_ || !handsValid_) {
    drawDialRect(0, 0, kDialD, kDialD, next);
    memcpy(hands_, next, sizeof(hands_));
    handsValid_ = true;
    analogFull_ = false;
    return;
  }
  // The second hand's old and new places, the others' only when they moved
  // (they're drawn where they were otherwise), and the centre cap.
  float x0 = kDialC - 8, y0 = kDialC - 8, x1 = kDialC + 8, y1 = kDialC + 8;
  auto grow = [&](const Hand& h) {
    const float m = max(h.ar, h.br) + 2;
    x0 = min(x0, min(h.ax, h.bx) - m);
    y0 = min(y0, min(h.ay, h.by) - m);
    x1 = max(x1, max(h.ax, h.bx) + m);
    y1 = max(y1, max(h.ay, h.by) + m);
  };
  Hand drawn[3];
  for (int i = 0; i < 3; ++i) {
    const bool moved = i == 2 || fabsf(next[i].bx - hands_[i].bx) + fabsf(next[i].by - hands_[i].by) > 0.25f;
    drawn[i] = moved ? next[i] : hands_[i];
    if (moved) {
      grow(hands_[i]);
      grow(next[i]);
    }
  }
  const int16_t ix0 = constrain(static_cast<int16_t>(floorf(x0)), 0, kDialD);
  const int16_t iy0 = constrain(static_cast<int16_t>(floorf(y0)), 0, kDialD);
  const int16_t ix1 = constrain(static_cast<int16_t>(ceilf(x1)), 0, kDialD);
  const int16_t iy1 = constrain(static_cast<int16_t>(ceilf(y1)), 0, kDialD);
  if (ix1 > ix0 && iy1 > iy0) drawDialRect(ix0, iy0, ix1 - ix0, iy1 - iy0, drawn);
  memcpy(hands_, drawn, sizeof(hands_));
}

// Rebuilds dial rectangle (x0, y0, w, h) from the mask, draws the hands over
// it and pushes it, kDialBandH rows at a time.
void DisplayManager::drawDialRect(int16_t x0, int16_t y0, int16_t w, int16_t h, const Hand* hands) {
  const uint16_t inks[2] = {C->dim, C->text};
  const uint16_t bg = swap16(C->bg);
  const bool themedBg = backdrop::active();
  const uint16_t accent = pal_.top;
  uint16_t* buf = static_cast<uint16_t*>(dialBand.getPointer());
  for (int16_t yy = y0; yy < y0 + h; yy += kDialBandH) {
    const int16_t hh = min<int16_t>(kDialBandH, y0 + h - yy);
    for (int16_t r = 0; r < hh; ++r) {
      const int16_t my = yy + r;
      const uint16_t ring = theme::mix(pal_.top, pal_.bottom, my * 255 / kDialD);
      const uint8_t* m = dialRow(my) + x0;
      uint16_t* out = buf + r * kDialD;
      if (themedBg) {
        faceRow(kDialY + my, kDialX + x0, w, out);
        for (int16_t c = 0; c < w; ++c) {
          const uint8_t v = m[c];
          if (v >> 4) out[c] = swap16(theme::mix(swap16(out[c]), (v & 3) == 2 ? ring : inks[v & 1], (v >> 4) * 17));
        }
        continue;
      }
      for (int16_t c = 0; c < w; ++c) {
        const uint8_t v = m[c];
        out[c] = v >> 4 ? swap16(theme::mix(C->bg, (v & 3) == 2 ? ring : inks[v & 1], (v >> 4) * 17))
                        : bg;
      }
    }
    // The hands, shifted into the band; the sprite clips them. 0x00FFFFFF
    // blends their edges with what's already there.
    const float ox = -x0, oy = -yy;
    for (int i = 0; i < 3; ++i) {
      const Hand& hd = hands[i];
      dialBand.drawWedgeLine(hd.ax + ox, hd.ay + oy, hd.bx + ox, hd.by + oy, hd.ar, hd.br,
                             i == 2 ? accent : C->text, 0x00FFFFFF);
    }
    dialBand.fillSmoothCircle(kDialC - x0, kDialC - yy, 5, accent, aaBg());
    dialBand.fillSmoothCircle(kDialC - x0, kDialC - yy, 2, C->bg, accent);
    dialBand.pushSprite(kDialX + x0, kDialY + yy, 0, 0, w, hh);
  }
}

// The analog face's right column: weekday, the day in large digits, month and
// year, the lunar date, a holiday, and the moon and air quality.
void DisplayManager::drawAnalogColumn() {
  if (mode_ != Mode::Clock || !analog_ || !analogInfoValid_) return;
  const AnalogInfo& a = analogInfo_;
  drawBlockAt(kColX, 0, kColW, kDialD, font_date, [&](TFT_eSprite& s, int16_t y0) {
    const int16_t cx = kColW / 2;
    s.setTextWrap(false, false);  // (TFT_eSPI wraps at the sprite's edge: "Wednesda / y")
    s.setTextDatum(MC_DATUM);
    ink(s, pal_.top);
    if (s.textWidth(a.weekday) > kColW - 4) {  // Wednesday: the smaller font
      s.unloadFont();
      s.loadFont(font_small);
    }
    s.drawString(a.weekday, cx, 16 - y0);
    s.unloadFont();
    s.loadFont(font_temp);
    ink(s, C->text);
    s.drawString(a.day, cx, 56 - y0);
    s.unloadFont();
    s.loadFont(font_small);
    ink(s, C->dim);
    s.drawString(a.month, cx, 92 - y0);
    s.drawFastHLine(14, 106 - y0, kColW - 28, C->faint);
    // With a holiday the lunar lines move up to make room for two more.
    const int16_t ly = a.holiday[0] ? 118 : 124;
    if (a.lunar[0]) {
      ink(s, C->text);
      s.drawString(a.lunar, cx, ly - y0);
      ink(s, C->dim);
      s.drawString(a.lunarYear, cx, ly + 17 - y0);
    }
    if (a.holiday[0]) {
      // One line, or two split at the space nearest the middle.
      const int16_t hy = a.lunar[0] ? ly + 38 : 128;
      ink(s, pal_.top);
      if (s.textWidth(a.holiday) <= kColW - 6) {
        s.drawString(a.holiday, cx, hy + 8 - y0);
      } else {
        const int len = strlen(a.holiday);
        int cut = -1;
        for (int i = 0; i < len; ++i) {
          if (a.holiday[i] == ' ' && (cut < 0 || abs(i - len / 2) < abs(cut - len / 2))) cut = i;
        }
        char first[32];
        strlcpy(first, a.holiday, cut > 0 ? min<size_t>(sizeof(first), cut + 1) : sizeof(first));
        s.drawString(first, cx, hy - y0);
        if (cut > 0) s.drawString(a.holiday + cut + 1, cx, hy + 17 - y0);
      }
    }
    // The moon and the air quality share the last row, centred.
    s.setTextDatum(ML_DATUM);
    char lit[8], aqi[8];
    snprintf(lit, sizeof(lit), "%u%%", a.moonLit);
    snprintf(aqi, sizeof(aqi), "%d", a.aqi);
    const int16_t moonW = a.moonAngle >= 0 ? 18 + s.textWidth(lit) : 0;
    const int16_t aqiW = a.aqi >= 0 ? 10 + s.textWidth(aqi) : 0;
    int16_t x = (kColW - moonW - aqiW - (moonW && aqiW ? 8 : 0)) / 2;
    if (moonW) {
      drawMoon(s, x + 7, 196 - y0, 7.5f, a.moonAngle, moonColor(night_), C->bg, themed());
      ink(s, C->dim);
      s.drawString(lit, x + 18, 197 - y0);
      x += moonW + 8;
    }
    if (aqiW) {
      uint16_t c = aqiColor(a.aqi);
      if (night_) c = theme::mix(C->bg, c, 140);
      s.fillSmoothCircle(x + 3, 196 - y0, 3, c, aaBg());
      ink(s, a.aqi > 100 ? c : C->dim);
      s.drawString(aqi, x + 10, 197 - y0);
    }
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
    freeBannerBg();
  } else if (bannerActive_) {
    return;  // the banner draws this line itself
  }
  if (notice_[0] == '\0' && weather_.valid) {
    drawWeatherLine();
    return;
  }
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    ink(s, noticeKind_ == NoticeKind::Error ? C->error : C->setup);
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
  freeBannerBg();
  if (backdrop::active() && !fxLeased_) {
    bannerBg = static_cast<uint16_t*>(malloc(kScreenW * kNoticeH * sizeof(uint16_t)));
    if (bannerBg) {
      for (int16_t y = 0; y < kNoticeH; ++y) faceRow(kNoticeTop + y, 0, kScreenW, bannerBg + y * kScreenW);
    }
  }
  return true;
}

void DisplayManager::drawBanner(uint32_t now) {
  // Enters from the right edge and leaves fully on the left.
  const int32_t offset = (int32_t)((uint64_t)(now - bannerStartMs_) * bannerSpeed_ / 1000);
  const int32_t x = kScreenW - offset;
  if (x + bannerW_ < 0) {
    bannerActive_ = false;
    freeBannerBg();
    drawNotice();  // back to the weather line
    return;
  }
  bannerSprite.fillSprite(TFT_BLACK);
  bannerSprite.drawString(bannerText_, x, kNoticeH / 2);
  Paint paint;
  paint.screenY = kNoticeTop;
  paint.under = bannerBg;
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
        const uint16_t to = bannerBg ? swap16(bannerBg[y * kScreenW + xx])
                            : backdrop::active() ? backdrop::at(xx, kNoticeTop + y) : C->bg;
        row[xx] = swap16(blend256(c, to, a));
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

    // Over a theme the icon's cut-outs (the moon's crescent) take its colour there.
    const uint16_t iconBg = themed() ? backdrop::at(x + iconW / 2, kNoticeTop + kNoticeH / 2) : C->bg;
    weather_icons::draw(s, x + iconW / 2, kNoticeH / 2, 22, weather::iconFor(w.code), w.isDay, iconBg,
                        night_ || weatherStale_, C->dim);
    x += iconW + 4;
    s.setTextDatum(ML_DATUM);
    ink(s, weatherStale_ ? C->dim : C->date);
    s.drawString(detail, x + tempW + 10, kNoticeH / 2);
    s.unloadFont();
    s.loadFont(font_face);
    ink(s, weatherStale_ ? C->dim : pal_.top);
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
  fillBackdrop(s, kBarX, kRowY - 9);
  s.loadFont(font_small);
  ink(s, pal_.top);
  s.setTextDatum(ML_DATUM);
  s.drawString(pm_ ? "PM" : "AM", 0, 9);
  s.unloadFont();
  s.pushSprite(kBarX, kRowY - 9);
  s.deleteSprite();
}

constexpr int16_t kCalTodayW = 58, kCalTodayH = 22;  // the calendar's Today button

void DisplayManager::drawScreenTitle(const char* title) {
  title_ = title;
  drawLine(0, 28, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(ML_DATUM);
    s.setTextColor(pal_.top, C->bg);
    s.drawString(title, 10, 15);
    if (mode_ == Mode::Calendar) {
      // Today: lit while another month shows.
      calTodayX_ = 10 + s.textWidth(title) + 12;
      const int16_t ty = 15 - kCalTodayH / 2;
      if (calView_.away) {
        s.fillSmoothRoundRect(calTodayX_, ty, kCalTodayW, kCalTodayH, kCalTodayH / 2, pal_.top, C->bg);
      } else {
        s.drawSmoothRoundRect(calTodayX_, ty, kCalTodayH / 2, kCalTodayH / 2 - 1, kCalTodayW, kCalTodayH,
                              C->faint, C->bg);
      }
      s.setTextDatum(MC_DATUM);
      s.setTextColor(calView_.away ? C->bg : C->dim, calView_.away ? pal_.top : C->bg);
      s.drawString("Today", calTodayX_ + kCalTodayW / 2, 15);
    }
    s.setTextDatum(MR_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(nowText_, kScreenW - 10, 15);
  });
}

// The time pill in a photo's bottom-right corner: the photo darkened in a
// rounded rectangle, the time on it. The photo under it is kept (photoCorner_).
constexpr int16_t kPillW = 96, kPillH = 28;
constexpr int16_t kPillX = kScreenW - kPillW - 6, kPillY = kScreenH - kPillH - 6;

void DisplayManager::drawPhotoTime() {
  if (mode_ != Mode::Photo || !photoCorner_ || !nowText_[0]) return;
  TFT_eSprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(kPillW, kPillH)) return;
  s.loadFont(font_date);
  const int16_t w = s.textWidth(nowText_) + 18;
  const int16_t x0 = kPillW - w;
  constexpr int16_t r = kPillH / 2;
  for (int16_t y = 0; y < kPillH; ++y) {
    for (int16_t x = 0; x < kPillW; ++x) {
      uint16_t c = swap16(photoCorner_[y * kPillW + x]);  // readRect gives byte-swapped pixels
      // Inside the pill (a rounded rectangle at the right): darkened.
      const int16_t cx = constrain(x, x0 + r, kPillW - 1 - r);
      const float d = sqrtf((x - cx) * (x - cx) + (y - r + 0.5f) * (y - r + 0.5f));
      const float in = constrain(r - d, 0.0f, 1.0f);
      if (x >= x0 && in > 0) c = theme::mix(c, TFT_BLACK, static_cast<uint8_t>(in * 150));
      s.drawPixel(x, y, c);
    }
  }
  s.setTextDatum(MC_DATUM);
  s.setTextColor(TFT_WHITE);  // no background: blends with the pixels under it
  s.drawString(nowText_, x0 + w / 2, kPillH / 2 + 1);
  s.unloadFont();
  s.pushSprite(kPillX, kPillY);
  s.deleteSprite();
}

void DisplayManager::drawWeatherHeader() {
  if (mode_ != Mode::Weather) return;
  drawLine(0, 28, font_small, [&](TFT_eSprite& s) {
    s.setTextDatum(ML_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(wxPlace_, 10, 15);
    const uint32_t ageS = (millis() - wxAnchorMs_) / 1000;
    const bool stale = wxHasAge_ && ageS > config::kWeatherStaleS;
    s.setTextDatum(MR_DATUM);
    s.setTextColor(C->text, C->bg);
    s.drawString(nowText_, kScreenW - 10, 15);
    int16_t right = s.textWidth(nowText_) + 10;
    if (stale) {
      char age[24];
      snprintf(age, sizeof(age), "%lu h old \xC2\xB7 ", (unsigned long)(ageS / 3600));
      s.setTextColor(C->warn, C->bg);
      s.drawString(age, kScreenW - right, 15);
      right += s.textWidth(age);
    }
    // Tonight's moon and how much of it is lit, after the place (if there's room).
    const time_t now = time(nullptr);
    const int16_t mx = 10 + s.textWidth(wxPlace_) + 16;
    if (now > 1700000000 && mx + 48 < kScreenW - 6 - right) {
      const moon::Phase mp = moon::at(now);
      drawMoon(s, mx, 14, 7.5f, mp.angle, moonColor(night_), C->bg);
      char lit[8];
      snprintf(lit, sizeof(lit), "%d%%", static_cast<int>(lroundf(mp.lit * 100)));
      s.setTextDatum(ML_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString(lit, mx + 12, 15);
    }
  });
}

// The calendar: weekday names under the title bar, then the weeks.
// The week numbers in a narrow column on the left.
constexpr int16_t kCalHeadTop = 29, kCalHeadH = 17, kCalGridTop = kCalHeadTop + kCalHeadH;
constexpr int16_t kCalGridH = kScreenH - kCalGridTop - 1;
constexpr int16_t kCalWeekW = 24, kCalCellW = 42;
constexpr int16_t kCalX0 = (kScreenW - kCalWeekW - 7 * kCalCellW) / 2 + kCalWeekW;

constexpr int16_t kCalCardX = 8, kCalCardW = kScreenW - 2 * kCalCardX;

void DisplayManager::showCalendarScreen(const CalendarView& v) {
  if (mode_ != Mode::Calendar) {
    enterMode(Mode::Calendar);
    drawBlock(kCalHeadTop, kCalHeadH, font_small, [&](TFT_eSprite& s, int16_t y0) {
      static const char* const kDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
      s.setTextDatum(MC_DATUM);
      for (int c = 0; c < 7; ++c) {
        s.setTextColor(c == 6 ? C->error : C->dim, C->bg);
        s.drawString(kDays[c], kCalX0 + c * kCalCellW + kCalCellW / 2, 8 - y0);
      }
      s.setTextColor(theme::mix(C->faint, C->dim, 128), C->bg);
      s.drawString("Wk", kCalX0 - kCalWeekW / 2, 8 - y0);
    });
  }
  calView_ = v;  // (before the title: its Today button reads it)
  strlcpy(calTitle_, v.title, sizeof(calTitle_));
  drawScreenTitle(calTitle_);
  const int rows = constrain(v.rows, 4, 6);
  const int16_t rowH = kCalGridH / rows;
  // The card, if open: in the half of the grid away from the chosen day. The
  // weeks under it aren't drawn, so it sits on a clear background.
  calCardY_ = -1;
  const bool lines3 = v.card[2][0] != '\0';
  if (v.selected >= 0) {
    calCardH_ = lines3 ? 74 : 54;
    calCardY_ = v.selected / 7 < rows / 2 ? kScreenH - calCardH_ - 6 : kCalGridTop + 2;
  }
  auto underCard = [&](int row) {
    const int16_t y = kCalGridTop + row * rowH;
    return calCardY_ >= 0 && y < calCardY_ + calCardH_ + 2 && y + rowH > calCardY_ - 2;
  };
  // The day (digits ~15 px high) and the lunar date (~8 px) 4 px apart, centred in the row.
  const int16_t pad = (rowH - 27) / 2, dayBase = pad + 15, lunarBase = dayBase + 12;
  drawBlock(kCalGridTop, kCalGridH, font_date, [&](TFT_eSprite& s, int16_t y0) {
    s.setTextDatum(MC_DATUM);
    for (int pass = 0; pass < 2; ++pass) {  // the days in font_date, then the lunar dates in font_tiny
      if (pass == 1) {
        s.unloadFont();
        s.loadFont(font_tiny);
      }
      for (int i = 0; i < rows * 7; ++i) {
        const int16_t top = (i / 7) * rowH - y0;
        if (top >= kBandH || top + rowH <= 0 || underCard(i / 7)) continue;  // not in this band
        const CalendarCell& c = v.cells[i];
        const int16_t cx = kCalX0 + (i % 7) * kCalCellW + kCalCellW / 2;
        const bool in = c.flags & CalendarCell::kInMonth;
        const bool today = c.flags & CalendarCell::kToday;
        const uint16_t bg = today ? pal_.top : C->bg;
        if (pass == 0) {
          // Today's box and the chosen day's ring: around the two numbers.
          const int16_t by = top + max(0, pad - 3), bh = min<int16_t>(rowH, 33);
          if (today) {
            s.fillSmoothRoundRect(cx - 19, by, 38, bh, 9, pal_.top, C->bg);
          } else if (i == v.selected) {
            s.drawSmoothRoundRect(cx - 19, by, 9, 8, 38, bh, pal_.top, C->bg);
          }
          if (in && (c.flags & CalendarCell::kHoliday)) {
            s.fillSmoothCircle(cx + 14, top + pad + 3, 2, today ? C->bg : C->warn, bg);
          }
          char n[4];
          snprintf(n, sizeof(n), "%u", c.day);
          const uint16_t ink = today                                                   ? C->bg
                               : !in                                                   ? C->faint
                               : (c.flags & CalendarCell::kHoliday) || i % 7 == 6      ? C->error
                                                                                       : C->text;
          s.setTextColor(ink, bg);
          s.setTextDatum(C_BASELINE);  // by the baselines: a clear gap even in 6-week months
          s.drawString(n, cx, top + dayBase);
        } else {
          char n[8];
          const bool first = c.flags & CalendarCell::kLunarFirst;
          if (first) {
            snprintf(n, sizeof(n), "%u/%u", c.lunarDay, c.lunarMonth);
          } else {
            snprintf(n, sizeof(n), "%u", c.lunarDay);
          }
          const uint16_t ink = today ? theme::mix(pal_.top, C->bg, 150)
                               : !in ? C->faint
                               : first ? pal_.top
                                       : C->dim;
          s.setTextColor(ink, bg);
          s.setTextDatum(C_BASELINE);
          s.drawString(n, cx, top + lunarBase);
          if (i % 7 == 0) {  // the week number
            snprintf(n, sizeof(n), "%u", v.weeks[i / 7]);
            s.setTextColor(i / 7 == v.todayRow ? pal_.top : theme::mix(C->faint, C->dim, 128), C->bg);
            s.drawString(n, kCalX0 - kCalWeekW / 2, top + (dayBase + lunarBase) / 2 - 2);
          }
        }
      }
    }
  });
  if (calCardY_ < 0) return;
  drawBlockAt(kCalCardX, calCardY_, kCalCardW, calCardH_, font_small, [&](TFT_eSprite& s, int16_t y0) {
    s.fillSmoothRoundRect(0, -y0, kCalCardW, calCardH_, 12, C->faint, C->bg);
    s.drawSmoothRoundRect(0, -y0, 12, 11, kCalCardW - 1, calCardH_ - 1, pal_.top, C->faint);
    s.setTextDatum(ML_DATUM);
    const uint16_t inks[3] = {C->text, C->dim, C->warn};
    for (int k = 0; k < (lines3 ? 3 : 2); ++k) {
      s.setTextColor(inks[k], C->faint);
      s.drawString(v.card[k], 14, 17 + k * 20 - y0);
    }
  });
}

int DisplayManager::calendarHitTest(int16_t x, int16_t y) const {
  if (mode_ != Mode::Calendar) return 0;
  if (y < kCalHeadTop) return kCalendarToday;  // the button, or anywhere on the title
  if (calCardY_ >= 0 && y >= calCardY_ && y < calCardY_ + calCardH_) return 0;
  const int rows = constrain(calView_.rows, 4, 6);
  const int16_t rowH = kCalGridH / rows;
  if (y < kCalGridTop || x < kCalX0 || x >= kCalX0 + 7 * kCalCellW) return 0;
  const int row = (y - kCalGridTop) / rowH, col = (x - kCalX0) / kCalCellW;
  if (row >= rows) return 0;
  const CalendarCell& c = calView_.cells[row * 7 + col];
  return c.flags & CalendarCell::kInMonth ? c.day : 0;
}

// Home screen tiles: 2 columns x 3 rows under the title bar.
constexpr int16_t kTileW = 148, kTileH = 62, kTileTop = 34, kTileGapY = 6;
constexpr int16_t kTileX[2] = {8, kScreenW - 8 - kTileW};
constexpr int16_t kTileRowY = 47;  // the centre of the icon, state and switch row
constexpr int16_t kTrackW = 40, kTrackH = 22, kTrackX = kTileW - 12 - kTrackW;
constexpr int16_t kTrackY = kTileRowY - kTrackH / 2;

void DisplayManager::showHomeScreen(const HomeView& v) {
  const bool first = mode_ != Mode::Home;
  if (first) {
    enterMode(Mode::Home);
    homeValid_ = false;
    drawScreenTitle("Home");
  }
  if (v.count == 0) {
    drawBlock(kTileTop, kScreenH - kTileTop, font_small, [&](TFT_eSprite& s, int16_t y0) {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(C->dim, C->bg);
      s.drawString("Choose devices on the web page", kScreenW / 2, 90 - y0);
      s.drawString("(Home Assistant section)", kScreenW / 2, 112 - y0);
    });
    homeView_ = v;
    homeValid_ = true;
    return;
  }
  if (homeValid_ && homeView_.count == 0) {
    drawBlock(kTileTop, kScreenH - kTileTop, font_small, [](TFT_eSprite&, int16_t) {});  // the message
    homeValid_ = false;
  }
  using Look = HomeTile::Look;
  auto isSwitch = [](const HomeTile& t) {
    return t.icon != HomeTile::Icon::Scene && (t.look == Look::On || t.look == Look::Off);
  };
  for (int i = 0; i < HomeView::kMax; ++i) {
    const bool has = i < v.count;
    const bool had = homeValid_ && i < homeView_.count;
    if (homeValid_ && has == had && (!has || memcmp(&v.tiles[i], &homeView_.tiles[i], sizeof(HomeTile)) == 0)) {
      continue;
    }
    const float to = has && v.tiles[i].look == Look::On ? 1 : 0;
    if (has && had && isSwitch(v.tiles[i]) && isSwitch(homeView_.tiles[i]) &&
        !strcmp(v.tiles[i].name, homeView_.tiles[i].name)) {
      // The same switch flipped: slide the knob there (tickHome draws it).
      if (to != homeTo_[i]) {
        homeFrom_[i] = homeKnob_[i];
        homeTo_[i] = to;
        homeAnimMs_[i] = millis() + 30;  // after the tile below is drawn
        homeAnim_ |= 1 << i;
      }
    } else {
      homeAnim_ &= ~(1 << i);
      homeKnob_[i] = homeTo_[i] = to;
    }
    drawHomeTile(i, has ? &v.tiles[i] : nullptr, homeKnob_[i]);
  }
  homeView_ = v;
  homeValid_ = true;
}

// Slides the knobs that are moving (about 60 fps, eased in and out).
void DisplayManager::tickHome(uint32_t now) {
  constexpr uint32_t kSlideMs = 220;
  if (now - lastHomeFrameMs_ < 16) return;
  lastHomeFrameMs_ = now;
  for (int i = 0; i < HomeView::kMax; ++i) {
    if (!(homeAnim_ & (1 << i))) continue;
    if (static_cast<int32_t>(now - homeAnimMs_[i]) < 0) continue;
    float t = (now - homeAnimMs_[i]) / static_cast<float>(kSlideMs);
    if (t >= 1) {
      t = 1;
      homeAnim_ &= ~(1 << i);
    }
    const float e = t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
    homeKnob_[i] = homeFrom_[i] + (homeTo_[i] - homeFrom_[i]) * e;
    drawHomeKnob(i, homeKnob_[i]);
  }
}

namespace {

// `text` cut to `w` pixels with an ellipsis.
String fitText(TFT_eSprite& s, const char* text, int w) {
  String t = text;
  if (s.textWidth(t) <= w) return t;
  while (t.length() > 1 && s.textWidth(t + "\xE2\x80\xA6") > w) {
    t.remove(t.length() - 1);
    while (t.length() > 1 && (t[t.length() - 1] & 0xC0) == 0x80) t.remove(t.length() - 1);  // whole UTF-8
    if ((t[t.length() - 1] & 0xC0) == 0xC0) t.remove(t.length() - 1);
  }
  t.trim();
  return t + "\xE2\x80\xA6";
}

// A device icon of about 18 px around (cx, cy).
void drawHomeIcon(TFT_eSprite& s, HomeTile::Icon icon, float cx, float cy, uint16_t ink, uint16_t bg) {
  using Icon = HomeTile::Icon;
  switch (icon) {
    case Icon::Light:  // a bulb
      s.fillSmoothCircle(cx, cy - 2, 6, ink, bg);
      s.drawWideLine(cx - 3, cy + 5, cx + 3, cy + 5, 2, ink, bg);
      s.drawWideLine(cx - 2, cy + 8, cx + 2, cy + 8, 1.6f, ink, bg);
      break;
    case Icon::Power:  // the power symbol (TFT_eSPI arcs: 0 degrees at the bottom)
      s.drawSmoothArc(cx, cy + 1, 8, 6, 0, 145, ink, bg, true);
      s.drawSmoothArc(cx, cy + 1, 8, 6, 215, 360, ink, bg, true);
      s.drawWideLine(cx, cy - 8, cx, cy, 2, ink, bg);
      break;
    case Icon::Fan:  // three blades
      for (int k = 0; k < 3; ++k) {
        const float a = k * 2 * PI / 3 + 0.3f;
        s.drawWedgeLine(cx, cy, cx + 8 * sinf(a), cy - 8 * cosf(a), 1.2f, 3.6f, ink, bg);
      }
      s.fillSmoothCircle(cx, cy, 2, bg, ink);
      break;
    case Icon::Climate:  // a snowflake
      for (int k = 0; k < 3; ++k) {
        const float a = k * PI / 3, dx = 8 * sinf(a), dy = 8 * cosf(a);
        s.drawWideLine(cx - dx, cy - dy, cx + dx, cy + dy, 1.8f, ink, bg);
      }
      break;
    case Icon::Tv:  // a screen on a stand
      s.drawSmoothRoundRect(cx - 9, cy - 8, 3, 1, 18, 12, ink, bg);
      s.drawWideLine(cx - 4, cy + 8, cx + 4, cy + 8, 1.8f, ink, bg);
      break;
    case Icon::Scene:  // a sparkle
      for (int k = 0; k < 4; ++k) {
        const float a = k * PI / 2;
        s.drawWedgeLine(cx, cy, cx + 9 * sinf(a), cy - 9 * cosf(a), 2.6f, 0.6f, ink, bg);
      }
      break;
  }
}

}  // namespace

// One tile: the name at the top; the icon, state and a switch (or a run
// button) along the bottom. `knob` 0..1 blends off (knob left on a dark
// track) into on (tinted card, accent track, knob right) as it slides.
void DisplayManager::drawHomeTile(int i, const HomeTile* t, float knob) {
  const int16_t x = kTileX[i % 2];
  const int16_t y = kTileTop + (i / 2) * (kTileH + kTileGapY);
  drawBlockAt(x, y, kTileW, kTileH, font_small, [&](TFT_eSprite& s, int16_t y0) {
    if (!t) return;
    using Look = HomeTile::Look;
    const bool run = t->icon == HomeTile::Icon::Scene;
    const bool unavailable = t->look == Look::Unavailable;
    // The card and text take the new state at once; the knob slides (tickHome).
    const uint8_t p = !run && t->look == Look::On ? 255 : 0;
    const uint16_t card = homeCard(*t);
    s.fillSmoothRoundRect(0, -y0, kTileW, kTileH, 12, card, C->bg);
    if (unavailable) s.drawSmoothRoundRect(0, -y0, 12, 11, kTileW - 1, kTileH - 1, C->faint, card);

    // The name: one line, or two split at the space nearest the middle.
    s.setTextDatum(ML_DATUM);
    s.setTextColor(unavailable ? C->dim : C->text, card);
    constexpr int kTextX = 12, kTextW = kTileW - 2 * kTextX;
    if (s.textWidth(t->name) <= kTextW) {
      s.drawString(t->name, kTextX, 18 - y0);
    } else {
      const int len = strlen(t->name);
      int cut = -1;
      for (int k = 0; k < len; ++k) {
        if (t->name[k] == ' ' && (cut < 0 || abs(k - len / 2) < abs(cut - len / 2))) cut = k;
      }
      char first[32];
      strlcpy(first, t->name, cut > 0 ? min<size_t>(sizeof(first), cut + 1) : sizeof(first));
      s.drawString(fitText(s, first, kTextW), kTextX, 12 - y0);
      if (cut > 0) s.drawString(fitText(s, t->name + cut + 1, kTextW), kTextX, 28 - y0);
    }

    // The bottom row.
    constexpr float kRowY = kTileRowY;
    const uint16_t ink = theme::mix(C->dim, pal_.top, p);
    drawHomeIcon(s, t->icon, 21, kRowY - y0, ink, card);
    s.setTextColor(ink, card);
    const int16_t textEnd = unavailable ? kTileW - 12 : run ? kTileW - 12 - kTrackH - 4 : kTrackX - 6;
    s.drawString(fitText(s, t->state, textEnd - 36), 36, kRowY - y0);
    if (unavailable) return;
    const int16_t ty = kRowY - kTrackH / 2 - y0;
    if (run) {
      // A round play button.
      const float bx = kTrackX + kTrackW - kTrackH / 2, by = kRowY - y0;
      const bool busy = t->look == Look::Pending;
      s.fillSmoothCircle(bx, by, kTrackH / 2, busy ? theme::mix(card, pal_.top, 110) : pal_.top, card);
      const uint16_t tri = busy ? pal_.top : card;
      s.fillTriangle(bx - 3, by - 6, bx - 3, by + 6, bx + 6, by, tri);
      return;
    }
    drawHomeSwitch(s, kTrackX, ty, knob, card);
  });
}

uint16_t DisplayManager::homeCard(const HomeTile& t) const {
  if (t.look == HomeTile::Look::Unavailable) return C->bg;
  const bool on = t.icon != HomeTile::Icon::Scene && t.look == HomeTile::Look::On;
  return on ? theme::mix(C->faint, pal_.top, 56) : C->faint;
}

// The switch at (x, y): track and knob, `knob` 0 (off, left) .. 1 (on, right).
void DisplayManager::drawHomeSwitch(TFT_eSprite& s, int16_t x, int16_t y, float knob, uint16_t card) {
  const uint8_t p = static_cast<uint8_t>(constrain(knob, 0.0f, 1.0f) * 255 + 0.5f);
  const uint16_t trackOff = theme::mix(C->bg, C->faint, 110);
  const uint16_t track = theme::mix(trackOff, pal_.top, p);
  s.fillSmoothRoundRect(x, y, kTrackW, kTrackH, kTrackH / 2, track, card);
  const float r = kTrackH / 2 - 3, cy = y + kTrackH / 2.0f;
  const float left = x + kTrackH / 2.0f, right = x + kTrackW - kTrackH / 2.0f;
  const float kx = left + (right - left) * p / 255.0f;
  const uint16_t knobInk = theme::mix(theme::mix(C->dim, C->text, 140), night_ ? C->text : theme::rgb(255, 255, 255), p);
  s.fillSmoothCircle(kx, cy + 1, r, theme::mix(track, 0, 90), track);  // a soft shadow
  s.fillSmoothCircle(kx, cy, r, knobInk, track);
}

// One frame of a sliding knob: only the switch's rectangle (no font, ~2 ms).
void DisplayManager::drawHomeKnob(int i, float knob) {
  if (i >= homeView_.count) return;
  TFT_eSprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(kTrackW, kTrackH + 1)) return;
  const uint16_t card = homeCard(homeView_.tiles[i]);
  s.fillSprite(card);
  drawHomeSwitch(s, 0, 0, knob, card);
  s.pushSprite(kTileX[i % 2] + kTrackX, kTileTop + (i / 2) * (kTileH + kTileGapY) + kTrackY);
  s.deleteSprite();
}

int DisplayManager::homeHitTest(int16_t x, int16_t y) const {
  if (mode_ != Mode::Home || y < kTileTop - 4) return -1;
  const int row = (y - kTileTop + kTileGapY / 2) / (kTileH + kTileGapY);
  const int col = x < kScreenW / 2 ? 0 : 1;
  const int i = min(row, 2) * 2 + col;
  return homeValid_ && i < homeView_.count ? i : -1;
}

void DisplayManager::setNow(const struct tm& local) {
  const int key = local.tm_hour * 60 + local.tm_min + (hour12_ ? 10000 : 0);
  if (key == nowKey_) return;
  nowKey_ = key;
  if (hour12_) {
    const int h = local.tm_hour % 12 ? local.tm_hour % 12 : 12;
    snprintf(nowText_, sizeof(nowText_), "%d:%02d %s", h, local.tm_min, local.tm_hour < 12 ? "AM" : "PM");
  } else {
    snprintf(nowText_, sizeof(nowText_), "%02d:%02d", local.tm_hour, local.tm_min);
  }
  switch (mode_) {
    case Mode::Info:
    case Mode::Settings:
    case Mode::Storage:
    case Mode::Update:
    case Mode::Prompt:
    case Mode::Confirm:
    case Mode::Home:
    case Mode::Calendar:
      if (title_) drawScreenTitle(title_);
      break;
    case Mode::Weather:
      drawWeatherHeader();
      break;
    case Mode::Photo:
      drawPhotoTime();
      break;
    default:
      break;
  }
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
        const bool lit = b == 2 && v.updateAvailable;
        const uint16_t fill = lit ? pal_.top : C->faint;
        s.fillSmoothRoundRect(x, 0, 96, kInfoBtnH, 8, fill, C->bg);
        s.setTextDatum(MC_DATUM);
        s.setTextColor(lit ? theme::rgb(0, 0, 0) : C->text, fill);
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
  drawBlock(kConfirmBtnY, kConfirmBtnH, font_date, [&](TFT_eSprite& s, int16_t y0) {
    s.fillSmoothRoundRect(10, -y0, 140, kConfirmBtnH, 12, C->faint, C->bg);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->faint);
    s.drawString("Back", 80, kConfirmBtnH / 2 + 1 - y0);
    if (v.action[0]) {
      const uint16_t fill = v.actionIsInstall ? pal_.top : C->faint;
      s.fillSmoothRoundRect(170, -y0, 140, kConfirmBtnH, 12, fill, C->bg);
      s.setTextColor(v.actionIsInstall ? theme::rgb(0, 0, 0) : C->text, fill);
      s.drawString(v.action, 240, kConfirmBtnH / 2 + 1 - y0);
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
  drawBlock(kConfirmBtnY, kConfirmBtnH, font_date, [&](TFT_eSprite& s, int16_t y0) {
    s.fillSmoothRoundRect(10, -y0, 140, kConfirmBtnH, 12, C->faint, C->bg);
    s.fillSmoothRoundRect(170, -y0, 140, kConfirmBtnH, 12, C->error, C->bg);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->text, C->faint);
    s.drawString("Cancel", 80, kConfirmBtnH / 2 + 1 - y0);
    s.setTextColor(theme::rgb(0, 0, 0), C->error);
    s.drawString("Forget", 240, kConfirmBtnH / 2 + 1 - y0);
  });
}

// The update popup: what's new and three buttons, the first one big.
constexpr int16_t kPromptNowY = 122, kPromptNowH = 46, kPromptRowY = 178, kPromptRowH = 46;

void DisplayManager::showUpdatePrompt(const char* tag, const char* installed) {
  enterMode(Mode::Prompt);
  drawScreenTitle("Software update");
  drawLine(38, 32, font_date, [&](TFT_eSprite& s) {
    char t[48];
    snprintf(t, sizeof(t), "%s is available", tag);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(pal_.top, C->bg);
    s.drawString(t, kScreenW / 2, 17);
  });
  drawLine(72, 46, font_small, [&](TFT_eSprite& s) {
    char t[48];
    snprintf(t, sizeof(t), "You have %s.", installed);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString(t, kScreenW / 2, 11);
    s.drawString("Your settings and photos are kept.", kScreenW / 2, 32);
  });
  drawBlock(kPromptNowY, kPromptNowH, font_date, [&](TFT_eSprite& s, int16_t y0) {
    s.fillSmoothRoundRect(10, -y0, kScreenW - 20, kPromptNowH, 12, pal_.top, C->bg);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(theme::rgb(0, 0, 0), pal_.top);
    s.drawString("Update now", kScreenW / 2, kPromptNowH / 2 + 1 - y0);
  });
  drawBlock(kPromptRowY, kPromptRowH, font_small, [&](TFT_eSprite& s, int16_t y0) {
    s.setTextDatum(MC_DATUM);
    s.fillSmoothRoundRect(10, -y0, 145, kPromptRowH, 12, C->faint, C->bg);
    s.setTextColor(C->text, C->faint);
    s.drawString("Later", 82, kPromptRowH / 2 + 1 - y0);
    s.fillSmoothRoundRect(165, -y0, 145, kPromptRowH, 12, C->faint, C->bg);
    s.setTextColor(C->dim, C->faint);
    s.drawString("Skip this version", 237, kPromptRowH / 2 + 1 - y0);
  });
}

DisplayManager::PromptHit DisplayManager::promptHitTest(int16_t x, int16_t y) const {
  if (mode_ != Mode::Prompt) return PromptHit::None;
  if (y >= kPromptNowY - 4 && y < kPromptNowY + kPromptNowH + 4) return PromptHit::Now;
  if (y >= kPromptRowY - 4) {
    if (x < 157) return PromptHit::Later;
    if (x >= 163) return PromptHit::Skip;  // (a gap between them)
  }
  return PromptHit::None;
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
constexpr int16_t kSetRowH = 30;
constexpr int16_t kBtnY = 2, kBtnH = 26;
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
    s.drawString(text, 12, sub ? 8 : kSetRowH / 2);
    if (sub) {
      s.setTextColor(C->dim, C->bg);
      s.drawString(sub, 12, 22);
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
    label(s, "Clock face", nullptr);
    drawButton(s, kToggleX, kToggleW,
               v.face == 2 ? "Mix" : v.face == 1 ? "Analog" : v.hour12 ? "12-hour" : "24-hour", true,
               accent);
  });
  row(4, [&](TFT_eSprite& s) {
    drawButton(s, 8, kArrowW, "<", false, accent);
    drawButton(s, kScreenW - 8 - kArrowW, kArrowW, ">", false, accent);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString("Colours", kScreenW / 2, 8);
    s.setTextColor(C->text, C->bg);
    s.drawString(v.colorName, kScreenW / 2, 22);
  });
  row(5, [&](TFT_eSprite& s) {
    drawButton(s, 8, kArrowW, "<", false, accent);
    drawButton(s, kScreenW - 8 - kArrowW, kArrowW, ">", false, accent);
    s.setTextDatum(MC_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString("Background", kScreenW / 2, 8);
    s.setTextColor(C->text, C->bg);
    s.drawString(v.themeName, kScreenW / 2, 22);
  });
  row(6, [&](TFT_eSprite& s) {
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
      if (x < kScreenW / 3) return SettingsAction::ThemePrev;
      if (x >= kScreenW * 2 / 3) return SettingsAction::ThemeNext;
      break;
    case 6:
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
  if (!photoCorner_) photoCorner_ = static_cast<uint16_t*>(malloc(kPillW * kPillH * sizeof(uint16_t)));
  if (photoCorner_) {
    tft.readRect(kPillX, kPillY, kPillW, kPillH, photoCorner_);
    drawPhotoTime();
  }
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

  // Header: place, moon, the time (and the data's age when it's old).
  strlcpy(wxPlace_, w.place, sizeof(wxPlace_));
  wxHasAge_ = ageS != UINT32_MAX;
  wxAnchorMs_ = millis() - (wxHasAge_ ? ageS * 1000 : 0);
  drawWeatherHeader();

  // Now: big icon, temperature, condition, details.
  drawBlock(28, 88, font_temp, [&](TFT_eSprite& s, int16_t y0) {
    weather_icons::draw(s, 54, 44 - y0, 74, weather::iconFor(w.code), w.isDay, C->bg);
    char t[8];
    deg(t, sizeof(t), (int)lroundf(w.temp));
    s.setTextDatum(L_BASELINE);
    s.setTextColor(accent, C->bg);
    s.drawString(t, 102, 58 - y0);
    s.unloadFont();
    s.loadFont(font_date);
    s.setTextColor(C->text, C->bg);
    s.drawString(weather::describe(w.code), 102, 84 - y0);
    s.unloadFont();
    s.loadFont(font_small);
    char line[24];
    s.setTextColor(C->dim, C->bg);
    snprintf(line, sizeof(line), "Feels %d\xC2\xB0", (int)lroundf(w.feels));
    s.drawString(line, 216, 22 - y0);
    snprintf(line, sizeof(line), "Humidity %u%%", w.humidity);
    s.drawString(line, 216, 41 - y0);
    if (w.dayCount) {
      snprintf(line, sizeof(line), "Rain %u%%", w.days[0].pop);
      s.drawString(line, 216, 60 - y0);
    }
    if (w.aqi >= 0) {
      snprintf(line, sizeof(line), "AQI %d", w.aqi);
      s.setTextColor(aqiColor(w.aqi), C->bg);
      s.drawString(line, 216, 79 - y0);
    }
  });

  // Next hours, every 3 h.
  drawBlock(116, 58, font_small, [&](TFT_eSprite& s, int16_t y0) {
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
      s.drawString(label, cx, 8 - y0);
      weather_icons::draw(s, cx, 29 - y0, 26, weather::iconFor(h.code), h.isDay, C->bg);
      char tv[8];
      deg(tv, sizeof(tv), h.temp);
      s.setTextColor(C->text, C->bg);
      s.drawString(tv, cx, 50 - y0);
    }
  });

  // Next days.
  drawBlock(174, 66, font_small, [&](TFT_eSprite& s, int16_t y0) {
    s.drawFastHLine(10, -y0, kScreenW - 20, C->faint);
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
      s.drawString(i == 0 ? "Today" : kShortDays[t.tm_wday % 7], cx, 14 - y0);
      weather_icons::draw(s, cx - 13, 34 - y0, 22, weather::iconFor(d.code), true, C->bg);
      char pop[8];
      snprintf(pop, sizeof(pop), "%u%%", d.pop);
      s.setTextDatum(ML_DATUM);
      s.setTextColor(d.pop >= 50 ? theme::rgb(0, 190, 255) : C->dim, C->bg);
      s.drawString(pop, cx + 2, 34 - y0);
      char hi[8], lo[8];
      deg(hi, sizeof(hi), d.tmax);
      deg(lo, sizeof(lo), d.tmin);
      const int16_t hw = s.textWidth(hi), lw = s.textWidth(lo);
      const int16_t x0 = cx - (hw + 6 + lw) / 2;
      s.setTextColor(C->text, C->bg);
      s.drawString(hi, x0, 54 - y0);
      s.setTextColor(C->dim, C->bg);
      s.drawString(lo, x0 + hw + 6, 54 - y0);
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

// Twinkling stars and shooting stars: only pixels still showing the theme's
// old colour change, so the time, text and dial drawn over it stay as they are.
// None at night.
void DisplayManager::tickBackdrop(uint32_t now) {
  if (!backdrop::active() || night_ || now - lastSparkMs_ < 33) return;
  lastSparkMs_ = now;
  backdrop::Spark sp[48];
  const int n = backdrop::sparkles(now, sp, 48);
  for (int i = 0; i < n; ++i) {
    if (sp[i].from == sp[i].to) continue;
    if (dimDial) {  // inside the dial the theme is darkened: compare and draw it so
      const int dx = sp[i].x - kDialAreaX - kDialAreaC, dy = sp[i].y - kDialAreaC;
      if (dx * dx + dy * dy < kDialAreaR * kDialAreaR) {
        sp[i].from = blend256(sp[i].from, C->bg, 150);  // as faceRow()
        sp[i].to = blend256(sp[i].to, C->bg, 150);
      }
    }
    if (tft.readPixel(sp[i].x, sp[i].y) == sp[i].from) tft.drawPixel(sp[i].x, sp[i].y, sp[i].to);
  }
}

void DisplayManager::setBackdrop(uint8_t theme, uint32_t seed, const char* photoPath) {
  const char* path = photoPath ? photoPath : "";
  if (theme == backdrop_ && seed == backdropSeed_ && !strcmp(path, backdropPhoto_)) return;
  backdrop_ = theme;
  backdropSeed_ = seed;
  strlcpy(backdropPhoto_, path, sizeof(backdropPhoto_));
  if (mode_ == Mode::Clock) mode_ = Mode::None;  // the next showTime() draws the face whole
}

bool DisplayManager::previewBackdrop(const char* name, uint32_t durationMs, const char* photoPath) {
  backdrop::Theme t;
  if (!backdrop::fromName(name, t)) return false;
  const uint32_t t0 = millis();
  dimDial = false;
  const bool ok = backdrop::setup(t, C->bg, pal_.top, pal_.bottom, night_, esp_random(), photoPath);
  const uint32_t t1 = millis();
  mode_ = Mode::Test;
  testUntilMs_ = millis() + durationMs;
  paintBackdrop(0, 0, kScreenW, kScreenH);
  Serial.printf("[THEME] %s: %s, tables %lu ms, screen %lu ms, heap %u\n", backdrop::title(t), ok ? "ok" : "failed",
                static_cast<unsigned long>(t1 - t0), static_cast<unsigned long>(millis() - t1), ESP.getFreeHeap());
  return true;
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
