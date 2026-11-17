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

class TFT_eSprite;

// Usage bar on an info row: used out of `total`; the rest is free.
struct InfoBar {
  uint32_t total = 0;  // 0: no bar
  uint32_t used = 0;
};

// The storage screen: how the flash chip and RAM are used (bytes).
struct StorageView {
  uint32_t chip, firmware, updateSlot, photos, system;
  uint32_t heapTotal, heapUsed, heapPeak;
};

// Another text for the clock's date line (e.g. the lunar date), shown in
// turn with the solar date: `head` like the weekday, then `rest` (or
// `shortRest` when that doesn't fit). UTF-8.
struct DateAlt {
  char head[32];
  char rest[48];
  char shortRest[48];
};

// The analog face's right column (UTF-8; empty strings are left out).
struct AnalogInfo {
  char weekday[12];
  char day[4];
  char month[16];      // "Nov 2026"
  char lunar[24];      // "ÂL 8/10"
  char lunarYear[24];  // "Bính Ngọ"
  char holiday[32];
  int16_t moonAngle;   // -1 to hide
  uint8_t moonLit;     // %
  int16_t aqi;         // -1 to hide
};

// The Home screen: Home Assistant devices as tiles (2 x 3).
struct HomeTile {
  enum class Look : uint8_t { Off, On, Unavailable, Run, Pending };
  enum class Icon : uint8_t { Light, Power, Fan, Climate, Tv, Scene };  // Scene: a run button, no switch
  char name[32];
  char state[16];  // next to the icon ("On", "Off", "Cool", ...)
  Look look;
  Icon icon;
};
struct HomeView {
  static constexpr int kMax = 6;
  HomeTile tiles[kMax];
  int count;
};

// The month calendar: 4-6 weeks from Monday, with the lunar date under each day.
struct CalendarCell {
  static constexpr uint8_t kInMonth = 1, kToday = 2, kHoliday = 4, kLunarFirst = 8;
  uint8_t day, lunarDay, lunarMonth, flags;
};
struct CalendarView {
  char title[24];  // "November 2026"
  uint8_t rows;    // weeks shown
  CalendarCell cells[42];
  uint8_t weeks[6];  // ISO week numbers of the rows
  int8_t todayRow;   // the row with today, -1: another month
  bool away;         // another month than this one (the Today button lights up)
  int8_t selected;   // the cell whose card is open, -1: none
  char card[3][64];  // the card: date, lunar date, holiday ("" when none)
};

// The software update screen.
struct UpdateView {
  char installed[24];  // "v0.13.0"
  char status[48];     // main line, e.g. "v0.14.0 is available"
  char detail[96];     // smaller line under it; " | " starts a second line
  char action[16];     // right button label ("Install", "Check again"); empty: none
  bool actionIsInstall;
};

struct InfoView {
  static constexpr int kMaxRows = 10;
  char rows[kMaxRows][2][40];  // label, value
  InfoBar bars[kMaxRows];
  int count = 0;
  bool updateAvailable = false;  // highlights the Update button
};

struct SettingsView {
  uint8_t dayPct;
  uint8_t nightPct;
  bool nightOn;
  char nightWindow[16];  // e.g. "22:00-06:30"
  bool hour12;
  uint8_t face;  // 0 digital, 1 analog, 2 mix
  char colorName[24];  // current colour style
  char place[32];
  int recentIndex;  // position of the current place in the recent list (-1 if not in it)
  int recentCount;
};

enum class SettingsAction : uint8_t {
  None, DayMinus, DayPlus, NightMinus, NightPlus, NightToggle, FormatToggle, ColorPrev,
  ColorNext, LocationPrev, LocationNext
};

// How the clock face is coloured (see theme::Style and the kFx flags).
struct ColorSettings {
  theme::Style style = theme::Style::TimeOfDay;
  theme::Palette custom = {0, 0};
  uint8_t effects = 0;
  uint8_t speedQuarters = 4;  // animation speed: 4 = x1
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
  int16_t aqi = -1;  // US AQI, -1 to hide
  int16_t moonAngle = -1;  // moon phase (elongation, degrees), -1 to hide
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

  // Up to kMaxDateAlts texts the date line takes turns with: the solar date
  // for 10 s, then each one for 5 s (10 s each with `even`). 0 to stop.
  static constexpr int kMaxDateAlts = 3;
  void setDateAlts(const DateAlt* alts, int count, bool even);

  // The time on the other screens (title bar, forecast header, a pill in a
  // photo's corner), updated when the minute changes. Call every second.
  void setNow(const struct tm& local);

  // Phrases shown in turn on the line above the digits (static strings, up to
  // 2); a long one is shown in parts. 0 clears the line.
  void setPhrases(const char* const* texts, int count);

  // The Home screen; calling again redraws only the tiles that changed.
  void showHomeScreen(const HomeView& v);
  int homeHitTest(int16_t x, int16_t y) const;  // tile index, or -1
  void showCalendarScreen(const CalendarView& v);
  // A day of the shown month (1-31), 0 elsewhere (other months' days, the card),
  // kCalendarToday for the Today button (or the title).
  static constexpr int kCalendarToday = -2;
  int calendarHitTest(int16_t x, int16_t y) const;

  // Analog face instead of the digits; its right column's content.
  void setAnalog(bool analog);
  bool analog() const { return analog_; }
  void setAnalogInfo(const AnalogInfo& info);

  // Weather shown on the clock face's bottom line when there is no notice.
  // Pass nullptr to clear. `stale` dims it and marks it as old.
  void setWeather(const weather::Data* data, bool stale);

  // Full-screen forecast; leave it with endOverlayScreen().
  void showWeatherScreen(const weather::Data& data, uint32_t ageS);
  // Info and settings screens. Calling again redraws in place (no flicker).
  void showInfoScreen(const InfoView& view);
  void showSettingsScreen(const SettingsView& view);
  SettingsAction settingsHitTest(int16_t x, int16_t y) const;
  // The info screen's buttons along the bottom.
  enum class InfoButton : uint8_t { None, Wifi, Storage, Update };
  InfoButton infoButtonHit(int16_t x, int16_t y) const;
  void showStorageScreen(const StorageView& view);
  // Redraws in place when called again.
  void showUpdateScreen(const UpdateView& view);
  // The popup when a new release is found: Update now / Later / Skip this version.
  enum class PromptHit : uint8_t { None, Now, Later, Skip };
  void showUpdatePrompt(const char* tag, const char* installed);
  PromptHit promptHitTest(int16_t x, int16_t y) const;

  // Confirmation before forgetting the Wi-Fi network: Cancel / Forget. The
  // update screen uses the same two buttons (Back / action).
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

  // Colour style and effects for the clock face; repaints it if it's showing.
  void setColors(const ColorSettings& colors);

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
  // Screen change animations (theme::Transition): animates the old screen
  // away, leaving it black. `dir` +1: towards the left, as when the new screen
  // comes from the right (swipe left); -1 the other way. Fade and None don't
  // animate here; the new screen then fades in.
  void transitionOut(theme::Transition t, int dir);

 private:
  enum class Mode { None, Clock, Error, Test, Weather, Info, Settings, Calibrate, Photo, Confirm,
                    Storage, Update, Home, Calendar, Prompt };
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
  void drawBar(bool full, bool recolor);
  void syncFxBuffers();
  void drawMeridiem();
  void drawScreenTitle(const char* title);
  int16_t barX() const;
  int16_t barW() const;
  void setPalette(const theme::Palette& palette);
  theme::Palette basePalette(const struct tm& local, uint32_t now);
  void refreshField(uint32_t now);
  uint16_t barColor(int x) const;
  bool effectsOn() const;

  Mode mode_ = Mode::None;
  bool spritesOk_ = false;

  char digits_[4] = {' ', ' ', ' ', ' '};
  Roll bigRoll_[4];
  char secDigits_[2] = {' ', ' '};
  Roll secRoll_[2];
  bool secAnimating_ = false;

  int barPx_ = -1;              // drawn bar length in 1/16 px (-1: redraw everything)
  uint32_t secondStartMs_ = 0;  // millis() when the shown second began (smooth bar)

  theme::Palette pal_{0, 0};
  bool palValid_ = false;
  CalendarView calView_ = {};
  char calTitle_[24] = "";
  int16_t calCardY_ = -1, calCardH_ = 0;  // the open card's place
  int16_t calTodayX_ = 0;                 // the Today button's left edge
  HomeView homeView_ = {};
  bool homeValid_ = false;
  // Switch knobs: 0 left (off) to 1 right (on), sliding to their new place.
  float homeKnob_[HomeView::kMax] = {};
  float homeFrom_[HomeView::kMax] = {};
  float homeTo_[HomeView::kMax] = {};
  uint32_t homeAnimMs_[HomeView::kMax] = {};
  uint8_t homeAnim_ = 0;  // bit per sliding knob
  uint32_t lastHomeFrameMs_ = 0;
  void tickHome(uint32_t now);
  void drawHomeTile(int i, const HomeTile* t, float knob);
  void drawHomeSwitch(TFT_eSprite& s, int16_t x, int16_t y, float knob, uint16_t card);
  void drawHomeKnob(int i, float knob);
  uint16_t homeCard(const HomeTile& t) const;
  // The time overlay on the other screens.
  char nowText_[12] = "";
  int nowKey_ = -1;
  const char* title_ = nullptr;  // the title bar's text (a static string)
  char wxPlace_[32] = "";        // the forecast header: place and data age
  uint32_t wxAnchorMs_ = 0;      // millis() when the data was fetched
  bool wxHasAge_ = false;
  uint16_t* photoCorner_ = nullptr;  // the photo under the time pill
  void drawWeatherHeader();
  void drawPhotoTime();
  // The phrase line: which phrase and part shows, and until when.
  const char* phrases_[2] = {};
  int phraseCount_ = 0;
  int phraseIdx_ = 0;
  int phrasePart_ = 0;
  int phraseParts_ = 1;
  uint32_t phraseNextMs_ = 0;
  void drawPhraseLine();
  // Analog face: a dial on the left, the date column on the right.
  bool analog_ = false;
  bool analogFull_ = true;  // redraw the whole dial on the next frame
  AnalogInfo analogInfo_ = {};
  bool analogInfoValid_ = false;
  uint32_t lastAnalogMs_ = 0;
  struct Hand {
    float ax, ay, bx, by;  // tail and tip, dial coordinates
    float ar, br;          // half widths at the tail and the tip
  };
  Hand hands_[3] = {};  // as drawn: hour, minute, second
  bool handsValid_ = false;
  void analogFrame();
  // The minute (h*60+m) the dial on the screen shows, -1: none (cleared). While
  // the dial's buffers are lent out (HTTPS, a firmware download) or don't fit,
  // the held frame stays until it's out of date or gone; then the dial is drawn
  // straight to the screen (hour and minute hands), each minute.
  int dialShownKey_ = -1;
  void drawDialPlain();
  void drawDialRect(int16_t x0, int16_t y0, int16_t w, int16_t h, const Hand* hands);
  void drawAnalogColumn();
  struct tm lastLocal_ = {};
  int lastYday_ = -1;
  DateAlt dateAlts_[kMaxDateAlts] = {};
  int dateAltCount_ = 0;
  bool dateAltEven_ = false;
  int dateView_ = 0;  // 0: the solar date, else dateAlts_[dateView_ - 1]
  int dateViewAt(const struct tm& local) const;
  uint32_t lastFrameMs_ = 0;

  StatusInfo status_;
  bool statusValid_ = false;
  bool fxLeased_ = false;  // animation buffers lent to the network (mem_lease)
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
  ColorSettings colors_;
  uint32_t lastAnimMs_ = 0;
  uint32_t animPhaseMs_ = 0;  // animation clock, advancing at the chosen speed
  uint32_t lastPhaseMs_ = 0;
};
