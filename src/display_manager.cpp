#include "display_manager.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

#include "config.h"
#include "fonts/font_date.h"
#include "fonts/font_seconds.h"
#include "fonts/font_small.h"
#include "fonts/font_time.h"
#include "ui_theme.h"

namespace {

TFT_eSPI tft;
TFT_eSprite digitSprite(&tft);   // one big digit cell, reused for every digit
TFT_eSprite secondsSprite(&tft); // both seconds digits

constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;

// Status bar
constexpr int16_t kStatusH = 26;
constexpr int16_t kStatusBaseline = 18;

// Big HH:MM digits: fixed-width cells so the time never shifts sideways.
constexpr int16_t kDigitW = 56;
constexpr int16_t kColonW = 24;
constexpr int16_t kDigitH = 78;
constexpr int16_t kDigitBaseline = 74;
constexpr int16_t kTimeTop = 66;

// Seconds: two cells, top-aligned with the big digits.
constexpr int16_t kSecCellW = 21;
constexpr int16_t kSecW = 2 * kSecCellW;
constexpr int16_t kSecH = 34;
constexpr int16_t kSecBaseline = 30;
constexpr int16_t kSecGap = 8;
constexpr int16_t kSecTopOffset = 1;

constexpr int16_t kBlockW = 4 * kDigitW + kColonW + kSecGap + kSecW;
constexpr int16_t kBlockX = (kScreenW - kBlockW) / 2;
constexpr int16_t kSecX = kBlockX + 4 * kDigitW + kColonW + kSecGap;

// Minute progress bar under the time
constexpr int16_t kBarY = kTimeTop + kDigitH + 12;
constexpr int16_t kBarH = 3;
constexpr int16_t kBarX = kBlockX;
constexpr int16_t kBarW = kBlockW;

// Date and notice lines
constexpr int16_t kDateTop = kBarY + 20;
constexpr int16_t kDateH = 30;
constexpr int16_t kNoticeTop = 212;
constexpr int16_t kNoticeH = 24;

constexpr uint32_t kRollMs = 280;
constexpr uint32_t kBarMs = 280;
constexpr uint32_t kFrameMs = 16;

const char* const kWeekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

int16_t digitX(int index) { return kBlockX + index * kDigitW + (index >= 2 ? kColonW : 0); }

float easeOutCubic(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float progressOf(uint32_t startMs, uint32_t durationMs, uint32_t nowMs) {
  uint32_t elapsed = nowMs - startMs;
  if (elapsed >= durationMs) return 1.0f;
  return easeOutCubic(static_cast<float>(elapsed) / durationMs);
}

// Draws one text line through a temporary full-width sprite (no flicker).
template <typename Fn>
void drawLine(int16_t top, int16_t height, const uint8_t* font, Fn&& draw) {
  TFT_eSprite line(&tft);
  line.setColorDepth(16);
  if (!line.createSprite(kScreenW, height)) return;
  line.fillSprite(theme::kBackground);
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
    s.drawSmoothArc(cx, baseY, radii[i], radii[i] - 2, 135, 225, c, theme::kBackground, true);
  }
  s.fillSmoothCircle(cx, baseY - 1, 1, bars > 0 ? on : off, theme::kBackground);
}

}  // namespace

bool DisplayManager::begin() {
  tft.init();  // also switches the backlight (TFT_BL) on
  tft.setRotation(1);
  tft.fillScreen(theme::kBackground);

  digitSprite.setColorDepth(16);
  secondsSprite.setColorDepth(16);
  spritesOk_ = digitSprite.createSprite(kDigitW, kDigitH) != nullptr &&
               secondsSprite.createSprite(kSecW, kSecH) != nullptr;
  if (!spritesOk_) {
    Serial.println("[DISPLAY] ERROR: sprite allocation failed");
    return false;
  }
  digitSprite.loadFont(font_time);
  secondsSprite.loadFont(font_seconds);
  return true;
}

void DisplayManager::enterMode(Mode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  tft.fillScreen(theme::kBackground);
  for (int i = 0; i < 4; ++i) {
    digits_[i] = ' ';
    bigRoll_[i].active = false;
  }
  secDigits_[0] = secDigits_[1] = ' ';
  secAnimating_ = false;
  barPx_ = -1;
  barAnimating_ = false;
  lastMinuteOfDay_ = -1;
  lastYday_ = -1;
  statusValid_ = false;
  notice_[0] = '\0';
  lastError_[0] = '\0';
}

void DisplayManager::setAccent(uint16_t accent) {
  if (accent == accent_ && lastMinuteOfDay_ != -1) return;
  accent_ = accent;
  drawColon();
  drawSeconds(1.0f);
  barPx_ = -1;  // force a full redraw in the new colour
}

void DisplayManager::showTime(const struct tm& local) {
  if (!spritesOk_) return;
  const bool firstFrame = mode_ != Mode::Clock;
  enterMode(Mode::Clock);
  const uint32_t now = millis();

  const int minuteOfDay = local.tm_hour * 60 + local.tm_min;
  if (minuteOfDay != lastMinuteOfDay_) {
    setAccent(theme::accentForMinute(minuteOfDay));
    lastMinuteOfDay_ = minuteOfDay;
  }

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
    if (sec[i] == secDigits_[i]) {
      secRoll_[i].from = secRoll_[i].to = sec[i];
      continue;
    }
    secRoll_[i].from = secDigits_[i];
    secRoll_[i].to = sec[i];
    secDigits_[i] = sec[i];
    secChanged = true;
  }
  if (secChanged) {
    secRoll_[0].startMs = secRoll_[1].startMs = now;
    secAnimating_ = !firstFrame;
    if (!secAnimating_) drawSeconds(1.0f);
  }

  const int barTarget = local.tm_sec * kBarW / 59;
  drawProgress(barTarget, !firstFrame);

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
  digitSprite.fillSprite(theme::kBackground);
  digitSprite.setTextColor(theme::kDigits, theme::kBackground);
  digitSprite.setTextDatum(BC_DATUM);
  const char to[2] = {r.active ? r.to : digits_[index], '\0'};
  if (r.active && progress < 1.0f && r.from != ' ') {
    // Old digit slides up and out while the new one rises from below.
    const int shift = static_cast<int>(progress * kDigitH);
    const char from[2] = {r.from, '\0'};
    digitSprite.setTextDatum(L_BASELINE);
    digitSprite.drawString(from, (kDigitW - digitSprite.textWidth(from)) / 2,
                           kDigitBaseline - shift);
    digitSprite.drawString(to, (kDigitW - digitSprite.textWidth(to)) / 2,
                           kDigitBaseline + kDigitH - shift);
  } else {
    digitSprite.setTextDatum(L_BASELINE);
    digitSprite.drawString(to, (kDigitW - digitSprite.textWidth(to)) / 2, kDigitBaseline);
  }
  digitSprite.pushSprite(digitX(index), kTimeTop);
}

void DisplayManager::drawColon() {
  if (mode_ != Mode::Clock) return;
  digitSprite.fillSprite(theme::kBackground);
  digitSprite.setTextColor(accent_, theme::kBackground);
  digitSprite.setTextDatum(L_BASELINE);
  digitSprite.drawString(":", (kColonW - digitSprite.textWidth(":")) / 2, kDigitBaseline - 4);
  digitSprite.pushSprite(kBlockX + 2 * kDigitW, kTimeTop, 0, 0, kColonW, kDigitH);
}

void DisplayManager::drawSeconds(float progress) {
  if (mode_ != Mode::Clock) return;
  secondsSprite.fillSprite(theme::kBackground);
  secondsSprite.setTextColor(accent_, theme::kBackground);
  secondsSprite.setTextDatum(L_BASELINE);
  const int shift = static_cast<int>(progress * kSecH);
  for (int i = 0; i < 2; ++i) {
    const Roll& r = secRoll_[i];
    const int16_t cellX = i * kSecCellW;
    const char to[2] = {secDigits_[i], '\0'};
    if (progress < 1.0f && r.from != r.to && r.from != ' ') {
      const char from[2] = {r.from, '\0'};
      secondsSprite.drawString(from, cellX + (kSecCellW - secondsSprite.textWidth(from)) / 2,
                               kSecBaseline - shift);
      secondsSprite.drawString(to, cellX + (kSecCellW - secondsSprite.textWidth(to)) / 2,
                               kSecBaseline + kSecH - shift);
    } else {
      secondsSprite.drawString(to, cellX + (kSecCellW - secondsSprite.textWidth(to)) / 2,
                               kSecBaseline);
    }
  }
  secondsSprite.pushSprite(kSecX, kTimeTop + kSecTopOffset);
}

void DisplayManager::drawProgress(int targetPx, bool animate) {
  if (barPx_ < 0) {
    // Full redraw (first frame or accent change).
    tft.fillRect(kBarX, kBarY, kBarW, kBarH, theme::dim(accent_, 56));
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
  if (targetPx > barPx_) {
    tft.fillRect(kBarX + barPx_, kBarY, targetPx - barPx_, kBarH, accent_);
  } else if (targetPx < barPx_) {
    tft.fillRect(kBarX + targetPx, kBarY, barPx_ - targetPx, kBarH, theme::dim(accent_, 56));
  }
  barPx_ = targetPx;
}

void DisplayManager::drawDate(const struct tm& local) {
  char text[48];
  snprintf(text, sizeof(text), "%s, %d %s %d", kWeekdays[local.tm_wday % 7], local.tm_mday,
           kMonths[local.tm_mon % 12], local.tm_year + 1900);
  drawLine(kDateTop, kDateH, font_date, [&](TFT_eSprite& s) {
    s.setTextColor(theme::kDate, theme::kBackground);
    s.setTextDatum(MC_DATUM);
    s.drawString(text, kScreenW / 2, kDateH / 2);
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
    uint16_t iconColor = theme::kTextDim;
    uint8_t bars = 0;
    switch (st.wifi) {
      case StatusInfo::Wifi::Connected:
        iconColor = theme::kText;
        bars = st.rssi > -60 ? 3 : st.rssi > -72 ? 2 : 1;
        break;
      case StatusInfo::Wifi::Connecting:
        iconColor = theme::kTextDim;
        break;
      case StatusInfo::Wifi::Setup:
        iconColor = theme::kSetup;
        bars = 3;
        break;
      case StatusInfo::Wifi::Offline:
        break;
    }
    drawWifiIcon(s, 18, kStatusBaseline + 1, bars, iconColor, theme::kTextFaint);

    s.setTextDatum(L_BASELINE);
    s.setTextColor(st.wifi == StatusInfo::Wifi::Setup ? theme::kSetup : theme::kTextDim,
                   theme::kBackground);
    s.drawString(st.wifiText, 38, kStatusBaseline);

    int16_t x = kScreenW - 10;
    s.setTextDatum(R_BASELINE);
    if (st.hasTemp) {
      char temp[12];
      snprintf(temp, sizeof(temp), "%d\xC2\xB0" "C", st.tempC);
      s.setTextColor(theme::kTextDim, theme::kBackground);
      s.drawString(temp, x, kStatusBaseline);
      x -= s.textWidth(temp) + 14;
    }
    s.setTextColor(st.ntpWarn ? theme::kWarn : theme::kTextDim, theme::kBackground);
    s.drawString(st.ntpText, x, kStatusBaseline);
  });
}

void DisplayManager::setNotice(const char* text, uint16_t color) {
  const char* value = text ? text : "";
  if (strcmp(value, notice_) == 0 && color == noticeColor_) return;
  strlcpy(notice_, value, sizeof(notice_));
  noticeColor_ = color;
  drawNotice();
}

void DisplayManager::drawNotice() {
  if (mode_ != Mode::Clock) return;
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(noticeColor_, theme::kBackground);
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
    s.setTextColor(theme::kError, theme::kBackground);
    s.setTextDatum(MC_DATUM);
    s.drawString(title, kScreenW / 2, 17);
  });
  drawLine(124, 24, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(theme::kText, theme::kBackground);
    s.setTextDatum(MC_DATUM);
    s.drawString(detail, kScreenW / 2, 12);
  });
  drawLine(kNoticeTop, kNoticeH, font_small, [&](TFT_eSprite& s) {
    s.setTextColor(theme::kTextFaint, theme::kBackground);
    s.setTextDatum(MC_DATUM);
    s.drawString(config::kProjectName, kScreenW / 2, kNoticeH / 2);
  });
}

void DisplayManager::screenshot(Print& out) {
  static uint16_t row[kScreenW];
  out.printf("\n[SHOT] BEGIN %d %d\n", kScreenW, kScreenH);
  for (int16_t y = 0; y < kScreenH; ++y) {
    tft.readRect(0, y, kScreenW, 1, row);
    out.write(reinterpret_cast<const uint8_t*>(row), sizeof(row));
  }
  out.printf("\n[SHOT] END\n");
}
