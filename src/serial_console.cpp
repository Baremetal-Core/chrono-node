#include "serial_console.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <stdio.h>
#include <string.h>

#include "clock_sync.h"
#include "config.h"
#include "display_manager.h"
#include "net_manager.h"
#include "rtc_manager.h"
#include "settings.h"
#include "time_utils.h"

namespace {

constexpr size_t kLineMax = 96;
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
  if (strcasecmp(text, "GET") == 0) {
    printRtcTime(rtc);
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

void serialConsolePrintHelp() {
  Serial.println(
      "[CONSOLE] Commands: SET YYYY-MM-DD HH:MM:SS (local) | GET | TZ [posix] | INFO | "
      "WIFI [PORTAL|FORGET] | NTP [SYNC|SERVER host] | BRIGHT [DAY|NIGHT n] | "
      "NIGHT [ON|OFF|HH:MM HH:MM] | SHOT | TEST [PALETTE] | HELP");
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
