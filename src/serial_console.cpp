#include "serial_console.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "clock_sync.h"
#include "config.h"
#include "display_manager.h"
#include "net_manager.h"
#include "ota.h"
#include "photos.h"
#include "rtc_manager.h"
#include "screens.h"
#include "settings.h"
#include "time_utils.h"
#include "touch.h"
#include "weather.h"

namespace {

constexpr size_t kLineMax = 200;  // banner text is up to 120 bytes of UTF-8
char line[kLineMax];
size_t lineLen = 0;
bool overflow = false;

void printRtcTime(RtcManager& rtc) {
  DateTime utc;
  if (!rtc.read(utc)) {
    Serial.println("[RTC] ERROR: DS3231 not readable");
    return;
  }
  char local[32];
  time_utils::formatLocal(utc, local, sizeof(local));
  Serial.printf("[TIME] %s (UTC %02u:%02u:%02u)\n", local, utc.hour(), utc.minute(),
                utc.second());
}

bool handleSet(RtcManager& rtc, const char* args) {
  int y, mo, d, h, mi, s;
  char extra;
  if (sscanf(args, "%4d-%2d-%2d %2d:%2d:%2d %c", &y, &mo, &d, &h, &mi, &s, &extra) != 6) {
    Serial.println("[CONSOLE] Usage: SET YYYY-MM-DD HH:MM:SS (local time)");
    return false;
  }
  DateTime utc;
  if (!time_utils::localToUtc(y, mo, d, h, mi, s, utc)) {
    Serial.println("[CONSOLE] ERROR: invalid local date/time (years 2000-2099)");
    return false;
  }
  if (!rtc.isPresent()) {
    Serial.println("[RTC] ERROR: DS3231 not detected, time not set");
    return false;
  }
  if (!rtc.setTime(utc)) {
    Serial.println("[RTC] ERROR: failed to write time");
    return false;
  }
  Serial.println("[RTC] Time set");
  printRtcTime(rtc);
  return true;
}

bool handleTz(RtcManager& rtc, const char* args) {
  while (*args == ' ') ++args;
  if (*args == '\0') {
    Serial.printf("[SETTINGS] TZ=%s\n", settings::timezone());
    return false;
  }
  if (!settings::setTimezone(args)) {
    Serial.println("[CONSOLE] ERROR: invalid POSIX TZ, e.g. TZ <+07>-7");
    return false;
  }
  Serial.printf("[SETTINGS] TZ=%s (saved)\n", settings::timezone());
  printRtcTime(rtc);
  return true;
}

// Interactive touch calibration with on-screen crosshairs (blocking, ~20 s).
// Waits for a press and returns the median position while held. A press that
// is too short to measure is ignored and the next one is used instead.
bool readHeld(uint16_t& rx, uint16_t& ry, uint32_t timeoutMs) {
  constexpr int kMax = 64;
  uint16_t xs[kMax], ys[kMax];
  uint16_t x, y, z;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (touch::readRaw(x, y, z)) delay(10);  // wait for release
    while (!touch::readRaw(x, y, z)) {
      if (millis() - start >= timeoutMs) return false;
      delay(10);
    }
    int n = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 1200 && touch::readRaw(x, y, z)) {
      if (millis() - t0 > 120 && n < kMax) {  // skip the landing
        xs[n] = x;
        ys[n] = y;
        ++n;
      }
      delay(15);
    }
    if (n >= 3) {
      std::sort(xs, xs + n);
      std::sort(ys, ys + n);
      rx = xs[n / 2];
      ry = ys[n / 2];
      return true;
    }
  }
  return false;
}

void calibrateTouch(DisplayManager& display) {
  const int16_t tx[4] = {20, 300, 300, 20};
  const int16_t ty[4] = {20, 20, 220, 220};
  uint16_t rx[4], ry[4];
  for (int i = 0; i < 4; ++i) {
    char text[48];
    snprintf(text, sizeof(text), "Touch and hold the cross (%d/4)", i + 1);
    display.showCalibrationTarget(tx[i], ty[i], text);
    if (!readHeld(rx[i], ry[i], 30000)) {
      Serial.println("[TOUCH] Calibration cancelled (no touch)");
      return;
    }
    Serial.printf("[TOUCH] Point %d: raw %u,%u\n", i + 1, rx[i], ry[i]);
  }
  touch::Point screen[4];
  for (int i = 0; i < 4; ++i) screen[i] = {tx[i], ty[i]};
  touch::Calibration c;
  if (!touch::fitCalibration(screen, rx, ry, 4, c)) {
    Serial.println("[TOUCH] Calibration failed (points are degenerate)");
    return;
  }
  touch::setCalibration(c);
  Serial.printf("[TOUCH] Calibrated (saved): x = %.5f*rx %+.5f*ry %+.1f, y = %.5f*rx %+.5f*ry %+.1f\n",
                c.ax, c.bx, c.cx, c.ay, c.by, c.cy);
  for (int i = 0; i < 4; ++i) {
    touch::Point m = touch::map(rx[i], ry[i]);
    Serial.printf("[TOUCH]   target %3d,%3d -> %3d,%3d\n", tx[i], ty[i], m.x, m.y);
  }
}

void printBanners() {
  Serial.printf("[BANNER] %s, every %u min%s, %u px/s, %d message(s)\n",
                settings::bannerEnabled() ? "on" : "off", settings::bannerEveryMinutes(),
                settings::bannerEveryMinutes() == 0 ? " (continuous)" : "", settings::bannerSpeed(),
                settings::bannerCount());
  for (int i = 0; i < settings::bannerCount(); ++i) {
    Serial.printf("[BANNER]   %d: %s\n", i + 1, settings::bannerText(i));
  }
}

bool handleBanner(const char* arg) {
  while (*arg == ' ') ++arg;
  bool ok = true;
  int n = 0, consumed = 0;
  if (*arg == '\0') {
    printBanners();
    return false;
  } else if (strncasecmp(arg, "ADD ", 4) == 0) {
    ok = settings::addBanner(arg + 4);
  } else if (strncasecmp(arg, "SET ", 4) == 0 && sscanf(arg + 4, "%d %n", &n, &consumed) == 1 &&
             consumed > 0) {
    ok = settings::setBanner(n - 1, arg + 4 + consumed);
  } else if (strncasecmp(arg, "DEL ", 4) == 0 && sscanf(arg + 4, "%d", &n) == 1) {
    ok = settings::removeBanner(n - 1);
  } else if (strcasecmp(arg, "CLEAR") == 0) {
    ok = settings::clearBanners();
  } else if (strcasecmp(arg, "ON") == 0 || strcasecmp(arg, "OFF") == 0) {
    ok = settings::setBannerEnabled(strcasecmp(arg, "ON") == 0);
  } else if (strncasecmp(arg, "EVERY ", 6) == 0 && sscanf(arg + 6, "%d", &n) == 1 && n >= 0) {
    ok = settings::setBannerEveryMinutes(n);
  } else if (strncasecmp(arg, "SPEED ", 6) == 0 && sscanf(arg + 6, "%d", &n) == 1 && n > 0) {
    ok = n <= 255 && settings::setBannerSpeed(n);
  } else if (strcasecmp(arg, "SHOW") == 0) {
    requestBannerNow();
    return false;
  } else {
    Serial.println("[CONSOLE] Usage: BANNER [ADD text|SET n text|DEL n|CLEAR|ON|OFF|EVERY min|"
                   "SPEED px/s|SHOW]");
    return false;
  }
  if (!ok) {
    Serial.printf("[CONSOLE] ERROR: rejected (max %d messages of %u bytes, every 0-60 min, "
                  "speed 20-200 px/s)\n", config::kBannerMaxMessages, (unsigned)config::kBannerMaxLen);
    return false;
  }
  printBanners();
  return true;
}

void printPhotos() {
  if (!photos::available()) {
    Serial.println("[PHOTO] Storage unavailable");
    return;
  }
  const uint8_t every = settings::photoEveryMinutes();
  Serial.printf("[PHOTO] %d of %d photo(s), slideshow %s%u%s, %u of %u KB used\n", photos::count(),
                config::kPhotoSlots, every ? "every " : "off", every ? every : 0,
                every ? " min" : "", (unsigned)(photos::usedBytes() / 1024),
                (unsigned)(photos::totalBytes() / 1024));
  int slots[config::kPhotoSlots];
  const int n = photos::list(slots, config::kPhotoSlots);
  for (int i = 0; i < n; ++i) {
    uint16_t w = 0, h = 0;
    String err;
    photos::checkJpeg(photos::path(slots[i]), w, h, err);
    Serial.printf("[PHOTO]   %d: %ux%u, %u bytes\n", slots[i], w, h,
                  (unsigned)photos::sizeOf(slots[i]));
  }
}

bool handlePhoto(const char* arg) {
  while (*arg == ' ') ++arg;
  int n = 0;
  if (*arg == '\0') {
    printPhotos();
    return false;
  } else if (strncasecmp(arg, "DEL ", 4) == 0 && sscanf(arg + 4, "%d", &n) == 1) {
    if (!photos::remove(n)) {
      Serial.printf("[CONSOLE] ERROR: no photo %d\n", n);
      return false;
    }
  } else if (strcasecmp(arg, "CLEAR") == 0) {
    photos::clear();
  } else if (strncasecmp(arg, "EVERY ", 6) == 0 && sscanf(arg + 6, "%d", &n) == 1 && n >= 0 &&
             n <= 60) {
    settings::setPhotoEveryMinutes(n);
  } else if (strcasecmp(arg, "SHOW") == 0 || (strncasecmp(arg, "SHOW ", 5) == 0 &&
                                              sscanf(arg + 5, "%d", &n) == 1)) {
    if (photos::count() == 0 || (n && !photos::exists(n))) {
      Serial.println("[CONSOLE] ERROR: no such photo (upload photos on the web page)");
      return false;
    }
    requestPhotoNow(n);
    return false;
  } else {
    Serial.println("[CONSOLE] Usage: PHOTO [DEL n|CLEAR|EVERY min|SHOW [n]]");
    return false;
  }
  printPhotos();
  return false;
}

void printBrightness() {
  Serial.printf("[NIGHT] Brightness day %u%%, night %u%%; night mode %s, %02u:%02u-%02u:%02u\n",
                settings::brightnessDay(), settings::brightnessNight(),
                settings::nightEnabled() ? "on" : "off", settings::nightStart() / 60,
                settings::nightStart() % 60, settings::nightEnd() / 60, settings::nightEnd() % 60);
}

void printInfo(RtcManager& rtc) {
  const esp_partition_t* part = esp_ota_get_running_partition();
  Serial.printf("[INFO] %s %s v%s (%s)\n", config::kProjectName, config::kFirmwareName,
                config::kFirmwareVersion, config::kGitRevision);
  Serial.printf("[INFO] Uptime %lus, heap free %u B (min %u B), partition %s\n",
                (unsigned long)(millis() / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                part ? part->label : "?");
  Serial.printf("[INFO] TZ=%s, NTP=%s\n", settings::timezone(), settings::ntpServer());
  float tempC;
  if (rtc.isPresent() && rtc.temperatureC(tempC)) {
    Serial.printf("[INFO] DS3231 present, lost power: %s, temperature %.2f C\n",
                  rtc.lostPower() ? "yes" : "no", tempC);
  } else {
    Serial.println("[INFO] DS3231 not detected");
  }
}

bool handleLine(RtcManager& rtc, DisplayManager& display, char* text) {
  while (*text == ' ') ++text;
  if (*text == '\0') return false;

  if (strncasecmp(text, "SET ", 4) == 0) return handleSet(rtc, text + 4);
  if (strncasecmp(text, "SETEPOCH ", 9) == 0) {
    char* end = nullptr;
    unsigned long epoch = strtoul(text + 9, &end, 10);
    DateTime utc(static_cast<uint32_t>(epoch));
    if (!end || *end != '\0' || epoch < 1577836800UL || !utc.isValid() || utc.year() > 2099) {
      Serial.println("[CONSOLE] Usage: SETEPOCH <unix time, UTC>");
      return false;
    }
    if (!rtc.isPresent() || !rtc.setTime(utc)) {
      Serial.println("[RTC] ERROR: could not set time");
      return false;
    }
    Serial.println("[RTC] Time set");
    printRtcTime(rtc);
    return true;
  }
  if (strcasecmp(text, "REBOOT") == 0) {
    Serial.println("[CONSOLE] Rebooting");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  if (strcasecmp(text, "TZ") == 0 || strncasecmp(text, "TZ ", 3) == 0) {
    return handleTz(rtc, text + 2);
  }
  if (strncasecmp(text, "NTP SERVER ", 11) == 0) {
    const char* host = text + 11;
    while (*host == ' ') ++host;
    if (settings::setNtpServer(host)) {
      Serial.printf("[SETTINGS] NTP=%s (saved)\n", settings::ntpServer());
      net::requestNtpSync();
    } else {
      Serial.println("[CONSOLE] ERROR: invalid host name");
    }
    return false;
  }
  if (strncasecmp(text, "BRIGHT ", 7) == 0) {
    char which[8];
    int pct;
    if (sscanf(text + 7, "%7s %d", which, &pct) != 2 || pct < 0 || pct > 100 ||
        (strcasecmp(which, "DAY") != 0 && strcasecmp(which, "NIGHT") != 0)) {
      Serial.println("[CONSOLE] Usage: BRIGHT DAY|NIGHT <0-100>");
      return false;
    }
    if (!settings::setBrightness(strcasecmp(which, "NIGHT") == 0, pct)) {
      Serial.println("[CONSOLE] ERROR: could not save");
      return false;
    }
    printBrightness();
    return true;
  }
  if (strncasecmp(text, "NIGHT ", 6) == 0) {
    const char* arg = text + 6;
    int h1, m1, h2, m2;
    bool ok;
    if (strcasecmp(arg, "ON") == 0 || strcasecmp(arg, "OFF") == 0) {
      ok = settings::setNightEnabled(strcasecmp(arg, "ON") == 0);
    } else if (sscanf(arg, "%d:%d %d:%d", &h1, &m1, &h2, &m2) == 4 && h1 >= 0 && h1 < 24 &&
               h2 >= 0 && h2 < 24 && m1 >= 0 && m1 < 60 && m2 >= 0 && m2 < 60) {
      ok = settings::setNightWindow(h1 * 60 + m1, h2 * 60 + m2);
    } else {
      Serial.println("[CONSOLE] Usage: NIGHT ON|OFF|HH:MM HH:MM");
      return false;
    }
    if (!ok) {
      Serial.println("[CONSOLE] ERROR: invalid or not saved");
      return false;
    }
    printBrightness();
    return true;
  }
  if (strncasecmp(text, "WEATHER", 7) == 0 && (text[7] == '\0' || text[7] == ' ')) {
    const char* arg = text + 7;
    while (*arg == ' ') ++arg;
    float lat, lon;
    int consumed = 0;
    if (*arg == '\0') {
      weather::printStatus();
    } else if (strcasecmp(arg, "UPDATE") == 0) {
      weather::requestUpdate();
    } else if (strcasecmp(arg, "SHOW") == 0) {
      weather::requestScreen(30000);
    } else if (strncasecmp(arg, "CITY ", 5) == 0 && arg[5]) {
      weather::requestCity(arg + 5);
    } else if (strncasecmp(arg, "LOC ", 4) == 0 &&
               sscanf(arg + 4, "%f %f %n", &lat, &lon, &consumed) == 2) {
      if (settings::setLocation(lat, lon, arg + 4 + consumed)) {
        Serial.printf("[WEATHER] Location %s (%.3f, %.3f) saved\n", settings::place(), lat, lon);
        weather::requestUpdate();
      } else {
        Serial.println("[CONSOLE] ERROR: invalid coordinates");
      }
    } else if (strcasecmp(arg, "CYCLE ON") == 0 || strcasecmp(arg, "CYCLE OFF") == 0) {
      settings::setWeatherCycle(strcasecmp(arg, "CYCLE ON") == 0);
      weather::printStatus();
    } else {
      Serial.println("[CONSOLE] Usage: WEATHER [UPDATE|SHOW|CITY name|LOC lat lon [name]|CYCLE ON|OFF]");
    }
    return false;
  }
  if (strncasecmp(text, "PHOTO", 5) == 0 && (text[5] == '\0' || text[5] == ' ')) {
    return handlePhoto(text + 5);
  }
  if (strncasecmp(text, "BANNER", 6) == 0 && (text[6] == '\0' || text[6] == ' ')) {
    return handleBanner(text + 6);
  }
  if (strcasecmp(text, "GET") == 0) {
    printRtcTime(rtc);
  } else if (strncasecmp(text, "SCREEN ", 7) == 0) {
    if (!requestScreenByName(text + 7)) {
      Serial.println("[CONSOLE] Usage: SCREEN CLOCK|FORECAST|PHOTO|INFO|SETTINGS");
    }
  } else if (strcasecmp(text, "TOUCH CAL") == 0) {
    calibrateTouch(display);
    return true;
  } else if (strcasecmp(text, "FORMAT 12") == 0 || strcasecmp(text, "FORMAT 24") == 0) {
    settings::setHour12(strcasecmp(text, "FORMAT 12") == 0);
    Serial.printf("[SETTINGS] %s-hour clock (saved)\n", settings::hour12() ? "12" : "24");
    return true;
  } else if (strcasecmp(text, "TOUCH") == 0) {
    // Prints readings for 10 s (blocking; development only).
    Serial.println("[TOUCH] Touch the screen; printing for 10 s");
    const uint32_t until = millis() + 10000;
    uint32_t lastPrint = 0;
    while ((int32_t)(millis() - until) < 0) {
      uint16_t rx, ry, z;
      if (millis() - lastPrint >= 100 && touch::readRaw(rx, ry, z)) {
        lastPrint = millis();
        touch::Point p = touch::map(rx, ry);
        Serial.printf("[TOUCH] raw %4u,%4u z %4u -> %3d,%3d\n", rx, ry, z, p.x, p.y);
      }
      delay(10);
    }
  } else if (strcasecmp(text, "OTA") == 0) {
    ota::printStatus();
  } else if (strcasecmp(text, "INFO") == 0) {
    printInfo(rtc);
    printBrightness();
  } else if (strcasecmp(text, "BRIGHT") == 0 || strcasecmp(text, "NIGHT") == 0) {
    printBrightness();
  } else if (strcasecmp(text, "TEST PALETTE") == 0) {
    display.showPaletteTest(30000);
  } else if (strcasecmp(text, "TEST") == 0) {
    display.showTestPattern(20000);
  } else if (strcasecmp(text, "SHOT") == 0) {
    display.screenshot(Serial);
  } else if (strcasecmp(text, "NTP") == 0) {
    Serial.printf("[NTP] Server %s\n", settings::ntpServer());
    clock_sync::printStatus();
  } else if (strcasecmp(text, "NTP SYNC") == 0) {
    net::requestNtpSync();
  } else if (strcasecmp(text, "WIFI") == 0) {
    net::printStatus();
  } else if (strcasecmp(text, "WIFI SCAN") == 0) {
    net::requestScan();
  } else if (strcasecmp(text, "WIFI PORTAL") == 0) {
    net::requestPortal();
  } else if (strcasecmp(text, "WIFI FORGET") == 0) {
    net::requestForget();
  } else {
    if (strcasecmp(text, "HELP") != 0) Serial.printf("[CONSOLE] Unknown command: %s\n", text);
    serialConsolePrintHelp();
  }
  return false;
}

}  // namespace

bool serialConsoleExecute(RtcManager& rtc, DisplayManager& display, const char* command) {
  char buf[kLineMax];
  strlcpy(buf, command, sizeof(buf));
  return handleLine(rtc, display, buf);
}

void serialConsolePrintHelp() {
  Serial.println(
      "[CONSOLE] Commands: SET YYYY-MM-DD HH:MM:SS (local) | GET | TZ [posix] | INFO | "
      "WIFI [PORTAL|FORGET] | NTP [SYNC|SERVER host] | BRIGHT [DAY|NIGHT n] | "
      "NIGHT [ON|OFF|HH:MM HH:MM] | FORMAT 12|24 | WEATHER [...] | BANNER [...] | PHOTO [...] | TOUCH [CAL] | "
      "SCREEN name | SETEPOCH n | REBOOT | OTA | SHOT | "
      "TEST [PALETTE] | HELP");
}

bool serialConsolePoll(RtcManager& rtc, DisplayManager& display) {
  bool changed = false;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (overflow) {
        Serial.println("[CONSOLE] ERROR: line too long");
      } else if (lineLen > 0) {
        line[lineLen] = '\0';
        changed |= handleLine(rtc, display, line);
      }
      lineLen = 0;
      overflow = false;
    } else if (c == '\b' || c == 0x7f) {
      if (lineLen > 0) --lineLen;
    } else if (lineLen < kLineMax - 1) {
      line[lineLen++] = c;
    } else {
      overflow = true;
    }
  }
  return changed;
}
