#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"

namespace {

constexpr const char* kNamespace = "chrono-node";
constexpr const char* kKeyTimezone = "tz";
constexpr const char* kKeyNtpServer = "ntp";
constexpr const char* kKeyBrightDay = "bri_day";
constexpr const char* kKeyBrightNight = "bri_night";
constexpr const char* kKeyNightOn = "night_on";
constexpr const char* kKeyNightStart = "night_start";
constexpr const char* kKeyNightEnd = "night_end";

uint8_t briDay = config::kDefaultBrightnessDay;
uint8_t briNight = config::kDefaultBrightnessNight;
bool nightOn = true;
uint16_t nightFrom = config::kDefaultNightStart;
uint16_t nightTo = config::kDefaultNightEnd;

char currentTz[config::kTimezoneMaxLen + 1] = "";
char currentNtp[config::kNtpServerMaxLen + 1] = "";

void applyTimezone(const char* tz) {
  strlcpy(currentTz, tz, sizeof(currentTz));
  setenv("TZ", currentTz, 1);
  tzset();
}

// Light sanity check only: POSIX TZ starts with a name ("ICT", "<+07>") and has no spaces.
bool looksLikePosixTz(const char* tz) {
  size_t len = strlen(tz);
  if (len < 4 || len > config::kTimezoneMaxLen) return false;
  if (!(isalpha(static_cast<unsigned char>(tz[0])) || tz[0] == '<')) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!isprint(static_cast<unsigned char>(tz[i])) || tz[i] == ' ') return false;
  }
  return true;
}

bool looksLikeHostname(const char* host) {
  size_t len = strlen(host);
  if (len < 1 || len > config::kNtpServerMaxLen) return false;
  for (size_t i = 0; i < len; ++i) {
    char c = host[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
  }
  return true;
}

}  // namespace

namespace settings {

void begin() {
  Preferences prefs;
  char tz[sizeof(currentTz)] = "";
  char ntp[sizeof(currentNtp)] = "";
  // Read-write so a missing namespace is created instead of logging NOT_FOUND on first boot.
  if (prefs.begin(kNamespace, false)) {
    if (prefs.isKey(kKeyTimezone)) prefs.getString(kKeyTimezone, tz, sizeof(tz));
    if (prefs.isKey(kKeyNtpServer)) prefs.getString(kKeyNtpServer, ntp, sizeof(ntp));
    briDay = min<uint8_t>(prefs.getUChar(kKeyBrightDay, briDay), 100);
    briNight = min<uint8_t>(prefs.getUChar(kKeyBrightNight, briNight), 100);
    nightOn = prefs.getBool(kKeyNightOn, nightOn);
    nightFrom = prefs.getUShort(kKeyNightStart, nightFrom) % 1440;
    nightTo = prefs.getUShort(kKeyNightEnd, nightTo) % 1440;
    prefs.end();
  }
  bool tzFromNvs = looksLikePosixTz(tz);
  applyTimezone(tzFromNvs ? tz : config::kDefaultTimezone);
  bool ntpFromNvs = looksLikeHostname(ntp);
  strlcpy(currentNtp, ntpFromNvs ? ntp : config::kDefaultNtpServer, sizeof(currentNtp));
  Serial.printf("[SETTINGS] TZ=%s (%s), NTP=%s (%s)\n", currentTz,
                tzFromNvs ? "saved" : "default", currentNtp, ntpFromNvs ? "saved" : "default");
  Serial.printf("[SETTINGS] Brightness day %u%% night %u%%, night mode %s %02u:%02u-%02u:%02u\n",
                briDay, briNight, nightOn ? "on" : "off", nightFrom / 60, nightFrom % 60,
                nightTo / 60, nightTo % 60);
}

const char* timezone() { return currentTz; }

bool setTimezone(const char* posixTz) {
  if (!looksLikePosixTz(posixTz)) return false;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = prefs.putString(kKeyTimezone, posixTz) > 0;
  prefs.end();
  if (ok) applyTimezone(posixTz);
  return ok;
}

const char* ntpServer() { return currentNtp; }

namespace {

template <typename Fn>
bool save(Fn&& write) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = write(prefs);
  prefs.end();
  return ok;
}

}  // namespace

uint8_t brightnessDay() { return briDay; }
uint8_t brightnessNight() { return briNight; }

bool setBrightness(bool night, uint8_t percent) {
  if (percent > 100) return false;
  bool ok = save([&](Preferences& p) {
    return p.putUChar(night ? kKeyBrightNight : kKeyBrightDay, percent) > 0;
  });
  if (ok) (night ? briNight : briDay) = percent;
  return ok;
}

bool nightEnabled() { return nightOn; }
uint16_t nightStart() { return nightFrom; }
uint16_t nightEnd() { return nightTo; }

bool setNightEnabled(bool enabled) {
  bool ok = save([&](Preferences& p) { return p.putBool(kKeyNightOn, enabled) > 0; });
  if (ok) nightOn = enabled;
  return ok;
}

bool setNightWindow(uint16_t startMinute, uint16_t endMinute) {
  if (startMinute >= 1440 || endMinute >= 1440 || startMinute == endMinute) return false;
  bool ok = save([&](Preferences& p) {
    return p.putUShort(kKeyNightStart, startMinute) > 0 && p.putUShort(kKeyNightEnd, endMinute) > 0;
  });
  if (ok) {
    nightFrom = startMinute;
    nightTo = endMinute;
  }
  return ok;
}

bool isNightAt(int minuteOfDay) {
  if (!nightOn) return false;
  if (nightFrom < nightTo) return minuteOfDay >= nightFrom && minuteOfDay < nightTo;
  return minuteOfDay >= nightFrom || minuteOfDay < nightTo;  // wraps past midnight
}

bool setNtpServer(const char* host) {
  if (!looksLikeHostname(host)) return false;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  bool ok = prefs.putString(kKeyNtpServer, host) > 0;
  prefs.end();
  if (ok) strlcpy(currentNtp, host, sizeof(currentNtp));
  return ok;
}

}  // namespace settings
