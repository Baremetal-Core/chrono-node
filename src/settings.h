// Chrono Node - persistent settings stored in ESP32 NVS.
#pragma once

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

}  // namespace settings
