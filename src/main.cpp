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
uint32_t lastTempMs = 0;
int lastSecond = -1;
bool hasTemp = false;
float tempC = 0;

// Night mode: switched behind a backlight fade so the repaint is never seen.
enum class NightPhase { Idle, FadingOut };
NightPhase nightPhase = NightPhase::Idle;
bool nightKnown = false;
bool nightActive = false;

uint8_t brightnessFor(bool night) {
  return night ? settings::brightnessNight() : settings::brightnessDay();
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
  if (want != nightActive && nightPhase == NightPhase::Idle) {
    nightActive = want;
    nightPhase = NightPhase::FadingOut;
    backlight::fadeTo(0, config::kNightFadeOutMs);
    Serial.println(want ? "[NIGHT] Night mode" : "[NIGHT] Day mode");
  }
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
  st.hasTemp = hasTemp;
  st.tempC = static_cast<int8_t>(lroundf(tempC));
  display.setStatus(st);

  if (rtcLostPower) {
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
  updateNight(local);
  display.showTime(local);
  updateStatus();
  if (forceLog || config::kLogEverySecond) logTime(utc);
}

}  // namespace

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

  rtcState = RtcState::Missing;
  if (rtc.begin()) {
    onRtcDetected();
    lastPollMs = millis();
    pollRtc(true);
  } else {
    Serial.println("[RTC] ERROR: DS3231 not detected");
    display.showError("RTC ERROR", "DS3231 not detected (SDA=27 SCL=22)");
  }

  if (!nightKnown) backlight::fadeTo(settings::brightnessDay(), config::kNightFadeInMs);

  // Wi-Fi starts after the first clock frame so the time is visible immediately.
  net::begin();
  serialConsolePrintHelp();
}

void loop() {
  backlight::loop();
  if (nightPhase == NightPhase::FadingOut && !backlight::isFading()) {
    // Screen is dark: switch colours, repaint, then fade back in.
    display.setNightMode(nightActive);
    lastSecond = -1;
    pollRtc();
    backlight::fadeTo(brightnessFor(nightActive), config::kNightFadeInMs);
    nightPhase = NightPhase::Idle;
  }

  if (clock_sync::loop(rtc)) {
    // DS3231 rewritten from NTP: this also clears its lost-power flag.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
  }

  display.tick();

  if (serialConsolePoll(rtc, display)) {
    // Time, timezone, brightness or schedule changed: re-read the
    // oscillator-stop flag, apply the brightness and redraw.
    rtcLostPower = rtc.lostPower();
    lastSecond = -1;
    if (nightPhase == NightPhase::Idle && nightKnown) {
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
