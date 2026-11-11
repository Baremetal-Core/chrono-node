// Chrono Node - persistent settings stored in ESP32 NVS.
#pragma once

#include <stdint.h>

namespace settings {

// Loads settings from NVS and applies the timezone. Call once at boot.
void begin();

// Current POSIX TZ string, for example "<+07>-7".
const char* timezone();

// Validates, saves and applies a POSIX TZ string. Returns false if rejected.
bool setTimezone(const char* posixTz);

// NTP server host name, for example "pool.ntp.org".
const char* ntpServer();

// Validates and saves an NTP host name. Returns false if rejected.
bool setNtpServer(const char* host);

// Backlight levels in percent (0-100).
uint8_t brightnessDay();
uint8_t brightnessNight();
bool setBrightness(bool night, uint8_t percent);

// Night mode schedule in minutes after local midnight; the window may wrap
// past midnight (e.g. 22:00-06:30).
bool nightEnabled();
uint16_t nightStart();
uint16_t nightEnd();
bool setNightEnabled(bool enabled);
bool setNightWindow(uint16_t startMinute, uint16_t endMinute);

// True if night mode applies at `minuteOfDay` with the current schedule.
bool isNightAt(int minuteOfDay);

// Weather location (decimal degrees) and its display name.
float latitude();
float longitude();
const char* place();
bool setLocation(float lat, float lon, const char* name);

// Periodic forecast screen on/off.
bool weatherCycle();
bool setWeatherCycle(bool enabled);

}  // namespace settings
