// Chrono Node - minimal serial console for development.
//
// Commands (case-insensitive, terminated by Enter):
//   SET YYYY-MM-DD HH:MM:SS   write the given 24h time to the DS3231
//   GET                       print the current DS3231 time
//   HELP                      list commands
#pragma once

class RtcManager;

// Non-blocking; call from loop(). Returns true if the RTC time was changed.
bool serialConsolePoll(RtcManager& rtc);

void serialConsolePrintHelp();
