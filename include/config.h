// Chrono Node - project-wide configuration constants.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace config {

constexpr const char* kProjectName = "Chrono Node";
constexpr const char* kFirmwareName = "chrono-node-fw";
constexpr const char* kFirmwareVersion = "0.13.0";

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

// XPT2046 touch controller on its own SPI bus (VSPI); the display uses HSPI.
constexpr int kTouchClkPin = 25;
constexpr int kTouchMosiPin = 32;
constexpr int kTouchMisoPin = 39;
constexpr int kTouchCsPin = 33;
constexpr int32_t kTouchPressure = 400;       // minimum Z to count as a press
constexpr int32_t kTouchFirmPressure = 1000;  // minimum Z for a reliable position

// CYD on-board RGB LED (active-low); kept off.
constexpr int kLedRedPin = 4;
constexpr int kLedGreenPin = 16;
constexpr int kLedBluePin = 17;

// ---- Backlight and night mode ----
constexpr uint8_t kBacklightPin = 21;  // same as TFT_BL
constexpr uint8_t kBacklightPwmChannel = 7;
constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 12;
constexpr uint8_t kDefaultBrightnessDay = 90;    // percent
constexpr uint8_t kDefaultBrightnessNight = 15;  // percent
constexpr uint16_t kDefaultNightStart = 22 * 60;     // minutes after midnight
constexpr uint16_t kDefaultNightEnd = 6 * 60 + 30;
constexpr uint32_t kNightFadeOutMs = 600;
constexpr uint32_t kNightFadeInMs = 1500;
constexpr uint32_t kBrightnessFadeMs = 400;

// ---- Weather (Open-Meteo, no API key) ----
constexpr float kDefaultLatitude = 10.823f;  // Ho Chi Minh City
constexpr float kDefaultLongitude = 106.630f;
constexpr const char* kDefaultPlace = "Ho Chi Minh City";
constexpr size_t kPlaceMaxLen = 31;
constexpr uint32_t kWeatherRefreshMs = 15UL * 60 * 1000;
constexpr uint32_t kWeatherRetryMs = 60UL * 1000;
constexpr uint32_t kWeatherStaleS = 2UL * 60 * 60;  // shown as old after this
// Forecast screen: shown every kWeatherCycleMinutes for kWeatherShowS seconds,
// starting at second kWeatherShowAtSecond (not in night mode).
constexpr int kWeatherCycleMinutes = 2;
constexpr int kWeatherShowAtSecond = 45;
constexpr uint32_t kWeatherShowS = 10;
// Back to the clock after this long without touch (forecast/info/settings).
constexpr uint32_t kScreenIdleMs = 30000;
// Touching at night raises the backlight to the day level for this long.
constexpr uint32_t kNightTapBoostMs = 10000;
constexpr uint32_t kScreenFadeOutMs = 150;
constexpr uint32_t kScreenFadeInMs = 300;

// ---- Firmware updates ----
constexpr uint32_t kOtaConfirmAfterMs = 60000;  // run this long before confirming a new image
constexpr uint32_t kOtaPinValidMs = 120000;
// Releases the web page offers to install; the clock downloads the asset from
// https://github.com/<repo>/releases/download/<tag>/<asset>.
constexpr const char* kReleaseRepo = "Baremetal-Core/chrono-node";
constexpr const char* kReleaseAsset = "chrono-node-fw.bin";
constexpr uint32_t kOtaErrorShowMs = 60000;  // a failed install is reported this long

// ---- Scrolling banner ----
constexpr int kBannerMaxMessages = 5;
constexpr size_t kBannerMaxLen = 120;           // bytes of UTF-8 per message
constexpr uint8_t kDefaultBannerEveryMin = 1;   // 0 = continuous
constexpr uint8_t kDefaultBannerSpeed = 70;     // pixels per second
constexpr int kBannerStartSecond = 15;          // periodic banners start at this second
constexpr uint32_t kBannerGapMs = 2000;         // pause between messages when continuous

// ---- Photos (LittleFS) ----
constexpr int kPhotoSlots = 5;
constexpr size_t kPhotoMaxBytes = 64 * 1024;    // per JPEG; the web page compresses to fit
constexpr uint8_t kDefaultPhotoEveryMin = 5;    // slideshow period, 0 = off
constexpr int kPhotoShowAtSecond = 28;          // between the banner (15) and forecast (45)
constexpr uint32_t kPhotoShowS = 12;
constexpr uint32_t kPhotoRequestShowMs = 20000; // PHOTO SHOW / web "show" duration

// Task watchdog. A TLS handshake with open-meteo.com takes ~7 s, most of it one
// uninterruptible CPU-bound step (software ECDSA P-384 certificate checks), which
// starves core 0's idle task beyond the 5 s default and resets the clock.
constexpr uint32_t kTaskWatchdogS = 15;

// DS3231 die temperature (shown in the status bar) is read this often.
constexpr uint32_t kTemperatureIntervalMs = 60000;

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
// Enterprise APs often reject a single attempt (e.g. stale association from
// before a reset), so a few quick retries come before the setup portal / backoff.
constexpr uint32_t kWifiQuickRetries = 3;
constexpr uint32_t kWifiQuickRetryMs = 2000;
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
