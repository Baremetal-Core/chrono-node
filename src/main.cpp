// Chrono Node - firmware entry point (milestone 1: display + DS3231 bring-up).
//
// The displayed time always comes directly from the DS3231. The RTC is never
// written at boot; use the serial console "SET" command during development.

#include <Arduino.h>

#include "config.h"
#include "display_manager.h"
#include "rtc_manager.h"
#include "serial_console.h"

namespace {

RtcManager rtc;
DisplayManager display;

enum class RtcState { Ok, Missing, InvalidTime };
RtcState rtcState = RtcState::Missing;
bool rtcLostPower = false;

uint32_t lastPollMs = 0;
uint32_t lastReprobeMs = 0;
int lastSecond = -1;

void logTime(const DateTime& t) {
  Serial.printf("[RTC] %02u:%02u:%02u %04u-%02u-%02u\n", t.hour(), t.minute(), t.second(),
                t.year(), t.month(), t.day());
}

void refreshLostPower() {
  rtcLostPower = rtc.lostPower();
  if (rtcLostPower) {
    Serial.println("[RTC] WARNING: DS3231 lost power, time is not valid. Use SET to fix.");
  }
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

void pollRtc(bool forceLog = false) {
  DateTime now;
  if (!rtc.read(now)) {
    if (!rtc.isPresent()) {
      enterMissing();
    } else if (rtcState != RtcState::InvalidTime) {
      Serial.println("[RTC] ERROR: DS3231 returned an invalid time. Use SET to fix.");
      rtcState = RtcState::InvalidTime;
      display.showError("RTC TIME INVALID", "Set time: SET YYYY-MM-DD HH:MM:SS");
    }
    return;
  }

  rtcState = RtcState::Ok;
  if (now.second() == lastSecond) return;
  lastSecond = now.second();

  display.showTime(now);
  display.setStatus(rtcLostPower ? "RTC lost power - time not set" : nullptr);
  if (forceLog || config::kLogEverySecond) logTime(now);
}

}  // namespace

void setup() {
  Serial.begin(config::kSerialBaud);
  Serial.println();
  Serial.printf("[%s] Booting %s v%s\n", config::kProjectName, config::kFirmwareName,
                config::kFirmwareVersion);

  if (display.begin()) {
    Serial.println("[DISPLAY] Initialized");
  } else {
    Serial.println("[DISPLAY] ERROR: initialization failed");
  }

  rtcState = RtcState::Missing;
  if (rtc.begin()) {
    onRtcDetected();
    lastPollMs = millis();
    pollRtc(true);
  } else {
    Serial.println("[RTC] ERROR: DS3231 not detected");
    display.showError("RTC ERROR", "DS3231 not detected (SDA=27 SCL=22)");
  }
  serialConsolePrintHelp();
}

void loop() {
  if (serialConsolePoll(rtc)) {
    rtcLostPower = false;  // writing the time clears the oscillator-stop flag
    lastSecond = -1;
  }

  const uint32_t nowMs = millis();
  if (rtcState == RtcState::Missing) {
    if (nowMs - lastReprobeMs >= config::kRtcReprobeIntervalMs) {
      lastReprobeMs = nowMs;
      if (rtc.probe()) onRtcDetected();
    }
    return;
  }

  if (nowMs - lastPollMs >= config::kRtcPollIntervalMs) {
    lastPollMs = nowMs;
    pollRtc();
  }
}
