#include "display_manager.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <string.h>

#include "config.h"
#include "fonts/font_date.h"
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
constexpr uint32_t kBarMs = 300;
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

// Turns white-on-black glyphs in a 16-bit sprite into a vertical gradient
// (top colour at row y0, bottom colour at row y1) over the background,
// keeping the glyphs' anti-aliasing. Sprite pixels are stored byte-swapped.
void tintSprite(TFT_eSprite& s, int16_t w, int16_t h, const theme::Palette& p, int16_t y0,
                int16_t y1) {
  uint16_t* px = static_cast<uint16_t*>(s.getPointer());
  if (!px) return;
  const uint16_t bg = C->bg;
  const int bgR = bg >> 11, bgG = (bg >> 5) & 0x3F, bgB = bg & 0x1F;
  const uint16_t bgSwapped = swap16(bg);
  for (int16_t y = 0; y < h; ++y) {
    int t = y <= y0 ? 0 : y >= y1 ? 255 : (y - y0) * 255 / (y1 - y0);
    const uint16_t fg = theme::mix(p.top, p.bottom, t);
    const int fR = fg >> 11, fG = (fg >> 5) & 0x3F, fB = fg & 0x1F;
    uint16_t* row = px + y * w;
    for (int16_t x = 0; x < w; ++x) {
      const uint16_t c = row[x];
      if (c == 0) {
        row[x] = bgSwapped;
        continue;
      }
      const int a = (swap16(c) >> 5) & 0x3F;  // glyph coverage 0..63
      const int r = bgR + (fR - bgR) * a / 63;
      const int g = bgG + (fG - bgG) * a / 63;
      const int b = bgB + (fB - bgB) * a / 63;
      row[x] = swap16((r << 11) | (g << 5) | b);
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
  barAnimating_ = false;
  palValid_ = false;
  lastYday_ = -1;
  statusValid_ = false;
  bannerActive_ = false;
  notice_[0] = '\0';
  lastError_[0] = '\0';
}

void DisplayManager::setPalette(const theme::Palette& palette) {
  if (palValid_ && palette == pal_) return;
  const bool firstFrame = !palValid_;
  pal_ = palette;
  palValid_ = true;
  if (firstFrame) return;  // showTime draws everything on the first frame
  // Re-tint everything that uses the gradient colours.
  for (int i = 0; i < 4; ++i) {
    if (!bigRoll_[i].active) drawBigDigit(i, 1.0f);
  }
  drawColon();
  if (!secAnimating_) drawSeconds(1.0f);
  barPx_ = -1;
  drawProgress(lastLocal_.tm_sec * barW() / 59, false);
  drawDate(lastLocal_);
  drawStatusBar();
}

void DisplayManager::showTime(const struct tm& local) {
  if (!spritesOk_) return;
  if (mode_ == Mode::Test && (int32_t)(millis() - testUntilMs_) < 0) return;
  const bool firstFrame = mode_ != Mode::Clock;
  enterMode(Mode::Clock);
  const uint32_t now = millis();
  lastLocal_ = local;

  setPalette(night_ ? theme::nightPalette()
                    : theme::paletteForMinute(local.tm_hour * 60 + local.tm_min));

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

  drawProgress(local.tm_sec * barW() / 59, !firstFrame);

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
  if (barAnimating_) {
    float p = progressOf(barStartMs_, kBarMs, now);
    int px = barFrom_ + static_cast<int>((barTo_ - barFrom_) * p + 0.5f);
    drawProgress(px, false);
    barAnimating_ = p < 1.0f;
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
  } else {
    drawGlyphString(digitSprite, to, 0, kDigitW, kDigitBaseline);
  }
  tintSprite(digitSprite, kDigitW, kDigitH, pal_, kGlyphTop, kDigitBaseline);
  digitSprite.pushSprite(digitX(index, timeOffset_), kTimeTop);
}

void DisplayManager::drawColon() {
  if (mode_ != Mode::Clock) return;
  digitSprite.fillSprite(TFT_BLACK);
  drawGlyphString(digitSprite, ":", 0, kColonW, kDigitBaseline - 6);
  tintSprite(digitSprite, kDigitW, kDigitH, pal_, kGlyphTop, kDigitBaseline);
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
  tintSprite(secondsSprite, kSecW, kSecH, pal_, 2, kSecBaseline);
  secondsSprite.pushSprite(kSecX, kRowY - kSecH / 2 - 1);
}

void DisplayManager::drawProgress(int targetPx, bool animate) {
  const uint16_t track = theme::mix(C->bg, pal_.bottom, 48);
  if (barPx_ < 0) {
    // Full redraw (first frame or palette change).
    tft.fillRect(kBarX, kBarY, kBarW, kBarH, C->bg);
    tft.fillRect(barX(), kBarY, barW(), kBarH, track);
    drawMeridiem();
    barPx_ = 0;
    animate = false;
  }
  if (animate && targetPx > barPx_) {
    barFrom_ = barPx_;
    barTo_ = targetPx;
    barStartMs_ = millis();
    barAnimating_ = true;
    return;
  }
  barAnimating_ = false;  // tick() re-arms it while an animation is running
  // Horizontal gradient from the palette's top to bottom colour.
  for (int x = barPx_; x < targetPx; ++x) {
    tft.drawFastVLine(barX() + x, kBarY, kBarH, theme::mix(pal_.top, pal_.bottom, x * 255 / barW()));
  }
  if (targetPx < barPx_) {
    tft.fillRect(barX() + targetPx, kBarY, barPx_ - targetPx, kBarH, track);
  }
  barPx_ = targetPx;
}

void DisplayManager::drawDate(const struct tm& local) {
  drawLine(kDateTop, kDateH, font_date, [&](TFT_eSprite& s) {
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
    s.setTextColor(pal_.top, C->bg);
    s.drawString(weekday, x, kDateH / 2);
    s.setTextColor(C->date, C->bg);
    s.drawString(rest, x + w1, kDateH / 2);
  });
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
  tintSprite(bannerSprite, kScreenW, kNoticeH, pal_, 3, kNoticeH - 4);
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
  drawLine(kNoticeTop, kNoticeH, font_date, [&](TFT_eSprite& s) {
    const int16_t iconW = 26;
    const int16_t tempW = s.textWidth(temp);
    s.unloadFont();
    s.loadFont(font_small);
    const int16_t detailW = s.textWidth(detail);
    int16_t x = (kScreenW - (iconW + 4 + tempW + 10 + detailW)) / 2;

    weather_icons::draw(s, x + iconW / 2, kNoticeH / 2, 22, weather::iconFor(w.code), w.isDay,
                        night_ || weatherStale_, C->dim);
    x += iconW + 4;
    s.setTextDatum(ML_DATUM);
    s.setTextColor(C->dim, C->bg);
    s.drawString(detail, x + tempW + 10, kNoticeH / 2);
    s.unloadFont();
    s.loadFont(font_date);
    s.setTextColor(weatherStale_ ? C->dim : C->text, C->bg);
    s.drawString(temp, x, kNoticeH / 2);
  });
}

void DisplayManager::endOverlayScreen() {
  if (mode_ == Mode::Weather || mode_ == Mode::Info || mode_ == Mode::Settings ||
      mode_ == Mode::Calibrate || mode_ == Mode::Photo || mode_ == Mode::Confirm) {
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
      s.fillSmoothRoundRect(10, 0, kScreenW - 20, kInfoBtnH, 8, C->faint, C->bg);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(C->text, C->faint);
      s.drawString("Change Wi-Fi network", kScreenW / 2, kInfoBtnH / 2 + 1);
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
        // Segments as small blocks with 1 px gaps over the free (faint) track.
        const uint16_t colors[3] = {pal_.top, theme::mix(pal_.top, C->faint, 120), pal_.bottom};
        const int16_t y = (kInfoRowH - kInfoBarH) / 2;
        s.fillSmoothRoundRect(x, y, kInfoBarW, kInfoBarH, 2, C->faint, C->bg);
        uint64_t sum = 0;
        int16_t from = 0;
        for (int k = 0; k < 3; ++k) {
          if (!bar.seg[k]) continue;
          sum += bar.seg[k];
          const int16_t to = min<int64_t>(kInfoBarW, (int64_t)(kInfoBarW * sum / bar.total));
          if (to - from >= 2) {
            s.fillSmoothRoundRect(x + from, y, to - from - 1, kInfoBarH, 2, colors[k], C->faint);
          }
          from = to;
        }
        x += kInfoBarW + 8;
      }
      s.setTextColor(C->text, C->bg);
      s.drawString(v.rows[i][1], x, kInfoRowH / 2);
    });
  }
}

bool DisplayManager::infoButtonHit(int16_t x, int16_t y) const {
  // Generous: the touch mapping can be ~15 px off before calibration.
  return mode_ == Mode::Info && y >= kInfoBtnY - 14;
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
  if (mode_ != Mode::Confirm || y < kConfirmBtnY - 24) return ConfirmHit::None;
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
    label(s, "Forecast cycle", "every 2 min");
    drawButton(s, kToggleX, kToggleW, v.cycleOn ? "On" : "Off", v.cycleOn, accent);
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
      if (x >= kToggleX - 20) return SettingsAction::CycleToggle;
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
    tintSprite(digitSprite, kDigitW, kDigitH, p, kGlyphTop, kDigitBaseline);
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

void DisplayManager::screenshot(Print& out) {
  // Finish running animations so the capture shows a settled frame.
  lastFrameMs_ = 0;
  for (int i = 0; i < 4; ++i) bigRoll_[i].startMs -= kRollMs;
  secRoll_[0].startMs -= kRollMs;
  barStartMs_ -= kBarMs;
  tick();

  static uint16_t row[kScreenW];
  out.printf("\n[SHOT] BEGIN %d %d\n", kScreenW, kScreenH);
  for (int16_t y = 0; y < kScreenH; ++y) {
    tft.readRect(0, y, kScreenW, 1, row);
    out.write(reinterpret_cast<const uint8_t*>(row), sizeof(row));
  }
  out.printf("\n[SHOT] END\n");
}
