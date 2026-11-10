// Chrono Node - keeps the DS3231, the ESP32 system clock and NTP consistent.
//
// - Between NTP syncs the DS3231 is the reference and seeds the system clock.
// - Right after an NTP sync the system clock is the reference: the DS3231
//   offset is measured on its next second edge and, if needed, the DS3231 is
//   rewritten exactly on a system-clock second boundary.
// All DS3231 access stays in the main task; the NTP callback only sets a flag.
#pragma once

#include <RTClib.h>
#include <stddef.h>
#include <stdint.h>

class RtcManager;

namespace clock_sync {

// Thread-safe. Called by the SNTP callback after it set the system clock.
void notifyNtpSync();

// Main task: call on a true DS3231 second edge. `rtcTrusted` is false when
// the DS3231 reports lost power.
void onRtcSecondEdge(const DateTime& utc, bool rtcTrusted);

// Main task: call every loop iteration. Returns true if the DS3231 was rewritten.
bool loop(RtcManager& rtc);

// Short status for the display: "NTP --", "NTP <1m", "NTP 5m", "NTP 3h", "NTP stale".
void statusText(char* buf, size_t len);

void printStatus();

}  // namespace clock_sync
