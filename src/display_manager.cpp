#include "display_manager.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

#include "config.h"
#include "fonts/font_date.h"
#include "fonts/font_seconds.h"
#include "fonts/font_small.h"
#include "fonts/font_time.h"

namespace {

TFT_eSPI tft;
const theme::Colors* C = &theme::kDayColors;  // active UI colours (day or night)
TFT_eSprite digitSprite(&tft);   // one big digit cell, reused for every digit and the colon
TFT_eSprite secondsSprite(&tft); // both seconds digits

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

int16_t digitX(int index) { return kTimeX + index * kDigitW + (index >= 2 ? kColonW : 0); }

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
  drawProgress(lastLocal_.tm_sec * kBarW / 59, false);
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

  const char next[4] = {char('0' + local.tm_hour / 10), char('0' + local.tm_hour % 10),
                        char('0' + local.tm_min / 10), char('0' + local.tm_min % 10)};
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

  drawProgress(local.tm_sec * kBarW / 59, !firstFrame);

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
  digitSprite.pushSprite(digitX(index), kTimeTop);
}

void DisplayManager::drawColon() {
  if (mode_ != Mode::Clock) return;
  digitSprite.fillSprite(TFT_BLACK);
  drawGlyphString(digitSprite, ":", 0, kColonW, kDigitBaseline - 6);
  tintSprite(digitSprite, kDigitW, kDigitH, pal_, kGlyphTop, kDigitBaseline);
  digitSprite.pushSprite(kTimeX + 2 * kDigitW, kTimeTop, 0, 0, kColonW, kDigitH);
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
    tft.fillRect(kBarX, kBarY, kBarW, kBarH, track);
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
    tft.drawFastVLine(kBarX + x, kBarY, kBarH, theme::mix(pal_.top, pal_.bottom, x * 255 / kBarW));
  }
  if (targetPx < barPx_) {
    tft.fillRect(kBarX + targetPx, kBarY, barPx_ - targetPx, kBarH, track);
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
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(noticeKind_ == NoticeKind::Error ? C->error : C->setup, C->bg);
    s.setTextDatum(MC_DATUM);
    s.drawString(notice_, kScreenW / 2, kNoticeH / 2);
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
