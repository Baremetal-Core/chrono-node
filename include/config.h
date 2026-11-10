// Chrono Node - project-wide configuration constants.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace config {

constexpr const char* kProjectName = "Chrono Node";
constexpr const char* kFirmwareName = "chrono-node-fw";
constexpr const char* kFirmwareVersion = "0.4.0";

#ifdef CHRONO_GIT_REV
constexpr const char* kGitRevision = CHRONO_GIT_REV;
#else
constexpr const char* kGitRevision = "unknown";
#endif

// POSIX TZ string used until one is saved with the serial "TZ" command.
// The DS3231 always holds UTC. Example: "<+07>-7" = UTC+7 without DST,
// "CET-1CEST,M3.5.0,M10.5.0/3" = Central Europe with DST.
constexpr const char* kDefaultTimezone = "<+07>-7";
constexpr size_t kTimezoneMaxLen = 63;

constexpr uint32_t kSerialBaud = 115200;

// External RTC on the CYD LEFT 4-pin connector (3.3V, GPIO27, GPIO22, GND).
// Pins are routed through the ESP32 GPIO matrix; do not rely on default I2C pins.
constexpr int kI2cSdaPin = 27;
constexpr int kI2cSclPin = 22;
constexpr uint32_t kI2cFrequencyHz = 100000;

constexpr uint8_t kDs3231Address = 0x68;

// The RTC is polled faster than 1 Hz so the display follows the DS3231 second
// boundary closely; the screen is only redrawn when the second changes.
constexpr uint32_t kRtcPollIntervalMs = 50;

// Between NTP syncs the DS3231 is the reference: the ESP32 system clock is
// re-seeded from it when they differ by more than this.
constexpr int32_t kSystemClockMaxDriftMs = 200;
constexpr uint32_t kRtcReprobeIntervalMs = 2000;

// ---- Network ----
// mDNS name (chrono-node.local) and DHCP hostname.
constexpr const char* kHostname = "chrono-node";
// Setup portal SSID prefix; the last two MAC bytes are appended, e.g. Chrono-Node-1A2B.
constexpr const char* kPortalSsidPrefix = "Chrono-Node-";
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kWifiRetryMinMs = 10000;
constexpr uint32_t kWifiRetryMaxMs = 300000;
// Portal auto-closes after this long when credentials exist (0 = stay open).
constexpr uint32_t kPortalTimeoutS = 180;
// Max time the display may pause while the portal tests newly entered credentials.
constexpr uint32_t kPortalSaveConnectTimeoutS = 15;

// ---- NTP ----
constexpr const char* kDefaultNtpServer = "pool.ntp.org";
constexpr size_t kNtpServerMaxLen = 63;
constexpr uint32_t kNtpSyncIntervalMs = 6UL * 60 * 60 * 1000;
// After an NTP sync the DS3231 is rewritten if it is off by more than this.
// Must stay below kSystemClockMaxDriftMs so a tolerated DS3231 offset never
// pulls the NTP-set system clock away.
constexpr int32_t kRtcCorrectThresholdMs = 100;
// Sync status is shown as stale when the last successful sync is older than this.
constexpr uint32_t kNtpStaleS = 24UL * 60 * 60;

// Set to false to silence the once-per-second time log.
constexpr bool kLogEverySecond = true;

}  // namespace config
