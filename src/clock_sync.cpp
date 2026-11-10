#include "clock_sync.h"

#include <Arduino.h>
#include <stdlib.h>
#include <sys/time.h>

#include <atomic>

#include "config.h"
#include "rtc_manager.h"

namespace clock_sync {
namespace {

// The edge is detected up to one poll interval late; assume the middle.
constexpr int32_t kEdgeLatencyMs = config::kRtcPollIntervalMs / 2;
constexpr uint32_t kMeasureTimeoutMs = 2500;
constexpr uint32_t kWriteTimeoutMs = 3000;
// The DS3231 write is started this close to a system second boundary.
constexpr int32_t kWriteWindowUs = 10000;
// Minimum interval between offset measurements for a drift estimate.
constexpr int64_t kMinDriftIntervalMs = 60LL * 60 * 1000;

std::atomic<bool> ntpEvent{false};
std::atomic<bool> everSynced{false};
std::atomic<uint32_t> lastSyncMs{0};
std::atomic<uint32_t> syncCount{0};

enum class Phase { Idle, Measure, Write };
Phase phase = Phase::Idle;
uint32_t phaseDeadlineMs = 0;
bool systemSeeded = false;

int32_t lastOffsetMs = 0;  // DS3231 minus NTP time at the last measurement
bool haveReference = false;
int64_t refSysMs = 0;
int32_t refOffsetMs = 0;

int64_t systemNowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void enterPhase(Phase p, uint32_t timeoutMs) {
  phase = p;
  phaseDeadlineMs = millis() + timeoutMs;
}

bool phaseExpired() { return (int32_t)(millis() - phaseDeadlineMs) >= 0; }

void setReference(int64_t sysMs, int32_t offsetMs) {
  haveReference = true;
  refSysMs = sysMs;
  refOffsetMs = offsetMs;
}

void measureAgainstNtp(const DateTime& utc, bool rtcTrusted) {
  const int64_t sysMs = systemNowMs() - kEdgeLatencyMs;
  const int32_t offsetMs = (int32_t)((int64_t)utc.unixtime() * 1000 - sysMs);
  lastOffsetMs = offsetMs;

  if (haveReference && sysMs - refSysMs >= kMinDriftIntervalMs) {
    double hours = (sysMs - refSysMs) / 3600000.0;
    double ppm = (offsetMs - refOffsetMs) * 1000.0 / (sysMs - refSysMs) * 1000.0;
    Serial.printf("[NTP] DS3231 offset %+ld ms (drift %+.1f ppm over %.1f h)\n", (long)offsetMs,
                  ppm, hours);
  } else {
    Serial.printf("[NTP] DS3231 offset %+ld ms\n", (long)offsetMs);
  }

  if (!rtcTrusted || labs(offsetMs) > config::kRtcCorrectThresholdMs) {
    enterPhase(Phase::Write, kWriteTimeoutMs);
  } else {
    setReference(sysMs, offsetMs);
    phase = Phase::Idle;
  }
}

void disciplineSystemClock(const DateTime& utc) {
  const int64_t rtcMs = (int64_t)utc.unixtime() * 1000 + kEdgeLatencyMs;
  const int64_t driftMs = systemNowMs() - rtcMs;
  if (systemSeeded && llabs(driftMs) <= config::kSystemClockMaxDriftMs) return;

  struct timeval tv = {static_cast<time_t>(utc.unixtime()), kEdgeLatencyMs * 1000};
  settimeofday(&tv, nullptr);
  if (systemSeeded) {
    Serial.printf("[TIME] System clock corrected from DS3231 (drift %lld ms)\n", driftMs);
  } else {
    Serial.println("[TIME] System clock seeded from DS3231");
  }
  systemSeeded = true;
}

// Waits (at most kWriteWindowUs) for the next system second and writes it to the DS3231.
// Writing the seconds register restarts the DS3231 1 Hz countdown, aligning both clocks.
bool writeOnSecondBoundary(RtcManager& rtc) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_usec < 1000000 - kWriteWindowUs) return false;
  const time_t target = tv.tv_sec + 1;
  do {
    gettimeofday(&tv, nullptr);
  } while (tv.tv_sec < target);
  return rtc.setTime(DateTime(static_cast<uint32_t>(target)));
}

}  // namespace

void notifyNtpSync() {
  lastSyncMs = millis();
  everSynced = true;
  syncCount++;
  ntpEvent = true;
}

void onRtcSecondEdge(const DateTime& utc, bool rtcTrusted) {
  switch (phase) {
    case Phase::Measure:
      measureAgainstNtp(utc, rtcTrusted);
      break;
    case Phase::Write:
      break;  // the system clock holds NTP time until the DS3231 is rewritten
    case Phase::Idle:
      if (rtcTrusted) disciplineSystemClock(utc);
      break;
  }
}

bool loop(RtcManager& rtc) {
  if (ntpEvent.exchange(false)) {
    Serial.println("[NTP] Time received");
    enterPhase(Phase::Measure, kMeasureTimeoutMs);
  }

  if (phase == Phase::Measure && phaseExpired()) {
    // No DS3231 second edge was seen: its time is invalid, or it is missing.
    if (rtc.isPresent()) {
      Serial.println("[NTP] DS3231 time not readable, rewriting it");
      enterPhase(Phase::Write, kWriteTimeoutMs);
    } else {
      Serial.println("[NTP] DS3231 not detected, not corrected");
      phase = Phase::Idle;
    }
  }

  if (phase != Phase::Write) return false;
  if (phaseExpired()) {
    Serial.println("[NTP] ERROR: could not write DS3231");
    phase = Phase::Idle;
    return false;
  }
  if (!writeOnSecondBoundary(rtc)) return false;

  phase = Phase::Idle;
  setReference(systemNowMs(), 0);
  Serial.println("[NTP] DS3231 corrected from NTP");
  return true;
}

void statusText(char* buf, size_t len) {
  if (!everSynced) {
    snprintf(buf, len, "NTP --");
    return;
  }
  uint32_t ageS = (millis() - lastSyncMs) / 1000;
  if (ageS > config::kNtpStaleS) {
    snprintf(buf, len, "NTP stale");
  } else if (ageS < 60) {
    snprintf(buf, len, "NTP <1m");
  } else if (ageS < 3600) {
    snprintf(buf, len, "NTP %lum", (unsigned long)(ageS / 60));
  } else {
    snprintf(buf, len, "NTP %luh", (unsigned long)(ageS / 3600));
  }
}

bool isStale() { return everSynced && (millis() - lastSyncMs) / 1000 > config::kNtpStaleS; }

void printStatus() {
  if (!everSynced) {
    Serial.println("[NTP] Not synced since boot");
    return;
  }
  Serial.printf("[NTP] Last sync %lus ago (%lu syncs since boot), last DS3231 offset %+ld ms\n",
                (unsigned long)((millis() - lastSyncMs) / 1000), (unsigned long)syncCount.load(),
                (long)lastOffsetMs);
}

}  // namespace clock_sync
