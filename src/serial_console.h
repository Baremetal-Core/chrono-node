// Chrono Node - minimal serial console for development.
//
// Commands (case-insensitive, terminated by Enter):
//   SET YYYY-MM-DD HH:MM:SS   set the DS3231 from local wall-clock time (stored as UTC)
//   GET                       print the current time (local and UTC)
//   TZ [posix]                show or set the timezone, e.g. TZ <+07>-7
//   INFO                      firmware, memory and RTC diagnostics
//   WIFI                      Wi-Fi status
//   WIFI PORTAL               open the Wi-Fi setup portal
//   WIFI FORGET               erase saved Wi-Fi credentials and open the portal
//   NTP                       NTP sync status
//   NTP SYNC                  sync with the NTP server now
//   NTP SERVER <host>         set and save the NTP server, then sync
//   BRIGHT [DAY|NIGHT <0-100>] show or set backlight levels (saved)
//   NIGHT [ON|OFF|HH:MM HH:MM] show or set the night mode schedule (saved)
//   WEATHER                   weather status and forecast
//   WEATHER UPDATE|SHOW       fetch now / show the forecast screen for 30 s
//   WEATHER CITY <name>       look up and save a location (Open-Meteo geocoding)
//   WEATHER LOC <lat> <lon> [name]  save a location by coordinates
//   WEATHER CYCLE ON|OFF      periodic forecast screen
//   TEST                      colour test pattern for 20 s (panel variant check)
//   TEST PALETTE              the five day-cycle palettes side by side for 30 s
//   SHOT                      dump the screen (tools/screenshot.py turns it into a PNG)
//   HELP                      list commands
#pragma once

class DisplayManager;
class RtcManager;

// Non-blocking; call from loop(). Returns true if the RTC time, timezone,
// brightness or night schedule changed.
bool serialConsolePoll(RtcManager& rtc, DisplayManager& display);

void serialConsolePrintHelp();
