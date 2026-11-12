// Chrono Node - TFT rendering for the CYD display (320x240, landscape).
//
// Layout: status bar (Wi-Fi, NTP, temperature), large gradient HH:MM, a
// minute progress bar with the seconds at its end, the date, and a bottom
// notice line. Gradient colours follow the time of day. Changing digits roll
// in; everything is drawn through sprites, so nothing flickers. Call tick()
// every loop iteration to advance animations.
#pragma once

#include <Print.h>
#include <stdint.h>
#include <time.h>

#include "ui_theme.h"
#include "weather.h"

struct InfoView {
  char rows[9][2][40];  // label, value
  int count = 0;
  // Optional usage bar drawn in this row before its value text.
  int barRow = -1;
  uint32_t barTotal = 0;
  uint32_t barUsed = 0;  // in use now
  uint32_t barPeak = 0;  // most ever in use (>= barUsed)
};

struct SettingsView {
  uint8_t dayPct;
  uint8_t nightPct;
  bool nightOn;
  char nightWindow[16];  // e.g. "22:00-06:30"
  bool hour12;
  bool cycleOn;
  char place[32];
  int recentIndex;  // position of the current place in the recent list (-1 if not in it)
  int recentCount;
};

enum class SettingsAction : uint8_t {
  None, DayMinus, DayPlus, NightMinus, NightPlus, NightToggle, FormatToggle, CycleToggle,
  LocationPrev, LocationNext
};

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

  enum class NoticeKind : uint8_t { Error, Setup };
  // Bottom notice line; nullptr clears it. Redrawn only when changed.
  void setNotice(const char* text, NoticeKind kind);

  // Scrolls `text` (UTF-8) once across the clock face's bottom line in the
  // time-of-day colours. Returns false if it can't start now (another screen,
  // or a notice is showing). Animated by tick().
  bool startBanner(const char* text, uint8_t pxPerSecond);
  bool bannerActive() const { return bannerActive_; }

  // Weather shown on the clock face's bottom line when there is no notice.
  // Pass nullptr to clear. `stale` dims it and marks it as old.
  void setWeather(const weather::Data* data, bool stale);

  // Full-screen forecast; leave it with endOverlayScreen().
  void showWeatherScreen(const weather::Data& data, uint32_t ageS);
  // Info and settings screens. Calling again redraws in place (no flicker).
  void showInfoScreen(const InfoView& view);
  void showSettingsScreen(const SettingsView& view);
  SettingsAction settingsHitTest(int16_t x, int16_t y) const;
  // The info screen's "Change Wi-Fi network" button.
  bool infoButtonHit(int16_t x, int16_t y) const;

  // Confirmation before forgetting the Wi-Fi network: Cancel / Forget.
  void showWifiResetScreen(const char* savedSsid, const char* portalSsid);
  enum class ConfirmHit : uint8_t { None, Cancel, Confirm };
  ConfirmHit confirmHitTest(int16_t x, int16_t y) const;

  // Leaves the forecast/info/settings screen; the clock face is repainted on
  // the next showTime().
  void endOverlayScreen();

  // Full-screen photo (a JPEG file in LittleFS), scaled down to fit and
  // centred on black. Returns false if it can't be decoded.
  bool showPhoto(const char* path);

  // Touch calibration target (crosshair) with an instruction line.
  void showCalibrationTarget(int16_t x, int16_t y, const char* text);

  // 12-hour clock with AM/PM.
  void setHour12(bool enabled);

  // Night mode: black background, dim red UI and the night palette. The
  // screen is repainted on the next showTime(); hide it behind a backlight fade.
  void setNightMode(bool night);
  bool nightMode() const { return night_; }

  // Full-screen error. Redrawn only when the message changes.
  void showError(const char* title, const char* detail);

  // Colour test pattern (labelled bars) shown for `durationMs`; the clock resumes afterwards.
  void showTestPattern(uint32_t durationMs);

  // All day-cycle palettes side by side for `durationMs`.
  void showPaletteTest(uint32_t durationMs);

  // Streams the screen as big-endian RGB565 between "[SHOT] BEGIN w h" and
  // "[SHOT] END" lines. Blocks for ~14 s at 115200 baud (development only).
  void screenshot(Print& out);

 private:
  enum class Mode { None, Clock, Error, Test, Weather, Info, Settings, Calibrate, Photo, Confirm };
  struct Roll {
    char from = ' ';
    char to = ' ';
    uint32_t startMs = 0;
    bool active = false;
  };

  void enterMode(Mode mode);
  void drawStatusBar();
  void drawNotice();
  void drawWeatherLine();
  void drawBanner(uint32_t now);
  void drawDate(const struct tm& local);
  void drawColon();
  void drawBigDigit(int index, float progress);
  void drawSeconds(float progress);
  void drawProgress(int targetPx, bool animate);
  void drawMeridiem();
  void drawScreenTitle(const char* title);
  int16_t barX() const;
  int16_t barW() const;
  void setPalette(const theme::Palette& palette);

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

  theme::Palette pal_{0, 0};
  bool palValid_ = false;
  struct tm lastLocal_ = {};
  int lastYday_ = -1;
  uint32_t lastFrameMs_ = 0;

  StatusInfo status_;
  bool statusValid_ = false;
  char notice_[64] = "";
  NoticeKind noticeKind_ = NoticeKind::Setup;
  bool night_ = false;
  weather::Data weather_;
  bool weatherStale_ = false;
  uint32_t weatherKey_ = 0;
  char lastError_[96] = "";
  uint32_t testUntilMs_ = 0;
  bool hour12_ = false;
  int16_t timeOffset_ = 0;
  bool bannerOk_ = false;
  bool bannerActive_ = false;
  char bannerText_[128] = "";
  int16_t bannerW_ = 0;
  uint8_t bannerSpeed_ = 70;
  uint32_t bannerStartMs_ = 0;
  bool pm_ = false;
};
