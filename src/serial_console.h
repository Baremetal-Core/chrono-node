// Chrono Node - minimal serial console for development.
//
// Commands (case-insensitive, terminated by Enter):
//   SET YYYY-MM-DD HH:MM:SS   set the DS3231 from local wall-clock time (stored as UTC)
//   GET                       print the current time (local and UTC)
//   TZ [posix]                show or set the timezone, e.g. TZ <+07>-7
//   INFO                      firmware, memory and RTC diagnostics
//   HELP                      list commands
#pragma once

class RtcManager;

// Non-blocking; call from loop(). Returns true if the RTC time or timezone changed.
bool serialConsolePoll(RtcManager& rtc);

void serialConsolePrintHelp();
