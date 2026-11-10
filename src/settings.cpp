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
    prefs.end();
  }
  bool tzFromNvs = looksLikePosixTz(tz);
  applyTimezone(tzFromNvs ? tz : config::kDefaultTimezone);
  bool ntpFromNvs = looksLikeHostname(ntp);
  strlcpy(currentNtp, ntpFromNvs ? ntp : config::kDefaultNtpServer, sizeof(currentNtp));
  Serial.printf("[SETTINGS] TZ=%s (%s), NTP=%s (%s)\n", currentTz,
                tzFromNvs ? "saved" : "default", currentNtp, ntpFromNvs ? "saved" : "default");
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
