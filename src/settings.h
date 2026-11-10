// Chrono Node - persistent settings stored in ESP32 NVS.
#pragma once

namespace settings {

// Loads settings from NVS and applies the timezone. Call once at boot.
void begin();

// Current POSIX TZ string, for example "<+07>-7".
const char* timezone();

// Validates, saves and applies a POSIX TZ string. Returns false if rejected.
bool setTimezone(const char* posixTz);

}  // namespace settings
