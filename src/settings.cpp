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
constexpr const char* kKeyLat = "wx_lat";
constexpr const char* kKeyLon = "wx_lon";
constexpr const char* kKeyPlace = "wx_place";
constexpr const char* kKeyWxCycle = "wx_cycle";
constexpr const char* kKeyRecent = "wx_recent";
constexpr const char* kKeyHour12 = "hour12";
constexpr const char* kKeyBannerOn = "bn_on";
constexpr const char* kKeyBannerEvery = "bn_every";  // default for new messages
constexpr const char* kKeyBannerEvs = "bn_evs";      // per-message period, one byte each
constexpr const char* kKeyBannerSpeed = "bn_speed";
constexpr const char* kKeyBannerText = "bn_text";  // all messages, '\n'-separated
constexpr const char* kKeyPhotoEvery = "ph_every";
constexpr const char* kKeyForecastShow = "fc_show";
constexpr const char* kKeyPhotoShow = "ph_show";
constexpr const char* kKeyScreenIdle = "idle_s";
constexpr const char* kKeyColorStyle = "clr_style";
constexpr const char* kKeyColorCustom = "clr_custom";  // RGB565 top << 16 | bottom
constexpr const char* kKeyEffects = "fx";
constexpr const char* kKeyColorSpeed = "clr_speed";
constexpr const char* kKeyMixEvery = "mix_every";
constexpr const char* kKeyTransition = "transition";

char banners[config::kBannerMaxMessages][config::kBannerMaxLen + 1] = {};
uint8_t bannerEvs[config::kBannerMaxMessages] = {};
int bannerN = 0;
uint32_t bannerVer = 1;
bool bannerOn = true;
uint8_t bannerEvery = config::kDefaultBannerEveryMin;
uint8_t bannerPxPerS = config::kDefaultBannerSpeed;
uint8_t photoEvery = config::kDefaultPhotoEveryMin;
uint8_t forecastShow = config::kDefaultForecastShowS;
uint8_t photoShow = config::kDefaultPhotoShowS;
uint16_t screenIdle = config::kDefaultScreenIdleS;
uint8_t colorStyleValue = 0;
uint32_t colorCustomValue = (0x07FEUL << 16) | 0xF819;  // cyan -> magenta
uint8_t effectsValue = 0;
uint8_t colorSpeedValue = 1;   // normal
uint8_t mixEveryValue = 5;     // minutes
uint8_t transitionValue = 1;   // wipe

void splitBanners(const char* joined) {
  bannerN = 0;
  const char* p = joined;
  while (*p && bannerN < config::kBannerMaxMessages) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0) {
      size_t n = len < config::kBannerMaxLen ? len : config::kBannerMaxLen;
      memcpy(banners[bannerN], p, n);
      banners[bannerN][n] = '\0';
      ++bannerN;
    }
    if (!nl) break;
    p = nl + 1;
  }
}

struct RecentLocation {
  char name[config::kPlaceMaxLen + 1];
  float lat;
  float lon;
};
RecentLocation recents[settings::kMaxRecentLocations] = {};
int recentCount = 0;
bool use12h = false;

float lat = config::kDefaultLatitude;
float lon = config::kDefaultLongitude;
char placeName[config::kPlaceMaxLen + 1] = "";
bool wxCycle = true;

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
    if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
      lat = prefs.getFloat(kKeyLat, lat);
      lon = prefs.getFloat(kKeyLon, lon);
    }
    if (prefs.isKey(kKeyPlace)) prefs.getString(kKeyPlace, placeName, sizeof(placeName));
    wxCycle = prefs.getBool(kKeyWxCycle, wxCycle);
    use12h = prefs.getBool(kKeyHour12, use12h);
    bannerOn = prefs.getBool(kKeyBannerOn, bannerOn);
    bannerEvery = prefs.getUChar(kKeyBannerEvery, bannerEvery);
    bannerPxPerS = prefs.getUChar(kKeyBannerSpeed, bannerPxPerS);
    photoEvery = prefs.getUChar(kKeyPhotoEvery, photoEvery);
    forecastShow = prefs.getUChar(kKeyForecastShow, forecastShow);
    photoShow = prefs.getUChar(kKeyPhotoShow, photoShow);
    screenIdle = prefs.getUShort(kKeyScreenIdle, screenIdle);
    colorStyleValue = prefs.getUChar(kKeyColorStyle, colorStyleValue);
    colorCustomValue = prefs.getULong(kKeyColorCustom, colorCustomValue);
    effectsValue = prefs.getUChar(kKeyEffects, effectsValue);
    colorSpeedValue = prefs.getUChar(kKeyColorSpeed, colorSpeedValue) % 4;
    mixEveryValue = prefs.getUChar(kKeyMixEvery, mixEveryValue);
    transitionValue = prefs.getUChar(kKeyTransition, transitionValue) % 5;
    if (prefs.isKey(kKeyBannerText)) {
      String joined = prefs.getString(kKeyBannerText, "");
      splitBanners(joined.c_str());
    }
    // Before per-message periods, one period applied to all messages.
    for (int i = 0; i < bannerN; ++i) bannerEvs[i] = bannerEvery;
    if (prefs.isKey(kKeyBannerEvs) && prefs.getBytesLength(kKeyBannerEvs) == (size_t)bannerN) {
      prefs.getBytes(kKeyBannerEvs, bannerEvs, bannerN);
      for (int i = 0; i < bannerN; ++i) bannerEvs[i] = min<uint8_t>(bannerEvs[i], 60);
    }
    if (prefs.isKey(kKeyRecent)) {
      size_t len = prefs.getBytesLength(kKeyRecent);
      if (len % sizeof(RecentLocation) == 0 && len <= sizeof(recents)) {
        prefs.getBytes(kKeyRecent, recents, len);
        recentCount = len / sizeof(RecentLocation);
      }
    }
    prefs.end();
  }
  bool tzFromNvs = looksLikePosixTz(tz);
  applyTimezone(tzFromNvs ? tz : config::kDefaultTimezone);
  bool ntpFromNvs = looksLikeHostname(ntp);
  strlcpy(currentNtp, ntpFromNvs ? ntp : config::kDefaultNtpServer, sizeof(currentNtp));
  Serial.printf("[SETTINGS] TZ=%s (%s), NTP=%s (%s)\n", currentTz,
                tzFromNvs ? "saved" : "default", currentNtp, ntpFromNvs ? "saved" : "default");
  if (placeName[0] == '\0') strlcpy(placeName, config::kDefaultPlace, sizeof(placeName));
  if (recentCount == 0) {
    strlcpy(recents[0].name, placeName, sizeof(recents[0].name));
    recents[0].lat = lat;
    recents[0].lon = lon;
    recentCount = 1;
  }
  Serial.printf("[SETTINGS] Weather %s (%.3f, %.3f), forecast screen %s\n", placeName, lat, lon,
                wxCycle ? "on" : "off");
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

float latitude() { return lat; }
float longitude() { return lon; }
const char* place() { return placeName; }

bool setLocation(float newLat, float newLon, const char* name, bool remember) {
  if (newLat < -90 || newLat > 90 || newLon < -180 || newLon > 180) return false;
  char n[sizeof(placeName)];
  if (name && *name) {
    strlcpy(n, name, sizeof(n));
  } else {
    snprintf(n, sizeof(n), "%.2f, %.2f", newLat, newLon);
  }
  bool ok = save([&](Preferences& p) {
    return p.putFloat(kKeyLat, newLat) > 0 && p.putFloat(kKeyLon, newLon) > 0 &&
           p.putString(kKeyPlace, n) > 0;
  });
  if (ok) {
    lat = newLat;
    lon = newLon;
    strlcpy(placeName, n, sizeof(placeName));
  }
  if (ok && remember) {
    // Move (or insert) this location to the front of the recent list.
    RecentLocation entry = {};
    strlcpy(entry.name, n, sizeof(entry.name));
    entry.lat = newLat;
    entry.lon = newLon;
    int keep = 0;
    RecentLocation next[kMaxRecentLocations];
    next[keep++] = entry;
    for (int i = 0; i < recentCount && keep < kMaxRecentLocations; ++i) {
      if (strcmp(recents[i].name, n) != 0) next[keep++] = recents[i];
    }
    memcpy(recents, next, sizeof(RecentLocation) * keep);
    recentCount = keep;
    save([&](Preferences& p) {
      return p.putBytes(kKeyRecent, recents, sizeof(RecentLocation) * recentCount) > 0;
    });
  }
  return ok;
}

int recentLocationCount() { return recentCount; }

bool recentLocation(int index, char* name, size_t nameLen, float& outLat, float& outLon) {
  if (index < 0 || index >= recentCount) return false;
  strlcpy(name, recents[index].name, nameLen);
  outLat = recents[index].lat;
  outLon = recents[index].lon;
  return true;
}

bool forgetRecentLocation(const char* name) {
  int i = 0;
  while (i < recentCount && strcmp(recents[i].name, name) != 0) ++i;
  if (i == recentCount) return false;
  memmove(&recents[i], &recents[i + 1], sizeof(RecentLocation) * (recentCount - i - 1));
  --recentCount;
  return save([&](Preferences& p) {
    return recentCount ? p.putBytes(kKeyRecent, recents, sizeof(RecentLocation) * recentCount) > 0
                       : p.remove(kKeyRecent);
  });
}

namespace {

// Banner text may hold any printable UTF-8; control characters are rejected.
bool validBanner(const char* text) {
  size_t len = strlen(text);
  if (len == 0 || len > config::kBannerMaxLen) return false;
  for (size_t i = 0; i < len; ++i) {
    if (static_cast<unsigned char>(text[i]) < 0x20) return false;
  }
  return true;
}

bool saveBanners() {
  String joined;
  for (int i = 0; i < bannerN; ++i) {
    if (i) joined += '\n';
    joined += banners[i];
  }
  ++bannerVer;
  return save([&](Preferences& p) {
    if (!joined.length()) {
      p.remove(kKeyBannerText);
      p.remove(kKeyBannerEvs);
      return true;
    }
    return p.putString(kKeyBannerText, joined) > 0 && p.putBytes(kKeyBannerEvs, bannerEvs, bannerN) > 0;
  });
}

}  // namespace

int bannerCount() { return bannerN; }

const char* bannerText(int index) { return index >= 0 && index < bannerN ? banners[index] : ""; }

bool addBanner(const char* text) {
  if (bannerN >= config::kBannerMaxMessages || !validBanner(text)) return false;
  bannerEvs[bannerN] = bannerEvery;
  strlcpy(banners[bannerN++], text, sizeof(banners[0]));
  return saveBanners();
}

bool setBanner(int index, const char* text) {
  if (index < 0 || index >= bannerN || !validBanner(text)) return false;
  strlcpy(banners[index], text, sizeof(banners[0]));
  return saveBanners();
}

bool removeBanner(int index) {
  if (index < 0 || index >= bannerN) return false;
  for (int i = index; i < bannerN - 1; ++i) {
    memcpy(banners[i], banners[i + 1], sizeof(banners[0]));
    bannerEvs[i] = bannerEvs[i + 1];
  }
  --bannerN;
  return saveBanners();
}

bool clearBanners() {
  bannerN = 0;
  return saveBanners();
}

bool bannerEnabled() { return bannerOn; }

bool setBannerEnabled(bool enabled) {
  bool ok = save([&](Preferences& p) { return p.putBool(kKeyBannerOn, enabled) > 0; });
  if (ok) bannerOn = enabled;
  return ok;
}

uint8_t bannerEveryMinutes(int index) {
  return index >= 0 && index < bannerN ? bannerEvs[index] : bannerEvery;
}

bool setBannerEveryMinutes(int index, uint8_t minutes) {
  if (minutes > 60 || index >= bannerN) return false;
  if (index >= 0) {
    bannerEvs[index] = minutes;
    return saveBanners();
  }
  // All messages, and the default for new ones.
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyBannerEvery, minutes) > 0; });
  if (!ok) return false;
  bannerEvery = minutes;
  for (int i = 0; i < bannerN; ++i) bannerEvs[i] = minutes;
  return saveBanners();
}

uint32_t bannerVersion() { return bannerVer; }

uint8_t forecastShowSeconds() { return forecastShow; }
uint8_t photoShowSeconds() { return photoShow; }
uint16_t screenIdleSeconds() { return screenIdle; }

bool setForecastShowSeconds(uint8_t s) {
  if (s < kMinShowS || s > kMaxShowS) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyForecastShow, s) > 0; });
  if (ok) forecastShow = s;
  return ok;
}

bool setPhotoShowSeconds(uint8_t s) {
  if (s < kMinShowS || s > kMaxShowS) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyPhotoShow, s) > 0; });
  if (ok) photoShow = s;
  return ok;
}

bool setScreenIdleSeconds(uint16_t s) {
  if (s < kMinIdleS || s > kMaxIdleS) return false;
  bool ok = save([&](Preferences& p) { return p.putUShort(kKeyScreenIdle, s) > 0; });
  if (ok) screenIdle = s;
  return ok;
}

uint8_t colorStyle() { return colorStyleValue; }

bool setColorStyle(uint8_t style) {
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyColorStyle, style) > 0; });
  if (ok) colorStyleValue = style;
  return ok;
}

uint32_t customColors() { return colorCustomValue; }

bool setCustomColors(uint16_t top, uint16_t bottom) {
  const uint32_t v = (static_cast<uint32_t>(top) << 16) | bottom;
  bool ok = save([&](Preferences& p) { return p.putULong(kKeyColorCustom, v) > 0; });
  if (ok) colorCustomValue = v;
  return ok;
}

uint8_t effects() { return effectsValue; }

bool setEffects(uint8_t fx) {
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyEffects, fx) > 0; });
  if (ok) effectsValue = fx;
  return ok;
}

uint8_t colorSpeed() { return colorSpeedValue; }

bool setColorSpeed(uint8_t speed) {
  if (speed > 3) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyColorSpeed, speed) > 0; });
  if (ok) colorSpeedValue = speed;
  return ok;
}

uint8_t mixEveryMinutes() { return mixEveryValue; }

bool setMixEveryMinutes(uint8_t minutes) {
  if (minutes < 1 || minutes > 60) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyMixEvery, minutes) > 0; });
  if (ok) mixEveryValue = minutes;
  return ok;
}

uint8_t transition() { return transitionValue; }

bool setTransition(uint8_t t) {
  if (t > 4) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyTransition, t) > 0; });
  if (ok) transitionValue = t;
  return ok;
}

uint8_t photoEveryMinutes() { return photoEvery; }

bool setPhotoEveryMinutes(uint8_t minutes) {
  if (minutes > 60) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyPhotoEvery, minutes) > 0; });
  if (ok) photoEvery = minutes;
  return ok;
}

uint8_t bannerSpeed() { return bannerPxPerS; }

bool setBannerSpeed(uint8_t pxPerSecond) {
  if (pxPerSecond < 20 || pxPerSecond > 200) return false;
  bool ok = save([&](Preferences& p) { return p.putUChar(kKeyBannerSpeed, pxPerSecond) > 0; });
  if (ok) bannerPxPerS = pxPerSecond;
  return ok;
}

bool hour12() { return use12h; }

bool setHour12(bool enabled) {
  bool ok = save([&](Preferences& p) { return p.putBool(kKeyHour12, enabled) > 0; });
  if (ok) use12h = enabled;
  return ok;
}

bool weatherCycle() { return wxCycle; }

bool setWeatherCycle(bool enabled) {
  bool ok = save([&](Preferences& p) { return p.putBool(kKeyWxCycle, enabled) > 0; });
  if (ok) wxCycle = enabled;
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
