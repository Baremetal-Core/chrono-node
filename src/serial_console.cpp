#include "serial_console.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "rtc_manager.h"

namespace {

constexpr size_t kLineMax = 48;
char line[kLineMax];
size_t lineLen = 0;
bool overflow = false;

void printRtcTime(RtcManager& rtc) {
  DateTime now;
  if (!rtc.read(now)) {
    Serial.println("[RTC] ERROR: DS3231 not readable");
    return;
  }
  Serial.printf("[RTC] %02u:%02u:%02u %04u-%02u-%02u\n", now.hour(), now.minute(),
                now.second(), now.year(), now.month(), now.day());
}

bool handleSet(RtcManager& rtc, const char* args) {
  int y, mo, d, h, mi, s;
  char extra;
  if (sscanf(args, "%4d-%2d-%2d %2d:%2d:%2d %c", &y, &mo, &d, &h, &mi, &s, &extra) != 6) {
    Serial.println("[CONSOLE] Usage: SET YYYY-MM-DD HH:MM:SS");
    return false;
  }
  if (y < 2000 || y > 2099) {
    Serial.println("[CONSOLE] ERROR: year must be 2000-2099");
    return false;
  }
  DateTime dt(y, mo, d, h, mi, s);
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59 || !dt.isValid() ||
      dt.day() != d) {
    Serial.println("[CONSOLE] ERROR: invalid date/time");
    return false;
  }
  if (!rtc.isPresent()) {
    Serial.println("[RTC] ERROR: DS3231 not detected, time not set");
    return false;
  }
  if (!rtc.setTime(dt)) {
    Serial.println("[RTC] ERROR: failed to write time");
    return false;
  }
  Serial.println("[RTC] Time set");
  printRtcTime(rtc);
  return true;
}

bool handleLine(RtcManager& rtc, char* text) {
  while (*text == ' ') ++text;
  if (*text == '\0') return false;

  if (strncasecmp(text, "SET ", 4) == 0) return handleSet(rtc, text + 4);
  if (strcasecmp(text, "GET") == 0) {
    printRtcTime(rtc);
    return false;
  }
  if (strcasecmp(text, "HELP") != 0) {
    Serial.printf("[CONSOLE] Unknown command: %s\n", text);
  }
  serialConsolePrintHelp();
  return false;
}

}  // namespace

void serialConsolePrintHelp() {
  Serial.println("[CONSOLE] Commands: SET YYYY-MM-DD HH:MM:SS | GET | HELP");
}

bool serialConsolePoll(RtcManager& rtc) {
  bool changed = false;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (overflow) {
        Serial.println("[CONSOLE] ERROR: line too long");
      } else if (lineLen > 0) {
        line[lineLen] = '\0';
        changed |= handleLine(rtc, line);
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
