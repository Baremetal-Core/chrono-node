// Chrono Node - TFT rendering for the CYD display (320x240, landscape).
//
// Layout: status bar (Wi-Fi, NTP, temperature), large HH:MM with small
// seconds, a minute progress bar, the date, and a bottom notice line.
// Changing digits roll in; everything is drawn through sprites, so nothing
// flickers. Call tick() every loop iteration to advance animations.
#pragma once

#include <Print.h>
#include <stdint.h>
#include <time.h>

struct StatusInfo {
  enum class Wifi : uint8_t { Offline, Connecting, Connected, Setup };
  Wifi wifi = Wifi::Offline;
  int8_t rssi = 0;           // dBm when connected
  char wifiText[40] = "";    // SSID or a short state message
  char ntpText[16] = "";     // e.g. "NTP 5m"
  bool ntpWarn = false;      // stale sync
  bool hasTemp = false;
  int8_t tempC = 0;
};

class DisplayManager {
 public:
  bool begin();

  // Call when the local second changes.
  void showTime(const struct tm& local);

  // Advances animations; cheap when idle.
  void tick();

  // Status bar contents. Redrawn only when changed.
  void setStatus(const StatusInfo& status);

  // Bottom notice line; nullptr clears it. Redrawn only when changed.
  void setNotice(const char* text, uint16_t color);

  // Full-screen error. Redrawn only when the message changes.
  void showError(const char* title, const char* detail);

  // Streams the screen as big-endian RGB565 between "[SHOT] BEGIN w h" and
  // "[SHOT] END" lines. Blocks for ~14 s at 115200 baud (development only).
  void screenshot(Print& out);

 private:
  enum class Mode { None, Clock, Error };
  struct Roll {
    char from = ' ';
    char to = ' ';
    uint32_t startMs = 0;
    bool active = false;
  };

  void enterMode(Mode mode);
  void drawStatusBar();
  void drawNotice();
  void drawDate(const struct tm& local);
  void drawColon();
  void drawBigDigit(int index, float progress);
  void drawSeconds(float progress);
  void drawProgress(int targetPx, bool animate);
  void setAccent(uint16_t accent);

  Mode mode_ = Mode::None;
  bool spritesOk_ = false;

  char digits_[4] = {' ', ' ', ' ', ' '};
  Roll bigRoll_[4];
  char secDigits_[2] = {' ', ' '};
  Roll secRoll_[2];
  bool secAnimating_ = false;

  int barPx_ = -1;      // drawn bar length
  int barFrom_ = 0;
  int barTo_ = 0;
  uint32_t barStartMs_ = 0;
  bool barAnimating_ = false;

  uint16_t accent_ = 0;
  int lastMinuteOfDay_ = -1;
  int lastYday_ = -1;
  uint32_t lastFrameMs_ = 0;

  StatusInfo status_;
  bool statusValid_ = false;
  char notice_[64] = "";
  uint16_t noticeColor_ = 0;
  char lastError_[96] = "";
};
