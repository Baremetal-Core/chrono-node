// Chrono Node - firmware entry point.
//
// Time model: the DS3231 is the timekeeper and always holds UTC. Local time
// is derived with the POSIX TZ from settings. The display is always driven by
// DS3231 reads. clock_sync keeps the ESP32 system clock in step with the
// DS3231 and corrects the DS3231 after each NTP sync. The RTC is never
// written at boot; without network use the serial console "SET" command.

#include <Arduino.h>

#include "backlight.h"
#include "clock_sync.h"
#include "config.h"
#include "display_manager.h"
#include "net_manager.h"
#include "ota.h"
#include "photos.h"
#include "rtc_manager.h"
#include "screens.h"
#include "serial_console.h"
#include "settings.h"
#include "time_utils.h"
#include "touch.h"
#include "weather.h"
#include "web_server.h"

namespace {

RtcManager rtc;
DisplayManager display;

enum class RtcState { Ok, Missing, InvalidTime };
RtcState rtcState = RtcState::Missing;
bool rtcLostPower = false;

uint32_t lastPollMs = 0;
uint32_t lastReadOkMs = 0;
uint32_t lastReprobeMs = 0;
uint32_t lastTempMs = 0;
int lastSecond = -1;
bool hasTemp = false;
float tempC = 0;

void pollRtc(bool forceLog = false);

// Night mode and screen changes are switched behind a backlight fade so the
// repaint is never seen: fade out, switch, repaint, fade in.
bool transitioning = false;
uint32_t transitionFadeInMs = 0;
bool nightKnown = false;
bool nightActive = false;

// Screens, in swipe order. The forecast is skipped while there is no data,
// the photo screen while there are no photos. WifiReset (the confirmation
// opened from Info) isn't in the swipe order.
enum class Screen : uint8_t { Clock, Forecast, Photo, Info, Settings, WifiReset };
constexpr int kScreenCount = 5;
Screen screenOn = Screen::Clock;
Screen screenWanted = Screen::Clock;  // target of the current/next transition
bool autoScreen = false;  // forecast/photo shown by a cycle or a SHOW command, not by touch
// Photos: the one showing, the one to show next (0 = next in rotation) and
// how long a PHOTO SHOW request keeps the screen up.
int photoSlot = 0;
int photoWanted = 0;
uint32_t photoRequestedUntilMs = 0;
uint32_t photoPausedUntilMs = 0;  // after a photo failed to decode
uint32_t lastTouchMs = 0;
uint32_t lastInfoMs = 0;

// Scrolling banner: rotates through the messages, periodically or continuously.
int bannerIndex = 0;
bool bannerRequested = false;
bool bannerWasActive = false;
uint32_t bannerNextMs = 0;

// Touching the screen at night raises it to the day level for a while.
bool nightBoost = false;
uint32_t nightBoostUntilMs = 0;

void startTransition(uint32_t fadeOutMs, uint32_t fadeInMs) {
  transitioning = true;
  transitionFadeInMs = fadeInMs;
  backlight::fadeTo(0, fadeOutMs);
}

uint8_t brightnessFor(bool night) {
  return night && !nightBoost ? settings::brightnessNight() : settings::brightnessDay();
}

// `again` repaints the current screen behind a fade (e.g. the next photo).
void requestScreen(Screen s, bool again = false) {
  if (transitioning || (!again && s == screenOn && s == screenWanted)) return;
  screenWanted = s;
  startTransition(config::kScreenFadeOutMs, config::kScreenFadeInMs);
}

// Called every second with the local time, before the frame is drawn.
void updateNight(const struct tm& local) {
  const bool want = settings::isNightAt(local.tm_hour * 60 + local.tm_min);
  if (!nightKnown) {
    // First frame after boot: pick the mode, then fade the backlight in.
    nightKnown = true;
    nightActive = want;
    display.setNightMode(want);
    backlight::fadeTo(brightnessFor(want), config::kNightFadeInMs);
    if (want) Serial.println("[NIGHT] Night mode");
    return;
  }
  if (want != nightActive && !transitioning) {
    nightActive = want;
    startTransition(config::kNightFadeOutMs, config::kNightFadeInMs);
    Serial.println(want ? "[NIGHT] Night mode" : "[NIGHT] Day mode");
  }
}

// Forecast screen: on request, or periodically when enabled (never at night
// or while a notice needs to be seen).
bool shouldShowWeather(const struct tm& local, bool hasData) {
  if (!hasData) return false;
  if ((int32_t)(millis() - weather::screenRequestedUntil()) < 0) return true;
  if (nightActive || !settings::weatherCycle() || rtcLostPower ||
      net::state() == net::State::Portal) {
    return false;
  }
  return local.tm_min % config::kWeatherCycleMinutes == 0 &&
         local.tm_sec >= config::kWeatherShowAtSecond &&
         local.tm_sec < config::kWeatherShowAtSecond + (int)config::kWeatherShowS;
}

// Photo slideshow: on request, or every N minutes (never at night or while
// a notice needs to be seen).
bool shouldShowPhoto(const struct tm& local) {
  if (photos::count() == 0 || (int32_t)(millis() - photoPausedUntilMs) < 0) return false;
  if ((int32_t)(millis() - photoRequestedUntilMs) < 0) return true;
  const uint8_t every = settings::photoEveryMinutes();
  if (nightActive || every == 0 || rtcLostPower || net::state() == net::State::Portal ||
      ota::activePin() || ota::progress() >= 0) {
    return false;
  }
  return (local.tm_hour * 60 + local.tm_min) % every == 0 &&
         local.tm_sec >= config::kPhotoShowAtSecond &&
         local.tm_sec < config::kPhotoShowAtSecond + (int)config::kPhotoShowS;
}

// Next photo slot in rotation after `slot` (0 if there are no photos).
int nextPhoto(int slot) {
  for (int i = 1; i <= config::kPhotoSlots; ++i) {
    const int s = (slot + i - 1) % config::kPhotoSlots + 1;
    if (photos::exists(s)) return s;
  }
  return 0;
}

// Automatic screen changes: the forecast and photo cycles, and back to the
// clock after a period without touch.
void updateAutoScreens(const struct tm& local, bool hasData) {
  if (transitioning) return;
  Screen want = Screen::Clock;
  if (shouldShowWeather(local, hasData)) {
    want = Screen::Forecast;
  } else if (shouldShowPhoto(local)) {
    want = Screen::Photo;
  }
  if (screenOn == Screen::Clock && want != Screen::Clock) {
    autoScreen = true;
    requestScreen(want);
  } else if (autoScreen && screenOn != want) {
    requestScreen(want);
  } else if (screenOn != Screen::Clock && !autoScreen &&
             millis() - lastTouchMs >= config::kScreenIdleMs) {
    requestScreen(Screen::Clock);
  }
}

// Called every second while the clock face is showing.
void updateBanner(const struct tm& local) {
  const bool active = display.bannerActive();
  if (bannerWasActive && !active) bannerNextMs = millis() + config::kBannerGapMs;
  bannerWasActive = active;
  const int count = settings::bannerCount();
  if (active || count == 0 || (!settings::bannerEnabled() && !bannerRequested)) return;

  const uint8_t every = settings::bannerEveryMinutes();
  bool due = bannerRequested;
  if (every == 0) {
    due |= (int32_t)(millis() - bannerNextMs) >= 0;
  } else {
    due |= local.tm_sec == config::kBannerStartSecond &&
           (local.tm_hour * 60 + local.tm_min) % every == 0;
  }
  if (!due) return;
  const int i = bannerIndex % count;
  if (display.startBanner(settings::bannerText(i), settings::bannerSpeed())) {
    bannerIndex = i + 1;
    bannerRequested = false;
    bannerWasActive = true;
  }
}

void buildSettingsView(SettingsView& v) {
  v.dayPct = settings::brightnessDay();
  v.nightPct = settings::brightnessNight();
  v.nightOn = settings::nightEnabled();
  snprintf(v.nightWindow, sizeof(v.nightWindow), "%02u:%02u-%02u:%02u",
           settings::nightStart() / 60, settings::nightStart() % 60, settings::nightEnd() / 60,
           settings::nightEnd() % 60);
  v.hour12 = settings::hour12();
  v.cycleOn = settings::weatherCycle();
  strlcpy(v.place, settings::place(), sizeof(v.place));
  v.recentCount = settings::recentLocationCount();
  v.recentIndex = -1;
  for (int i = 0; i < v.recentCount; ++i) {
    char name[32];
    float la, lo;
    if (settings::recentLocation(i, name, sizeof(name), la, lo) &&
        strcmp(name, settings::place()) == 0) {
      v.recentIndex = i;
      break;
    }
  }
}

void buildInfoView(InfoView& v) {
  v.count = 0;
  auto add = [&](const char* label, const char* fmt, auto... args) {
    if (v.count >= 9) return;
    strlcpy(v.rows[v.count][0], label, sizeof(v.rows[0][0]));
    snprintf(v.rows[v.count][1], sizeof(v.rows[0][1]), fmt, args...);
    ++v.count;
  };
  add("Firmware", "v%s (%s)", config::kFirmwareVersion, config::kGitRevision);
  char ssid[33], ip[16];
  net::ssid(ssid, sizeof(ssid));
  net::ip(ip, sizeof(ip));
  if (net::isConnected()) {
    add("Wi-Fi", "%s  %d dBm", ssid, net::rssi());
    add("IP", "%s", ip);
  } else {
    add("Wi-Fi", "%s", net::state() == net::State::Portal ? "setup portal open" : "not connected");
    add("IP", "-");
  }
  add("Host", "%s.local", config::kHostname);
  char ntp[16];
  clock_sync::statusText(ntp, sizeof(ntp));
  add("Time sync", "%s", ntp);
  const uint32_t age = weather::ageSeconds();
  if (age == UINT32_MAX) {
    add("Weather", "%s, no data", settings::place());
  } else {
    add("Weather", "%s, %lu min ago", settings::place(), (unsigned long)(age / 60));
  }
  const uint32_t up = millis() / 1000;
  add("Uptime", "%lud %02lu:%02lu:%02lu", (unsigned long)(up / 86400),
      (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  v.barRow = v.count;
  v.barTotal = ESP.getHeapSize();
  v.barUsed = v.barTotal - ESP.getFreeHeap();
  add("Memory", "%u / %u KB", v.barUsed / 1024, v.barTotal / 1024);
  if (hasTemp) {
    add("RTC", "DS3231, %.1f \xC2\xB0" "C%s", tempC, rtcLostPower ? ", lost power" : "");
  } else {
    add("RTC", "%s", rtc.isPresent() ? "DS3231" : "not detected");
  }
}

void showSettings() {
  SettingsView v;
  buildSettingsView(v);
  display.showSettingsScreen(v);
}

void applySettingsAction(SettingsAction a) {
  auto step = [](int value, int delta, int lo, int hi) {
    return static_cast<uint8_t>(constrain(value + delta, lo, hi));
  };
  switch (a) {
    case SettingsAction::None:
      return;
    case SettingsAction::DayMinus:
    case SettingsAction::DayPlus:
      settings::setBrightness(false, step(settings::brightnessDay(),
                                          a == SettingsAction::DayPlus ? 10 : -10, 10, 100));
      break;
    case SettingsAction::NightMinus:
    case SettingsAction::NightPlus:
      settings::setBrightness(true, step(settings::brightnessNight(),
                                         a == SettingsAction::NightPlus ? 5 : -5, 0, 100));
      break;
    case SettingsAction::NightToggle:
      settings::setNightEnabled(!settings::nightEnabled());
      break;
    case SettingsAction::FormatToggle:
      settings::setHour12(!settings::hour12());
      display.setHour12(settings::hour12());
      break;
    case SettingsAction::CycleToggle:
      settings::setWeatherCycle(!settings::weatherCycle());
      break;
    case SettingsAction::LocationPrev:
    case SettingsAction::LocationNext: {
      SettingsView v;
      buildSettingsView(v);
      if (v.recentCount < 2) break;
      int i = v.recentIndex < 0 ? 0 : v.recentIndex;
      i = (i + (a == SettingsAction::LocationNext ? 1 : v.recentCount - 1)) % v.recentCount;
      char name[32];
      float la, lo;
      if (settings::recentLocation(i, name, sizeof(name), la, lo) &&
          settings::setLocation(la, lo, name, false)) {
        Serial.printf("[WEATHER] Location: %s\n", name);
        weather::requestUpdate();
      }
      break;
    }
  }
  showSettings();
  // Preview brightness changes (at night the boost shows the day level).
  if (a == SettingsAction::NightMinus || a == SettingsAction::NightPlus) {
    nightBoost = false;
  }
  backlight::fadeTo(brightnessFor(nightActive), config::kBrightnessFadeMs);
}

Screen neighbour(Screen from, int dir) {
  if (from == Screen::WifiReset) return Screen::Info;
  const bool hasForecast = weather::version() != 0;
  const bool hasPhotos = photos::count() > 0;
  int i = static_cast<int>(from);
  for (int n = 0; n < kScreenCount; ++n) {
    i = (i + dir + kScreenCount) % kScreenCount;
    const Screen s = static_cast<Screen>(i);
    if ((s != Screen::Forecast || hasForecast) && (s != Screen::Photo || hasPhotos)) break;
  }
  return static_cast<Screen>(i);
}

void handleTouch() {
  const touch::Event ev = touch::poll();
  if (ev.gesture == touch::Gesture::None) return;
  const uint32_t now = millis();
  lastTouchMs = now;

  if (ev.gesture == touch::Gesture::Down) {
    if (nightActive && !transitioning) {
      if (!nightBoost) backlight::fadeTo(settings::brightnessDay(), config::kBrightnessFadeMs);
      nightBoost = true;
      nightBoostUntilMs = now + config::kNightTapBoostMs;
    }
    return;
  }
  if (transitioning) return;
  static const char* const kGestureNames[] = {"none", "down", "tap", "swipe left", "swipe right"};
  Serial.printf("[TOUCH] %s at %d,%d\n", kGestureNames[static_cast<int>(ev.gesture)], ev.at.x,
                ev.at.y);
  switch (ev.gesture) {
    case touch::Gesture::SwipeLeft:
    case touch::Gesture::SwipeRight:
      autoScreen = false;
      requestScreen(neighbour(screenOn, ev.gesture == touch::Gesture::SwipeLeft ? 1 : -1));
      break;
    case touch::Gesture::Tap:
      if (screenOn == Screen::Forecast) autoScreen = false;  // keep it while being read
      if (screenOn == Screen::Photo) {
        autoScreen = false;  // tap: next photo, and stay
        requestScreen(Screen::Photo, true);
      }
      if (screenOn == Screen::Settings) applySettingsAction(display.settingsHitTest(ev.at.x, ev.at.y));
      if (screenOn == Screen::Info && display.infoButtonHit(ev.at.x, ev.at.y)) {
        requestScreen(Screen::WifiReset);
      }
      if (screenOn == Screen::WifiReset) {
        switch (display.confirmHitTest(ev.at.x, ev.at.y)) {
          case DisplayManager::ConfirmHit::Cancel:
            requestScreen(Screen::Info);
            break;
          case DisplayManager::ConfirmHit::Confirm:
            Serial.println("[WIFI] Forget requested on the screen");
            net::requestForget();
            requestScreen(Screen::Clock);  // shows "Join Wi-Fi ... to set up"
            break;
          case DisplayManager::ConfirmHit::None:
            break;
        }
      }
      break;
    default:
      break;
  }
}

// Screen is dark: switch colours/screen, repaint, then fade back in.
void finishTransition() {
  display.setNightMode(nightActive);
  display.setHour12(settings::hour12());
  weather::Data wx;
  Screen next = screenWanted;
  if (next == Screen::Forecast && !weather::get(wx)) next = Screen::Clock;
  if (next == Screen::Photo) {
    const int slot = photoWanted && photos::exists(photoWanted) ? photoWanted : nextPhoto(photoSlot);
    photoWanted = 0;
    if (slot) photoSlot = slot;  // a broken photo is skipped next time
    if (slot == 0 || !display.showPhoto(photos::path(slot).c_str())) {
      // Don't bring the photo screen straight back in this slideshow window.
      next = Screen::Clock;
      photoRequestedUntilMs = millis();
      photoPausedUntilMs = millis() + (config::kPhotoShowS + 2) * 1000;
    }
  }
  switch (next) {
    case Screen::Photo:
      break;
    case Screen::Forecast:
      display.showWeatherScreen(wx, weather::ageSeconds());
      break;
    case Screen::Info: {
      InfoView v;
      buildInfoView(v);
      display.showInfoScreen(v);
      lastInfoMs = millis();
      break;
    }
    case Screen::Settings:
      showSettings();
      break;
    case Screen::WifiReset: {
      char saved[33];
      net::savedSsid(saved, sizeof(saved));
      display.showWifiResetScreen(saved, net::portalSsid());
      break;
    }
    case Screen::Clock:
      display.endOverlayScreen();
      autoScreen = false;
      break;
  }
  screenOn = screenWanted = next;
  transitioning = false;
  lastSecond = -1;
  pollRtc();
  backlight::fadeTo(brightnessFor(nightActive), transitionFadeInMs);
}

void logTime(const DateTime& utc) {
  char local[32];
  time_utils::formatLocal(utc, local, sizeof(local));
  Serial.printf("[TIME] %s\n", local);
}

void refreshLostPower() {
  rtcLostPower = rtc.lostPower();
  if (rtcLostPower) {
    Serial.println("[RTC] WARNING: DS3231 lost power, time is not valid. Use SET to fix.");
  }
}

void updateStatus() {
  StatusInfo st;
  switch (net::state()) {
    case net::State::Connected:
      st.wifi = StatusInfo::Wifi::Connected;
      st.rssi = static_cast<int8_t>(net::rssi());
      net::ssid(st.wifiText, sizeof(st.wifiText));
      break;
    case net::State::Connecting:
      st.wifi = StatusInfo::Wifi::Connecting;
      strlcpy(st.wifiText, "Connecting", sizeof(st.wifiText));
      break;
    case net::State::WaitingRetry:
      st.wifi = StatusInfo::Wifi::Offline;
      strlcpy(st.wifiText, "Offline", sizeof(st.wifiText));
      break;
    case net::State::Portal:
      st.wifi = StatusInfo::Wifi::Setup;
      strlcpy(st.wifiText, "Setup mode", sizeof(st.wifiText));
      break;
  }
  // RSSI changes constantly; quantise so the bar is only redrawn on a level change.
  st.rssi = st.rssi > -60 ? -50 : st.rssi > -72 ? -65 : st.rssi < 0 ? -80 : 0;
  clock_sync::statusText(st.ntpText, sizeof(st.ntpText));
  st.ntpWarn = clock_sync::isStale();
  // The DS3231 die temperature is only a fallback when there is no weather.
  weather::Data wx;
  const bool hasWeather = weather::get(wx);
  st.hasTemp = hasTemp && !hasWeather;
  st.tempC = static_cast<int8_t>(lroundf(tempC));
  display.setStatus(st);
  const uint32_t age = weather::ageSeconds();
  display.setWeather(hasWeather ? &wx : nullptr, age != UINT32_MAX && age > config::kWeatherStaleS);

  const int otaProgress = ota::progress();
  if (otaProgress >= 0) {
    char text[48];
    snprintf(text, sizeof(text), "Updating firmware... %d%%", otaProgress);
    display.setNotice(text, DisplayManager::NoticeKind::Setup);
  } else if (ota::activePin()) {
    char text[48];
    snprintf(text, sizeof(text), "Firmware update PIN: %04lu", (unsigned long)ota::activePin());
    display.setNotice(text, DisplayManager::NoticeKind::Setup);
  } else if (rtcLostPower) {
    display.setNotice("RTC lost power - waiting for NTP or SET", DisplayManager::NoticeKind::Error);
  } else if (net::state() == net::State::Portal) {
    char text[64];
    snprintf(text, sizeof(text), "Join Wi-Fi \"%s\" to set up", net::portalSsid());
    display.setNotice(text, DisplayManager::NoticeKind::Setup);
  } else {
    display.setNotice(nullptr, DisplayManager::NoticeKind::Setup);
  }
}

void readTemperature() {
  float t;
  hasTemp = rtc.isPresent() && rtc.temperatureC(t);
  if (hasTemp) tempC = t;
}

void enterMissing() {
  if (rtcState != RtcState::Missing) {
    Serial.println("[RTC] ERROR: DS3231 not detected");
  }
  rtcState = RtcState::Missing;
  lastSecond = -1;
  display.showError("RTC ERROR", "DS3231 not detected (SDA=27 SCL=22)");
}

void onRtcDetected() {
  Serial.printf("[RTC] DS3231 detected at 0x%02X\n", config::kDs3231Address);
  rtcState = RtcState::Ok;
  lastSecond = -1;
  refreshLostPower();
}

void pollRtc(bool forceLog) {
  DateTime utc;
  if (!rtc.read(utc)) {
    if (!rtc.isPresent()) {
      enterMissing();
    } else if (rtcState != RtcState::InvalidTime) {
      Serial.println("[RTC] ERROR: DS3231 returned an invalid time. Use SET to fix.");
      rtcState = RtcState::InvalidTime;
      lastSecond = -1;
      display.showError("RTC TIME INVALID", "Set time: SET YYYY-MM-DD HH:MM:SS");
    }
    return;
  }

  rtcState = RtcState::Ok;
  const uint32_t nowMs = millis();
  // Only a tick seen by consecutive polls is a true second boundary; after a
  // stall the first new second could be seen anywhere within that second.
  const bool recentRead = nowMs - lastReadOkMs <= 2 * config::kRtcPollIntervalMs;
  lastReadOkMs = nowMs;
  if (utc.second() == lastSecond) return;
  const bool secondEdge = lastSecond != -1 && recentRead;
  lastSecond = utc.second();

  if (secondEdge) clock_sync::onRtcSecondEdge(utc, !rtcLostPower);

  struct tm local;
  time_utils::utcToLocal(utc, local);
  updateNight(local);
  updateAutoScreens(local, weather::version() != 0);
  if (screenOn == Screen::Clock && !transitioning) {
    display.showTime(local);
    updateStatus();
    updateBanner(local);
  }
  if (forceLog || config::kLogEverySecond) logTime(utc);

  web::ClockState cs = {};
  cs.rtcPresent = rtc.isPresent();
  cs.rtcLostPower = rtcLostPower;
  cs.hasTemp = hasTemp;
  cs.tempC = tempC;
  cs.night = nightActive;
  static const char* const kScreenNames[] = {"clock", "forecast", "photo", "info", "settings",
                                             "wifi-reset"};
  strlcpy(cs.screen, kScreenNames[static_cast<int>(screenOn)], sizeof(cs.screen));
  web::publishClockState(cs);
}

}  // namespace

bool requestScreenByName(const char* name) {
  Screen s;
  if (strcasecmp(name, "clock") == 0) s = Screen::Clock;
  else if (strcasecmp(name, "forecast") == 0) s = Screen::Forecast;
  else if (strcasecmp(name, "photo") == 0) s = Screen::Photo;
  else if (strcasecmp(name, "info") == 0) s = Screen::Info;
  else if (strcasecmp(name, "settings") == 0) s = Screen::Settings;
  else if (strcasecmp(name, "wifi-reset") == 0) s = Screen::WifiReset;
  else return false;
  autoScreen = false;
  lastTouchMs = millis();
  requestScreen(s);
  return true;
}

void requestPhotoNow(int slot) {
  photoWanted = slot;
  photoRequestedUntilMs = millis() + config::kPhotoRequestShowMs;
  autoScreen = true;
  requestScreen(Screen::Photo, true);
}

void requestBannerNow() {
  bannerRequested = true;
  if (screenOn != Screen::Clock) requestScreenByName("clock");
}

void setup() {
  // The CYD RGB LED is active-low and glows dimly if left floating: switch it off.
  for (int pin : {config::kLedRedPin, config::kLedGreenPin, config::kLedBluePin}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  Serial.begin(config::kSerialBaud);
  Serial.println();
  Serial.printf("[%s] Booting %s v%s (%s)\n", config::kProjectName, config::kFirmwareName,
                config::kFirmwareVersion, config::kGitRevision);

  settings::begin();

  if (display.begin()) {
    Serial.println("[DISPLAY] Initialized");
  } else {
    Serial.println("[DISPLAY] ERROR: initialization failed");
  }
  backlight::begin(0);  // faded in once the first frame is drawn

  photos::begin();
  weather::begin();
  touch::begin();
  ota::begin();

  rtcState = RtcState::Missing;
  if (rtc.begin()) {
    onRtcDetected();
    lastPollMs = millis();
    pollRtc(true);
  } else {
    Serial.println("[RTC] ERROR: DS3231 not detected");
    display.showError("RTC ERROR", "DS3231 not detected (SDA=27 SCL=22)");
  }

  display.setHour12(settings::hour12());
  if (!nightKnown) backlight::fadeTo(settings::brightnessDay(), config::kNightFadeInMs);

  // Wi-Fi starts after the first clock frame so the time is visible immediately.
  net::begin();
  serialConsolePrintHelp();
}

void loop() {
  backlight::loop();
  ota::loop();
#ifdef CHRONO_CRASH_AFTER_MS
  // Rollback test image only (see README): crash before the image is confirmed.
  if (millis() > CHRONO_CRASH_AFTER_MS) abort();
#endif
  if (transitioning && !backlight::isFading()) finishTransition();

  handleTouch();
  const uint32_t nowMsTouch = millis();
  if (nightBoost && (int32_t)(nowMsTouch - nightBoostUntilMs) >= 0) {
    nightBoost = false;
    if (!transitioning) backlight::fadeTo(brightnessFor(nightActive), config::kNightFadeInMs);
  }
  if (screenOn == Screen::Info && !transitioning && nowMsTouch - lastInfoMs >= 2000) {
    lastInfoMs = nowMsTouch;
    InfoView v;
    buildInfoView(v);
    display.showInfoScreen(v);
  }

  if (clock_sync::loop(rtc)) {
    // DS3231 rewritten from NTP: this also clears its lost-power flag.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
  }

  display.tick();

  bool changed = serialConsolePoll(rtc, display);
  char webCommand[200];
  while (web::nextCommand(webCommand, sizeof(webCommand))) {
    changed |= serialConsoleExecute(rtc, display, webCommand);
  }
  if (changed) {
    // Time, timezone, brightness or schedule changed: re-read the
    // oscillator-stop flag, apply the brightness and redraw.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
    display.setHour12(settings::hour12());
    if (screenOn == Screen::Settings && !transitioning) showSettings();
    if (!transitioning && nightKnown) {
      backlight::fadeTo(brightnessFor(nightActive), config::kBrightnessFadeMs);
    }
  }

  const uint32_t nowMs = millis();
  if (rtcState == RtcState::Missing) {
    if (nowMs - lastReprobeMs >= config::kRtcReprobeIntervalMs) {
      lastReprobeMs = nowMs;
      if (rtc.probe()) onRtcDetected();
    }
  } else if (nowMs - lastPollMs >= config::kRtcPollIntervalMs) {
    lastPollMs = nowMs;
    if (lastTempMs == 0 || nowMs - lastTempMs >= config::kTemperatureIntervalMs) {
      lastTempMs = nowMs;
      readTemperature();
    }
    pollRtc();
  }

  delay(5);  // yield; keeps the loop responsive without spinning at 100% CPU
}
