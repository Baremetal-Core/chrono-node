// Chrono Node - firmware entry point.
//
// Time model: the DS3231 is the timekeeper and always holds UTC. Local time
// is derived with the POSIX TZ from settings. The display is always driven by
// DS3231 reads. clock_sync keeps the ESP32 system clock in step with the
// DS3231 and corrects the DS3231 after each NTP sync. The RTC is never
// written at boot; without network use the serial console "SET" command.

#include <Arduino.h>

#include "clock_sync.h"
#include "config.h"
#include "display_manager.h"
#include "net_manager.h"
#include "rtc_manager.h"
#include "serial_console.h"
#include "settings.h"
#include "time_utils.h"

namespace {

RtcManager rtc;
DisplayManager display;

enum class RtcState { Ok, Missing, InvalidTime };
RtcState rtcState = RtcState::Missing;
bool rtcLostPower = false;

uint32_t lastPollMs = 0;
uint32_t lastReadOkMs = 0;
uint32_t lastReprobeMs = 0;
int lastSecond = -1;

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

DisplayManager::NetLevel netLevel(net::State s) {
  switch (s) {
    case net::State::Connected: return DisplayManager::NetLevel::Ok;
    case net::State::Connecting: return DisplayManager::NetLevel::Busy;
    case net::State::Portal: return DisplayManager::NetLevel::Setup;
    case net::State::WaitingRetry: break;
  }
  return DisplayManager::NetLevel::Offline;
}

void updateNetworkStatus() {
  char wifi[48];
  net::statusText(wifi, sizeof(wifi));
  if (net::state() == net::State::Portal) {
    display.setNetworkStatus(wifi, netLevel(net::state()));
    return;
  }
  char ntp[16];
  clock_sync::statusText(ntp, sizeof(ntp));
  char text[72];
  snprintf(text, sizeof(text), "%s   %s", wifi, ntp);
  display.setNetworkStatus(text, netLevel(net::state()));
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
  display.showTime(local);
  display.setStatus(rtcLostPower ? "RTC lost power - time not set" : nullptr);
  updateNetworkStatus();
  if (forceLog || config::kLogEverySecond) logTime(utc);
}

}  // namespace

void setup() {
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

  rtcState = RtcState::Missing;
  if (rtc.begin()) {
    onRtcDetected();
    lastPollMs = millis();
    pollRtc(true);
  } else {
    Serial.println("[RTC] ERROR: DS3231 not detected");
    display.showError("RTC ERROR", "DS3231 not detected (SDA=27 SCL=22)");
  }

  // Wi-Fi starts after the first clock frame so the time is visible immediately.
  net::begin();
  serialConsolePrintHelp();
}

void loop() {
  if (clock_sync::loop(rtc)) {
    // DS3231 rewritten from NTP: this also clears its lost-power flag.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
  }

  if (serialConsolePoll(rtc)) {
    // Time or timezone changed: re-read the oscillator-stop flag and redraw.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
  }

  const uint32_t nowMs = millis();
  if (rtcState == RtcState::Missing) {
    if (nowMs - lastReprobeMs >= config::kRtcReprobeIntervalMs) {
      lastReprobeMs = nowMs;
      if (rtc.probe()) onRtcDetected();
    }
  } else if (nowMs - lastPollMs >= config::kRtcPollIntervalMs) {
    lastPollMs = nowMs;
    pollRtc();
  }

  delay(5);  // yield; keeps the loop responsive without spinning at 100% CPU
}
