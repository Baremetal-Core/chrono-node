#include "display_manager.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

#include "config.h"

namespace {

TFT_eSPI tft;
TFT_eSprite timeSprite(&tft);

constexpr uint16_t kBackground = TFT_BLACK;
constexpr uint16_t kTimeColor = TFT_WHITE;
constexpr uint16_t kDateColor = TFT_LIGHTGREY;
constexpr uint16_t kStatusColor = TFT_YELLOW;
constexpr uint16_t kErrorColor = TFT_RED;

constexpr uint8_t kTimeFont = 7;  // 7-segment, 48 px high
constexpr uint8_t kDateFont = 4;  // 26 px high
constexpr uint8_t kSmallFont = 2;  // 16 px high

constexpr int16_t kTimeSpriteW = 300;
constexpr int16_t kTimeSpriteH = 56;
constexpr int16_t kTimeCenterY = 95;
constexpr int16_t kDateCenterY = 155;
constexpr int16_t kStatusCenterY = 222;

}  // namespace

bool DisplayManager::begin() {
  tft.init();  // also switches the backlight (TFT_BL) on
  tft.setRotation(1);  // landscape, 320x240
  tft.fillScreen(kBackground);

  // Render the time off-screen and push it in one go to avoid flicker.
  timeSprite.setColorDepth(16);
  spriteOk_ = timeSprite.createSprite(kTimeSpriteW, kTimeSpriteH) != nullptr;
  if (!spriteOk_) {
    Serial.println("[DISPLAY] WARNING: time sprite allocation failed, drawing directly");
  }
  return true;
}

void DisplayManager::enterMode(Mode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  tft.fillScreen(kBackground);
  lastTime_[0] = '\0';
  lastDate_[0] = '\0';
  lastStatus_[0] = '\0';
  lastError_[0] = '\0';
}

void DisplayManager::showTime(const DateTime& now) {
  enterMode(Mode::Clock);

  char timeText[sizeof(lastTime_)];
  snprintf(timeText, sizeof(timeText), "%02u:%02u:%02u", now.hour(), now.minute(),
           now.second());
  if (strcmp(timeText, lastTime_) != 0) {
    if (spriteOk_) {
      timeSprite.fillSprite(kBackground);
      timeSprite.setTextColor(kTimeColor, kBackground);
      timeSprite.setTextDatum(MC_DATUM);
      timeSprite.drawString(timeText, kTimeSpriteW / 2, kTimeSpriteH / 2, kTimeFont);
      timeSprite.pushSprite((tft.width() - kTimeSpriteW) / 2, kTimeCenterY - kTimeSpriteH / 2);
    } else {
      tft.setTextColor(kTimeColor, kBackground);
      tft.setTextDatum(MC_DATUM);
      tft.setTextPadding(tft.textWidth("88:88:88", kTimeFont));
      tft.drawString(timeText, tft.width() / 2, kTimeCenterY, kTimeFont);
      tft.setTextPadding(0);
    }
    strcpy(lastTime_, timeText);
  }

  char dateText[sizeof(lastDate_)];
  snprintf(dateText, sizeof(dateText), "%02u/%02u/%04u", now.day(), now.month(), now.year());
  if (strcmp(dateText, lastDate_) != 0) {
    tft.setTextColor(kDateColor, kBackground);
    tft.setTextDatum(MC_DATUM);
    tft.setTextPadding(tft.textWidth("88/88/8888", kDateFont));
    tft.drawString(dateText, tft.width() / 2, kDateCenterY, kDateFont);
    tft.setTextPadding(0);
    strcpy(lastDate_, dateText);
  }
}

void DisplayManager::setStatus(const char* text) {
  if (mode_ != Mode::Clock) return;
  const char* value = text ? text : "";
  if (strcmp(value, lastStatus_) == 0) return;

  tft.setTextColor(kStatusColor, kBackground);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(tft.width());
  tft.drawString(value, tft.width() / 2, kStatusCenterY, kSmallFont);
  tft.setTextPadding(0);
  strlcpy(lastStatus_, value, sizeof(lastStatus_));
}

void DisplayManager::showError(const char* title, const char* detail) {
  enterMode(Mode::Error);

  char key[sizeof(lastError_)];
  snprintf(key, sizeof(key), "%s|%s", title, detail);
  if (strcmp(key, lastError_) == 0) return;

  tft.fillScreen(kBackground);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(kErrorColor, kBackground);
  tft.drawString(title, tft.width() / 2, 95, kDateFont);
  tft.setTextColor(kTimeColor, kBackground);
  tft.drawString(detail, tft.width() / 2, 135, kSmallFont);
  tft.setTextColor(kDateColor, kBackground);
  tft.drawString(config::kProjectName, tft.width() / 2, kStatusCenterY, kSmallFont);
  strlcpy(lastError_, key, sizeof(lastError_));
}
